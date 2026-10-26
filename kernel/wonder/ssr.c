// SPDX-License-Identifier: GPL-2.0
/* Local fail-closed shutdown; provider re-admission needs a separate protocol. */
#define LOG_MODULE_NAME "ssr"
#include <linux/netdevice.h>
#include "core.h"
#include "mac80211.h"
#include "wonder_log.h"
#include "ssr.h"

static void wonder_request_quiesce(struct wonder_data *wonder, bool lost)
{
	unsigned long flags;

	spin_lock_irqsave(&wonder->lifecycle_lock, flags);
	if (lost) {
		WRITE_ONCE(wonder->provider_lost, true);
		smp_store_release(&wonder->wondertap_data.bound, false);
	}
	WRITE_ONCE(wonder->quiescing, true);
	WRITE_ONCE(wonder->tx_open, false);
	/* Submit before dropping the gate: remove cannot miss a late enqueue. */
	if (!wonder->removing)
		schedule_work(&wonder->pdev_down_work);
	spin_unlock_irqrestore(&wonder->lifecycle_lock, flags);
}

static void wonder_pdev_down_work_handler(struct work_struct *work)
{
	struct wonder_data *wonder = container_of(work, struct wonder_data, pdev_down_work);

	/* No component_mutex, RTNL or lifecycle lock is held here. */
	wonder_features_exit(wonder);
	/* Intentionally no re-register: the old provider may no longer exist. */
}

static int wonder_netdev_event(struct notifier_block *nb, unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct wonder_data *wonder = container_of(nb, struct wonder_data, netdev_notifier);

	if (dev != wonder->pdev)
		return NOTIFY_DONE;
	if (event == NETDEV_DOWN) {
		wonder_info("Physical device %s down; shutting down local hardware.\n", dev->name);
		wonder_request_quiesce(wonder, false);
	}
	return NOTIFY_DONE;
}

int wonder_ssr_init(struct wonder_data *wonder)
{
	int ret;

	INIT_WORK(&wonder->pdev_down_work, wonder_pdev_down_work_handler);
	wonder->netdev_notifier.notifier_call = wonder_netdev_event;
	ret = register_netdevice_notifier(&wonder->netdev_notifier);
	if (!ret)
		wonder->notifier_registered = true;
	return ret;
}

void wonder_ssr_provider_lost(struct wonder_data *wonder)
{
	/* Called under component_mutex: mark and queue only, never wait. */
	wonder_request_quiesce(wonder, true);
}

void wonder_ssr_exit(struct wonder_data *wonder)
{
	unsigned long flags;

	spin_lock_irqsave(&wonder->lifecycle_lock, flags);
	wonder->removing = true;
	WRITE_ONCE(wonder->quiescing, true);
	WRITE_ONCE(wonder->tx_open, false);
	spin_unlock_irqrestore(&wonder->lifecycle_lock, flags);
	if (wonder->notifier_registered) {
		unregister_netdevice_notifier(&wonder->netdev_notifier);
		wonder->notifier_registered = false;
	}
	/* Not under RTNL/component_mutex/lifecycle_lock, including error paths. */
	cancel_work_sync(&wonder->pdev_down_work);
}
