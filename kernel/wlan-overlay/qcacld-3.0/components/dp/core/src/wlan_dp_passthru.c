/* SPDX-License-Identifier: ISC */
#include <qdf_atomic.h>
#include <qdf_lock.h>
#include <qdf_mem.h>
#include <cdp_txrx_cmn.h>
#include <wlan_dp_passthru.h>

#ifdef DRIVER_PASSTHRU_OWNER
enum wlan_dp_passthru_state {
	WLAN_DP_PASSTHRU_PREPARING,
	WLAN_DP_PASSTHRU_ACTIVE,
	WLAN_DP_PASSTHRU_PAUSED,
	WLAN_DP_PASSTHRU_CLOSING,
	WLAN_DP_PASSTHRU_JOINED,
};

struct wlan_dp_passthru_endpoint {
	qdf_atomic_t refs;
	qdf_spinlock_t lock;
	enum wlan_dp_passthru_state state;
	unsigned int users;
	bool control_busy;
	bool pause_done;
	bool close_done;
	bool notified;
	bool owner_held;
	struct cdp_soc_t *soc;
	struct cdp_passthru_handle *handle;
	struct wlan_dp_passthru_owner_ops owner_ops;
	void *owner_context;
};

static bool
wlan_dp_passthru_enter(struct wlan_dp_passthru_endpoint *endpoint)
{
	bool admitted = false;

	qdf_spin_lock_bh(&endpoint->lock);
	if (endpoint->state == WLAN_DP_PASSTHRU_ACTIVE) {
		endpoint->users++;
		admitted = true;
	}
	qdf_spin_unlock_bh(&endpoint->lock);
	return admitted;
}

static void
wlan_dp_passthru_exit(struct wlan_dp_passthru_endpoint *endpoint)
{
	qdf_spin_lock_bh(&endpoint->lock);
	QDF_ASSERT(endpoint->users);
	endpoint->users--;
	qdf_spin_unlock_bh(&endpoint->lock);
}

/* Both raw and cached RX callers require unambiguous list consumption. */
static QDF_STATUS wlan_dp_passthru_rx(void *context, qdf_nbuf_t head)
{
	struct wlan_dp_passthru_endpoint *endpoint = context;
	qdf_nbuf_t next;
	bool admitted = endpoint && wlan_dp_passthru_enter(endpoint);

	while (head) {
		next = qdf_nbuf_next(head);
		qdf_nbuf_set_next(head, NULL);
		if (admitted)
			endpoint->owner_ops.rx(endpoint->owner_context, head);
		else
			qdf_nbuf_free(head);
		head = next;
	}
	if (admitted)
		wlan_dp_passthru_exit(endpoint);
	return QDF_STATUS_SUCCESS;
}

/* No parent lookup, owner release, completion signal or freeing here. */
static void wlan_dp_passthru_notify(void *context)
{
	struct wlan_dp_passthru_endpoint *endpoint = context;

	qdf_spin_lock_bh(&endpoint->lock);
	endpoint->notified = true;
	if (endpoint->state != WLAN_DP_PASSTHRU_JOINED)
		endpoint->state = WLAN_DP_PASSTHRU_CLOSING;
	qdf_spin_unlock_bh(&endpoint->lock);
}

bool ucfg_dp_passthru_get(struct wlan_dp_passthru_endpoint *endpoint)
{
	return endpoint && qdf_atomic_inc_not_zero(&endpoint->refs);
}

void ucfg_dp_passthru_put(struct wlan_dp_passthru_endpoint *endpoint)
{
	if (!endpoint || !qdf_atomic_dec_and_test(&endpoint->refs))
		return;

	QDF_ASSERT(endpoint->state == WLAN_DP_PASSTHRU_JOINED);
	QDF_ASSERT(!endpoint->owner_held && !endpoint->handle);
	QDF_ASSERT(!endpoint->users && !endpoint->control_busy);
	qdf_spinlock_destroy(&endpoint->lock);
	qdf_mem_free(endpoint);
}

QDF_STATUS
ucfg_dp_passthru_create(struct cdp_soc_t *soc, uint8_t vdev_id,
		       uint16_t frequency,
		       const struct wlan_dp_passthru_owner_ops *owner_ops,
		       void *owner_context,
		       struct wlan_dp_passthru_endpoint **out)
{
	struct wlan_dp_passthru_endpoint *endpoint;
	struct ol_txrx_ops ops = {0};
	QDF_STATUS status;

	if (!out || *out || !soc || !owner_ops || !owner_ops->get ||
	    !owner_ops->put || !owner_ops->rx)
		return QDF_STATUS_E_INVAL;

	endpoint = qdf_mem_malloc(sizeof(*endpoint));
	if (!endpoint)
		return QDF_STATUS_E_NOMEM;
	qdf_mem_zero(endpoint, sizeof(*endpoint));
	qdf_atomic_init(&endpoint->refs);
	qdf_atomic_set(&endpoint->refs, 2); /* creator + installation */
	qdf_spinlock_create(&endpoint->lock);
	endpoint->state = WLAN_DP_PASSTHRU_PREPARING;
	endpoint->soc = soc;
	endpoint->owner_ops = *owner_ops;
	endpoint->owner_context = owner_context;
	if (!endpoint->owner_ops.get(owner_context)) {
		status = QDF_STATUS_E_FAILURE;
		goto free_endpoint;
	}
	endpoint->owner_held = true;
	ops.rx.rx = wlan_dp_passthru_rx;
	ops.vdev_del_notify = wlan_dp_passthru_notify;
	status = cdp_passthru_managed_bind(soc, vdev_id,
					 (ol_osif_vdev_handle)endpoint, &ops,
					 frequency, &endpoint->handle);
	if (QDF_IS_STATUS_ERROR(status)) {
		/* Managed bind failure guarantees no installed callback. */
		QDF_ASSERT(!endpoint->handle);
		endpoint->owner_ops.put(owner_context);
		goto free_endpoint;
	}

	qdf_spin_lock_bh(&endpoint->lock);
	if (endpoint->state == WLAN_DP_PASSTHRU_PREPARING &&
	    !endpoint->notified)
		endpoint->state = WLAN_DP_PASSTHRU_ACTIVE;
	qdf_spin_unlock_bh(&endpoint->lock);
	*out = endpoint;
	return QDF_STATUS_SUCCESS;

free_endpoint:
	qdf_spinlock_destroy(&endpoint->lock);
	qdf_mem_free(endpoint);
	return status;
}

qdf_nbuf_t ucfg_dp_passthru_tx(struct wlan_dp_passthru_endpoint *endpoint,
			     qdf_nbuf_t nbuf)
{
	qdf_nbuf_t remaining;

	if (!endpoint || !nbuf || !wlan_dp_passthru_enter(endpoint))
		return nbuf;
	remaining = cdp_passthru_managed_tx(endpoint->soc, endpoint->handle,
					   nbuf);
	wlan_dp_passthru_exit(endpoint);
	return remaining;
}

/* Caller holds its endpoint reference. Nonwaiting admission close; retry BUSY
 * outside RTNL/driver transitions. No new packet passes after PAUSED is set.
 */
QDF_STATUS ucfg_dp_passthru_pause(struct wlan_dp_passthru_endpoint *endpoint)
{
	QDF_STATUS status;

	if (!endpoint)
		return QDF_STATUS_E_INVAL;
	qdf_spin_lock_bh(&endpoint->lock);
	if (endpoint->state != WLAN_DP_PASSTHRU_ACTIVE &&
	    endpoint->state != WLAN_DP_PASSTHRU_PAUSED) {
		qdf_spin_unlock_bh(&endpoint->lock);
		return QDF_STATUS_E_FAILURE;
	}
	/* Do not let a concurrent pause invalidate an in-progress commit. */
	if (endpoint->control_busy) {
		qdf_spin_unlock_bh(&endpoint->lock);
		return QDF_STATUS_E_BUSY;
	}
	endpoint->state = WLAN_DP_PASSTHRU_PAUSED;
	if (endpoint->users) {
		qdf_spin_unlock_bh(&endpoint->lock);
		return QDF_STATUS_E_BUSY;
	}
	if (endpoint->pause_done) {
		qdf_spin_unlock_bh(&endpoint->lock);
		return QDF_STATUS_SUCCESS;
	}
	endpoint->control_busy = true;
	qdf_spin_unlock_bh(&endpoint->lock);

	status = cdp_passthru_managed_pause(endpoint->soc, endpoint->handle);
	qdf_spin_lock_bh(&endpoint->lock);
	if (endpoint->state != WLAN_DP_PASSTHRU_PAUSED)
		status = QDF_STATUS_E_FAILURE; /* A stop/notify won the race. */
	else if (QDF_IS_STATUS_SUCCESS(status))
		endpoint->pause_done = true;
	endpoint->control_busy = false;
	qdf_spin_unlock_bh(&endpoint->lock);
	return status;
}

/* Only after a matched successful FW response, never after timeout or enqueue
 * acceptance. Commit failure leaves admission closed. The outer controller
 * serializes requests and owns all policy/DSC/FW-result checks.
 */
QDF_STATUS
ucfg_dp_passthru_set_channel(struct wlan_dp_passthru_endpoint *endpoint,
			    uint16_t frequency)
{
	QDF_STATUS status;

	if (!endpoint || frequency < 2300 || frequency > 7125)
		return QDF_STATUS_E_INVAL;
	qdf_spin_lock_bh(&endpoint->lock);
	if (endpoint->control_busy ||
	    endpoint->state != WLAN_DP_PASSTHRU_PAUSED ||
	    !endpoint->pause_done || endpoint->users) {
		qdf_spin_unlock_bh(&endpoint->lock);
		return QDF_STATUS_E_BUSY;
	}
	endpoint->control_busy = true;
	qdf_spin_unlock_bh(&endpoint->lock);

	status = cdp_passthru_managed_set_channel(endpoint->soc,
						 endpoint->handle, frequency);
	qdf_spin_lock_bh(&endpoint->lock);
	if (endpoint->state != WLAN_DP_PASSTHRU_PAUSED) {
		status = QDF_STATUS_E_FAILURE; /* Never resurrect a closed endpoint. */
	} else if (QDF_IS_STATUS_SUCCESS(status)) {
		endpoint->pause_done = false;
		endpoint->state = WLAN_DP_PASSTHRU_ACTIVE;
	}
	endpoint->control_busy = false;
	qdf_spin_unlock_bh(&endpoint->lock);
	return status;
}

QDF_STATUS ucfg_dp_passthru_stop(struct wlan_dp_passthru_endpoint *endpoint)
{
	QDF_STATUS status;

	if (!endpoint)
		return QDF_STATUS_E_INVAL;
	qdf_spin_lock_bh(&endpoint->lock);
	if (endpoint->state != WLAN_DP_PASSTHRU_JOINED)
		endpoint->state = WLAN_DP_PASSTHRU_CLOSING;
	if (endpoint->control_busy) {
		qdf_spin_unlock_bh(&endpoint->lock);
		return QDF_STATUS_E_BUSY;
	}
	if (endpoint->state == WLAN_DP_PASSTHRU_JOINED) {
		qdf_spin_unlock_bh(&endpoint->lock);
		return QDF_STATUS_SUCCESS;
	}
	if (endpoint->users) {
		qdf_spin_unlock_bh(&endpoint->lock);
		return QDF_STATUS_E_BUSY;
	}
	endpoint->control_busy = true;
	qdf_spin_unlock_bh(&endpoint->lock);

	status = cdp_passthru_managed_close(endpoint->soc, endpoint->handle);
	qdf_spin_lock_bh(&endpoint->lock);
	if (QDF_IS_STATUS_SUCCESS(status))
		endpoint->close_done = true;
	endpoint->control_busy = false;
	qdf_spin_unlock_bh(&endpoint->lock);
	return status;
}

QDF_STATUS ucfg_dp_passthru_join(struct wlan_dp_passthru_endpoint *endpoint)
{
	QDF_STATUS status;
	bool release_installation = false;

	if (!endpoint)
		return QDF_STATUS_E_INVAL;
	qdf_spin_lock_bh(&endpoint->lock);
	if (endpoint->control_busy) {
		qdf_spin_unlock_bh(&endpoint->lock);
		return QDF_STATUS_E_BUSY;
	}
	if (endpoint->state == WLAN_DP_PASSTHRU_JOINED) {
		qdf_spin_unlock_bh(&endpoint->lock);
		return QDF_STATUS_SUCCESS;
	}
	if (!endpoint->close_done || endpoint->users) {
		qdf_spin_unlock_bh(&endpoint->lock);
		return QDF_STATUS_E_BUSY;
	}
	endpoint->control_busy = true;
	qdf_spin_unlock_bh(&endpoint->lock);

	/* This returns only after the real one-argument notify has returned. */
	status = cdp_passthru_managed_join(endpoint->soc, &endpoint->handle);
	qdf_spin_lock_bh(&endpoint->lock);
	if (QDF_IS_STATUS_SUCCESS(status)) {
		if (!endpoint->notified || endpoint->handle) {
			/* Preserve ownership on a violated backend contract. */
			status = QDF_STATUS_E_FAILURE;
		} else {
			endpoint->state = WLAN_DP_PASSTHRU_JOINED;
			endpoint->owner_held = false;
			release_installation = true;
		}
	}
	qdf_spin_unlock_bh(&endpoint->lock);

	if (release_installation)
		endpoint->owner_ops.put(endpoint->owner_context);
	qdf_spin_lock_bh(&endpoint->lock);
	endpoint->control_busy = false;
	qdf_spin_unlock_bh(&endpoint->lock);
	if (release_installation)
		ucfg_dp_passthru_put(endpoint);
	return status;
}
#endif /* DRIVER_PASSTHRU_OWNER */
