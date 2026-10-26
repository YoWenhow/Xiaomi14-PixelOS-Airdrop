/* SPDX-License-Identifier: ISC */
#ifndef SME_PASSTHRU_REQUEST_H
#define SME_PASSTHRU_REQUEST_H
#ifdef DRIVER_PASSTHRU_OWNER
#include "qdf_types.h"
#include "qdf_status.h"
#include "sir_types.h"

struct wlan_objmgr_vdev;
struct wlan_objmgr_psoc;
struct mac_context;
struct scheduler_msg;
struct sme_passthru_request;

enum sme_passthru_request_action {
	SME_PASSTHRU_REQUEST_CREATE,
	SME_PASSTHRU_REQUEST_DELETE,
};

enum sme_passthru_session_state {
	SME_PASSTHRU_SESSION_UNKNOWN,
	SME_PASSTHRU_SESSION_ABSENT,
	SME_PASSTHRU_SESSION_PRESENT,
};

/* A snapshot of local PE work only, never firmware STOP/DOWN completion. */
struct sme_passthru_request_result {
	QDF_STATUS status;
	enum sme_passthru_session_state session_state;
	bool changed;
	bool cancel_requested;
};

/*
 * Caller holds a valid vdev reference while submitting, and a request reference
 * throughout every request API. Submission takes a separate exact-vdev ref.
 * The owner must keep MAC, scheduler and code alive, close admission before
 * driver shutdown, and collect every request. A vdev ref is not a CDP soc pin.
 * Successful post transfers one queue ref, independently of the returned ref.
 * No callback registration, PE-thread wait or implicit HDD publication occurs.
 */
QDF_STATUS sme_passthru_request_submit(mac_handle_t mac_handle,
		struct wlan_objmgr_vdev *vdev,
		enum sme_passthru_request_action action, const uint8_t *bssid,
		struct sme_passthru_request **out);
bool sme_passthru_request_get(struct sme_passthru_request *request);
void sme_passthru_request_put(struct sme_passthru_request *request);
/* SUCCESS prevents a not-yet-started execution, but does not remove its queue
 * entry. BUSY means execution has started and may commit; collect the result.
 */
QDF_STATUS sme_passthru_request_cancel(struct sme_passthru_request *request);
QDF_STATUS sme_passthru_request_query(struct sme_passthru_request *request,
		struct sme_passthru_request_result *result);
/* Sleeping wait: never under RTNL, SME/DSC/transition locks or on PE worker.
 * Timeout/interrupt neither cancels nor releases ownership. A query returning
 * SUCCESS only means result is available; inspect result.status separately.
 * DONE can be observed before the executor/disposer wake and queue-put tail
 * returns. This is not a runtime drain/join or permission to unload code/MAC.
 * Runtime shutdown still needs the external worker/owner fence. The queue
 * drain API below covers only owned request tails, not all runtime users.
 */
QDF_STATUS sme_passthru_request_wait(struct sme_passthru_request *request,
		uint32_t timeout_ms, struct sme_passthru_request_result *result);

/* Close is one-way until a new MAC/SME open, serialized with submission.
 * drain_status requires closed admission: AGAIN=open, BUSY=queue refs remain,
 * SUCCESS=no request finish/wake/queue-put tail still borrows MAC. This does
 * not count the surrounding PE dispatcher/worker tail. outstanding is written
 * only for BUSY/SUCCESS. No waiting occurs. External callers must still fence
 * submitters, all legacy producers, scheduler/RX workers, and caller-held
 * request/vdev refs before destroying MAC/CDP/objmgr/code. This is not a
 * complete runtime pin, module join, firmware completion, or SSR rearm.
 */
QDF_STATUS sme_passthru_requests_close(mac_handle_t mac_handle);
QDF_STATUS sme_passthru_requests_drain_status(mac_handle_t mac_handle,
		uint32_t *outstanding);

struct sme_passthru_purge_result {
	uint32_t requests;
	uint32_t sessions;
	uint32_t outstanding;
};
/* Internal teardown-only APIs. Caller serializes global shutdown and has
 * already closed every producer, stopped scheduler processing and joined RX.
 * MAC/PE/vdev and DP pdev must still be alive. No SME/RTNL/spin lock is held
 * across purge. Deletes local PASS PE state only; never sends MLME DOWN or
 * waits for firmware. No proof of DP endpoint detach/join or DMA quiescence.
 */
uint32_t lim_passthru_purge_deferred(struct mac_context *mac);
QDF_STATUS lim_passthru_force_purge(struct mac_context *mac,
		struct sme_passthru_purge_result *result);

/* Internal SME/LIM queue contract: exactly one queue ref travels through the
 * scheduler and optional LIM deferred queue. Copying the message transfers
 * that ref; it must not duplicate execution or disposal. Queue serialization
 * is provided by the scheduler/LIM worker, not by this record's result lock.
 */
struct sme_passthru_request_input {
	struct wlan_objmgr_vdev *vdev;
	struct wlan_objmgr_psoc *psoc;
	enum sme_passthru_request_action action;
	uint8_t vdev_id;
	uint8_t bssid[6];
};
const struct sme_passthru_request_input *
sme_passthru_request_input(struct sme_passthru_request *request);
bool sme_passthru_request_begin(struct sme_passthru_request *request);
void sme_passthru_request_finish(struct sme_passthru_request *request,
		QDF_STATUS status, enum sme_passthru_session_state state,
		bool changed);
bool sme_passthru_request_dispose_msg(struct scheduler_msg *msg,
		QDF_STATUS status);
void lim_process_passthru_request(struct mac_context *mac,
		struct scheduler_msg *msg);
#endif
#endif
