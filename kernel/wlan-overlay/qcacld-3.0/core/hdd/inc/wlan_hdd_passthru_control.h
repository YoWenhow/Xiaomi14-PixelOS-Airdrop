/* SPDX-License-Identifier: ISC */
#ifndef _WLAN_HDD_PASSTHRU_CONTROL_H_
#define _WLAN_HDD_PASSTHRU_CONTROL_H_

#include <linux/types.h>
#include <linux/bitops.h>
#include "wonder/wondertap.h"

struct hdd_context;
struct hdd_pt_iface;

/* This initial profile accepts only enabled non-DFS/non-passive 2.4/5
 * GHz channels, 20 MHz, 1SS HT MCS 0..7, long/short GI, no aggregation and
 * zero retry requests. Country must already match; validation never changes it.
 */
int hdd_pt_control_validate(struct hdd_context *hdd,
			   const struct wondertap_init_params *params);

/* Serialized runtime worker, outside Wonder/RTNL/SME locks. The caller pins
 * iface, adapter and the entire WLAN runtime until completion. Submit ONCE
 * after BOUND. A zero return is queue acceptance, not START/UP completion.
 * Owner PE identity is the adapter MAC; MLME BSSID is params->bssid.
 */
/* Nonzero module-unique PT cookie; runtime must arm a finite deadline first.
 * No generic START timer is used. On timeout/retire call abort before cleanup.
 */
int hdd_pt_control_submit_start(struct hdd_pt_iface *iface,
			       const struct wondertap_init_params *params, u32 cookie);
/* ACTIVE owner, paused data path and owner ticket must remain held until
 * matching completion/commit. Existing peer/BSSID/rate remain unchanged.
 */
int hdd_pt_control_submit_channel(struct hdd_pt_iface *iface,
                                 const struct wondertap_init_params *params,
                                 u32 cookie);
/* Must pass after matching START/UP events and before any finish/activate. */
int hdd_pt_control_check_start(struct hdd_pt_iface *iface, u32 cookie);
void hdd_pt_control_abort(struct hdd_pt_iface *iface);

/* Call ONCE after both successful START_RESPONSE and UP_SUBMITTED for this
 * live slot. Failures retain all objects for owner close/reap; do not retry a
 * partially applied finish. Success checks command submission and local peer
 * setup, not unacknowledged firmware application or over-the-air behavior.
 * The caller alone activates the owner. DP frequency was committed by bind.
 */
int hdd_pt_control_finish_start(struct hdd_pt_iface *iface,
			       const struct wondertap_init_params *params);

#endif
