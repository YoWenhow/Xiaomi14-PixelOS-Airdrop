/* SPDX-License-Identifier: ISC */
#ifndef _WLAN_PASSTHRU_TXN_H_
#define _WLAN_PASSTHRU_TXN_H_

/* All helpers require the owning vdev's object lock. Zero-init once per vdev.
 * The HDD runtime supplies a module-wide monotonic sequence in this namespace;
 * it owns the deadline and seals on timeout/close. No generic START timer is
 * armed for these transactions: queued timer work carries no request identity.
 * High-bit tokens also identify stale PT responses after vdev-ID reuse.
 */
#define WLAN_PT_COOKIE_BIT 0x80000000U
enum wlan_pt_txn_phase {
	WLAN_PT_TXN_IDLE,
	WLAN_PT_TXN_PREPARED,
	WLAN_PT_TXN_SENT,
	WLAN_PT_TXN_STARTED,
	WLAN_PT_TXN_UP,
	WLAN_PT_TXN_DONE,
	WLAN_PT_TXN_SEALED,
};
struct wlan_pt_txn {
	uint32_t cookie;
	uint32_t frequency;
	enum wlan_pt_txn_phase phase;
	bool restart;
	bool tx_busy;
	bool rx_busy;
};

static inline void wlan_pt_txn_seal(struct wlan_pt_txn *t)
{
	t->phase = WLAN_PT_TXN_SEALED;
}

static inline QDF_STATUS
wlan_pt_txn_prepare(struct wlan_pt_txn *t, uint32_t cookie, uint32_t frequency)
{
	if (!(cookie & WLAN_PT_COOKIE_BIT) || cookie == WLAN_PT_COOKIE_BIT ||
	    cookie <= t->cookie || !frequency)
		return QDF_STATUS_E_INVAL;
	if ((t->phase != WLAN_PT_TXN_IDLE && t->phase != WLAN_PT_TXN_DONE) ||
	    t->tx_busy || t->rx_busy)
		return QDF_STATUS_E_BUSY;
	t->cookie = cookie;
	t->frequency = frequency;
	t->phase = WLAN_PT_TXN_PREPARED;
	return QDF_STATUS_SUCCESS;
}

static inline bool
wlan_pt_txn_prepared(struct wlan_pt_txn *t, uint32_t cookie, uint32_t frequency)
{
	return t->phase == WLAN_PT_TXN_PREPARED && cookie &&
		t->cookie == cookie && t->frequency == frequency;
}

static inline QDF_STATUS
wlan_pt_txn_send_begin(struct wlan_pt_txn *t, uint32_t cookie,
		       uint32_t frequency, bool restart)
{
	if (!wlan_pt_txn_prepared(t, cookie, frequency) ||
	    t->tx_busy || t->rx_busy)
		return QDF_STATUS_E_INVAL;
	t->restart = restart;
	t->phase = WLAN_PT_TXN_SENT;
	t->tx_busy = true;
	return QDF_STATUS_SUCCESS;
}

static inline QDF_STATUS
wlan_pt_txn_send_end(struct wlan_pt_txn *t, uint32_t cookie, QDF_STATUS status)
{
	if (t->cookie != cookie || !t->tx_busy)
		return QDF_STATUS_E_INVAL;
	t->tx_busy = false;
	if (QDF_IS_STATUS_ERROR(status))
		wlan_pt_txn_seal(t);
	if (t->phase == WLAN_PT_TXN_SEALED)
		return QDF_STATUS_E_FAILURE;
	return status;
}

/* Claim before any timer access, WMA/PE mutation or upper-layer callback.
 * The current response may arrive before send_end; only a NEW transaction
 * is excluded by tx_busy. Duplicate, stale and wrong-type packets do nothing.
 */
static inline QDF_STATUS
wlan_pt_txn_response(struct wlan_pt_txn *t, uint32_t cookie, bool restart,
		     bool success)
{
	if (!cookie || t->cookie != cookie || t->restart != restart ||
	    t->phase != WLAN_PT_TXN_SENT || t->rx_busy)
		return QDF_STATUS_E_INVAL;
	t->rx_busy = true;
	t->phase = success ? WLAN_PT_TXN_STARTED : WLAN_PT_TXN_SEALED;
	return QDF_STATUS_SUCCESS;
}

static inline void
wlan_pt_txn_response_end(struct wlan_pt_txn *t, uint32_t cookie,
			 QDF_STATUS status)
{
	if (t->cookie != cookie || !t->rx_busy)
		return;
	if (QDF_IS_STATUS_ERROR(status))
		wlan_pt_txn_seal(t);
	t->rx_busy = false;
}

static inline QDF_STATUS
wlan_pt_txn_up_begin(struct wlan_pt_txn *t, uint32_t *cookie)
{
	if (t->phase != WLAN_PT_TXN_STARTED || !cookie)
		return QDF_STATUS_E_INVAL;
	*cookie = t->cookie;
	t->phase = WLAN_PT_TXN_UP;
	return QDF_STATUS_SUCCESS;
}

static inline QDF_STATUS
wlan_pt_txn_up_end(struct wlan_pt_txn *t, uint32_t cookie, QDF_STATUS status)
{
	if (t->cookie != cookie || t->phase != WLAN_PT_TXN_UP)
		return QDF_STATUS_E_FAILURE;
	t->phase = QDF_IS_STATUS_SUCCESS(status) ?
		WLAN_PT_TXN_DONE : WLAN_PT_TXN_SEALED;
	return status;
}

/* Queued START/UP success alone is insufficient: their enclosing RX or TX
 * call can still fail after posting them. Commit only after BOTH tails exit.
 */
static inline QDF_STATUS
wlan_pt_txn_completion(struct wlan_pt_txn *t, uint32_t cookie)
{
	if (!cookie || cookie != t->cookie || t->phase == WLAN_PT_TXN_SEALED)
		return QDF_STATUS_E_FAILURE;
	if (t->tx_busy || t->rx_busy || t->phase != WLAN_PT_TXN_DONE)
		return QDF_STATUS_E_PENDING;
	return QDF_STATUS_SUCCESS;
}
#endif
