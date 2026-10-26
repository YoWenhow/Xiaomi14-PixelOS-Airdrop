/* SPDX-License-Identifier: ISC */
#ifndef _WLAN_DP_PASSTHRU_H_
#define _WLAN_DP_PASSTHRU_H_

#include <qdf_types.h>
#include <qdf_status.h>
#include <qdf_nbuf.h>

#ifdef DRIVER_PASSTHRU_OWNER
struct cdp_soc_t;
struct wlan_dp_passthru_endpoint;

/*
 * get/put retain the actual consumer context. They do not substitute for
 * the caller's WLAN runtime ownership: soc/pdev and their code must remain
 * alive until join returns and the existing device teardown barriers run.
 * A psoc object-manager reference alone does not keep CDP alive.
 * rx consumes one detached nbuf, including on a delivery failure. Async
 * consumers must retain their own context references beyond this call.
 */
struct wlan_dp_passthru_owner_ops {
	bool (*get)(void *context);
	void (*put)(void *context);
	void (*rx)(void *context, qdf_nbuf_t nbuf);
};

/*
 * Process-context constructor. The returned endpoint owns one caller ref
 * and one installation ref. It borrows no legacy dp_link/dp_intf fields.
 * Failed bind publishes nothing and balances the consumer reference.
 * The caller must arrange stop BEFORE ordinary MLME/FW vdev deletion.
 */
QDF_STATUS
ucfg_dp_passthru_create(struct cdp_soc_t *soc, uint8_t vdev_id,
		       uint16_t frequency,
		       const struct wlan_dp_passthru_owner_ops *owner_ops,
		       void *owner_context,
		       struct wlan_dp_passthru_endpoint **out);

/* get requires an already-live pointer protected by an owner/registry ref. */
bool ucfg_dp_passthru_get(struct wlan_dp_passthru_endpoint *endpoint);
void ucfg_dp_passthru_put(struct wlan_dp_passthru_endpoint *endpoint);

/* Every public operation requires a caller reference for its entire call. */
qdf_nbuf_t ucfg_dp_passthru_tx(struct wlan_dp_passthru_endpoint *endpoint,
			     qdf_nbuf_t nbuf);

/*
 * Sticky, non-waiting admission close. BUSY retains all ownership and must
 * be retried by the outer owner, outside RTNL/transition/component locks.
 * SUCCESS covers synchronous calls only, never hardware/DMA/IRQ drain.
 */
QDF_STATUS ucfg_dp_passthru_stop(struct wlan_dp_passthru_endpoint *endpoint);

/*
 * Reversible CPU-data admission pause, preserving the exact endpoint/vdev.
 * BUSY after pause closes new admission; retry until existing CPU users leave.
 * No hardware/DMA fence or FW command. Teardown remains sticky and wins races.
 * Caller serializes the entire pause -> FW operation -> commit transaction.
 */
QDF_STATUS ucfg_dp_passthru_pause(struct wlan_dp_passthru_endpoint *endpoint);
/*
 * Requires successful pause AND a correlated successful FW channel result.
 * Commits radiotap frequency then reopens admission; failure remains paused.
 * No timeout/rollback success, no request-ID/DSC/regulatory checks inside DP.
 * Pending device RX may still predate the channel switch: this interface
 * alone cannot prove packet-level RF frequency attribution.
 */
QDF_STATUS
ucfg_dp_passthru_set_channel(struct wlan_dp_passthru_endpoint *endpoint,
			    uint16_t frequency);

/*
 * After stop succeeds, the owner initiates the actual vdev deletion using
 * its existing control path. join returns BUSY until CDP's final retained
 * reference can be released. SUCCESS means the real delete notification
 * returned synchronously, then the consumer and installation refs were
 * released. It is idempotent while the caller keeps the endpoint alive.
 * This is NOT a full-module execution/IRQ/DMA fence. No timeout forces free.
 */
QDF_STATUS ucfg_dp_passthru_join(struct wlan_dp_passthru_endpoint *endpoint);
#endif /* DRIVER_PASSTHRU_OWNER */
#endif /* _WLAN_DP_PASSTHRU_H_ */
