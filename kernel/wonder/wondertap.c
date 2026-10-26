/* SPDX-License-Identifier: GPL-2.0 */
#define LOG_MODULE_NAME "wondertap"
#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/delay.h>
#include "include/wonder/wondertap.h"
#include "wondertap_internal.h"
#include "wonder_log.h"

/* Only a live lease may expose metadata or ops; consumer survives master_del. */
int wondertap_call_get(struct wondertap_data *wt, struct cnss_wonder_lease *lease)
{
	const struct wondertap_priv *priv;
	int ret;

	if (!smp_load_acquire(&wt->bound))
		return -ESHUTDOWN;
	ret = cnss_wonder_call_get(wt->consumer, READ_ONCE(wt->generation), lease);
	if (ret)
		return ret;
	priv = cnss_wonder_lease_priv(lease);
	if (!smp_load_acquire(&wt->bound) || !priv || !priv->wonder_ops ||
	    cnss_wonder_lease_generation(lease) != READ_ONCE(wt->generation)) {
		cnss_wonder_call_put(lease);
		return -ESHUTDOWN;
	}
	return 0;
}

static const struct wondertap_ops *wondertap_lease_ops(struct cnss_wonder_lease *lease)
{
	const struct wondertap_priv *priv = cnss_wonder_lease_priv(lease);
	return priv->wonder_ops;
}

int wondertap_init(struct wondertap_data *wt, const struct wondertap_init_params *unused)
{
	struct cnss_wonder_lease lease;
	const struct wondertap_ops *ops;
	struct wondertap_init_params params = { 0 };
	void *handle = NULL;
	int retry, ret;

	mutex_lock(&wt->lock);
	if (wt->state != WONDERTAP_STATE_DOWN) {
		ret = -EBUSY;
		goto out;
	}
	if ((wt->cache_flags & (WONDERTAP_CACHE_FREQ_SET |
		WONDERTAP_CACHE_TX_RATE_SET | WONDERTAP_CACHE_BSSID_SET |
		WONDERTAP_CACHE_COUNTRY_CODE_SET)) !=
	    (WONDERTAP_CACHE_FREQ_SET | WONDERTAP_CACHE_TX_RATE_SET |
		WONDERTAP_CACHE_BSSID_SET | WONDERTAP_CACHE_COUNTRY_CODE_SET)) {
		ret = -EINVAL;
		goto out;
	}
	ret = wondertap_call_get(wt, &lease);
	if (ret)
		goto out;
	ops = wondertap_lease_ops(&lease);
	if (!ops->init) {
		ret = -EOPNOTSUPP;
		goto put;
	}
	eth_random_addr(wt->mac_addr);
	params.channel = wt->cached_freq;
	params.tx_rate = wt->cached_tx_rate;
	memcpy(params.bssid, wt->cached_bssid, ETH_ALEN);
	memcpy(params.mac_addr, wt->mac_addr, ETH_ALEN);
	memcpy(params.country_code, wt->cached_country_code, sizeof(params.country_code));
	/* One lease covers all retries, sleeps and the final state/handle commit. */
	for (retry = 0; retry < 4; retry++) {
		ret = ops->init(&handle, &params);
		if (!ret)
			break;
		/* Failed init must not publish an owned handle. Do not blindly retry
		 * an implementation violating that contract or guess how to free it.
		 */
		if (WARN_ON(handle)) {
			wt->state = WONDERTAP_STATE_FAILED;
			break;
		}
		if (retry < 3)
			msleep(400);
	}
	if (!ret) {
		wt->vendor_handle = handle;
		wt->state = WONDERTAP_STATE_UP;
	}
put:
	cnss_wonder_call_put(&lease);
out:
	mutex_unlock(&wt->lock);
	return ret;
}

int wondertap_deinit(struct wondertap_data *wt)
{
	struct cnss_wonder_lease lease;
	const struct wondertap_ops *ops;
	struct wondertap_deinit_params params = { 0 };
	int ret = 0;

	mutex_lock(&wt->lock);
	if (wt->state == WONDERTAP_STATE_DOWN)
		goto out;
	if (wt->state == WONDERTAP_STATE_FAILED) {
		ret = -EIO;
		goto out;
	}
	ret = wondertap_call_get(wt, &lease);
	if (ret)
		goto out;
	ops = wondertap_lease_ops(&lease);
	if (!ops->deinit) {
		ret = -EOPNOTSUPP;
	} else {
		memcpy(params.country_code, wt->cached_country_code, sizeof(params.country_code));
		ops->deinit(wt->vendor_handle, &params);
		/* Void callback returned; this cannot prove its backend cleanup. */
		wt->vendor_handle = NULL;
		wt->state = WONDERTAP_STATE_DOWN;
	}
	cnss_wonder_call_put(&lease);
out:
	wt->last_deinit_status = ret;
	mutex_unlock(&wt->lock);
	return ret;
}

void wondertap_forget_provider(struct wondertap_data *wt)
{
	mutex_lock(&wt->lock);
	wt->vendor_handle = NULL;
	wt->state = WONDERTAP_STATE_DOWN;
	mutex_unlock(&wt->lock);
}

int wondertap_set_freq(struct wondertap_data *wt, const struct wondertap_set_freq_params *params)
{
	struct cnss_wonder_lease lease;
	const struct wondertap_ops *ops;
	int ret = 0;

	mutex_lock(&wt->lock);
	wt->cached_freq = *params;
	wt->cache_flags |= WONDERTAP_CACHE_FREQ_SET;
	if (wt->state == WONDERTAP_STATE_UP) {
		ret = wondertap_call_get(wt, &lease);
		if (!ret) {
			ops = wondertap_lease_ops(&lease);
			ret = ops->set_freq ? ops->set_freq(wt->vendor_handle, params) : -EOPNOTSUPP;
			cnss_wonder_call_put(&lease);
		}
	}
	mutex_unlock(&wt->lock);
	return ret;
}

int wondertap_set_filter(struct wondertap_data *wt, enum wondertap_filter_type type, const void *params)
{
	struct cnss_wonder_lease lease;
	const struct wondertap_ops *ops;
	int ret = -ENODEV;

	mutex_lock(&wt->lock);
	if (wt->state == WONDERTAP_STATE_UP) {
		ret = wondertap_call_get(wt, &lease);
		if (!ret) {
			ops = wondertap_lease_ops(&lease);
			ret = ops->set_filter ? ops->set_filter(wt->vendor_handle, type, params) : -EOPNOTSUPP;
			cnss_wonder_call_put(&lease);
		}
	}
	mutex_unlock(&wt->lock);
	return ret;
}

int wondertap_set_fixed_tx_rate(struct wondertap_data *wt, const struct wondertap_fixed_tx_rate_params *params)
{
	struct cnss_wonder_lease lease;
	const struct wondertap_ops *ops;
	int ret = 0;

	mutex_lock(&wt->lock);
	wt->cached_tx_rate = *params;
	wt->cache_flags |= WONDERTAP_CACHE_TX_RATE_SET;
	if (wt->state == WONDERTAP_STATE_UP) {
		ret = wondertap_call_get(wt, &lease);
		if (!ret) {
			ops = wondertap_lease_ops(&lease);
			ret = ops->set_fixed_tx_rate ? ops->set_fixed_tx_rate(wt->vendor_handle, params) : -EOPNOTSUPP;
			cnss_wonder_call_put(&lease);
		}
	}
	mutex_unlock(&wt->lock);
	return ret;
}

int wondertap_set_tx_rate_mask(struct wondertap_data *wt, const struct wondertap_tx_rate_mask_params *params)
{
	struct cnss_wonder_lease lease;
	const struct wondertap_ops *ops;
	int ret = -ENODEV;

	mutex_lock(&wt->lock);
	if (wt->state == WONDERTAP_STATE_UP) {
		ret = wondertap_call_get(wt, &lease);
		if (!ret) {
			ops = wondertap_lease_ops(&lease);
			ret = ops->set_tx_rate_mask ? ops->set_tx_rate_mask(wt->vendor_handle, params) : -EOPNOTSUPP;
			cnss_wonder_call_put(&lease);
		}
	}
	mutex_unlock(&wt->lock);
	return ret;
}

int wondertap_get_capabilities(struct wondertap_data *wt, struct wondertap_capability *features)
{
	struct cnss_wonder_lease lease;
	const struct wondertap_ops *ops;
	int ret;

	mutex_lock(&wt->lock);
	ret = wondertap_call_get(wt, &lease);
	if (!ret) {
		ops = wondertap_lease_ops(&lease);
		ret = ops->get_capabilities ? ops->get_capabilities(wt->vendor_handle, features) : -EOPNOTSUPP;
		cnss_wonder_call_put(&lease);
	}
	mutex_unlock(&wt->lock);
	return ret;
}

int wondertap_set_reg(struct wondertap_data *wt, const char *country_code)
{
	mutex_lock(&wt->lock);
	memcpy(wt->cached_country_code, country_code, sizeof(wt->cached_country_code));
	wt->cache_flags |= WONDERTAP_CACHE_COUNTRY_CODE_SET;
	mutex_unlock(&wt->lock);
	return 0;
}

int wondertap_get_interface_mac_address(struct wondertap_data *wt, u8 (*mac_addr)[ETH_ALEN])
{
	int ret = -ENODEV;
	mutex_lock(&wt->lock);
	if (wt->state == WONDERTAP_STATE_UP) {
		memcpy(*mac_addr, wt->mac_addr, ETH_ALEN);
		ret = 0;
	}
	mutex_unlock(&wt->lock);
	return ret;
}

int wondertap_set_bssid_filter(struct wondertap_data *wt, const u8 *bssid)
{
	mutex_lock(&wt->lock);
	memcpy(wt->cached_bssid, bssid, ETH_ALEN);
	wt->cache_flags |= WONDERTAP_CACHE_BSSID_SET;
	mutex_unlock(&wt->lock);
	return 0;
}
