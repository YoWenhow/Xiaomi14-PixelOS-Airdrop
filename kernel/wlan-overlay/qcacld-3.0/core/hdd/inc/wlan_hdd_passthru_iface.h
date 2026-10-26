/* SPDX-License-Identifier: ISC */
#ifndef _WLAN_HDD_PASSTHRU_IFACE_H_
#define _WLAN_HDD_PASSTHRU_IFACE_H_

#include "wlan_hdd_passthru_owner.h"
#include "wlan_hdd_passthru_vdev.h"
struct hdd_context;
struct hdd_adapter;
struct net_device;

/* The runtime coordinator owns this preallocated record and serializes its
 * methods. Keep it and registry alive through netdev unregister, all provider
 * leases and driver work/IRQ fences. No pointer to Wonder is retained.
 */
struct hdd_pt_iface {
	struct hdd_context *hdd;
	struct hdd_adapter *adapter;
	struct hdd_pt_registry *registry;
	struct hdd_pt_vdev_record vdev_record;
	u64 generation;
	u16 frequency;
	bool create_attempted;
	bool vdev_created;
	bool owner_started;
	bool owner_reaped;
	int error;
};

/* Called by hdd_alloc_station_adapter BEFORE netdev registration. */
void hdd_pt_set_netdev_ops(struct net_device *dev);

/* Caller holds RTNL and exclusive runtime ownership; slot is zeroed once.
 * After any returned error, a non-NULL slot->adapter remains owned by slot.
 * Never discard it or repeat CREATE: a FW-create failure may be ambiguous.
 * No other thread may close/destroy this adapter via ordinary HDD teardown.
 * Success means owner CREATE accepted, not FW UP or usable data.
 * Initial experiment excludes existing connections; this is NOT a complete
 * concurrency reservation. Runtime publication still needs the policy gate.
 */
int hdd_pt_iface_create(struct hdd_pt_iface *slot, struct hdd_context *hdd,
			struct hdd_pt_registry *registry, u64 generation,
			const u8 mac[6], const u8 bssid[6], u16 frequency);

/* Non-waiting: can be called by Wonder deinit with RTNL/wt lock/lease held. */
void hdd_pt_iface_close(struct hdd_pt_iface *slot,
			const char country[3], unsigned int reason);

/* Worker only, OUTSIDE RTNL and driver transitions, serialized against create
 * and all slot users. Busy never frees. If Wonder's RX handler is still
 * attached, retain the physical adapter until consumer quiesce removes it.
 * Unknown FW creation/deletion remains blocked for explicit SSR cleanup.
 */
int hdd_pt_iface_cleanup(struct hdd_pt_iface *slot);
/* Post-CDS recovery finish only, caller ALREADY holds RTNL and exclusive
 * runtime control. No wait for owner work/users; no recursive RTNL acquire.
 * Caller must run rtnl_unlock (unregister tails) before releasing runtime.
 */
int hdd_pt_iface_recovery_cleanup(struct hdd_pt_iface *slot);
#endif
