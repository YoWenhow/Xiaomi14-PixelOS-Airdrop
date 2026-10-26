/* SPDX-License-Identifier: ISC */
#ifndef _WLAN_HDD_PASSTHRU_OWNER_H_
#define _WLAN_HDD_PASSTHRU_OWNER_H_

#ifdef HOUJI_PT_OWNER_HOST_TEST
#include "owner_host.h"
#else
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/types.h>
#endif

struct wlan_hdd_link_info;
struct hdd_pt_owner;
struct hdd_pt_vdev_record;
struct sk_buff;

enum hdd_pt_phase {
	HDD_PT_CREATING,
	HDD_PT_BOUND,
	HDD_PT_ACTIVE,
	HDD_PT_CLOSING,
	HDD_PT_BLOCKED,
	HDD_PT_RECOVERING,
	HDD_PT_DRAINED,
};

/* One registry per WLAN runtime, initialized before publication.
 * Its caller must keep adapter/link, MAC, scheduler, CDP, pdev and module text
 * alive until retire + reap finish AND the normal driver teardown fences run.
 * A registry or objmgr reference is not a replacement for those lifetimes.
 */
struct hdd_pt_registry {
	spinlock_t lock;
	wait_queue_head_t idle;
	struct hdd_pt_owner *owner;
	u64 generation;
	unsigned int users;
	bool closed;
	bool reaping;
};

struct hdd_pt_status {
	enum hdd_pt_phase phase;
	int error;
	int detach_error;
	bool detach_attempted;
	bool recovery_prepared;
	bool recovery_collected;
};

void hdd_pt_registry_init(struct hdd_pt_registry *registry, u64 generation);

/* Adopt ONE already-created PASSTHRU vdev and its physical HDD netdev.
 * Process context; initial submission does not wait for PE/DP/worker work.
 * It may run under RTNL; runtime teardown must be excluded. Caller owns adapter
 * allocation/registration; this record owns its exclusive vdev-delete right.
 * Zero means accepted asynchronously, NOT ready. Negative means not adopted.
 * No second start is allowed until reap, including pending/failed CREATE.
 * Never pass Wonder's virtual netdev, or an adapter ordinary teardown can
 * destroy concurrently. MODE/interface construction is deliberately separate.
 */
int hdd_pt_start(struct hdd_pt_registry *registry,
		 struct wlan_hdd_link_info *link, u16 frequency,
		 const u8 bssid[6]);
/* Journal stays alive until owner reap; normal unjournaled API is retained
 * for existing standalone owner users. MODE always uses this tracked entry.
 */
int hdd_pt_start_record(struct hdd_pt_registry *registry,
                        struct wlan_hdd_link_info *link, u16 frequency,
                        const u8 bssid[6], struct hdd_pt_vdev_record *record);

/* Recovery PREPARATION only, outside RTNL/transitions/component locks.
 * Sticky: closes admission, joins the ordinary worker, stops endpoint calls,
 * and releases only this owner's objmgr reference after control/TX/RX tickets
 * drain. BUSY retains that reference and can be retried. Pending requests,
 * PE, DP installation and physical netdev remain owned, including on success.
 * Does not send DELETE, reset hardware, purge PE, join DP or permit parent
 * destruction. No resume in the same epoch. The post-fence recovery route is
 * still required before production publication.
 */
int hdd_pt_recovery_freeze(struct hdd_pt_registry *registry, u64 generation);

/* After successful freeze AND this epoch's scheduler/WMI/RX fences
 * and PE purge; all parent contexts remain alive and teardown is serialized.
 * Collect completed request references only with another proven pin, then join
 * the stopped DP endpoint. Never submits/repeats vdev DELETE. Must precede
 * the global DP receipt check. Success retains RECOVERING, gate/dev_hold and
 * registry; it does NOT permit physical iface or parent destruction.
 */
int hdd_pt_recovery_collect(struct hdd_pt_registry *registry, u64 generation);

int hdd_pt_status(struct hdd_pt_registry *registry, u64 generation,
		  struct hdd_pt_status *status);

/* BOUND only proves local PE + DP binding. The control owner calls activate
 * only AFTER actual channel/start/up completion and all setup commits.
 */
/* Synchronous control operation, worker context only. A gate ticket
 * pins the adopted vdev/endpoint against owner deletion for the entire
 * callback. The control mutex is NOT held across the callback, which
 * may query status or activate. Close stays nonblocking; teardown waits
 * for this ticket. Operation must not reap/retire/wait on this owner.
 */
int hdd_pt_with_bound(struct hdd_pt_registry *registry, u64 generation,
		       int (*operation)(void *context), void *context);

/* Synchronous active-session channel transaction. Runtime serializes calls;
 * caller pins parents with DSC and supplies an independently armed deadline.
 * Pause closes CPU admission; it does not drain firmware/DMA queues.
 * operation must return actual matched FW completion, never queue acceptance.
 * Any failure after pause closes this owner. No RTNL/work/transition wait.
 */
int hdd_pt_with_channel(struct hdd_pt_registry *registry, u64 generation,
                       u16 frequency, unsigned long deadline,
                       int (*operation)(void *context), void *context);

int hdd_pt_activate(struct hdd_pt_registry *registry, u64 generation);

/* Non-sleeping readiness query for netdev ndo_open; no control mutex. */
bool hdd_pt_data_ready(struct hdd_pt_registry *registry, u64 generation);

/* Always consumes the entire skb list, also on stale/closed/error paths. */
void hdd_pt_tx(struct hdd_pt_registry *registry, u64 generation,
	       struct sk_buff *head);

/* Non-waiting, preallocated handoff, usable from Wonder void deinit.
 * Wonder must first close TX and synchronously unregister its RX handler.
 */
int hdd_pt_close(struct hdd_pt_registry *registry, u64 generation,
		  const char country[3], unsigned int reason);

/* Close registry admission before CNSS lease wait / transition acquisition.
 * This does not stop scheduler/RX, purge SSR state, or claim firmware is dead.
 */
int hdd_pt_retire(struct hdd_pt_registry *registry, unsigned int reason);

/* Process context, outside RTNL/transitions. Returns BUSY until actual endpoint
 * stop -> PE delete -> hdd_vdev_destroy -> DP join completed. Unknown/error
 * outcomes retain ownership; never retry real vdev delete or force success.
 * On success extra dev_hold is already released and worker/public tails joined.
 * Caller must then unregister/free the physical adapter and run normal runtime
 * fences. Registry storage cannot be freed while external entrypoints exist.
 * SSR host-only recovery is not implemented here: BLOCKED prevents deployment
 * until that route and the outer HDD hooks are wired.
 */
int hdd_pt_reap(struct hdd_pt_registry *registry);
/* After completed freeze, current CDS PE/DP/WMI/RX fences and WMA reap.
 * Nonwaiting recovery reap: requires no public users, joined work, no handles,
 * and a completed ordinary vdev-delete journal (or already DRAINED owner).
 * Timeout/ambiguous delete remains blocked. Does not retry DELETE or imply
 * that all objmgr component destructors have completed. Caller still owns
 * physical netdev/parent lifetimes and must run ordinary teardown barriers.
 */
int hdd_pt_recovery_reap(struct hdd_pt_registry *registry, u64 generation);
#endif
