/* SPDX-License-Identifier: ISC */
#include "wlan_hdd_passthru_iface.h"
#ifndef HOUJI_PT_IFACE_HOST_TEST
#include <linux/etherdevice.h>
#include <linux/rtnetlink.h>
#include "wlan_hdd_main.h"
#include "wlan_policy_mgr_ucfg.h"
#endif

static netdev_tx_t hdd_pt_netdev_tx(struct sk_buff *skb,
				    struct net_device *dev)
{
	struct hdd_adapter *adapter = netdev_priv(dev);
	struct hdd_pt_registry *registry =
		READ_ONCE(adapter->passthru_registry);

	if (registry)
		hdd_pt_tx(registry, adapter->passthru_generation, skb);
	else
		dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static int hdd_pt_netdev_open(struct net_device *dev)
{
	struct hdd_adapter *adapter = netdev_priv(dev);
	struct hdd_pt_registry *registry =
		READ_ONCE(adapter->passthru_registry);

	if (!registry ||
	    !hdd_pt_data_ready(registry, adapter->passthru_generation))
		return -EAGAIN;
	netif_carrier_on(dev);
	netif_tx_start_all_queues(dev);
	return 0;
}

static int hdd_pt_netdev_stop(struct net_device *dev)
{
	struct hdd_adapter *adapter = netdev_priv(dev);
	struct hdd_pt_registry *registry =
		READ_ONCE(adapter->passthru_registry);

	netif_tx_stop_all_queues(dev);
	netif_carrier_off(dev);
	if (registry)
		hdd_pt_close(registry, adapter->passthru_generation, "00", 2);
	return 0;
}

static const struct net_device_ops hdd_pt_netdev_ops = {
	.ndo_open = hdd_pt_netdev_open,
	.ndo_stop = hdd_pt_netdev_stop,
	.ndo_start_xmit = hdd_pt_netdev_tx,
};

void hdd_pt_set_netdev_ops(struct net_device *dev)
{
	dev->netdev_ops = &hdd_pt_netdev_ops;
}

int hdd_pt_iface_create(struct hdd_pt_iface *slot, struct hdd_context *hdd,
			struct hdd_pt_registry *registry, u64 generation,
			const u8 mac[6], const u8 bssid[6], u16 frequency)
{
	struct hdd_adapter_create_param params = {0};
	u8 address[6];
	int ret;

	ASSERT_RTNL();
	if (!slot || !hdd || !registry || !generation ||
	    registry->generation != generation || !mac || !bssid ||
	    !is_valid_ether_addr(mac) || !is_valid_ether_addr(bssid) ||
	    frequency < 2300 || frequency > 7125)
		return -EINVAL;
	if (slot->create_attempted || slot->adapter)
		return -EBUSY;
	if (ucfg_policy_mgr_get_connection_count(hdd->psoc))
		return -EBUSY;
	/* Reserve before any side effect. Failure stays visible to the runtime
	 * coordinator instead of producing a fresh adapter on init retry.
	 */
	slot->create_attempted = true;
	slot->hdd = hdd;
	slot->registry = registry;
	slot->generation = generation;
	slot->vdev_record.generation = generation;
	slot->frequency = frequency;
	memcpy(address, mac, sizeof(address));
	params.num_sessions = 1;
	slot->adapter = hdd_open_adapter(hdd, QDF_PASSTHRU_MODE, "wondertap0",
					 address, NET_NAME_UNKNOWN, true, &params);
	if (!slot->adapter) {
		slot->error = -EIO;
		return slot->error;
	}
	slot->adapter->passthru_generation = generation;
	WRITE_ONCE(slot->adapter->passthru_registry, registry);
	/* Deliberately bypass station initialization/ordinary Ethernet TX:
	 * PASSTHRU PE creation and raw DP bind belong to the owner.
	 */
	ret = hdd_pt_vdev_create(slot->adapter->deflink, &slot->vdev_record);
	if (ret)
		goto blocked;
	slot->vdev_created = true;
	/* LIM channel-change looks up PE by the interface's own MAC.
	 * The target BSSID is set separately in MLME by control_submit_start.
	 */
	ret = hdd_pt_start_record(registry, slot->adapter->deflink, frequency,
				  mac, &slot->vdev_record);
	if (ret)
		goto blocked;
	slot->owner_started = true;
	return 0;
blocked:
	slot->error = ret;
	return ret;
}

void hdd_pt_iface_close(struct hdd_pt_iface *slot,
			const char country[3], unsigned int reason)
{
	if (slot && slot->owner_started && !slot->owner_reaped)
		hdd_pt_close(slot->registry, slot->generation, country, reason);
}

/* Caller owns RTNL and the slot. Keep this shared tail free of owner waits. */
static int hdd_pt_iface_cleanup_locked(struct hdd_pt_iface *slot)
{
	struct hdd_adapter *adapter = slot->adapter;

	ASSERT_RTNL();
	if (rcu_access_pointer(adapter->dev->rx_handler))
		return -EBUSY;
	dev_close(adapter->dev);
	WRITE_ONCE(adapter->passthru_registry, NULL);
	synchronize_net();
	slot->adapter = NULL;
	hdd_close_adapter(slot->hdd, adapter, true);
	return 0;
}

int hdd_pt_iface_cleanup(struct hdd_pt_iface *slot)
{
	int ret;

	if (!slot || !slot->adapter)
		return 0;
	if (!slot->owner_started)
		return -EUCLEAN;
	if (!slot->owner_reaped) {
		hdd_pt_iface_close(slot, "00", 4);
		ret = hdd_pt_reap(slot->registry);
		if (ret)
			return ret;
		slot->owner_reaped = true;
	}
	rtnl_lock();
	ret = hdd_pt_iface_cleanup_locked(slot);
	rtnl_unlock();
	return ret;
}

int hdd_pt_iface_recovery_cleanup(struct hdd_pt_iface *slot)
{
	int ret;

	ASSERT_RTNL();
	if (!slot)
		return -EINVAL;
	if (!slot->adapter)
		return slot->owner_started && !slot->owner_reaped ? -EUCLEAN : 0;
	if (!slot->owner_started)
		return -EUCLEAN;
	/* Retain the whole owner as well when the consumer still owns RX. */
	if (rcu_access_pointer(slot->adapter->dev->rx_handler))
		return -EBUSY;
	if (!slot->owner_reaped) {
		ret = hdd_pt_recovery_reap(slot->registry, slot->generation);
		if (ret)
			return ret;
		slot->owner_reaped = true;
	}
	/* No extra gate dev_hold remains before netdev unregister. */
	return hdd_pt_iface_cleanup_locked(slot);
}
