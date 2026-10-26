/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Google Wonder WiFi Virtual Soft-MAC Driver
 *
 * Main entry point for the Wonder driver module. This file handles the
 * initialization and cleanup of the driver, locating the physical
 * network device and kicking off the mac80211 registration process.
 */

#include <linux/module.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/platform_device.h>
#include <linux/component.h>
#include <linux/of.h>
#include <linux/of_platform.h>

#include "core.h"
#include "mac80211.h"
#include "ssr.h"
#include "wonder_log.h"
#include "include/wonder/wondertap.h"
#include "wondertap_internal.h"

/* Module parameter for setting the physical device name */
module_param(physical_name, charp, 0444);
MODULE_PARM_DESC(physical_name, "Interface name to use (e.g., wlan0, radiotap0, ...)");

/* Explicit single-instance experiment; default keeps the original DT route. */
static bool synthetic_provider;
module_param(synthetic_provider, bool, 0444);
MODULE_PARM_DESC(synthetic_provider,
                "Use the isolated houji-wonder-provider bridge (default: false)");
static struct platform_device *wonder_synthetic_device;
static int wonder_synthetic_probe_status;

#define WONDER_MAX_COMPAT_VERSIONS 4
static int wonder_ver_match_table[WONDER_VERSION_MAX][WONDER_MAX_COMPAT_VERSIONS] = {
	{ WONDER_VERSION_1_0, -1 },
	{ WONDER_VERSION_1_1, -1 },
	{ WONDER_VERSION_1_2, -1 },
	{ WONDER_VERSION_1_3, -1 },
	{ WONDER_VERSION_1_4, -1 },
	{ WONDER_VERSION_1_4_1, WONDER_VERSION_1_4, -1 },
	{ WONDER_VERSION_1_5, WONDER_VERSION_1_4, WONDER_VERSION_1_4_1, -1 },
};

static bool wonder_ver_can_support(enum wondertap_ver slave, enum wondertap_ver master)
{
	int i;

	if (master < 0 || master >= WONDER_VERSION_MAX)
		return false;
	if (slave < 0 || slave >= WONDER_VERSION_MAX)
		return false;

	for (i = 0; i < WONDER_MAX_COMPAT_VERSIONS; i++) {
		if (wonder_ver_match_table[master][i] == -1)
			break;
		if (wonder_ver_match_table[master][i] == slave) {
			return true;
		}
	}
	return false;
}

static int wonder_master_bind(struct device *dev)
{
	struct wondertap_data *wt = dev_get_drvdata(dev);
	struct wonder_data *wonder = container_of(wt, struct wonder_data, wondertap_data);
	struct cnss_wonder_lease lease;
	const struct wondertap_priv *priv;
	unsigned long flags;
	int ret, version;

	if (READ_ONCE(wonder->quiescing))
		return -ESHUTDOWN;
	ret = component_bind_all(dev, NULL);
	if (ret)
		return ret;
	/* component_mutex is held: snapshot_get is nonblocking and never invokes
	 * vendor code. All metadata reads end before the inspection lease put.
	 */
	ret = cnss_wonder_snapshot_get(wt->provider_dev, &lease);
	if (ret)
		goto unbind;
	priv = cnss_wonder_lease_priv(&lease);
	if (!priv || !priv->wonder_ops || !wonder_ver_can_support(priv->ver, wt->ver)) {
		ret = -EINVAL;
		goto put;
	}
	version = priv->ver;
	spin_lock_irqsave(&wonder->lifecycle_lock, flags);
	if (wonder->quiescing) {
		ret = -ESHUTDOWN;
	} else {
		WRITE_ONCE(wt->generation, cnss_wonder_lease_generation(&lease));
		smp_store_release(&wt->bound, true);
	}
	spin_unlock_irqrestore(&wonder->lifecycle_lock, flags);
	if (!ret)
		dev_info(dev, "Connected to leased wlan ver %d\n", version);
put:
	cnss_wonder_call_put(&lease);
	if (!ret)
		return 0;
unbind:
	component_unbind_all(dev, NULL);
	return ret;
}

static void wonder_master_unbind(struct device *dev)
{
	struct wondertap_data *wt = dev_get_drvdata(dev);

	/* Short gate close only. No cancel, wait or detach under component_mutex. */
	cnss_wonder_consumer_close(wt->consumer);
	smp_store_release(&wt->bound, false);
	wonder_ssr_provider_lost(container_of(wt, struct wonder_data, wondertap_data));
	component_unbind_all(dev, NULL);
}

static void wonder_consumer_quiesce(void *ctx, u64 generation,
				    enum cnss_wonder_retire_reason reason)
{
	struct wonder_data *wonder = ctx;
	struct wondertap_data *wt = &wonder->wondertap_data;
	unsigned long flags;

	/* Endpoint is single-use. A stale generation must never rearm hardware.
	 * A mismatch is a protocol violation; fail closed for the local instance.
	 */
	WARN_ON(READ_ONCE(wt->generation) && generation &&
		READ_ONCE(wt->generation) != generation);
	spin_lock_irqsave(&wonder->lifecycle_lock, flags);
	WRITE_ONCE(wonder->quiescing, true);
	WRITE_ONCE(wonder->tx_open, false);
	if (reason != CNSS_WONDER_CONSUMER_REMOVE) {
		WRITE_ONCE(wonder->provider_lost, true);
		smp_store_release(&wt->bound, false);
	}
	spin_unlock_irqrestore(&wonder->lifecycle_lock, flags);
	/* Broker chose the sole actor, outside component/RTNL/state locks.
	 * ssr_exit cancels work before features_exit takes its local mutex.
	 * Normal remove may take regular leases; retire must never call deinit.
	 */
	wonder_mac80211_quiesce(wonder);
	smp_store_release(&wt->bound, false);
	wondertap_forget_provider(wt);
}

static const struct cnss_wonder_consumer_ops wonder_consumer_ops = {
	.quiesce = wonder_consumer_quiesce,
};

static const struct component_master_ops wonder_comp_ops = {
	.bind = wonder_master_bind,
	.unbind = wonder_master_unbind,
};

static int wonder_compare_dev(struct device *dev, void *data)
{
	return (dev->of_node == data);
}

static void wonder_release_node(struct device *dev, void *data)
{
	of_node_put(data);
}

/* The synthetic match owns its own device ref, separate from provider_dev. */
static int wonder_compare_identity(struct device *dev, void *data)
{
	return dev == data;
}

static void wonder_release_device(struct device *dev, void *data)
{
	put_device(data);
}

static int wonder_probe_instance(struct platform_device *pdev)
{
	struct wonder_data *wonder;
	struct wondertap_data *wondertap;
	struct component_match *match = NULL;
	struct device_node *provider_node = NULL;
	struct platform_device *provider;
	struct device *provider_dev;
	int ret;

	dev_info(&pdev->dev, "%s(): probe\n", __func__);
	if (synthetic_provider) {
		/* Do not also bind a DT device when the explicit experiment is on. */
		if (pdev != wonder_synthetic_device)
			return -ENODEV;
		provider_dev = bus_find_device_by_name(&platform_bus_type, NULL,
						     "houji-wonder-provider");
		if (!provider_dev)
			return -ENODEV;
		component_match_add_release(&pdev->dev, &match,
					    wonder_release_device,
					    wonder_compare_identity,
					    get_device(provider_dev));
		if (IS_ERR(match)) {
			ret = PTR_ERR(match);
			/* Failed insertion did not consume the match reference. */
			put_device(provider_dev);
			put_device(provider_dev);
			return ret;
		}
	} else {
		provider_node = of_parse_phandle(pdev->dev.of_node,
					       "wondertap-provider", 0);
		if (!provider_node) {
			dev_err(&pdev->dev, "Missing 'wondertap-provider' in DTS\n");
			return -ENODEV;
		}
		/* The match owns the parsed node ref, as in the DT-only route. */
		component_match_add_release(&pdev->dev, &match,
					    wonder_release_node,
					    wonder_compare_dev, provider_node);
		if (IS_ERR(match)) {
			ret = PTR_ERR(match);
			of_node_put(provider_node);
			return ret;
		}
		provider = of_find_device_by_node(provider_node);
		if (!provider)
			return -EPROBE_DEFER;
		provider_dev = &provider->dev;
	}

	wonder = wonder_mac80211_init();
	if (IS_ERR(wonder)) {
		put_device(provider_dev);
		return PTR_ERR(wonder);
	}
	wondertap = &wonder->wondertap_data;
	wondertap->wlan_node = provider_node; /* NULL only on identity-match path. */
	wondertap->ver = WONDER_VERSION_1_5;
	wondertap->provider_dev = provider_dev;
	/* Attach does not call the hook; all callback state is ready beforehand. */
	ret = cnss_wonder_consumer_attach(wondertap->provider_dev, THIS_MODULE,
					&wonder_consumer_ops, wonder, &wondertap->consumer);
	if (ret)
		goto put_device;
	platform_set_drvdata(pdev, wondertap);
	ret = component_master_add_with_match(&pdev->dev, &wonder_comp_ops, match);
	if (ret) {
		dev_err(&pdev->dev, "%s(): Component registration failed: %d\n",
			__func__, ret);
		/* Failed master_add has already removed its aggregate object. */
		platform_set_drvdata(pdev, NULL);
		cnss_wonder_consumer_detach(wondertap->consumer);
		/* Failed master_add removed its aggregate and finished callbacks. */
		cnss_wonder_consumer_put(wondertap->consumer);
		wondertap->consumer = NULL;
		goto put_device;
	}

	/* Optional diagnostics must not turn an established binding into failure. */
	ret = wonder_debugfs_init(wonder);
	if (ret)
		dev_warn(&pdev->dev, "%s(): Debugfs unavailable: %d\n", __func__, ret);
	return 0;
put_device:
	put_device(wondertap->provider_dev);
	wondertap->provider_dev = NULL;
	wonder_mac80211_exit(wonder);
	return ret;
}

static int wonder_probe(struct platform_device *pdev)
{
	int ret = wonder_probe_instance(pdev);

	if (synthetic_provider && pdev == wonder_synthetic_device) {
		/* One-shot init reports failure; never leave a deferred synthetic
		 * probe racing module-init rollback.
		 */
		if (ret == -EPROBE_DEFER)
			ret = -ENODEV;
		WRITE_ONCE(wonder_synthetic_probe_status, ret);
	}
	return ret;
}

static void wonder_remove(struct platform_device *pdev)
{
	struct wondertap_data *wondertap = platform_get_drvdata(pdev);
	struct wonder_data *wonder = container_of(wondertap, struct wonder_data, wondertap_data);

	wonder_debugfs_exit();
	/* Detach completes the actor cleanup, but retains this cookie reference. */
	cnss_wonder_consumer_detach(wondertap->consumer);
	component_master_del(&pdev->dev, &wonder_comp_ops);
	cnss_wonder_consumer_put(wondertap->consumer);
	wondertap->consumer = NULL;
	put_device(wondertap->provider_dev);
	wondertap->provider_dev = NULL;
	platform_set_drvdata(pdev, NULL);
	wonder_mac80211_free(wonder);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
static int wonder_remove_wrapper(struct platform_device *pdev)
{
	wonder_remove(pdev);
	return 0;
}
#define wonder_remove wonder_remove_wrapper
#endif /* LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0) */

static const struct of_device_id wonder_dt_ids[] = {
	{ .compatible = "google,wonder-drv-v1" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, wonder_dt_ids);

static struct platform_driver wonder_driver = {
	.probe = wonder_probe,
	.remove = wonder_remove,
	.driver = {
		.name = "wonder",
		.of_match_table = wonder_dt_ids,
		.probe_type = PROBE_FORCE_SYNCHRONOUS,
	},
};
static int __init wonder_init(void)
{
	struct platform_device *pdev;
	int ret;

	if (!synthetic_provider)
		return platform_driver_register(&wonder_driver);
	pdev = platform_device_alloc("wonder", PLATFORM_DEVID_NONE);
	if (!pdev)
		return -ENOMEM;
	wonder_synthetic_device = pdev;
	WRITE_ONCE(wonder_synthetic_probe_status, -ENODEV);
	ret = platform_driver_register(&wonder_driver);
	if (ret)
		goto put_device;
	ret = platform_device_add(pdev);
	if (ret)
		goto unregister_driver;
	ret = READ_ONCE(wonder_synthetic_probe_status);
	if (ret) {
		platform_device_unregister(pdev);
		wonder_synthetic_device = NULL;
		platform_driver_unregister(&wonder_driver);
		return ret;
	}
	return 0;

unregister_driver:
	platform_driver_unregister(&wonder_driver);
put_device:
	wonder_synthetic_device = NULL;
	platform_device_put(pdev);
	return ret;
}

static void __exit wonder_exit(void)
{
	if (wonder_synthetic_device) {
		/* Synchronous remove finishes consumer/component work before
		 * device devres drops the identity-match provider reference.
		 */
		platform_device_unregister(wonder_synthetic_device);
		wonder_synthetic_device = NULL;
	}
	platform_driver_unregister(&wonder_driver);
}
module_init(wonder_init);
module_exit(wonder_exit);

MODULE_DESCRIPTION("Google Wonder Virtual mac80211 Driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Google Android WiFi Team");
