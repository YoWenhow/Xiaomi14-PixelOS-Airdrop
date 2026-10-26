/* SPDX-License-Identifier: ISC */
#include "wlan_hdd_passthru_owner.h"
#include "wlan_hdd_wonder_handoff.h"
#include "wlan_hdd_passthru_vdev.h"
#ifndef HOUJI_PT_OWNER_HOST_TEST
#include <linux/etherdevice.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include "wlan_hdd_main.h"
#include "wlan_hdd_object_manager.h"
#include "wlan_vdev_mlme_api.h"
#include "sme_passthru_request.h"
#include "wlan_dp_passthru.h"
#include "cds_api.h"
#include "wma_api.h"
#endif

#define HDD_PT_RECHECK_MS 20
#define HDD_PT_STOP_MS 6000

struct hdd_pt_owner {
	refcount_t refs;
	struct mutex control;
	spinlock_t work_lock;
	atomic_t recovering;
	bool recovery_prepared;
	bool recovery_collected;
	struct hdd_pt_vdev_record *record;
	struct delayed_work work;
	struct hdd_wh_gate gate;
	struct hdd_pt_registry *registry;
	struct wlan_hdd_link_info *link;
	struct wlan_objmgr_vdev *vdev;
	struct cdp_soc_t *soc;
	mac_handle_t mac;
	struct sme_passthru_request *request;
	struct wlan_dp_passthru_endpoint *endpoint;
	enum sme_passthru_session_state pe_state;
	enum sme_passthru_request_action action;
	enum hdd_pt_phase phase;
	atomic_t closing;
	u16 frequency;
	u8 vdev_id;
	u8 bssid[6];
	bool active;
	bool channel_busy;
	bool detach_attempted;
	bool stop_armed;
	bool stop_wait_started;
	unsigned long stop_deadline;
	int error;
	int detach_error;
};

static void hdd_pt_put(void *context)
{
	struct hdd_pt_owner *owner = context;

	if (refcount_dec_and_test(&owner->refs))
		kfree(owner);
}

static bool hdd_pt_get(void *context)
{
	struct hdd_pt_owner *owner = context;

	return refcount_inc_not_zero(&owner->refs);
}

/* Public operation tails are counted separately from the DP consumer ref.
 * The registry pins the record until users == 0 AND cancel_work_sync returns.
 */
static struct hdd_pt_owner *
hdd_pt_acquire(struct hdd_pt_registry *registry, u64 generation)
{
	struct hdd_pt_owner *owner = NULL;

	spin_lock_bh(&registry->lock);
	if (registry->generation == generation && !registry->reaping &&
	    registry->owner && registry->users != UINT_MAX) {
		owner = registry->owner;
		registry->users++;
	}
	spin_unlock_bh(&registry->lock);
	return owner;
}

static void hdd_pt_release(struct hdd_pt_registry *registry)
{
	spin_lock_bh(&registry->lock);
	registry->users--;
	if (!registry->users)
		wake_up_all(&registry->idle);
	spin_unlock_bh(&registry->lock);
}

static void hdd_pt_free_list(struct sk_buff *head)
{
	struct sk_buff *next;

	while (head) {
		next = qdf_nbuf_next(head);
		qdf_nbuf_set_next(head, NULL);
		qdf_nbuf_free(head);
		head = next;
	}
}

static void hdd_pt_rx(void *context, qdf_nbuf_t skb)
{
	struct hdd_pt_owner *owner = context;
	struct hdd_wh_ticket ticket = {0};

	if (hdd_wh_enter(&owner->gate, owner->gate.generation, &ticket)) {
		qdf_nbuf_free(skb);
		return;
	}
	if (!smp_load_acquire(&owner->active)) {
		qdf_nbuf_free(skb);
	} else {
		/* DP already supplied radiotap + raw 802.11, not Ethernet.
		 * The physical netdev's registered Wonder RX handler consumes it.
		 * Never retain/call a pointer into Wonder's context or module.
		 */
		skb->dev = owner->gate.sink;
		skb->protocol = htons(ETH_P_802_2);
		skb_reset_mac_header(skb);
		skb->pkt_type = PACKET_OTHERHOST;
		skb->ip_summed = CHECKSUM_NONE;
		netif_rx(skb);
	}
	hdd_wh_leave(&ticket);
}

static const struct wlan_dp_passthru_owner_ops hdd_pt_dp_ops = {
	.get = hdd_pt_get,
	.put = hdd_pt_put,
	.rx = hdd_pt_rx,
};

static void hdd_pt_schedule(struct hdd_pt_owner *owner, unsigned long delay)
{
	/* Serialize check+enqueue against freeze before cancel_work_sync.
	 * An atomic flag alone permits a late enqueue after cancellation.
	 */
	spin_lock_bh(&owner->work_lock);
	if (!atomic_read(&owner->recovering))
		mod_delayed_work(system_unbound_wq, &owner->work, delay);
	spin_unlock_bh(&owner->work_lock);
}

static void hdd_pt_close_owner(struct hdd_pt_owner *owner,
			       const char country[3], unsigned int reason)
{
	hdd_wh_close(&owner->gate, owner->gate.generation, country, reason);
	atomic_set(&owner->closing, 1);
	smp_store_release(&owner->active, false);
}

static void hdd_pt_block(struct hdd_pt_owner *owner, int error)
{
	if (!owner->error)
		owner->error = error;
	owner->phase = HDD_PT_BLOCKED;
}

/* No sleeping request wait under RTNL. Late CREATE results are collected by
 * the same preallocated worker; cancellation is never treated as completion.
 */
static int hdd_pt_collect(struct hdd_pt_owner *owner)
{
	struct sme_passthru_request_result result;
	QDF_STATUS status;

	if (!owner->request)
		return 0;
	if (atomic_read(&owner->closing) &&
	    owner->action == SME_PASSTHRU_REQUEST_CREATE)
		sme_passthru_request_cancel(owner->request);
	status = sme_passthru_request_query(owner->request, &result);
	if (status == QDF_STATUS_E_PENDING)
		return -EINPROGRESS;
	if (QDF_IS_STATUS_ERROR(status))
		return qdf_status_to_os_return(status);
	owner->pe_state = result.session_state;
	sme_passthru_request_put(owner->request);
	owner->request = NULL;
	if (owner->pe_state == SME_PASSTHRU_SESSION_UNKNOWN)
		return -EUCLEAN;
	if (owner->action == SME_PASSTHRU_REQUEST_DELETE &&
	    owner->pe_state != SME_PASSTHRU_SESSION_ABSENT)
		return QDF_IS_STATUS_ERROR(result.status) ?
			qdf_status_to_os_return(result.status) : -EUCLEAN;
	if (QDF_IS_STATUS_ERROR(result.status)) {
		if (!owner->error)
			owner->error = qdf_status_to_os_return(result.status);
		hdd_pt_close_owner(owner, "00", 1);
	}
	return 0;
}

/* Caller holds owner->control and its close claim. No firmware waits.
 * A PE delete result only releases PE ownership, not the asynchronous WMA
 * stop. Keep the vdev pin until actual STOP/DOWN and their call tails finish.
 */
static int hdd_pt_stop_barrier(struct hdd_pt_owner *owner, bool arm_only)
{
	QDF_STATUS status;
#ifndef HOUJI_PT_OWNER_HOST_TEST
	struct vdev_mlme_obj *mlme = wlan_vdev_mlme_get_cmpt_obj(owner->vdev);

	if (!mlme)
		return -ENODEV;
#endif
	if (!owner->stop_wait_started) {
		owner->stop_wait_started = true;
		owner->stop_deadline = jiffies + msecs_to_jiffies(HDD_PT_STOP_MS);
	}
	if (time_after_eq(jiffies, owner->stop_deadline))
		return -ETIMEDOUT;
#ifdef HOUJI_PT_OWNER_HOST_TEST
	status = host_stop_barrier(owner->vdev, &owner->stop_armed, arm_only);
#else
	wlan_vdev_obj_lock(owner->vdev);
	if (!owner->stop_armed) {
		if (mlme->passthru_txn.tx_busy || mlme->passthru_txn.rx_busy) {
			status = QDF_STATUS_E_PENDING;
			goto unlock;
		}
		/* Nonzero cookie conservatively includes queued/failed STARTs.
		 * Never treat local INIT after a failed START as firmware proof.
		 */
		status = wlan_pt_stop_arm(&mlme->passthru_stop,
					  mlme->passthru_txn.cookie != 0);
		if (QDF_IS_STATUS_ERROR(status))
			goto unlock;
		wlan_pt_txn_seal(&mlme->passthru_txn);
		owner->stop_armed = true;
	}
	status = arm_only ? QDF_STATUS_SUCCESS :
		wlan_pt_stop_completion(&mlme->passthru_stop);
unlock:
	wlan_vdev_obj_unlock(owner->vdev);
#endif
	return status == QDF_STATUS_E_PENDING ? -EINPROGRESS :
		qdf_status_to_os_return(status);
}

static void hdd_pt_work(struct work_struct *work)
{
	struct hdd_pt_owner *owner =
		container_of(to_delayed_work(work), struct hdd_pt_owner, work);
	struct hdd_wh_claim claim = {0};
	QDF_STATUS status;
	int ret;
	bool again = false;

	mutex_lock(&owner->control);
	if (atomic_read(&owner->recovering) ||
	    owner->phase == HDD_PT_DRAINED || owner->phase == HDD_PT_BLOCKED)
		goto out;
	ret = hdd_pt_collect(owner);
	if (ret == -EINPROGRESS) {
		again = true;
		goto out;
	}
	if (ret) {
		hdd_pt_close_owner(owner, "00", 1);
		hdd_pt_block(owner, ret);
		goto out;
	}
	if (!atomic_read(&owner->closing)) {
		if (owner->pe_state != SME_PASSTHRU_SESSION_PRESENT) {
			hdd_pt_close_owner(owner, "00", 1);
			owner->error = -EIO;
		} else if (!owner->endpoint) {
			status = ucfg_dp_passthru_create(owner->soc, owner->vdev_id,
					owner->frequency, &hdd_pt_dp_ops, owner,
					&owner->endpoint);
			if (QDF_IS_STATUS_ERROR(status)) {
				owner->error = qdf_status_to_os_return(status);
				hdd_pt_close_owner(owner, "00", 1);
			} else {
				owner->phase = HDD_PT_BOUND;
			}
		}
		if (!atomic_read(&owner->closing))
			goto out;
	}

	owner->phase = HDD_PT_CLOSING;
	if (hdd_wh_try_claim(&owner->gate, owner->gate.generation, &claim)) {
		again = true;
		goto out;
	}
	if (owner->endpoint) {
		status = ucfg_dp_passthru_stop(owner->endpoint);
		if (status == QDF_STATUS_E_BUSY) {
			again = true;
			goto finish;
		}
		if (QDF_IS_STATUS_ERROR(status)) {
			hdd_pt_block(owner, qdf_status_to_os_return(status));
			goto finish;
		}
	}
	if (owner->pe_state == SME_PASSTHRU_SESSION_PRESENT) {
		ret = hdd_pt_stop_barrier(owner, true);
		if (ret == -EINPROGRESS) {
			again = true;
			goto finish;
		}
		if (ret) {
			hdd_pt_block(owner, ret);
			goto finish;
		}
		owner->action = SME_PASSTHRU_REQUEST_DELETE;
		status = sme_passthru_request_submit(owner->mac, owner->vdev,
				owner->action, NULL, &owner->request);
		if (QDF_IS_STATUS_ERROR(status)) {
			hdd_pt_block(owner, qdf_status_to_os_return(status));
			goto finish;
		}
		again = true;
		goto finish;
	}
	if (owner->pe_state != SME_PASSTHRU_SESSION_ABSENT) {
		hdd_pt_block(owner, -EUCLEAN);
		goto finish;
	}
	if (!owner->detach_attempted) {
		ret = hdd_pt_stop_barrier(owner, false);
		if (ret == -EINPROGRESS) {
			again = true;
			goto finish;
		}
		if (ret) {
			hdd_pt_block(owner, ret);
			goto finish;
		}
		/* Release our objmgr ref before invoking the real HDD deletion.
		 * A timed-out deletion is NEVER submitted a second time.
		 */
		hdd_objmgr_put_vdev_by_user(owner->vdev, WLAN_OSIF_ID);
		owner->vdev = NULL;
		owner->detach_attempted = true;
		owner->detach_error = owner->record ?
			hdd_pt_vdev_destroy(owner->link, owner->record) :
			hdd_vdev_destroy(owner->link);
	}
	if (owner->endpoint) {
		status = ucfg_dp_passthru_join(owner->endpoint);
		if (status == QDF_STATUS_E_BUSY) {
			again = true;
			goto finish;
		}
		if (QDF_IS_STATUS_ERROR(status)) {
			hdd_pt_block(owner, qdf_status_to_os_return(status));
			goto finish;
		}
		ucfg_dp_passthru_put(owner->endpoint);
		owner->endpoint = NULL;
	}
	if (owner->detach_error) {
		hdd_pt_block(owner, owner->detach_error);
		goto finish;
	}
	/* Extra sink ref goes away BEFORE caller unregisters the netdev.
	 * Network backlog is handled by that real unregister, not this flag.
	 */
	hdd_wh_finish(&claim, true);
	hdd_wh_release(&owner->gate);
	owner->phase = HDD_PT_DRAINED;
	goto out;
finish:
	hdd_wh_finish(&claim, false);
out:
	if (again)
		hdd_pt_schedule(owner, msecs_to_jiffies(HDD_PT_RECHECK_MS));
	mutex_unlock(&owner->control);
}

void hdd_pt_registry_init(struct hdd_pt_registry *registry, u64 generation)
{
	memset(registry, 0, sizeof(*registry));
	spin_lock_init(&registry->lock);
	init_waitqueue_head(&registry->idle);
	registry->generation = generation;
	registry->closed = !generation;
}

int hdd_pt_start_record(struct hdd_pt_registry *registry,
		 struct wlan_hdd_link_info *link, u16 frequency,
		 const u8 bssid[6], struct hdd_pt_vdev_record *record)
{
	struct hdd_pt_owner *owner;
	QDF_STATUS status;
	int ret = -EINVAL;

	if (!link || !link->adapter || !link->adapter->dev ||
	    !frequency || !bssid || !registry->generation)
		return -EINVAL;
	if (record && !hdd_pt_vdev_adoptable(record, link, registry->generation))
		return -EINVAL;
	owner = kzalloc(sizeof(*owner), GFP_KERNEL);
	if (!owner)
		return -ENOMEM;
	owner->vdev = hdd_objmgr_get_vdev_by_user(link, WLAN_OSIF_ID);
	if (!owner->vdev)
		goto free;
	if (record && record->vdev_id != wlan_vdev_get_id(owner->vdev))
		goto put_vdev;
	if (wlan_vdev_mlme_get_opmode(owner->vdev) != QDF_PASSTHRU_MODE)
		goto put_vdev;
	owner->soc = cds_get_context(QDF_MODULE_ID_SOC);
	owner->mac = WLAN_HDD_GET_CTX(link->adapter)->mac_handle;
	if (!owner->soc || !owner->mac)
		goto put_vdev;
	ret = hdd_wh_init(&owner->gate, link->adapter->dev,
			  registry->generation);
	if (ret)
		goto put_vdev;
	refcount_set(&owner->refs, 1); /* registry; DP installation adds its own */
	mutex_init(&owner->control);
	spin_lock_init(&owner->work_lock);
	atomic_set(&owner->recovering, 0);
	owner->record = record;
	INIT_DELAYED_WORK(&owner->work, hdd_pt_work);
	atomic_set(&owner->closing, 0);
	owner->registry = registry;
	owner->link = link;
	owner->frequency = frequency;
	owner->vdev_id = wlan_vdev_get_id(owner->vdev);
	memcpy(owner->bssid, bssid, sizeof(owner->bssid));
	owner->phase = HDD_PT_CREATING;
	owner->pe_state = SME_PASSTHRU_SESSION_ABSENT;
	owner->action = SME_PASSTHRU_REQUEST_CREATE;
	/* Hold the control mutex BEFORE publishing: retire may immediately
	 * close/schedule us, but must not race initial request publication.
	 */
	mutex_lock(&owner->control);
	spin_lock_bh(&registry->lock);
	if (registry->closed || registry->reaping || registry->owner) {
		ret = registry->closed ? -ESHUTDOWN : -EBUSY;
		spin_unlock_bh(&registry->lock);
		mutex_unlock(&owner->control);
		dev_put(owner->gate.sink); /* never published */
		goto put_vdev;
	}
	registry->owner = owner;
	registry->users++; /* initial submit/schedule tail */
	spin_unlock_bh(&registry->lock);
	if (!atomic_read(&owner->closing)) {
		status = sme_passthru_request_submit(owner->mac, owner->vdev,
				owner->action, owner->bssid, &owner->request);
		if (QDF_IS_STATUS_ERROR(status)) {
			owner->error = qdf_status_to_os_return(status);
			hdd_pt_close_owner(owner, "00", 1);
		}
	}
	hdd_pt_schedule(owner, 0);
	mutex_unlock(&owner->control);
	hdd_pt_release(registry);
	return 0;
put_vdev:
	hdd_objmgr_put_vdev_by_user(owner->vdev, WLAN_OSIF_ID);
free:
	kfree(owner);
	return ret;
}

int hdd_pt_start(struct hdd_pt_registry *registry,
		 struct wlan_hdd_link_info *link, u16 frequency,
		 const u8 bssid[6])
{
	return hdd_pt_start_record(registry, link, frequency, bssid, NULL);
}

int hdd_pt_recovery_freeze(struct hdd_pt_registry *registry, u64 generation)
{
	struct hdd_pt_owner *owner;
	struct hdd_wh_claim claim = {0};
	QDF_STATUS status;
	int ret = 0;

	/* Stale recovery must not close a new epoch. Pin owner during cancellation. */
	spin_lock_bh(&registry->lock);
	if (registry->generation != generation || registry->reaping) {
		spin_unlock_bh(&registry->lock);
		return -ESTALE;
	}
	registry->closed = true;
	owner = registry->owner;
	if (!owner) {
		spin_unlock_bh(&registry->lock);
		return 0;
	}
	if (registry->users == UINT_MAX) {
		spin_unlock_bh(&registry->lock);
		return -EOVERFLOW;
	}
	registry->users++;
	spin_unlock_bh(&registry->lock);
	spin_lock_bh(&owner->work_lock);
	atomic_set(&owner->recovering, 1);
	spin_unlock_bh(&owner->work_lock);
	hdd_pt_close_owner(owner, "00", 8);
	cancel_delayed_work_sync(&owner->work);
	mutex_lock(&owner->control);
	if (owner->phase == HDD_PT_DRAINED) {
		owner->recovery_prepared = true;
		goto out;
	}
	owner->phase = HDD_PT_RECOVERING;
	if (owner->recovery_prepared)
		goto out;
	ret = hdd_wh_try_claim(&owner->gate, generation, &claim);
	if (ret)
		goto out;
	if (owner->endpoint) {
		status = ucfg_dp_passthru_stop(owner->endpoint);
		if (QDF_IS_STATUS_ERROR(status)) {
			ret = qdf_status_to_os_return(status);
			goto finish;
		}
	}
	/* Requests/DP hold their own refs. Preserve them until real completion.
	 * Releasing ours allows later host teardown to make progress.
	 */
	if (owner->vdev) {
		hdd_objmgr_put_vdev_by_user(owner->vdev, WLAN_OSIF_ID);
		owner->vdev = NULL;
	}
	owner->recovery_prepared = true;
finish:
	hdd_wh_finish(&claim, false);
out:
	mutex_unlock(&owner->control);
	hdd_pt_release(registry);
	return ret;
}

/* The CDS caller has closed admission, joined scheduler/WMI/RX and completed
 * this epoch's PE purge. Do not reuse pre-purge request session-state results.
 * This collects local handles only; gate/netdev/registry ownership stays put.
 */
int hdd_pt_recovery_collect(struct hdd_pt_registry *registry, u64 generation)
{
	struct hdd_pt_owner *owner;
	struct hdd_wh_claim claim = {0};
	struct sme_passthru_request_result result;
	const struct sme_passthru_request_input *input;
	struct sme_passthru_request *request;
	struct hdd_pt_vdev_record *record;
	QDF_STATUS status;
	uint32_t outstanding = ~0U;
	bool hdd_pin;
	int ret = 0;

	spin_lock_bh(&registry->lock);
	if (registry->generation != generation || registry->reaping) {
		spin_unlock_bh(&registry->lock);
		return -ESTALE;
	}
	if (!registry->closed || registry->users == UINT_MAX) {
		spin_unlock_bh(&registry->lock);
		return -EBUSY;
	}
	owner = registry->owner;
	if (!owner) {
		spin_unlock_bh(&registry->lock);
		return 0;
	}
	registry->users++;
	spin_unlock_bh(&registry->lock);
	/* CDS may hold a transition. Never wait for a control operation there. */
	if (!mutex_trylock(&owner->control)) {
		ret = -EBUSY;
		goto release;
	}
	if (owner->phase == HDD_PT_DRAINED)
		goto unlock;
	if (!atomic_read(&owner->recovering) || !owner->recovery_prepared ||
	    owner->vdev || owner->phase != HDD_PT_RECOVERING) {
		ret = -EBUSY;
		goto unlock;
	}
	if (owner->recovery_collected)
		goto unlock;
	if (owner->mac != cds_get_context(QDF_MODULE_ID_SME) ||
	    owner->soc != cds_get_context(QDF_MODULE_ID_SOC)) {
		ret = -ESTALE;
		goto unlock;
	}
	status = sme_passthru_requests_drain_status(owner->mac, &outstanding);
	if (QDF_IS_STATUS_ERROR(status) || outstanding) {
		ret = QDF_IS_STATUS_ERROR(status) ?
			qdf_status_to_os_return(status) : -EBUSY;
		goto unlock;
	}
	ret = hdd_wh_try_claim(&owner->gate, generation, &claim);
	if (ret)
		goto unlock;
	if (owner->request) {
		status = sme_passthru_request_query(owner->request, &result);
		if (QDF_IS_STATUS_ERROR(status)) {
			ret = status == QDF_STATUS_E_PENDING ? -EBUSY :
				qdf_status_to_os_return(status);
			goto finish;
		}
		input = sme_passthru_request_input(owner->request);
		if (!input || !input->vdev || input->vdev_id != owner->vdev_id ||
		    wlan_vdev_get_id(input->vdev) != owner->vdev_id) {
			ret = -EUCLEAN;
			goto finish;
		}
		/* request_put could otherwise be the last objmgr ref, reentering
		 * ordinary FW deletion after scheduler shutdown. Require an
		 * independently owned HDD or blocked-SME ref to keep it alive.
		 */
		record = owner->record;
		hdd_pin = record && record->generation == generation &&
			record->link == owner->link &&
			record->vdev_id == owner->vdev_id &&
			(record->steps & HDD_PT_VDEV_HDD_REF_ACQUIRED) &&
			!(record->steps & HDD_PT_VDEV_HDD_REF_RELEASED);
		if (!hdd_pin) {
			status = wma_passthru_recovery_request_guard(input->vdev);
			if (QDF_IS_STATUS_ERROR(status)) {
				ret = qdf_status_to_os_return(status);
				goto finish;
			}
		}
		if (!owner->error && QDF_IS_STATUS_ERROR(result.status))
			owner->error = qdf_status_to_os_return(result.status);
		request = owner->request;
		owner->request = NULL;
		sme_passthru_request_put(request);
	}
	/* Current absence comes from this invocation's external PE purge.
	 * An old CREATE/PRESENT result must not initiate a new DELETE here.
	 */
	owner->pe_state = SME_PASSTHRU_SESSION_ABSENT;
	if (owner->endpoint) {
		status = ucfg_dp_passthru_join(owner->endpoint);
		if (QDF_IS_STATUS_ERROR(status)) {
			ret = qdf_status_to_os_return(status);
			goto finish;
		}
		ucfg_dp_passthru_put(owner->endpoint);
		owner->endpoint = NULL;
	}
	owner->recovery_collected = true;
finish:
	/* Keep the sink hold and RECOVERING phase, even after successful join.
	 * CDS must still check global DP receipts and reap WMA parameters.
	 */
	hdd_wh_finish(&claim, false);
unlock:
	mutex_unlock(&owner->control);
release:
	hdd_pt_release(registry);
	return ret;
}


int hdd_pt_status(struct hdd_pt_registry *registry, u64 generation,
		  struct hdd_pt_status *status)
{
	struct hdd_pt_owner *owner;

	if (!status)
		return -EINVAL;
	owner = hdd_pt_acquire(registry, generation);
	if (!owner)
		return -ENOENT;
	mutex_lock(&owner->control);
	status->phase = owner->phase;
	status->error = owner->error;
	status->detach_error = owner->detach_error;
	status->detach_attempted = owner->detach_attempted;
	status->recovery_prepared = owner->recovery_prepared;
	status->recovery_collected = owner->recovery_collected;
	mutex_unlock(&owner->control);
	hdd_pt_release(registry);
	return 0;
}

int hdd_pt_with_bound(struct hdd_pt_registry *registry, u64 generation,
		       int (*operation)(void *context), void *context)
{
	struct hdd_pt_owner *owner;
	struct hdd_wh_ticket ticket = {0};
	int ret;

	if (!operation)
		return -EINVAL;
	owner = hdd_pt_acquire(registry, generation);
	if (!owner)
		return -ENOENT;
	mutex_lock(&owner->control);
	if (owner->phase != HDD_PT_BOUND || atomic_read(&owner->closing))
		ret = -ESHUTDOWN;
	else
		ret = hdd_wh_enter(&owner->gate, generation, &ticket);
	mutex_unlock(&owner->control);
	if (!ret) {
		ret = operation(context);
		hdd_wh_leave(&ticket);
	}
	hdd_pt_release(registry);
	return ret;
}

/* Runtime serializes channel requests. The gate ticket pins endpoint/vdev
 * through polling and the final callback tail; no owner mutex crosses WMI.
 * May run under Wonder's mutex/RTNL. Never wait for work or take RTNL here.
 */
int hdd_pt_with_channel(struct hdd_pt_registry *registry, u64 generation,
		       u16 frequency, unsigned long deadline,
		       int (*operation)(void *context), void *context)
{
	struct hdd_pt_owner *owner;
	struct hdd_wh_ticket ticket = {0};
	QDF_STATUS status;
	int ret;

	if (!operation || !frequency)
		return -EINVAL;
	owner = hdd_pt_acquire(registry, generation);
	if (!owner)
		return -ENOENT;
	if (!mutex_trylock(&owner->control)) {
		ret = -EBUSY;
		goto release;
	}
	if (owner->phase != HDD_PT_ACTIVE || !owner->endpoint ||
	    owner->channel_busy || atomic_read(&owner->closing)) {
		ret = -ESHUTDOWN;
		goto unlock;
	}
	ret = hdd_wh_enter(&owner->gate, generation, &ticket);
	if (ret)
		goto unlock;
	owner->channel_busy = true;
	smp_store_release(&owner->active, false);
	mutex_unlock(&owner->control);

	for (;;) {
		ret = -ESHUTDOWN;
		if (atomic_read(&owner->closing))
			break;
		ret = -ETIMEDOUT;
		if (time_after_eq(jiffies, deadline))
			break;
		status = ucfg_dp_passthru_pause(owner->endpoint);
		ret = qdf_status_to_os_return(status);
		if (status != QDF_STATUS_E_BUSY)
			break;
		usleep_range(1000, 2000);
	}
	if (!ret)
		ret = operation(context);
	if (!ret && atomic_read(&owner->closing))
		ret = -ESHUTDOWN;
	if (!ret && time_after_eq(jiffies, deadline))
		ret = -ETIMEDOUT;
	if (!ret)
		ret = qdf_status_to_os_return(
			ucfg_dp_passthru_set_channel(owner->endpoint, frequency));

	/* With our gate ticket alive, close work cannot enter vdev deletion.
	 * It may briefly own control, but cannot wait for us while holding it.
	 */
	mutex_lock(&owner->control);
	if (!ret && atomic_read(&owner->closing))
		ret = -ESHUTDOWN;
	if (!ret && time_after_eq(jiffies, deadline))
		ret = -ETIMEDOUT;
	if (ret) {
		if (!owner->error)
			owner->error = ret;
		hdd_pt_close_owner(owner, "00", 6);
		hdd_pt_schedule(owner, 0);
	} else {
		owner->frequency = frequency;
		smp_store_release(&owner->active, true);
		/* A concurrent close always seals the gate, even if it races this
		 * store. No packet may enter using active alone.
		 */
		if (atomic_read(&owner->closing)) {
			smp_store_release(&owner->active, false);
			ret = -ESHUTDOWN;
		}
	}
	owner->channel_busy = false;
	hdd_wh_leave(&ticket);
unlock:
	mutex_unlock(&owner->control);
release:
	hdd_pt_release(registry);
	return ret;
}

int hdd_pt_activate(struct hdd_pt_registry *registry, u64 generation)
{
	struct hdd_pt_owner *owner = hdd_pt_acquire(registry, generation);
	struct hdd_wh_ticket ticket = {0};
	int ret = -ENOENT;

	if (!owner)
		return ret;
	mutex_lock(&owner->control);
	ret = hdd_wh_enter(&owner->gate, generation, &ticket);
	if (!ret) {
		if (owner->phase != HDD_PT_BOUND || atomic_read(&owner->closing)) {
			ret = -EBUSY;
		} else {
			smp_store_release(&owner->active, true);
			owner->phase = HDD_PT_ACTIVE;
		}
		hdd_wh_leave(&ticket);
	}
	mutex_unlock(&owner->control);
	hdd_pt_release(registry);
	return ret;
}

bool hdd_pt_data_ready(struct hdd_pt_registry *registry, u64 generation)
{
	struct hdd_pt_owner *owner = hdd_pt_acquire(registry, generation);
	struct hdd_wh_ticket ticket = {0};
	bool ready = false;

	if (owner && !hdd_wh_enter(&owner->gate, generation, &ticket)) {
		ready = smp_load_acquire(&owner->active);
		hdd_wh_leave(&ticket);
	}
	if (owner)
		hdd_pt_release(registry);
	return ready;
}

void hdd_pt_tx(struct hdd_pt_registry *registry, u64 generation,
	       struct sk_buff *head)
{
	struct hdd_pt_owner *owner = hdd_pt_acquire(registry, generation);
	struct hdd_wh_ticket ticket = {0};

	if (owner && !hdd_wh_enter(&owner->gate, generation, &ticket)) {
		if (smp_load_acquire(&owner->active))
			head = ucfg_dp_passthru_tx(owner->endpoint, head);
		hdd_wh_leave(&ticket);
	}
	hdd_pt_free_list(head);
	if (owner)
		hdd_pt_release(registry);
}

int hdd_pt_close(struct hdd_pt_registry *registry, u64 generation,
		  const char country[3], unsigned int reason)
{
	struct hdd_pt_owner *owner;

	if (!country)
		return -EINVAL;
	owner = hdd_pt_acquire(registry, generation);
	if (!owner)
		return -ENOENT;
	hdd_pt_close_owner(owner, country, reason);
	hdd_pt_schedule(owner, 0);
	hdd_pt_release(registry);
	return 0;
}

int hdd_pt_retire(struct hdd_pt_registry *registry, unsigned int reason)
{
	struct hdd_pt_owner *owner;

	spin_lock_bh(&registry->lock);
	registry->closed = true;
	owner = registry->owner;
	if (owner && !registry->reaping)
		registry->users++;
	else
		owner = NULL;
	spin_unlock_bh(&registry->lock);
	if (owner) {
		hdd_pt_close_owner(owner, "00", reason);
		hdd_pt_schedule(owner, 0);
		hdd_pt_release(registry);
	}
	return 0;
}

int hdd_pt_reap(struct hdd_pt_registry *registry)
{
	struct hdd_pt_owner *owner;

	spin_lock_bh(&registry->lock);
	owner = registry->owner;
	if (!owner) {
		spin_unlock_bh(&registry->lock);
		return 0;
	}
	if (registry->reaping || READ_ONCE(owner->phase) != HDD_PT_DRAINED) {
		spin_unlock_bh(&registry->lock);
		return -EBUSY;
	}
	registry->reaping = true;
	spin_unlock_bh(&registry->lock);
	wait_event(registry->idle, !READ_ONCE(registry->users));
	cancel_delayed_work_sync(&owner->work);
	spin_lock_bh(&registry->lock);
	registry->owner = NULL;
	registry->reaping = false;
	spin_unlock_bh(&registry->lock);
	hdd_pt_put(owner);
	return 0;
}

/* Post-CDS fences only. Unlike ordinary reap, this never waits for registry
 * users or work while the caller may hold a driver transition and RTNL.
 * Freeze already joined the worker; closed/reaping exclude new public users.
 */
int hdd_pt_recovery_reap(struct hdd_pt_registry *registry, u64 generation)
{
	struct hdd_pt_owner *owner;
	struct hdd_pt_vdev_record *record;
	struct hdd_wh_claim claim = {0};
	enum hdd_wh_state state;
	u32 required = HDD_PT_VDEV_DELETE_CALLED |
		HDD_PT_VDEV_DELETE_RETURNED | HDD_PT_VDEV_HDD_REF_RELEASED |
		HDD_PT_VDEV_SME_DELETE_CALLED | HDD_PT_VDEV_DELETE_WAIT_STARTED |
		HDD_PT_VDEV_DELETE_WAIT_COMPLETED;
	int ret = 0;

	spin_lock_bh(&registry->lock);
	if (registry->generation != generation) {
		spin_unlock_bh(&registry->lock);
		return -ESTALE;
	}
	if (!registry->closed || registry->reaping || registry->users) {
		spin_unlock_bh(&registry->lock);
		return -EBUSY;
	}
	owner = registry->owner;
	if (!owner) {
		spin_unlock_bh(&registry->lock);
		return 0;
	}
	registry->reaping = true;
	spin_unlock_bh(&registry->lock);
	if (!mutex_trylock(&owner->control)) {
		ret = -EBUSY;
		goto retain;
	}
	if (!atomic_read(&owner->recovering) || !owner->recovery_prepared ||
	    owner->vdev || owner->request || owner->endpoint ||
	    refcount_read(&owner->refs) != 1) {
		ret = -EBUSY;
		goto unlock;
	}
	if (owner->phase != HDD_PT_DRAINED) {
		if (owner->phase != HDD_PT_RECOVERING ||
		    !owner->recovery_collected ||
		    owner->pe_state != SME_PASSTHRU_SESSION_ABSENT) {
			ret = -EBUSY;
			goto unlock;
		}
		record = owner->record;
		if (!record || record->generation != generation ||
		    record->link != owner->link ||
		    record->vdev_id != owner->vdev_id) {
			ret = -EUCLEAN;
			goto unlock;
		}
		/* This narrow finish accepts the completed ordinary delete path.
		 * A timeout, vanished ID, or successful WMA parameter reap alone
		 * is not that proof. Keep ambiguous recovery objects for a later,
		 * explicitly joined objmgr completion route.
		 */
		if (!owner->detach_attempted || owner->detach_error ||
		    (record->steps & required) != required ||
		    (record->steps & HDD_PT_VDEV_DELETE_WAIT_TIMED_OUT) ||
		    record->delete_result ||
		    QDF_IS_STATUS_ERROR(record->sme_delete_status)) {
			ret = -EBUSY;
			goto unlock;
		}
	}
	spin_lock_bh(&owner->gate.lock);
	state = owner->gate.state;
	if (owner->gate.generation != generation ||
	    owner->gate.users || owner->gate.actor)
		ret = -EBUSY;
	if (!ret && owner->phase == HDD_PT_DRAINED &&
	    (state != HDD_WH_RELEASED || owner->gate.sink))
		ret = -EUCLEAN;
	spin_unlock_bh(&owner->gate.lock);
	if (ret)
		goto unlock;
	if (state == HDD_WH_CLOSING) {
		ret = hdd_wh_try_claim(&owner->gate, generation, &claim);
		if (ret)
			goto unlock;
		ret = hdd_wh_finish(&claim, true);
		if (ret)
			goto unlock;
		state = HDD_WH_DRAINED;
	}
	if (state == HDD_WH_DRAINED) {
		ret = hdd_wh_release(&owner->gate);
		if (ret)
			goto unlock;
	} else if (state != HDD_WH_RELEASED) {
		ret = -EBUSY;
		goto unlock;
	}
	owner->phase = HDD_PT_DRAINED;
	mutex_unlock(&owner->control);
	spin_lock_bh(&registry->lock);
	registry->owner = NULL;
	registry->reaping = false;
	spin_unlock_bh(&registry->lock);
	hdd_pt_put(owner);
	return 0;
unlock:
	mutex_unlock(&owner->control);
retain:
	spin_lock_bh(&registry->lock);
	registry->reaping = false;
	spin_unlock_bh(&registry->lock);
	return ret;
}
