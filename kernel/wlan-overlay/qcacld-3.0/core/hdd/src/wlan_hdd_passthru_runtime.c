/* SPDX-License-Identifier: ISC */
#ifdef HOUJI_PT_RUNTIME_HOST_TEST
#include "runtime_host.h"
#else
#include <linux/etherdevice.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include "wlan_hdd_main.h"
#include "sme_api.h"
#include "osif_psoc_sync.h"
#include "cds_api.h"
#include "wlan_scan_ucfg_api.h"
#include <net/net_namespace.h>
#include "wlan_hdd_passthru_experiment.h"
#include "wlan_hdd_driver_ops.h"
#include "wlan_policy_mgr_ucfg.h"
#include "wlan_hdd_passthru_runtime.h"
#include "wlan_hdd_passthru_iface.h"
#include "wlan_hdd_passthru_control.h"
#endif

#define HDD_PT_POLL_MS 20
#define HDD_PT_INIT_MS 2000
#define HDD_PT_CHANNEL_MS 2000
#define HDD_PT_INIT_ATTEMPTS 4
#define HDD_PT_START_SEEN BIT(0)
#define HDD_PT_UP_SENT BIT(1)

enum hdd_pt_runtime_phase {
	HDD_PT_RT_IDLE,
	HDD_PT_RT_CREATING,
	HDD_PT_RT_STARTING,
	HDD_PT_RT_READY,
	HDD_PT_RT_CLOSING,
	HDD_PT_RT_DRAINED,
	HDD_PT_RT_BLOCKED,
	HDD_PT_RT_RECOVERING,
};

struct hdd_pt_runtime {
	struct hdd_context *hdd;
	struct cnss_wonder_provider *provider;
	struct hdd_pt_registry registry;
	struct hdd_pt_iface iface;
	struct wondertap_init_params params;
	struct delayed_work work;
	struct mutex control;
	spinlock_t event_lock;
	spinlock_t work_lock;
	atomic_t recovering;
	atomic_t closing;
	enum hdd_pt_runtime_phase phase;
	unsigned long deadline;
	unsigned int attempts;
	unsigned int events;
	int event_error;
	int error;
	u32 event_cookie;
	u8 event_vdev_id;
	bool event_armed;
	bool callback_registered;
	bool handle_published;
	bool recovery_prepared;
};

/* A CNSS regular lease pins ops/runtime publication. SME callbacks instead
 * use this spinlock; clearing their registration is a synchronous SME barrier.
 * Neither callback nor deinit waits for control/RTNL/worker completion.
 */
static DEFINE_MUTEX(hdd_pt_lifecycle_lock);
static DEFINE_SPINLOCK(hdd_pt_public_lock);
static struct hdd_pt_runtime *hdd_pt_current;
static u64 hdd_pt_generation;
/* Only one published runtime; its control mutex serializes allocation.
 * Never reset or reuse across retire. High bit identifies PT wire responses.
 */
static u32 hdd_pt_cookie_sequence;
/* Single-use recovery epoch: a recovered instance never republishes ops.
 * A fresh module instance is required to reopen this experimental provider.
 */
static bool hdd_pt_registration_sealed;
static bool hdd_pt_module_pinned;


/* Explicit first-START-only firmware compatibility. No runtime toggling. */
static bool pt_zero_start;
module_param(pt_zero_start, bool, 0400);
MODULE_PARM_DESC(pt_zero_start,
	"Accept zero wire cookie only for first exclusive START; disable retune");

bool hdd_pt_zero_start_compat(void);
bool hdd_pt_zero_start_compat(void)
{
	return READ_ONCE(pt_zero_start) && hdd_pt_exclusive_mode();
}

/* Wire-only experiment: retain the module-monotonic tagged local cookie.
 * Zero-cookie compatibility takes precedence if both parameters are supplied.
 */
static bool pt_low_cookie;
module_param(pt_low_cookie, bool, 0400);
MODULE_PARM_DESC(pt_low_cookie, "Use nonzero low-31-bit wire START cookies in exclusive mode");

bool hdd_pt_low_cookie_mode(void);
bool hdd_pt_low_cookie_mode(void)
{
	return READ_ONCE(pt_low_cookie) && hdd_pt_exclusive_mode() &&
		!READ_ONCE(pt_zero_start);
}

/* Limited native-style serialization experiment, at most START + 4 retunes.
 * Firmware does not echo request IDs. A duplicate successful RESTART response
 * arriving during the next RESTART cannot be distinguished; not production.
 */
static bool pt_zero_serial;
static atomic_t hdd_pt_zero_serial_closed = ATOMIC_INIT(0);
module_param(pt_zero_serial, bool, 0400);
MODULE_PARM_DESC(pt_zero_serial, "Bounded zero-cookie serial experiment; max four retunes");

bool hdd_pt_zero_serial_mode(void);
bool hdd_pt_zero_serial_mode(void)
{
	return READ_ONCE(pt_zero_serial) && hdd_pt_exclusive_mode() &&
		!READ_ONCE(pt_zero_start) && !READ_ONCE(pt_low_cookie);
}

static void hdd_pt_runtime_queue(struct hdd_pt_runtime *rt, unsigned int ms)
{
	spin_lock_bh(&rt->work_lock);
	if (!atomic_read(&rt->recovering))
		mod_delayed_work(system_unbound_wq, &rt->work, msecs_to_jiffies(ms));
	spin_unlock_bh(&rt->work_lock);
}

static void hdd_pt_runtime_close(struct hdd_pt_runtime *rt)
{
	if (hdd_pt_zero_serial_mode())
		atomic_set(&hdd_pt_zero_serial_closed, 1);
	atomic_set(&rt->closing, 1);
	hdd_pt_retire(&rt->registry, 5);
	hdd_pt_runtime_queue(rt, 0);
}

static void hdd_pt_runtime_fail(struct hdd_pt_runtime *rt, int error)
{
	hdd_info("PT runtime failed phase %u error %d cookie %08x",
		 rt->phase, error, rt->event_cookie);
	hdd_pt_control_abort(&rt->iface);
	if (!rt->error)
		rt->error = error;
	hdd_pt_runtime_close(rt);
}

/* SME holds its global lock. Never invoke SME/control APIs here. */
static void hdd_pt_runtime_event(uint8_t vdev_id,
				 enum sir_passthru_event event,
				 QDF_STATUS result, uint32_t cookie)
{
	struct hdd_pt_runtime *rt;
	unsigned int bit;

	if (event == SIR_PASSTHRU_START_RESPONSE)
		bit = HDD_PT_START_SEEN;
	else if (event == SIR_PASSTHRU_UP_SUBMITTED)
		bit = HDD_PT_UP_SENT;
	else
		return;
	spin_lock_bh(&hdd_pt_public_lock);
	rt = hdd_pt_current;
	if (rt) {
		spin_lock_bh(&rt->event_lock);
		if (rt->event_armed && rt->event_cookie == cookie &&
		    cookie && rt->event_vdev_id == vdev_id &&
		    !atomic_read(&rt->closing)) {
			hdd_info("PT event vdev %u cookie %08x bit %u result %d",
				 vdev_id, cookie, bit, result);
			rt->events |= bit;
			if (QDF_IS_STATUS_ERROR(result) && !rt->event_error)
				rt->event_error = qdf_status_to_os_return(result);
			hdd_pt_runtime_queue(rt, 0);
		}
		spin_unlock_bh(&rt->event_lock);
	}
	spin_unlock_bh(&hdd_pt_public_lock);
}

static int hdd_pt_runtime_submit(void *context)
{
	struct hdd_pt_runtime *rt = context;

	if (atomic_read(&rt->closing))
		return -ESHUTDOWN;
	return hdd_pt_control_submit_start(&rt->iface, &rt->params,
					   rt->event_cookie);
}

static int hdd_pt_runtime_finish(void *context)
{
	struct hdd_pt_runtime *rt = context;
	int ret;

	if (atomic_read(&rt->closing))
		return -ESHUTDOWN;
	ret = hdd_pt_control_finish_start(&rt->iface, &rt->params);
	if (!ret && !atomic_read(&rt->closing))
		ret = hdd_pt_activate(&rt->registry, rt->registry.generation);
	return ret;
}

static void hdd_pt_runtime_work(struct work_struct *work)
{
	struct hdd_pt_runtime *rt =
		container_of(to_delayed_work(work), struct hdd_pt_runtime, work);
	struct hdd_pt_status status;
	unsigned int events;
	int error, ret;
	bool again = false;

	mutex_lock(&rt->control);
	if (atomic_read(&rt->recovering))
		goto out;
	if (atomic_read(&rt->closing))
		goto closing;
	if (rt->phase == HDD_PT_RT_IDLE)
		goto out;
	/* Even a READY result with no consumer handle must not leak forever
	 * after Wonder's final failed retry.
	 */
	if (!rt->handle_published && time_after_eq(jiffies, rt->deadline)) {
		hdd_pt_runtime_fail(rt, -ETIMEDOUT);
		goto closing;
	}
	if (rt->phase == HDD_PT_RT_CREATING) {
		ret = hdd_pt_status(&rt->registry, rt->registry.generation, &status);
		if (ret || status.error ||
		    status.phase == HDD_PT_BLOCKED ||
		    status.phase == HDD_PT_CLOSING ||
		    status.phase == HDD_PT_DRAINED) {
			hdd_pt_runtime_fail(rt, ret ? ret :
					    (status.error ? status.error : -EIO));
			goto closing;
		}
		if (status.phase != HDD_PT_BOUND) {
			again = true;
			goto out;
		}
		/* A finite runtime deadline is already armed. Consume a unique
		 * cookie before PE/WMI submission; wrap permanently refuses.
		 */
		if (hdd_pt_cookie_sequence == 0x7fffffffU) {
			hdd_pt_runtime_fail(rt, -EOVERFLOW);
			goto closing;
		}
		rt->event_cookie = 0x80000000U | ++hdd_pt_cookie_sequence;
		spin_lock_bh(&rt->event_lock);
		rt->event_vdev_id = rt->iface.adapter->deflink->vdev_id;
		rt->events = 0;
		rt->event_error = 0;
		rt->event_armed = true;
		spin_unlock_bh(&rt->event_lock);
		rt->phase = HDD_PT_RT_STARTING;
		ret = hdd_pt_with_bound(&rt->registry, rt->registry.generation,
					hdd_pt_runtime_submit, rt);
		if (ret) {
			hdd_pt_runtime_fail(rt, ret);
			goto closing;
		}
	}
	if (rt->phase == HDD_PT_RT_STARTING) {
		spin_lock_bh(&rt->event_lock);
		events = rt->events;
		error = rt->event_error;
		spin_unlock_bh(&rt->event_lock);
		if (error) {
			hdd_pt_runtime_fail(rt, error);
			goto closing;
		}
		if (events != (HDD_PT_START_SEEN | HDD_PT_UP_SENT)) {
			again = true;
			goto out;
		}
		/* Success events may precede a failed RX/TX return. Wait for
		 * both enclosing call tails before applying final configuration.
		 */
		ret = hdd_pt_control_check_start(&rt->iface, rt->event_cookie);
		if (ret == -EINPROGRESS) {
			again = true;
			goto out;
		}
		if (ret) {
			hdd_pt_runtime_fail(rt, ret);
			goto closing;
		}
		/* UP_SENT proves command submission, not a firmware UP ACK.
		 * START is the actual start response. Both are necessary.
		 */
		ret = hdd_pt_with_bound(&rt->registry, rt->registry.generation,
					hdd_pt_runtime_finish, rt);
		if (ret || atomic_read(&rt->closing)) {
			hdd_pt_runtime_fail(rt, ret ? ret : -ESHUTDOWN);
			goto closing;
		}
		spin_lock_bh(&rt->event_lock);
		rt->event_armed = false;
		spin_unlock_bh(&rt->event_lock);
		rt->phase = HDD_PT_RT_READY;
	}
	again = !rt->handle_published;
	goto out;
closing:
	hdd_pt_control_abort(&rt->iface);
	spin_lock_bh(&rt->event_lock);
	rt->event_armed = false;
	spin_unlock_bh(&rt->event_lock);
	rt->phase = HDD_PT_RT_CLOSING;
	ret = hdd_pt_iface_cleanup(&rt->iface);
	if (ret == -EBUSY) {
		/* Owner BLOCKED is terminal until an explicit SSR recovery;
		 * do not spend CPU polling an unrecoverable outcome.
		 */
		if (rt->iface.owner_started &&
		    !hdd_pt_status(&rt->registry, rt->registry.generation, &status) &&
		    status.phase == HDD_PT_BLOCKED) {
			rt->phase = HDD_PT_RT_BLOCKED;
			if (!rt->error)
				rt->error = status.error ? status.error : -EUCLEAN;
		} else {
			again = true;
		}
	} else if (ret) {
		rt->phase = HDD_PT_RT_BLOCKED;
		if (!rt->error)
			rt->error = ret;
	} else {
		rt->phase = HDD_PT_RT_DRAINED;
	}
out:
	if (again)
		hdd_pt_runtime_queue(rt, HDD_PT_POLL_MS);
	mutex_unlock(&rt->control);
}

/* Ignore C padding/reserved bytes when matching retries. */
static bool hdd_pt_same_init(const struct wondertap_init_params *a,
			    const struct wondertap_init_params *b)
{
	return a->channel.freq == b->channel.freq &&
		a->channel.bandwidth == b->channel.bandwidth &&
		a->tx_rate.preamble == b->tx_rate.preamble &&
		a->tx_rate.bw == b->tx_rate.bw &&
		a->tx_rate.gi == b->tx_rate.gi &&
		a->tx_rate.nss == b->tx_rate.nss &&
		a->tx_rate.mcs == b->tx_rate.mcs &&
		ether_addr_equal(a->mac_addr, b->mac_addr) &&
		ether_addr_equal(a->bssid, b->bssid) &&
		!memcmp(a->country_code, b->country_code, 3) &&
		a->mgmt_retry_limit == b->mgmt_retry_limit &&
		a->data_retry_limit == b->data_retry_limit &&
		a->amsdu_enable == b->amsdu_enable &&
		a->ampdu_enable == b->ampdu_enable;
}

static int hdd_pt_wonder_init(void **handle,
			     const struct wondertap_init_params *params)
{
	struct hdd_pt_runtime *rt = READ_ONCE(hdd_pt_current);
	int ret;

	ASSERT_RTNL();
	if (!handle || *handle || !params)
		return -EINVAL;
	if (!rt || atomic_read(&hdd_pt_zero_serial_closed))
		return -ESHUTDOWN;
	/* Cleanup may need RTNL. Never wait for its mutex while holding RTNL. */
	if (!mutex_trylock(&rt->control))
		return -EAGAIN;
	if (atomic_read(&rt->closing)) {
		ret = rt->error ? rt->error : -ESHUTDOWN;
		goto out;
	}
	if (rt->handle_published) {
		ret = -EBUSY;
		goto out;
	}
	if (rt->phase == HDD_PT_RT_IDLE) {
		ret = hdd_pt_control_validate(rt->hdd, params);
		if (ret)
			goto out;
		rt->params = *params;
		rt->attempts = 0;
		rt->deadline = jiffies + msecs_to_jiffies(HDD_PT_INIT_MS);
		rt->phase = HDD_PT_RT_CREATING;
		ret = hdd_pt_iface_create(&rt->iface, rt->hdd, &rt->registry,
					  rt->registry.generation,
					  params->mac_addr, params->bssid,
					  params->channel.freq);
		if (ret) {
			hdd_pt_runtime_fail(rt, ret);
			goto out;
		}
		hdd_pt_runtime_queue(rt, 0);
	} else if (!hdd_pt_same_init(&rt->params, params)) {
		ret = -EBUSY;
		goto out;
	}
	rt->attempts++;
	if (time_after_eq(jiffies, rt->deadline)) {
		ret = -ETIMEDOUT;
		hdd_pt_runtime_fail(rt, ret);
		goto out;
	}
	if (rt->phase == HDD_PT_RT_READY) {
		if (!hdd_pt_data_ready(&rt->registry, rt->registry.generation)) {
			ret = -ESHUTDOWN;
			hdd_pt_runtime_fail(rt, ret);
			goto out;
		}
		ret = dev_open(rt->iface.adapter->dev, NULL);
		if (ret || atomic_read(&rt->closing)) {
			ret = ret ? ret : -ESHUTDOWN;
			hdd_pt_runtime_fail(rt, ret);
			goto out;
		}
		rt->handle_published = true;
		*handle = rt;
		ret = 0;
	} else if (rt->attempts >= HDD_PT_INIT_ATTEMPTS) {
		ret = -ETIMEDOUT;
		hdd_pt_runtime_fail(rt, ret);
	} else {
		ret = -EAGAIN;
	}
out:
	mutex_unlock(&rt->control);
	return ret;
}

static void hdd_pt_wonder_deinit(void *handle,
				const struct wondertap_deinit_params *params)
{
	struct hdd_pt_runtime *rt = READ_ONCE(hdd_pt_current);

	/* No allocation, RTNL acquisition, lease wait or control mutex here.
	 * Consumer has already detached its RX handler and stopped TX.
	 */
	if (rt && handle == rt)
		hdd_pt_runtime_close(rt);
}

struct hdd_pt_channel_call {
	struct hdd_pt_runtime *rt;
	struct wondertap_init_params target;
	unsigned long deadline;
};

/* Runs synchronously under runtime control + DSC + the owner's gate ticket.
 * Events never take control and do not require the runtime worker to run.
 */
static int hdd_pt_runtime_channel(void *context)
{
	struct hdd_pt_channel_call *call = context;
	struct hdd_pt_runtime *rt = call->rt;
	unsigned int events;
	int error, ret;

	if (atomic_read(&rt->closing))
		return -ESHUTDOWN;
	if (time_after_eq(jiffies, call->deadline))
		return -ETIMEDOUT;
	if (hdd_pt_cookie_sequence == 0x7fffffffU)
		return -EOVERFLOW;
	spin_lock_bh(&rt->event_lock);
	rt->event_cookie = 0x80000000U | ++hdd_pt_cookie_sequence;
	rt->event_vdev_id = rt->iface.adapter->deflink->vdev_id;
	rt->events = 0;
	rt->event_error = 0;
	rt->event_armed = true;
	spin_unlock_bh(&rt->event_lock);

	ret = hdd_pt_control_submit_channel(&rt->iface, &call->target,
					   rt->event_cookie);
	if (ret)
		return ret;
	for (;;) {
		if (atomic_read(&rt->closing))
			return -ESHUTDOWN;
		if (time_after_eq(jiffies, call->deadline))
			return -ETIMEDOUT;
		spin_lock_bh(&rt->event_lock);
		events = rt->events;
		error = rt->event_error;
		spin_unlock_bh(&rt->event_lock);
		if (error)
			return error;
		if (events == (HDD_PT_START_SEEN | HDD_PT_UP_SENT)) {
			ret = hdd_pt_control_check_start(&rt->iface,
							rt->event_cookie);
			if (ret != -EINPROGRESS)
				return ret;
		}
		usleep_range(1000, 2000);
	}
}

static int hdd_pt_wonder_set_freq(void *handle,
				const struct wondertap_set_freq_params *params)
{
	struct hdd_pt_runtime *rt = READ_ONCE(hdd_pt_current);
	struct osif_psoc_sync *sync;
	struct hdd_pt_channel_call call;
	int ret;

	/* No retune in the zero-cookie experiment, including same-frequency
	 * calls. Refusal must not submit WMI or close the working session.
	 */
	if (hdd_pt_zero_start_compat())
		return -EOPNOTSUPP;

	if (!rt || handle != rt || !params || atomic_read(&rt->closing))
		return -ESHUTDOWN;
	/* Cleanup may hold control while waiting for RTNL held by our caller.
	 * This entry must never wait for control, a worker, or a DSC transition.
	 */
	if (!mutex_trylock(&rt->control))
		return -EBUSY;
	if (atomic_read(&rt->closing) || !rt->handle_published ||
	    rt->phase != HDD_PT_RT_READY) {
		ret = -ESHUTDOWN;
		goto out;
	}
	if (hdd_pt_zero_serial_mode() && hdd_pt_cookie_sequence >= 5) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	ret = osif_psoc_sync_op_start(rt->hdd->parent_dev, &sync);
	if (ret)
		goto out;
	call.rt = rt;
	call.target = rt->params;
	call.target.channel = *params;
	ret = hdd_pt_control_validate(rt->hdd, &call.target);
	if (ret)
		goto stop_sync;
	/* Immutable experiment gates exclude ordinary network admission. */
	if (ucfg_policy_mgr_get_connection_count(rt->hdd->psoc)) {
		ret = -EBUSY;
		goto stop_sync;
	}
	if (!hdd_pt_data_ready(&rt->registry, rt->registry.generation)) {
		ret = -ESHUTDOWN;
		goto stop_sync;
	}
	if (params->freq == rt->params.channel.freq &&
	    params->bandwidth == rt->params.channel.bandwidth) {
		ret = 0;
		goto stop_sync;
	}
	/* A NEW bounded deadline for every change, independent of init/handle
	 * publication. The callback returns only after actual matching results.
	 */
	call.deadline = jiffies + msecs_to_jiffies(HDD_PT_CHANNEL_MS);
	ret = hdd_pt_with_channel(&rt->registry, rt->registry.generation,
				 params->freq, call.deadline,
				 hdd_pt_runtime_channel, &call);
	spin_lock_bh(&rt->event_lock);
	rt->event_armed = false;
	spin_unlock_bh(&rt->event_lock);
	if (!ret && atomic_read(&rt->closing))
		ret = -ESHUTDOWN;
	if (ret) {
		/* This can be ambiguous after WMI submission. Seal and close,
		 * never pretend to restore the previous frequency.
		 */
		hdd_pt_runtime_fail(rt, ret);
	} else {
		rt->params.channel = *params;
		rt->iface.frequency = params->freq;
	}
stop_sync:
	osif_psoc_sync_op_stop(sync);
out:
	mutex_unlock(&rt->control);
	return ret;
}

static int hdd_pt_wonder_capabilities(void *handle,
				     struct wondertap_capability *features)
{
	struct hdd_pt_runtime *rt = READ_ONCE(hdd_pt_current);

	if (!features)
		return -EINVAL;
	if (!rt || atomic_read(&rt->closing))
		return -ESHUTDOWN;
	memset(features, 0, sizeof(*features));
	/* No coexistence, dynamic rate, filtering or aggregation claims.
	 * Init controls are checked explicitly by control_validate().
	 */
	return 0;
}

static const struct wondertap_ops hdd_pt_wonder_ops = {
	.init = hdd_pt_wonder_init,
	.deinit = hdd_pt_wonder_deinit,
	.set_freq = hdd_pt_wonder_set_freq,
	.get_capabilities = hdd_pt_wonder_capabilities,
};

static const struct wondertap_priv hdd_pt_wonder_priv = {
	.ver = WONDER_VERSION_1_4_1,
	.wonder_ops = &hdd_pt_wonder_ops,
};

/* The bridge adopts the activation pin and may release it only after a
 * successful external quiesce callback. Failed activation stays pinned until
 * a clean explicit release or reboot. Activation remains LIVE-only.
 */
static int hdd_pt_experiment_quiesce(void *context);

static int hdd_pt_runtime_pin_module(struct hdd_context *hdd)
{
	struct module *owner = THIS_MODULE;
	int ret;

	if (!owner || hdd_pt_module_pinned)
		return 0;
	if (READ_ONCE(owner->state) != MODULE_STATE_LIVE)
		return -EBUSY;
	if (!try_module_get(owner))
		return -ENODEV;
	hdd_pt_module_pinned = true;
	ret = cnss_wonder_pin_adopt(owner, hdd_pt_experiment_quiesce, hdd);
	if (ret)
		hdd_pt_registration_sealed = true; /* retain pin, require reboot */
	return ret;
}

int hdd_pt_runtime_register(struct hdd_context *hdd)
{
	struct hdd_pt_runtime *rt;
	QDF_STATUS status;
	int ret;

	if (!hdd || !hdd->mac_handle)
		return -EINVAL;
	if (!hdd_pt_exclusive_mode())
		return -EPERM;
	mutex_lock(&hdd_pt_lifecycle_lock);
	if (hdd_pt_registration_sealed || atomic_read(&hdd_pt_zero_serial_closed)) {
		ret = -ESHUTDOWN;
		goto unlock;
	}
	if (hdd_pt_current) {
		ret = -EBUSY;
		goto unlock;
	}
	if (hdd_pt_generation == U64_MAX) {
		ret = -EOVERFLOW;
		goto unlock;
	}
	rt = kzalloc(sizeof(*rt), GFP_KERNEL);
	if (!rt) {
		ret = -ENOMEM;
		goto unlock;
	}
	ret = hdd_pt_runtime_pin_module(hdd);
	if (ret) {
		kfree(rt);
		goto unlock; /* Never published; no callbacks or provider to undo. */
	}
	rt->hdd = hdd;
	mutex_init(&rt->control);
	spin_lock_init(&rt->event_lock);
	spin_lock_init(&rt->work_lock);
	atomic_set(&rt->recovering, 0);
	atomic_set(&rt->closing, 0);
	INIT_DELAYED_WORK(&rt->work, hdd_pt_runtime_work);
	hdd_pt_registry_init(&rt->registry, ++hdd_pt_generation);
	spin_lock_bh(&hdd_pt_public_lock);
	WRITE_ONCE(hdd_pt_current, rt);
	spin_unlock_bh(&hdd_pt_public_lock);
	status = sme_set_passthrough_mode_cb(hdd->mac_handle, hdd_pt_runtime_event);
	if (QDF_IS_STATUS_ERROR(status)) {
		ret = qdf_status_to_os_return(status);
		goto unpublish;
	}
	rt->callback_registered = true;
	ret = cnss_wonder_provider_publish(&hdd_pt_wonder_priv, THIS_MODULE,
					   &rt->provider);
	if (!ret)
		goto unlock;
	/* No consumer call survives failed publish (CNSS rolls it back).
	 * Keep storage if SME cannot join its callback; caller must retire.
	 */
	atomic_set(&rt->closing, 1);
	status = sme_set_passthrough_mode_cb(hdd->mac_handle, NULL);
	if (QDF_IS_STATUS_ERROR(status)) {
		rt->error = ret;
		goto unlock;
	}
	rt->callback_registered = false;
unpublish:
	spin_lock_bh(&hdd_pt_public_lock);
	WRITE_ONCE(hdd_pt_current, NULL);
	spin_unlock_bh(&hdd_pt_public_lock);
	kfree(rt);
unlock:
	mutex_unlock(&hdd_pt_lifecycle_lock);
	return ret;
}

int hdd_pt_runtime_retire(struct hdd_context *hdd,
			 enum cnss_wonder_retire_reason reason)
{
	struct hdd_pt_runtime *rt;
	QDF_STATUS status;
	int ret = 0;

	mutex_lock(&hdd_pt_lifecycle_lock);
	rt = hdd_pt_current;
	if (!rt)
		goto unlock;
	if (rt->hdd != hdd) {
		ret = -EINVAL;
		goto unlock;
	}
	if (atomic_read(&rt->recovering)) {
		ret = -EUCLEAN; /* A normal retire cannot bypass recovery fences. */
		goto unlock;
	}
	/* Reject/cancel work BEFORE waiting for CNSS leases. Holding an RTNL
	 * or DSC transition here would deadlock consumer quiesce/cleanup.
	 */
	hdd_pt_runtime_close(rt);
	if (rt->provider) {
		cnss_wonder_provider_retire(rt->provider, reason);
		cnss_wonder_provider_put(rt->provider);
		rt->provider = NULL;
	}
	if (rt->callback_registered) {
		status = sme_set_passthrough_mode_cb(hdd->mac_handle, NULL);
		if (QDF_IS_STATUS_ERROR(status)) {
			ret = qdf_status_to_os_return(status);
			goto unlock;
		}
		rt->callback_registered = false;
	}
	cancel_delayed_work_sync(&rt->work);
	mutex_lock(&rt->control);
	hdd_pt_control_abort(&rt->iface);
	ret = hdd_pt_iface_cleanup(&rt->iface);
	mutex_unlock(&rt->control);
	if (ret)
		goto unlock; /* Explicit retention; caller may not tear down parents. */
	spin_lock_bh(&hdd_pt_public_lock);
	WRITE_ONCE(hdd_pt_current, NULL);
	spin_unlock_bh(&hdd_pt_public_lock);
	kfree(rt);
unlock:
	mutex_unlock(&hdd_pt_lifecycle_lock);
	return ret;
}


int hdd_pt_runtime_recovery_freeze(struct hdd_context *hdd,
                                 enum cnss_wonder_retire_reason reason)
{
	struct hdd_pt_runtime *rt;
	QDF_STATUS status;
	int ret = 0, owner_ret = 0;

	mutex_lock(&hdd_pt_lifecycle_lock);
	rt = hdd_pt_current;
	if (rt && rt->hdd != hdd) {
		ret = -EINVAL;
		goto unlock;
	}
	/* Seal even an absent runtime before dropping this lock. This excludes
	 * a late register between freeze and the caller's DSC acquisition.
	 */
	hdd_pt_registration_sealed = true;
	if (!rt)
		goto unlock;
	rt->recovery_prepared = false;
	/* Disable enqueue BEFORE joining the worker. Init already in progress is
	 * joined by control below; later init observes closing and rejects.
	 */
	spin_lock_bh(&rt->work_lock);
	atomic_set(&rt->recovering, 1);
	atomic_set(&rt->closing, 1);
	spin_unlock_bh(&rt->work_lock);
	cancel_delayed_work_sync(&rt->work);
	mutex_lock(&rt->control);
	hdd_pt_control_abort(&rt->iface);
	rt->phase = HDD_PT_RT_RECOVERING;
	spin_lock_bh(&rt->event_lock);
	rt->event_armed = false;
	spin_unlock_bh(&rt->event_lock);
	if (rt->iface.owner_started && !rt->iface.owner_reaped)
		owner_ret = hdd_pt_recovery_freeze(&rt->registry,
						 rt->registry.generation);
	else if (rt->iface.adapter && !rt->iface.owner_reaped)
		owner_ret = -EUCLEAN;
	mutex_unlock(&rt->control);
	/* No control mutex across consumer quiesce, RTNL or lease waits. */
	if (rt->provider) {
		cnss_wonder_provider_retire(rt->provider, reason);
		cnss_wonder_provider_put(rt->provider);
		rt->provider = NULL;
	}
	if (rt->callback_registered) {
		status = sme_set_passthrough_mode_cb(hdd->mac_handle, NULL);
		if (QDF_IS_STATUS_ERROR(status)) {
			ret = qdf_status_to_os_return(status);
			goto unlock;
		}
		rt->callback_registered = false;
	}
	/* Consumer shutdown may have released the last data/control ticket. */
	if (owner_ret == -EBUSY)
		owner_ret = hdd_pt_recovery_freeze(&rt->registry,
						 rt->registry.generation);
	ret = owner_ret;
	rt->recovery_prepared = !ret;
unlock:
	mutex_unlock(&hdd_pt_lifecycle_lock);
	return ret;
}

/* lifecycle_lock is held, but this check never waits on consumer/RTNL work. */
static int hdd_pt_runtime_recovery_ready(struct hdd_pt_runtime *rt,
				       struct hdd_context *hdd)
{
	if (rt->hdd != hdd)
		return -EINVAL;
	if (!rt->recovery_prepared || !atomic_read(&rt->recovering) ||
	    !atomic_read(&rt->closing) || rt->phase != HDD_PT_RT_RECOVERING ||
	    rt->provider || rt->callback_registered)
		return -EBUSY;
	return 0;
}

int hdd_pt_runtime_recovery_check(struct hdd_context *hdd)
{
	int ret = 0;

	/* Stop may already hold a DSC transition. A competing retire can be
	 * waiting on that transition via a consumer lease: never block here.
	 */
	if (!mutex_trylock(&hdd_pt_lifecycle_lock))
		return -EBUSY;
	if (hdd_pt_current)
		ret = hdd_pt_runtime_recovery_ready(hdd_pt_current, hdd);
	mutex_unlock(&hdd_pt_lifecycle_lock);
	return ret;
}

int hdd_pt_runtime_recovery_collect(struct hdd_context *hdd)
{
	struct hdd_pt_runtime *rt;
	int ret = 0;

	if (!mutex_trylock(&hdd_pt_lifecycle_lock))
		return -EBUSY;
	rt = hdd_pt_current;
	if (!rt)
		goto out;
	ret = hdd_pt_runtime_recovery_ready(rt, hdd);
	if (ret)
		goto out;
	if (!mutex_trylock(&rt->control)) {
		ret = -EBUSY;
		goto out;
	}
	if (rt->iface.owner_started && !rt->iface.owner_reaped)
		ret = hdd_pt_recovery_collect(&rt->registry,
					     rt->registry.generation);
	else if (rt->iface.adapter && !rt->iface.owner_reaped)
		ret = -EUCLEAN;
	/* No retire/reap/netdev cleanup here; the external teardown still owns
	 * this runtime until its remaining parent-lifetime fences complete.
	 */
	mutex_unlock(&rt->control);
out:
	mutex_unlock(&hdd_pt_lifecycle_lock);
	return ret;
}

int hdd_pt_runtime_teardown_check(struct hdd_context *hdd)
{
	int ret = 0;

	if (!mutex_trylock(&hdd_pt_lifecycle_lock))
		return -EBUSY;
	if (hdd_pt_current)
		ret = hdd_pt_current->hdd == hdd ? -EBUSY : -EINVAL;
	mutex_unlock(&hdd_pt_lifecycle_lock);
	/* Collection does not release the sink/iface/provider storage. Until
	 * the outer finish route has retired it, parents must stay alive.
	 */
	return ret;
}

/* Invoked only after the current CDS purge, DP receipt and WMA reap succeed.
 * Registration remains excluded for the whole outer teardown. Do not join
 * CNSS leases here or acquire RTNL with a blocking lock under a transition.
 */
int hdd_pt_runtime_recovery_finish(struct hdd_context *hdd)
{
	struct hdd_pt_runtime *rt;
	int ret = 0;

	if (!mutex_trylock(&hdd_pt_lifecycle_lock))
		return -EBUSY;
	rt = hdd_pt_current;
	if (!rt)
		goto out;
	ret = hdd_pt_runtime_recovery_ready(rt, hdd);
	if (ret)
		goto out;
	if (!mutex_trylock(&rt->control)) {
		ret = -EBUSY;
		goto out;
	}
	if (!rtnl_trylock()) {
		ret = -EBUSY;
		goto unlock;
	}
	ret = hdd_pt_iface_recovery_cleanup(&rt->iface);
	/* Unregister's deferred network/free tails run at this unlock while
	 * the registry/runtime and DP/psoc parents are still alive.
	 */
	rtnl_unlock();
unlock:
	mutex_unlock(&rt->control);
	if (ret)
		goto out;
	spin_lock_bh(&hdd_pt_public_lock);
	WRITE_ONCE(hdd_pt_current, NULL);
	spin_unlock_bh(&hdd_pt_public_lock);
	kfree(rt);
out:
	mutex_unlock(&hdd_pt_lifecycle_lock);
	return ret;
}

#ifndef HOUJI_PT_RUNTIME_HOST_TEST
/* Called only by the broker while its permanent and temporary pins are held.
 * The parent DSC operation excludes teardown, but no transition or RTNL is
 * held across CNSS consumer quiesce. Any failure retains both publication
 * storage (where necessary) and the broker-owned permanent module pin.
 */
static int hdd_pt_experiment_quiesce(void *context)
{
	struct hdd_context *hdd = context;
	struct osif_psoc_sync *sync;
	struct hdd_pt_runtime *rt;
	int ret;

	if (!hdd_pt_exclusive_mode() ||
	    hdd != cds_get_context(QDF_MODULE_ID_HDD))
		return -ENODEV;
	ret = wlan_hdd_validate_context(hdd);
	if (ret)
		return ret;
	ret = osif_psoc_sync_op_start(hdd->parent_dev, &sync);
	if (ret)
		return ret;
	ret = hdd_pt_driver_teardown_status();
	if (ret)
		goto stop_sync;
	mutex_lock(&hdd_pt_lifecycle_lock);
	/* Seal before inspecting: this instance is never reactivated. */
	hdd_pt_registration_sealed = true;
	rt = hdd_pt_current;
	if (rt) {
		mutex_lock(&rt->control);
		if (rt->hdd != hdd || atomic_read(&rt->recovering) ||
		    rt->error || (rt->phase != HDD_PT_RT_DRAINED &&
				  rt->phase != HDD_PT_RT_IDLE)) {
			ret = -EBUSY;
		} else {
			/* Close admission before releasing control for consumer joins.
			 * handle_published is historical and is not a release receipt.
			 */
			atomic_set(&rt->closing, 1);
		}
		mutex_unlock(&rt->control);
	}
	mutex_unlock(&hdd_pt_lifecycle_lock);
	if (ret)
		goto stop_sync;
	ret = hdd_pt_runtime_retire(hdd, CNSS_WONDER_HDD_UNLOAD);
	if (!ret)
		ret = hdd_pt_runtime_teardown_check(hdd);
stop_sync:
	osif_psoc_sync_op_stop(sync);
	if (!ret) {
		mutex_lock(&hdd_pt_lifecycle_lock);
		hdd_pt_module_pinned = false;
		mutex_unlock(&hdd_pt_lifecycle_lock);
		/* The broker, not this callback, releases the real module refs. */
	}
	return ret;
}

/*
 * Explicit, single-boot bring-up only. pt_exclusive is immutable after load:
 * ordinary netdev open / connect / scan / AP / ROC / mgmt TX stay blocked
 * before publication and after failure. No runtime coexistence claim.
 * The experiment runner must also stop Android Wi-Fi HAL/cnd and scanning.
 */
static bool pt_exclusive;
module_param(pt_exclusive, bool, 0400);
MODULE_PARM_DESC(pt_exclusive, "Reserve this module instance for PT bring-up");

bool hdd_pt_exclusive_mode(void)
{
	return pt_exclusive;
}

/* Opt-in administrative STA interface only: no ordinary vdev or RF use.
 * The immutable default keeps all previous experiments unchanged.
 */
static bool pt_framework_netdev;
module_param(pt_framework_netdev, bool, 0400);
MODULE_PARM_DESC(pt_framework_netdev, "Allow zero-resource STA administrative UP in exclusive PT mode");

bool hdd_pt_framework_netdev_mode(void)
{
	return READ_ONCE(pt_framework_netdev) && hdd_pt_exclusive_mode();
}

bool hdd_pt_framework_netdev_ready(struct hdd_context *hdd)
{
	struct hdd_pt_runtime *rt;
	bool ready;

	if (!hdd_pt_framework_netdev_mode())
		return false;
	/* Do not wait on lifecycle/control while ndo_open holds RTNL/DSC.
	 * This lock prevents rt retirement while its admission state is read.
	 */
	spin_lock_bh(&hdd_pt_public_lock);
	rt = hdd_pt_current;
	ready = rt && READ_ONCE(rt->provider) && rt->hdd == hdd &&
		READ_ONCE(hdd_pt_module_pinned) &&
		!READ_ONCE(hdd_pt_registration_sealed) && !READ_ONCE(rt->error) &&
		!atomic_read(&rt->recovering) && !atomic_read(&rt->closing);
	spin_unlock_bh(&hdd_pt_public_lock);
	return ready;
}

static int hdd_pt_experiment_activate(const char *value,
				    const struct kernel_param *kp)
{
	struct hdd_context *hdd;
	struct osif_psoc_sync *sync;
	struct net_device *dev;
	bool enable;
	int ret;

	ret = kstrtobool(value, &enable);
	if (ret)
		return ret;
	if (!enable || !pt_exclusive)
		return -EPERM;
	if ((int)pt_zero_start + (int)pt_low_cookie + (int)pt_zero_serial > 1)
		return -EINVAL;
	if (atomic_read(&hdd_pt_zero_serial_closed))
		return -ESHUTDOWN;
	/* insmod pt_activate=1 is intentionally rejected. */
	if (THIS_MODULE && READ_ONCE(THIS_MODULE->state) != MODULE_STATE_LIVE)
		return -EBUSY;
	hdd = cds_get_context(QDF_MODULE_ID_HDD);
	ret = wlan_hdd_validate_context(hdd);
	if (ret)
		return ret;
	ret = osif_psoc_sync_op_start(hdd->parent_dev, &sync);
	if (ret)
		return ret;
	if (hdd->driver_status != DRIVER_MODULES_ENABLED ||
	    !hdd->mac_handle || !hdd->pdev || !hdd->psoc) {
		ret = -ENODEV;
		goto stop_sync;
	}
	if (ucfg_policy_mgr_get_connection_count(hdd->psoc) ||
	    ucfg_scan_get_pdev_status(hdd->pdev) != SCAN_NOT_IN_PROGRESS) {
		ret = -EBUSY;
		goto stop_sync;
	}
	if (!rtnl_trylock()) {
		ret = -EBUSY;
		goto stop_sync;
	}
	ret = 0;
	for_each_netdev(&init_net, dev) {
		if (dev->ieee80211_ptr &&
		    dev->ieee80211_ptr->wiphy == hdd->wiphy &&
		    netif_running(dev)) {
			ret = -EBUSY;
			break;
		}
	}
	rtnl_unlock();
	if (!ret)
		/* No RTNL or DSC transition held; the DSC op pins parents. */
		ret = hdd_pt_runtime_register(hdd);
	hdd_info("PT experiment activation result %d", ret);
stop_sync:
	osif_psoc_sync_op_stop(sync);
	return ret;
}

static int hdd_pt_experiment_status(char *buf, const struct kernel_param *kp)
{
	struct hdd_pt_runtime *rt;
	int ret;

	if (!mutex_trylock(&hdd_pt_lifecycle_lock))
		return -EBUSY;
	rt = hdd_pt_current;
	if (!rt) {
		ret = scnprintf(buf, PAGE_SIZE,
				"exclusive=%u published=0 pinned=%u sealed=%u\n",
				pt_exclusive, hdd_pt_module_pinned,
				hdd_pt_registration_sealed);
		goto out;
	}
	if (!mutex_trylock(&rt->control)) {
		ret = -EBUSY;
		goto out;
	}
	spin_lock_bh(&rt->event_lock);
	ret = scnprintf(buf, PAGE_SIZE,
			"exclusive=%u published=%u pinned=%u phase=%u error=%d "
			"closing=%d handle=%u freq=%u cookie=%08x events=%u "
			"event_error=%d\n",
			pt_exclusive, !!rt->provider, hdd_pt_module_pinned,
			rt->phase, rt->error, atomic_read(&rt->closing),
			rt->handle_published, rt->params.channel.freq,
			rt->event_cookie, rt->events, rt->event_error);
	spin_unlock_bh(&rt->event_lock);
	mutex_unlock(&rt->control);
out:
	mutex_unlock(&hdd_pt_lifecycle_lock);
	return ret;
}

static const struct kernel_param_ops hdd_pt_activate_ops = {
	.set = hdd_pt_experiment_activate,
	.get = hdd_pt_experiment_status,
};
module_param_cb(pt_activate, &hdd_pt_activate_ops, NULL, 0600);
MODULE_PARM_DESC(pt_activate, "Write 1 after LIVE to publish PT; broker release then reload to reset");
#endif
