/* SPDX-License-Identifier: ISC */
#ifndef _WLAN_HDD_WONDER_HANDOFF_H_
#define _WLAN_HDD_WONDER_HANDOFF_H_
#ifdef HOUJI_HANDOFF_HOST_TEST
#include "tests/host_kernel.h"
#else
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/netdevice.h>
#endif

enum hdd_wh_state { HDD_WH_OPEN, HDD_WH_CLOSING, HDD_WH_DRAINED, HDD_WH_RELEASED };
/* Preallocated, single-runtime record. No Wonder object or callback pointer.
 * Registry ownership pins this record across EVERY helper/actor invocation.
 * Process/softirq contexts only: spin_lock_bh is not a hardirq/NMI contract.
 * sink is an HDD physical netdev, never Wonder's virtual netdev. dev_hold
 * does NOT pin Wonder, CDP, IRQ, DMA, module code, or queued packet tails.
 */
struct hdd_wh_gate {
	spinlock_t lock;
	wait_queue_head_t changed;
	struct net_device *sink;
	u64 generation;
	u64 next_cookie;
	u64 current_cookie;
	unsigned int users;
	unsigned int reasons;
	enum hdd_wh_state state;
	bool actor;
	char country[3];
};
struct hdd_wh_ticket { struct hdd_wh_gate *gate; u64 generation; };
struct hdd_wh_claim { struct hdd_wh_gate *gate; u64 generation; u64 cookie; };

/* Single-threaded, before registry publication; never reinitialize a record. */
int hdd_wh_init(struct hdd_wh_gate *g, struct net_device *physical_sink, u64 generation);
/* All control operations and RX forwarding require a non-copied ticket.
 * Caller keeps the registry record pinned until leave returns. No callback
 * is executed by these helpers, and no waiting for other users occurs.
 */
int hdd_wh_enter(struct hdd_wh_gate *g, u64 generation, struct hdd_wh_ticket *ticket);
int hdd_wh_leave(struct hdd_wh_ticket *ticket);
/* Valid same-generation close is allocation-free, idempotent and non-waiting.
 * First close copies country; later closes only OR their retirement reasons.
 * Wonder RX handler MUST already have been synchronously unregistered by
 * the existing .stop/rollback path before its backend deinit calls close.
 */
int hdd_wh_close(struct hdd_wh_gate *g, u64 generation, const char country[3], unsigned int reason);
bool hdd_wh_work_pending(struct hdd_wh_gate *g);
/* Worker/retire actor runs outside RTNL/wt-lock/CNSS lease and incompatible
 * runtime transitions. Exactly one claimant, only after all tickets leave.
 */
int hdd_wh_try_claim(struct hdd_wh_gate *g, u64 generation, struct hdd_wh_claim *claim);
/* done=true requires ACTUAL endpoint stop -> actual vdev detach -> join and
 * callback/producer drain while the parent objects still exist. This helper
 * supplies none of these. It does NOT require final netdev unregistration:
 * that comes AFTER release of this gate's extra dev_hold (see below).
 * done=false retains CLOSING and all ownership, allowing a future retry.
 */
int hdd_wh_finish(struct hdd_wh_claim *claim, bool done);
/* Only after registry unpublication, all helper/caller/actor tails returned,
 * and actual backend/producer drain. The outer HDD owner must still own the
 * registered physical netdev and its allocation independently of this gate.
 * Release this extra dev_hold BEFORE synchronous unregister_netdev or the
 * rtnl_unlock that runs a queued unregister: netdev_wait_allrefs_any waits
 * for refcount == 1, so unregister-then-release can deadlock on our reference.
 * The outer owner subsequently performs unregister and required network /
 * parent teardown barriers. Only after those and all owner execution tails
 * end may it free the record. This helper neither unregisters nor frees it.
 * DRAINED is not proof of these external ownership/lifetime preconditions.
 */
int hdd_wh_release(struct hdd_wh_gate *g);
#endif
