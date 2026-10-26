/* SPDX-License-Identifier: ISC */
#ifndef _DP_PASSTHRU_RECEIPTS_H_
#define _DP_PASSTHRU_RECEIPTS_H_

#include <qdf_lock.h>
#include <qdf_status.h>

/* One ledger per DP soc, initialized before its first vdev. A successful begin
 * owes exactly one end, after real vdev destruction AND both legacy callbacks.
 * No vdev reference is added: this also covers attach rollback and unbound DP.
 * close serializes with begin, so status SUCCESS cannot race a new admission.
 * Soc/module/hardware lifetime fences remain the caller's responsibility.
 */
struct dp_passthru_receipts {
	qdf_spinlock_t lock;
	uint32_t pending;
	bool closed;
	bool poisoned;
};

static inline void
dp_passthru_receipts_init(struct dp_passthru_receipts *r)
{
	qdf_spinlock_create(&r->lock);
	r->pending = 0;
	r->closed = false;
	r->poisoned = false;
}

static inline QDF_STATUS
dp_passthru_receipts_begin(struct dp_passthru_receipts *r)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	qdf_spin_lock_bh(&r->lock);
	if (r->poisoned) {
		status = QDF_STATUS_E_FAILURE;
	} else if (r->closed) {
		status = QDF_STATUS_E_BUSY;
	} else if (r->pending == (uint32_t)~0U) {
		r->poisoned = true;
		status = QDF_STATUS_E_FAILURE;
	} else {
		r->pending++;
	}
	qdf_spin_unlock_bh(&r->lock);
	return status;
}

static inline void
dp_passthru_receipts_close(struct dp_passthru_receipts *r)
{
	qdf_spin_lock_bh(&r->lock);
	r->closed = true;
	qdf_spin_unlock_bh(&r->lock);
}

static inline QDF_STATUS
dp_passthru_receipts_end(struct dp_passthru_receipts *r)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	qdf_spin_lock_bh(&r->lock);
	if (!r->pending) {
		r->poisoned = true;
		status = QDF_STATUS_E_FAILURE;
	} else {
		r->pending--;
		if (r->poisoned)
			status = QDF_STATUS_E_FAILURE;
	}
	qdf_spin_unlock_bh(&r->lock);
	return status;
}

static inline QDF_STATUS
dp_passthru_receipts_status(struct dp_passthru_receipts *r, uint32_t *pending)
{
	QDF_STATUS status;

	if (!pending)
		return QDF_STATUS_E_INVAL;
	qdf_spin_lock_bh(&r->lock);
	*pending = r->pending;
	if (r->poisoned)
		status = QDF_STATUS_E_FAILURE;
	else if (!r->closed)
		status = QDF_STATUS_E_AGAIN;
	else
		status = r->pending ? QDF_STATUS_E_BUSY : QDF_STATUS_SUCCESS;
	qdf_spin_unlock_bh(&r->lock);
	return status;
}

/* Normal DP soc teardown already requires all producers/vdevs gone.
 * This diagnostic is not an alternative to the status-bearing outer fence.
 */
static inline void
dp_passthru_receipts_deinit(struct dp_passthru_receipts *r)
{
	QDF_ASSERT(!r->pending);
	qdf_spinlock_destroy(&r->lock);
}
#endif
