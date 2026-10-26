/* SPDX-License-Identifier: ISC */
#ifndef _WLAN_HDD_PASSTHRU_RUNTIME_H_
#define _WLAN_HDD_PASSTHRU_RUNTIME_H_

#include "cnss_wonder_lifecycle.h"
struct hdd_context;

/*
 * Experimental coordinator; NOT called by ordinary HDD lifecycle yet.
 * Register only with a live SME/DP runtime and an exclusive PASSTHRU policy
 * reservation. Parent lifetime is the caller's responsibility.
 *
 * Both functions run outside RTNL, DSC transitions and component locks.
 * Retire must precede every parent teardown, including idle stop and SSR.
 * Any retire error means parents MUST remain alive: no force-free fallback.
 * A BLOCKED/pre-owner orphan still requires a separate SSR recovery route.
 * Do not enable publication in HDD until that route and all hooks exist.
 *
 * One interface per SME epoch: events lack generation tags. After unregister,
 * SME deliberately refuses callback rearming until a fresh SME open.
 */
/* Experimental activation after successful module init (MODULE_STATE_LIVE).
 * First admitted attempt permanently pins this module until full reboot,
 * even if later publication fails. Normal rmmod is intentionally unavailable
 * afterwards; retire/SSR success does not drop the pin. Built-in code needs
 * no pin. This does not protect device unbind, forced removal or soft-unload.
 */
int hdd_pt_runtime_register(struct hdd_context *hdd);
int hdd_pt_runtime_retire(struct hdd_context *hdd,
			 enum cnss_wonder_retire_reason reason);
/* Recovery preparation; same lock/context restrictions as retire.
 * Retires CNSS leases, unregisters SME callback, freezes runtime/owner work.
 * Zero only means preparation, NOT permission to destroy parents.
 * Pre-owner ambiguous creation returns EUCLEAN with storage retained.
 * A complete post-fence PE/WMA/DP collection path is still required.
 */
/* Also permanently seals registration for this module instance, including
 * when no runtime exists. A rejected mismatched context does not seal it.
 */
int hdd_pt_runtime_recovery_freeze(struct hdd_context *hdd,
                                 enum cnss_wonder_retire_reason reason);
/* CDS stop preflight: absent runtime or completed recovery preparation only.
 * Try-locks avoid inverse waits while the caller holds a DSC transition.
 * Caller excludes registration for the whole stop epoch.
 */
int hdd_pt_runtime_recovery_check(struct hdd_context *hdd);
/* After scheduler/WMI/RX fences and successful PE purge, before DP receipts.
 * Collect/join only; no RTNL, provider waits, iface free or phase promotion.
 */
int hdd_pt_runtime_recovery_collect(struct hdd_context *hdd);
/* Final parent-lifetime guard: only a fully retired/absent runtime succeeds.
 * collect success alone must not allow CDS to destroy its parent contexts.
 */
int hdd_pt_runtime_teardown_check(struct hdd_context *hdd);
/* After this CDS epoch's PE purge, global DP receipts and WMA reap.
 * Trylocks only for lifecycle/control/RTNL. Finishes the narrow proven-delete
 * path; unknown outcomes retain runtime. Normal parent barriers still apply.
 */
int hdd_pt_runtime_recovery_finish(struct hdd_context *hdd);
#endif
