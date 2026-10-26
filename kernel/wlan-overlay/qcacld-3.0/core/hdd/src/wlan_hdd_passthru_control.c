/* SPDX-License-Identifier: ISC */
#include "wlan_hdd_passthru_control.h"
#include <linux/etherdevice.h>
#include "wlan_hdd_main.h"
#include "wlan_hdd_object_manager.h"
#include "wlan_hdd_assoc.h"
#include "wlan_hdd_passthru_iface.h"
#include "wlan_reg_services_api.h"
#include "wlan_reg_ucfg_api.h"
#include "wlan_scan_ucfg_api.h"
#include "wlan_vdev_mgr_api.h"
#include "wlan_vdev_mlme.h"
#include "wlan_vdev_mgr_tgt_if_tx_defs.h"
#include "cdp_txrx_peer_ops.h"
#include "sme_api.h"
#include "wma.h"

static int hdd_pt_control_channel(struct hdd_context *hdd, u32 freq)
{
	if (!WLAN_REG_IS_24GHZ_CH_FREQ(freq) &&
	    !WLAN_REG_IS_5GHZ_CH_FREQ(freq))
		return -EOPNOTSUPP;
	/* Channel 14 permits legacy DSSS, not this HT-only profile. */
	if (freq == 2484)
		return -EOPNOTSUPP;
	if (wlan_reg_get_channel_state_for_pwrmode(hdd->pdev, freq,
			REG_CURRENT_PWR_MODE) != CHANNEL_STATE_ENABLE ||
	    wlan_reg_is_dfs_for_freq(hdd->pdev, freq) ||
	    wlan_reg_is_passive_for_freq(hdd->pdev, freq))
		return -EPERM;
	return 0;
}

int hdd_pt_control_validate(struct hdd_context *hdd,
			   const struct wondertap_init_params *params)
{
	u8 country[3] = {0};
	QDF_STATUS status;

	if (!hdd || !hdd->pdev || !hdd->psoc || !params)
		return -EINVAL;
	if (!is_valid_ether_addr(params->mac_addr) ||
	    !is_valid_ether_addr(params->bssid) ||
	    params->country_code[2] || !params->country_code[0] ||
	    !params->country_code[1] || params->reserved1 ||
	    params->reserved2 || params->reserved3 ||
	    params->tx_rate.reserved[0] || params->tx_rate.reserved[1])
		return -EINVAL;
	if (params->channel.bandwidth != WONDERTAP_RATE_BW_20 ||
	    params->tx_rate.bw != WONDERTAP_RATE_BW_20 ||
	    params->tx_rate.preamble != WONDERTAP_RATE_PREAMBLE_HT ||
	    params->tx_rate.nss != 1 || params->tx_rate.mcs > 7 ||
	    (params->tx_rate.gi != WONDERTAP_RATE_GI_DEFAULT &&
	     params->tx_rate.gi != WONDERTAP_RATE_GI_SHORT) ||
	    params->amsdu_enable || params->ampdu_enable ||
	    params->mgmt_retry_limit || params->data_retry_limit)
		return -EOPNOTSUPP;
	status = ucfg_reg_get_current_country(hdd->psoc, country);
	if (QDF_IS_STATUS_ERROR(status))
		return qdf_status_to_os_return(status);
	if (memcmp(country, params->country_code, 2))
		return -EOPNOTSUPP;
	if (ucfg_scan_get_pdev_status(hdd->pdev) != SCAN_NOT_IN_PROGRESS)
		return -EBUSY;
	return hdd_pt_control_channel(hdd, params->channel.freq);
}

static int hdd_pt_control_state(struct hdd_pt_iface *iface,
				const struct wondertap_init_params *params,
				bool changing)
{
	struct hdd_pt_status state;
	int ret;

	if (!iface || !iface->adapter || !iface->adapter->deflink ||
	    !iface->registry || !iface->vdev_created ||
	    !iface->owner_started || iface->owner_reaped)
		return -EINVAL;
	ret = hdd_pt_control_validate(iface->hdd, params);
	if (ret)
		return ret;
	if (iface->adapter->device_mode != QDF_PASSTHRU_MODE ||
	    WLAN_HDD_GET_CTX(iface->adapter) != iface->hdd ||
	    !ether_addr_equal(iface->adapter->mac_addr.bytes,
			      params->mac_addr) ||
	    (!changing && iface->frequency != params->channel.freq))
		return -EINVAL;
	ret = hdd_pt_status(iface->registry, iface->generation, &state);
	if (ret)
		return ret;
	if (state.error)
		return state.error;
	return state.phase == (changing ? HDD_PT_ACTIVE : HDD_PT_BOUND) ?
		0 : -EAGAIN;
}

static int hdd_pt_control_submit(struct hdd_pt_iface *iface,
		const struct wondertap_init_params *params, u32 cookie, bool changing)
{
	struct channel_change_req request = {0};
	struct ch_params channel = { .ch_width = CH_WIDTH_20MHZ };
	struct wlan_objmgr_vdev *vdev;
	struct vdev_mlme_obj *mlme;
	struct hdd_adapter *adapter;
	QDF_STATUS status;
	int ret;

	ret = hdd_pt_control_state(iface, params, changing);
	if (ret)
		return ret;
	adapter = iface->adapter;
	ret = hdd_validate_channel_and_bandwidth(adapter, params->channel.freq,
						 CH_WIDTH_20MHZ);
	if (ret)
		return ret;
	wlan_reg_set_channel_params_for_pwrmode(iface->hdd->pdev,
			params->channel.freq, 0, &channel, REG_CURRENT_PWR_MODE);
	if (channel.ch_width != CH_WIDTH_20MHZ)
		return -EOPNOTSUPP;

	vdev = hdd_objmgr_get_vdev_by_user(adapter->deflink, WLAN_OSIF_ID);
	if (!vdev)
		return -ENODEV;
	if (!changing) {
		status = wlan_vdev_mgr_set_param_bssid(vdev, params->bssid);
		if (QDF_IS_STATUS_ERROR(status))
			goto put;
	}
	mlme = wlan_vdev_mlme_get_cmpt_obj(vdev);
	if (!mlme) {
		status = QDF_STATUS_E_INVAL;
		goto put;
	}
	wlan_vdev_obj_lock(vdev);
	status = wlan_pt_txn_prepare(&mlme->passthru_txn, cookie,
				     params->channel.freq);
	wlan_vdev_obj_unlock(vdev);
	if (QDF_IS_STATUS_ERROR(status))
		goto put;
	request.passthru_cookie = cookie;

	request.vdev_id = adapter->deflink->vdev_id;
	request.target_chan_freq = params->channel.freq;
	request.ch_width = channel.ch_width;
	request.sec_ch_offset = channel.sec_ch_offset;
	request.center_freq_seg0 = channel.center_freq_seg0;
	request.center_freq_seg1 = channel.center_freq_seg1;
	sme_fill_channel_change_request(iface->hdd->mac_handle, &request,
				       eCSR_DOT11_MODE_11n);
	/* SME fills from global PHY policy; this PT profile is fixed HT. */
	request.dot11mode = MLME_DOT11_MODE_11N;
	status = sme_send_channel_change_req(iface->hdd->mac_handle, &request);
	if (QDF_IS_STATUS_ERROR(status)) {
		wlan_vdev_obj_lock(vdev);
		wlan_pt_txn_seal(&mlme->passthru_txn);
		wlan_vdev_obj_unlock(vdev);
	}
put:
	hdd_objmgr_put_vdev_by_user(vdev, WLAN_OSIF_ID);
	return qdf_status_to_os_return(status);
}

int hdd_pt_control_submit_start(struct hdd_pt_iface *iface,
                               const struct wondertap_init_params *params,
                               u32 cookie)
{
	return hdd_pt_control_submit(iface, params, cookie, false);
}

int hdd_pt_control_submit_channel(struct hdd_pt_iface *iface,
                                 const struct wondertap_init_params *params,
                                 u32 cookie)
{
	return hdd_pt_control_submit(iface, params, cookie, true);
}

/* No configuration writes: callers may retry PENDING until their deadline. */
int hdd_pt_control_check_start(struct hdd_pt_iface *iface, u32 cookie)
{
	struct wlan_objmgr_vdev *vdev;
	struct vdev_mlme_obj *mlme;
	QDF_STATUS status = QDF_STATUS_E_FAILURE;

	if (!iface || !iface->adapter || !iface->adapter->deflink ||
	    !iface->vdev_created || iface->owner_reaped)
		return -EINVAL;
	vdev = hdd_objmgr_get_vdev_by_user(iface->adapter->deflink, WLAN_OSIF_ID);
	if (!vdev)
		return -ENODEV;
	mlme = wlan_vdev_mlme_get_cmpt_obj(vdev);
	if (mlme) {
		wlan_vdev_obj_lock(vdev);
		status = wlan_pt_txn_completion(&mlme->passthru_txn, cookie);
		wlan_vdev_obj_unlock(vdev);
	}
	hdd_objmgr_put_vdev_by_user(vdev, WLAN_OSIF_ID);
	return status == QDF_STATUS_E_PENDING ? -EINPROGRESS :
		qdf_status_to_os_return(status);
}

/* Serialized runtime control only; no waits or firmware commands. */
void hdd_pt_control_abort(struct hdd_pt_iface *iface)
{
	struct wlan_objmgr_vdev *vdev;
	struct vdev_mlme_obj *mlme;

	if (!iface || !iface->adapter || !iface->adapter->deflink ||
	    !iface->vdev_created || iface->owner_reaped)
		return;
	vdev = hdd_objmgr_get_vdev_by_user(iface->adapter->deflink, WLAN_OSIF_ID);
	if (!vdev)
		return;
	mlme = wlan_vdev_mlme_get_cmpt_obj(vdev);
	if (mlme) {
		wlan_vdev_obj_lock(vdev);
		wlan_pt_txn_seal(&mlme->passthru_txn);
		wlan_vdev_obj_unlock(vdev);
	}
	hdd_objmgr_put_vdev_by_user(vdev, WLAN_OSIF_ID);
}

static int hdd_pt_control_vdev_param(tp_wma_handle wma, u8 vdev_id,
				    u32 id, u32 value)
{
	struct vdev_set_params param = {
		.vdev_id = vdev_id,
		.param_id = id,
		.param_value = value,
	};

	/* Unlike wma_cli_set_command, return the actual WMI send result. */
	return qdf_status_to_os_return(
		wmi_unified_vdev_set_param_send(wma->wmi_handle, &param));
}

/* Same TLV as wma_set_sw_retry_by_qos. Its public wrapper skips zero,
 * so explicitly send zero for both aggregation classes on every AC.
 * This controls firmware SOFTWARE retries; it is not proof that hardware
 * retries are disabled. Do not advertise a hardware-retry capability.
 */
static int hdd_pt_control_no_sw_retry(tp_wma_handle wma, u8 vdev_id)
{
	wmi_vdev_set_custom_sw_retry_th_cmd_fixed_param *cmd;
	wmi_buf_t buf;
	u32 type, ac;
	int ret;

	for (type = 0; type < WMI_VDEV_CUSTOM_SW_RETRY_TYPE_MAX; type++) {
		for (ac = 0; ac < WMI_AC_MAX; ac++) {
			buf = wmi_buf_alloc(wma->wmi_handle, sizeof(*cmd));
			if (!buf)
				return -ENOMEM;
			cmd = (void *)wmi_buf_data(buf);
			qdf_mem_zero(cmd, sizeof(*cmd));
			WMITLV_SET_HDR(&cmd->tlv_header,
				WMITLV_TAG_STRUC_wmi_vdev_set_custom_sw_retry_th_cmd_fixed_param,
				WMITLV_GET_STRUCT_TLVLEN(
					wmi_vdev_set_custom_sw_retry_th_cmd_fixed_param));
			cmd->vdev_id = vdev_id;
			cmd->ac_type = ac;
			cmd->sw_retry_type = type;
			cmd->sw_retry_th = 0;
			ret = wmi_unified_cmd_send(wma->wmi_handle, buf,
						  sizeof(*cmd),
					WMI_VDEV_SET_CUSTOM_SW_RETRY_TH_CMDID);
			if (ret) {
				wmi_buf_free(buf);
				return qdf_status_to_os_return(ret);
			}
		}
	}
	return 0;
}

/* The documented maximum is one MPDU/MSDU. Leave RX BA limits untouched.
 * This prevents combining TX packets; firmware application has no ACK here.
 */
static int hdd_pt_control_single_tx(tp_wma_handle wma, u8 vdev_id)
{
	wmi_vdev_set_custom_aggr_size_cmd_fixed_param *cmd;
	wmi_buf_t buf;
	u32 type;
	int ret;

	for (type = 0; type < WMI_VDEV_CUSTOM_AGGR_TYPE_MAX; type++) {
		buf = wmi_buf_alloc(wma->wmi_handle, sizeof(*cmd));
		if (!buf)
			return -ENOMEM;
		cmd = (void *)wmi_buf_data(buf);
		qdf_mem_zero(cmd, sizeof(*cmd));
		WMITLV_SET_HDR(&cmd->tlv_header,
			WMITLV_TAG_STRUC_wmi_vdev_set_custom_aggr_size_cmd_fixed_param,
			WMITLV_GET_STRUCT_TLVLEN(
				wmi_vdev_set_custom_aggr_size_cmd_fixed_param));
		cmd->vdev_id = vdev_id;
		cmd->tx_aggr_size = 1;
		WMI_VDEV_CUSTOM_AGGR_TYPE_SET(cmd->enable_bitmap, type);
		WMI_VDEV_CUSTOM_RX_AGGR_SZ_DIS_SET(cmd->enable_bitmap, 1);
		ret = wmi_unified_cmd_send(wma->wmi_handle, buf, sizeof(*cmd),
					  WMI_VDEV_SET_CUSTOM_AGGR_SIZE_CMDID);
		if (ret) {
			wmi_buf_free(buf);
			return qdf_status_to_os_return(ret);
		}
	}
	return 0;
}

int hdd_pt_control_finish_start(struct hdd_pt_iface *iface,
			       const struct wondertap_init_params *params)
{
	struct ol_txrx_desc_type peer = {0};
	struct hdd_adapter *adapter;
	tp_wma_handle wma;
	void *soc;
	QDF_STATUS status;
	u8 vdev_id;
	int ret, rate;

	ret = hdd_pt_control_state(iface, params, false);
	if (ret)
		return ret;
	adapter = iface->adapter;
	vdev_id = adapter->deflink->vdev_id;
	wma = cds_get_context(QDF_MODULE_ID_WMA);
	soc = cds_get_context(QDF_MODULE_ID_SOC);
	if (!wma || !wma->wmi_handle || !soc)
		return -ENODEV;
	rate = hdd_assemble_rate_code(WMI_RATE_PREAMBLE_HT, 0,
				    params->tx_rate.mcs);
	if (rate < 0)
		return rate;
	ret = hdd_pt_control_vdev_param(wma, vdev_id,
					wmi_vdev_param_fixed_rate, rate);
	if (ret)
		return ret;
	ret = hdd_pt_control_vdev_param(wma, vdev_id, wmi_vdev_param_sgi,
			params->tx_rate.gi == WONDERTAP_RATE_GI_SHORT);
	if (ret)
		return ret;
	ret = hdd_pt_control_vdev_param(wma, vdev_id,
					wmi_vdev_param_chwidth, 0);
	if (ret)
		return ret;
	ret = hdd_pt_control_no_sw_retry(wma, vdev_id);
	if (ret)
		return ret;
	ret = hdd_pt_control_single_tx(wma, vdev_id);
	if (ret)
		return ret;

	qdf_mem_copy(peer.peer_addr.bytes, adapter->mac_addr.bytes,
		     QDF_MAC_ADDR_SIZE);
	peer.is_qos_enabled = 1;
	peer.bw = hdd_convert_ch_width_to_cdp_peer_bw(CH_WIDTH_20MHZ);
	status = cdp_peer_register(soc, OL_TXRX_PDEV_ID, &peer);
	if (QDF_IS_STATUS_ERROR(status))
		return qdf_status_to_os_return(status);
	status = hdd_change_peer_state(adapter->deflink,
				      adapter->mac_addr.bytes,
				      OL_TXRX_PEER_STATE_AUTH);
	return qdf_status_to_os_return(status);
}
