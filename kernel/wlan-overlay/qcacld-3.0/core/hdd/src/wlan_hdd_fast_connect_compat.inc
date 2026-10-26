/*
 * Independent, offline FAST_CONNECT compatibility implementation.
 * Reconstructed from the original ROM contract and public driver interfaces;
 * this is not recovered OEM source. See HDD-COMPAT-zh.txt for evidence/changes.
 * Included once by wlan_hdd_cfg80211.c before its vendor command array.
 */
#ifdef WLAN_FEATURE_FAST_CONNECT
#include "parser_api.h"
#include "dot11f_fast_connect_compat.h"

static_assert(QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_MAX == 5);

static const struct nla_policy
wlan_hdd_scan_entry_policy[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_MAX + 1] = {
	[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_FREQUENCIES] = { .type = NLA_U32 },
	[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_BANDWIDTH] = { .type = NLA_U32 },
	[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_SSID] = {
		.type = NLA_BINARY, .len = WLAN_SSID_MAX_LEN
	},
	[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_BSSID] =
		NLA_POLICY_EXACT_LEN(QDF_MAC_ADDR_SIZE),
	[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_IES] = {
		.type = NLA_BINARY, .len = 2304
	},
};

static int
__wlan_hdd_cfg80211_vendor_add_scan_entry(struct wiphy *wiphy,
					struct wireless_dev *wdev,
					const void *data, int data_len)
{
	struct hdd_context *hdd_ctx = wiphy_priv(wiphy);
	struct mac_context *mac = MAC_CONTEXT(hdd_ctx->mac_handle);
	struct nlattr *tb[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_MAX + 1];
	struct scan_cache_entry params = {0};
	struct ch_params ch_params = {0};
	tDot11ffast_connect frame = {0};
	struct mlme_vht_capabilities_info *vht_cap_info;
	QDF_STATUS status;
	uint32_t bandwidth, packed_len = 0, dot11f_status, total_len;
	uint8_t *ies;
	int ret;

	/* ROM +0x12c..0x17c: FTM precedes context validation. */
	hdd_enter_dev(wdev->netdev);
	if (hdd_get_conparam() == QDF_GLOBAL_FTM_MODE) {
		hdd_err("Command not allowed in FTM mode");
		return -EPERM;
	}
	ret = wlan_hdd_validate_context(hdd_ctx);
	if (ret == -EAGAIN)
		return -EBUSY;
	if (ret)
		return ret;

	/* ROM +0x18c/+0x198: maximum 5 and NL_VALIDATE_STRICT. */
	if (wlan_cfg80211_nla_parse(tb, QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_MAX,
				    data, data_len, wlan_hdd_scan_entry_policy)) {
		hdd_err("invalid data");
		status = QDF_STATUS_E_INVAL;
		goto out;
	}
	if (!tb[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_FREQUENCIES] ||
	    nla_len(tb[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_FREQUENCIES]) != 4) {
		hdd_err("invalid freq attr");
		status = QDF_STATUS_E_INVAL;
		goto out;
	}
	params.channel.chan_freq =
		nla_get_u32(tb[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_FREQUENCIES]);
	if (!tb[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_BANDWIDTH] ||
	    nla_len(tb[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_BANDWIDTH]) != 4) {
		hdd_err("invalid bandwidth attr");
		status = QDF_STATUS_E_INVAL;
		goto out;
	}
	bandwidth = nla_get_u32(tb[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_BANDWIDTH]);
	/* ROM +0x1f0..0x238 uses enum values 1/2/3, not 0/1/2. */
	switch (bandwidth) {
	case 0:
		ch_params.ch_width = CH_WIDTH_40MHZ;
		break;
	case 1:
		ch_params.ch_width = CH_WIDTH_80MHZ;
		break;
	case 2:
		ch_params.ch_width = CH_WIDTH_160MHZ;
		break;
	default:
		ch_params.ch_width =
			wlan_reg_is_24ghz_ch_freq(params.channel.chan_freq) ?
			CH_WIDTH_40MHZ : CH_WIDTH_80MHZ;
		break;
	}
	wlan_reg_set_channel_params_for_pwrmode(hdd_ctx->pdev,
		params.channel.chan_freq, 0, &ch_params, REG_CURRENT_PWR_MODE);

	if (!tb[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_BSSID] ||
	    nla_len(tb[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_BSSID]) < QDF_MAC_ADDR_SIZE) {
		hdd_err("invalid bssid attr");
		status = QDF_STATUS_E_INVAL;
		goto out;
	}
	qdf_mem_copy(&params.bssid,
		     nla_data(tb[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_BSSID]),
		     QDF_MAC_ADDR_SIZE);
	qdf_mem_copy(&params.mac_addr,
		     nla_data(tb[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_BSSID]),
		     QDF_MAC_ADDR_SIZE);
	if (!tb[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_SSID] ||
	    nla_len(tb[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_SSID]) < 1 ||
	    nla_len(tb[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_SSID]) > WLAN_SSID_MAX_LEN) {
		hdd_err("invalid ssid attr");
		status = QDF_STATUS_E_INVAL;
		goto out;
	}
	params.ssid.length = nla_len(tb[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_SSID]);
	qdf_mem_copy(params.ssid.ssid,
		     nla_data(tb[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_SSID]),
		     nla_len(tb[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_SSID]));
	if (!tb[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_IES] ||
	    nla_len(tb[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_IES]) < 2) {
		hdd_err("invalid ie attr");
		status = QDF_STATUS_E_INVAL;
		goto out;
	}

	/* ROM +0x390..0x3d8: constant channel 15, NULL session, ignored status. */
	populate_dot11f_supp_rates(mac, 15, &frame.SuppRates, NULL);
	populate_dot11f_ht_caps(mac, NULL, &frame.HTCaps);
	populate_dot11f_vht_caps(mac, NULL, &frame.VHTCaps);
	populate_dot11f_he_caps(mac, NULL, &frame.he_cap);
	if (ch_params.ch_width != CH_WIDTH_20MHZ) {
		frame.HTCaps.supportedChannelWidthSet = 1;
		frame.HTCaps.shortGI40MHz = 1;
	}
	frame.DSParams.present = 1;
	frame.DSParams.curr_channel =
		wlan_reg_freq_to_chan(hdd_ctx->pdev, params.channel.chan_freq);
	frame.HTInfo.present = 1;
	frame.HTInfo.primaryChannel =
		wlan_reg_freq_to_chan(hdd_ctx->pdev, params.channel.chan_freq);
	/* ROM sets this bit even when regulatory selection returns 20 MHz. */
	frame.HTInfo.recommendedTxWidthSet = 1;
	if (ch_params.ch_width != CH_WIDTH_20MHZ)
		frame.HTInfo.secondaryChannelOffset =
			params.channel.chan_freq < ch_params.mhz_freq_seg0 ? 1 : 3;
	frame.VHTOperation.present = 1;
	if (ch_params.ch_width > CH_WIDTH_40MHZ) {
		frame.VHTOperation.chanWidth = 1;
		frame.VHTOperation.chan_center_freq_seg0 = ch_params.center_freq_seg0;
		if (ch_params.ch_width == CH_WIDTH_160MHZ ||
		    ch_params.ch_width == CH_WIDTH_80P80MHZ)
			frame.VHTOperation.chan_center_freq_seg1 = ch_params.center_freq_seg1;
		else
			frame.VHTOperation.chan_center_freq_seg1 = 0;
	} else {
		frame.VHTOperation.chanWidth = 0;
		frame.VHTOperation.chan_center_freq_seg0 = 0;
		frame.VHTOperation.chan_center_freq_seg1 = 0;
	}
	/* ROM +0x4b0..0x4c4; exact field offsets verified by layout-probe.c. */
	vht_cap_info = &mac->mlme_cfg->vht_caps.vht_cap_info;
	frame.VHTOperation.basicMCSSet =
		(vht_cap_info->basic_mcs_set & 0xfffffff3) |
		(vht_cap_info->rx_mcs2x2 << 2);
	/* frame.SSID is deliberately left zero, as in the ROM. */
	dot11f_status = dot11f_get_packed_fast_connect_size(mac, &frame, &packed_len);
	if (DOT11F_FAILED(dot11f_status)) {
		hdd_err("Failed to calculate the packed size for a Fast Connect (0x%08x)",
			dot11f_status);
		packed_len = 256;
	} else if (DOT11F_WARNED(dot11f_status)) {
		hdd_warn("There were warnings while packing a Fast Connect (0x%08x)",
			 dot11f_status);
	}
	/* ROM passes allocation/estimated length, not the eventual consumed length. */
	total_len = nla_len(tb[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_IES]) + packed_len;
	ies = qdf_mem_malloc_atomic(total_len);
	if (!ies) {
		hdd_err("alloc ie failed");
		status = QDF_STATUS_E_NOMEM;
		goto out;
	}
	qdf_mem_copy(ies, nla_data(tb[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_IES]),
		     nla_len(tb[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_IES]));
	dot11f_status = dot11f_pack_fast_connect(mac, &frame,
		ies + nla_len(tb[QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_IES]),
		packed_len, &packed_len);
	if (DOT11F_FAILED(dot11f_status)) {
		/* Original log names Probe Request even for FAST_CONNECT. */
		hdd_err("Failed to pack a Probe Request (0x%08x)", dot11f_status);
		qdf_mem_free(ies);
		status = QDF_STATUS_E_FAILURE;
		goto out;
	} else if (DOT11F_WARNED(dot11f_status)) {
		hdd_warn("There were warnings while packing a Probe Request (0x%08x)",
			 dot11f_status);
	}
	hdd_debug("Add Scan Entry IEs: ");
	QDF_TRACE_HEX_DUMP(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_DEBUG, ies, total_len);
	status = ucfg_scan_add_result(hdd_ctx->pdev, &params, ies, total_len);
	qdf_mem_free(ies);
out:
	hdd_exit();
	return qdf_status_to_os_return(status);
}

static int
wlan_hdd_cfg80211_vendor_add_scan_entry(struct wiphy *wiphy,
				      struct wireless_dev *wdev,
				      const void *data, int data_len)
{
	struct osif_psoc_sync *psoc_sync;
	int ret;

	ret = osif_psoc_sync_op_start(wiphy_dev(wiphy), &psoc_sync);
	if (ret)
		return ret;
	ret = __wlan_hdd_cfg80211_vendor_add_scan_entry(wiphy, wdev, data, data_len);
	osif_psoc_sync_op_stop(psoc_sync);
	return ret;
}

/* Deliberate correction: the ROM registers 13 against a 0..5 policy. */
#define HDD_FAST_CONNECT_COMPAT_VENDOR_COMMAND \
	{ \
		.info.vendor_id = QCA_NL80211_VENDOR_ID, \
		.info.subcmd = QCA_NL80211_VENDOR_SUBCMD_ADD_SCAN_ENTRY, \
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | \
			 WIPHY_VENDOR_CMD_NEED_NETDEV | WIPHY_VENDOR_CMD_NEED_RUNNING, \
		.doit = wlan_hdd_cfg80211_vendor_add_scan_entry, \
		vendor_command_policy(wlan_hdd_scan_entry_policy, \
			QCA_WLAN_VENDOR_ATTR_SCAN_ENTRY_MAX) \
	},
#else
#define HDD_FAST_CONNECT_COMPAT_VENDOR_COMMAND
#endif /* WLAN_FEATURE_FAST_CONNECT */
