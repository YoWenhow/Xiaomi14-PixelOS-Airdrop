// SPDX-License-Identifier: GPL-2.0
/*
 * Google Wonder WiFi Virtual Soft-MAC Driver
 *
 * This driver acts as a middleware layer using the mac80211 framework.
 * It provides a vendor-agnostic interface to userspace and
 * translates standard mac80211 calls into proprietary vendor driver functions.
 */
#define LOG_MODULE_NAME "mac80211"

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <net/genetlink.h>
#include <linux/slab.h>
#include <linux/compiler.h>
#include <linux/version.h>
#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/rtnetlink.h>

#include "core.h"
#include "mac80211.h"
#include "mac80211_txs.h"
#include "wonder_log.h"
#include "wondertap_internal.h"
#include "reg.h"
#include "nl80211_ven_cmd.h"
#include "ssr.h"

enum {
	WONDER_DATA_80211_RADIOTAP = 0,
	WONDER_DATA_80211,
	WONDER_DATA_8023,
	WONDER_DATA_MAX,
};

/* TXS/bands remain global; reject concurrent instances before allocation. */
static atomic_t wonder_instance_claimed = ATOMIC_INIT(0);

static inline void wonder_pdev_put(struct wonder_data *wonder)
{
	if (!wonder || !wonder->pdev)
		return;

	dev_put(wonder->pdev);
	wonder->pdev = NULL;
}

static inline int wonder_pdev_get(struct wonder_data *wonder, const char *pdev_name)
{
	struct net_device *pdev = dev_get_by_name(&init_net, pdev_name);
	if (!pdev) {
		wonder_error("Could not find physical device %s\n", pdev_name);
		return -ENODEV;
	}
	wonder->pdev = pdev;
	return 0;
}

/* local function implementation */
static bool wonder_80211_filter(struct ieee80211_hdr *hdr)
{
	if (!IS_ENABLED(CONFIG_WONDER_RX_FILTER_SUPPORT)) {
		return true;
	}
	if (ieee80211_is_ctl(hdr->frame_control)) {
		return false;
	}
	if (ieee80211_is_mgmt(hdr->frame_control) && !ieee80211_is_action(
		hdr->frame_control)) {
		return false;
	}
	return true;
}

static rx_handler_result_t wonder_rx_80211_frame(struct wonder_data *wonder, struct sk_buff *skb)
{
	struct ieee80211_hdr *hdr;
	struct ieee80211_radiotap_header *radhdr;
	struct net_device *vdev = NULL;

	if (!wonder || !wonder->vdev) {
		wonder_error("RX received but interface not active. Dropping.\n");
		goto drop;
	}

	vdev = wonder->vdev;
	/* --- Packet Filtering & Validation (mac80211/Wonder Driver responsibility) --- */
	if (skb->len < 24) { /* Basic check for 802.11 header length */
		wonder_error("Dropping short RX frame.\n");
		vdev->stats.rx_length_errors++;
		goto drop;
	}

	/* Filtering */
	radhdr = (struct ieee80211_radiotap_header *)skb->data;
	hdr = (struct ieee80211_hdr *)(skb->data + radhdr->it_len);
	if (IS_ENABLED(CONFIG_WONDER_RX_DEBUG)) {
		wonder_error("%s(): receiv packet from %s, send to mac80211, skb->protocol: %x,"
			"radhdr_len: %d\n", __func__, skb->dev->name, skb->protocol, radhdr->it_len);
		hexdump("wonder_rx_header: ", (u8 *)hdr, sizeof(struct ieee80211_hdr));
	}
	if (!wonder_80211_filter(hdr)) {
		vdev->stats.rx_dropped++;
		if (IS_ENABLED(CONFIG_WONDER_RX_DEBUG)) {
			wonder_error("Dropping frame with frame control: %x.\n", hdr->frame_control);
		}
		goto drop;
	}
	/* Populate necessary metadata for mac80211 */
	skb->dev = wonder->vdev;

	if (wonder->data_version == WONDER_DATA_80211 ||
		wonder->data_version == WONDER_DATA_80211_RADIOTAP) {
		dev_sw_netstats_rx_add(vdev, skb->len);
		vdev->stats.rx_packets++;
		vdev->stats.rx_bytes += skb->len;
	}
	/* Pass the raw 802.11 frame into the mac80211 processing pipeline. */
	/* mac80211 now handles de-AMSDU, 802.11 -> 802.3 conversion, and netif_rx(). */
	switch (wonder->data_version) {
	case WONDER_DATA_80211:
		ieee80211_rx_ni(wonder->hw, skb);
		break;
	case WONDER_DATA_80211_RADIOTAP:
		netif_receive_skb(skb);
		break;
	default:
		vdev->stats.rx_dropped++;
		wonder_error("Dropping Not supported data_version %d.\n", wonder->data_version);
		goto drop;
	}
	return RX_HANDLER_CONSUMED;
drop:
	/* Drop Frames */
	if (vdev) {
		dev_core_stats_rx_dropped_inc(vdev);
	}
	kfree_skb(skb);
	return RX_HANDLER_CONSUMED;
}

static rx_handler_result_t wonder_rx_8023_frame(struct wonder_data *wonder, struct sk_buff *skb)
{
	/* Change the skb's dev pointer to the virtual device */
	if (wonder->vdev)
		skb->dev = wonder->vdev;
	netif_rx(skb);

	return RX_HANDLER_CONSUMED;
}

static rx_handler_result_t wonder_rx_monitor_handler(struct wonder_data *wonder,
	struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;

	/*
	 *  It's expected the mac_header and protocol values will be filled in vendor's driver.
	 *	skb_reset_mac_header(skb);
	 *	skb->protocol = htons(ETH_P_802_2);
	 */
	/* LLC */
	if (skb->protocol == htons(ETH_P_802_2)) {
		return wonder_rx_80211_frame(wonder, skb);
	} else if (0 && skb->protocol == htons(ETH_P_802_3)) {
		return wonder_rx_8023_frame(wonder, skb);
	}
	return RX_HANDLER_PASS;
}

static bool wonder_80211_common_filter(struct wonder_data *wonder,
	struct ieee80211_hdr_3addr *hdr)
{
	struct net_device *vdev = wonder->vdev;
	unsigned int filters = wonder->config_filters;

	/* Receiving all packets owned by device. */
	if (memcmp(hdr->addr1, vdev->dev_addr, ETH_ALEN) == 0) {
		return true;
	}

	if (is_multicast_ether_addr(hdr->addr1)) {
		/* Receiving all BMC frames */
		if (filters & FIF_ALLMULTI)
			return true;
		/* Receiving my BSSID frames */
		if (wonder->vif) {
			struct ieee80211_bss_conf *bss_conf = &wonder->vif->bss_conf;
			if (memcmp(hdr->addr3, bss_conf->bssid, ETH_ALEN) == 0) {
				return true;
			}
		}
	}
	/* Receiving Beacon, and probe response frames. */
	if ((filters & FIF_BCN_PRBRESP_PROMISC) &&
		(ieee80211_is_probe_resp(hdr->frame_control) ||
		ieee80211_is_beacon(hdr->frame_control)))
		return true;

	/* Receiving control frames. */
	if ((filters & FIF_CONTROL) && ieee80211_is_ctl(hdr->frame_control))
		return true;
	/* Receiving probe request frames. */
	if ((filters & FIF_PROBE_REQ) && ieee80211_is_probe_req(hdr->frame_control))
		return true;
	/* Receiving action frames. */
	if ((filters & FIF_MCAST_ACTION) && ieee80211_is_action(hdr->frame_control))
		return true;
	return false;
}

static int wonder_fill_rx_status(struct ieee80211_radiotap_header *rth,
	struct ieee80211_rx_status *status)
{
	struct ieee80211_radiotap_iterator iterator;
	int ret;
	u16 rtap_len;

	/* Read the total length from the header (Radiotap fields are little-endian) */
	rtap_len = le16_to_cpu(rth->it_len);

	/* Initialize the radiotap iterator */
	ret = ieee80211_radiotap_iterator_init(&iterator,
								rth,
								rtap_len,
								NULL);
	if (ret) {
		wonder_warn("%s(): Invalid radiotap header (ret %d)\n", __func__, ret);
		return ret;
	}

	/*
	* The iterator automatically handles the 'it_present' bitmap,
	* field alignment, and field skipping.
	*/
	while (ieee80211_radiotap_iterator_next(&iterator) == 0) {

		/* Check 'iterator.this_arg_index' to identify the current field */
		switch (iterator.this_arg_index) {
		case IEEE80211_RADIOTAP_CHANNEL:
		{
			/*
			* Radiotap channel field contains 2 bytes freq (MHz)
			* and 2 bytes flags
			*/
			__le16 *chan_data = (__le16 *)iterator.this_arg;
			u16 freq = le16_to_cpu(chan_data[0]);
			u16 flags = le16_to_cpu(chan_data[1]);

			status->freq = freq;
			/* Infer band from radiotap channel flags */
			if (flags & IEEE80211_CHAN_2GHZ)
				status->band = NL80211_BAND_2GHZ;
			else if (flags & IEEE80211_CHAN_5GHZ)
				status->band = NL80211_BAND_5GHZ;
		}
			break;
		case IEEE80211_RADIOTAP_TSFT:
			/* TSF (MAC Timestamp) */
			/* Radiotap unit is microseconds */
			status->mactime = le64_to_cpu(*(__le64 *)iterator.this_arg);
			status->flag |= RX_FLAG_MACTIME_START; // Indicate mactime is valid
			break;
		case IEEE80211_RADIOTAP_FLAGS:
			/* Detection flags */
			if (*iterator.this_arg & IEEE80211_RADIOTAP_F_BADFCS)
				status->flag |= RX_FLAG_FAILED_FCS_CRC;

			if (*iterator.this_arg & IEEE80211_RADIOTAP_F_SHORTPRE)
				status->enc_flags |= RX_ENC_FLAG_SHORTPRE;
			break;
		case IEEE80211_RADIOTAP_DBM_ANTSIGNAL:
			/* Signal strength (dBm) */
			/* Radiotap stores as s8, rx_status also uses s8 */
			status->signal = (s8)*iterator.this_arg;
			break;
		case IEEE80211_RADIOTAP_ANTENNA:
			/* Antenna index (0-based) */
			status->antenna = *iterator.this_arg;
			break;
		case IEEE80211_RADIOTAP_MCS:
		{
			/* HT-MCS information */
			u8 *mcs_data = (u8 *)iterator.this_arg;
			// u8 known = mcs_data[0];
			u8 flags = mcs_data[1];
			u8 mcs_index = mcs_data[2];

			status->rate_idx = mcs_index;
			status->encoding = RX_ENC_HT; // Mark as HT frame

			/* Set bandwidth and GI from HT flags */
			if (flags & IEEE80211_RADIOTAP_MCS_SGI)
				status->enc_flags |= RX_ENC_FLAG_SHORT_GI;

			if (flags & IEEE80211_RADIOTAP_MCS_BW_40)
				status->bw = RATE_INFO_BW_40;
			else
				status->bw = RATE_INFO_BW_20; // Default 20
		}
			break;
		case IEEE80211_RADIOTAP_VHT:
		{
			u8 *vht_data;
			u16 known;
			u8 flags;
			u8 bw;
			u8 mcs_nss_0;

			/* Use a u8 pointer for byte-level access */
			vht_data = (u8 *)iterator.this_arg;

			/* Get 'known' field (u16, little-endian) at offset 0 */
			known = get_unaligned_le16(vht_data + 0);

			/* Get 'flags' field (u8) at offset 2 */
			flags = vht_data[2];

			/* Get 'bw' field (u8) at offset 3 */
			bw = vht_data[3];

			/* Get 'mcs_nss' for user 0 (u8) at offset 4 */
			mcs_nss_0 = vht_data[4];

			status->encoding = RX_ENC_VHT; // Mark as VHT frame

			/*
			* Parse MCS/NSS for User 0
			* The standard radiotap VHT layout is:
			* High 4 bits = MCS index (0-based)
			* Low 4 bits = NSS (1-based)
			*/
			status->rate_idx = (mcs_nss_0 >> 4);
			status->nss = (mcs_nss_0 & 0x0F);

			/* VHT SGI flag */
			if ((known & IEEE80211_RADIOTAP_VHT_KNOWN_GI) &&
				(flags & IEEE80211_RADIOTAP_VHT_FLAG_SGI)) {
				status->enc_flags |= RX_ENC_FLAG_SHORT_GI;
			}

			/* VHT Bandwidth */
			if (known & IEEE80211_RADIOTAP_VHT_KNOWN_BANDWIDTH) {
				switch (bw) {
				case 0:
					status->bw = RATE_INFO_BW_20;
					break;
				case 1:
					status->bw = RATE_INFO_BW_40;
					break;
				case 4:
					status->bw = RATE_INFO_BW_80;
					break;
				}
			}
		}
		break;
		/* TODO: Add more cases here */
		default:
			/* Ignore fields we don't care about */
			break;
		}
	}

	if (IS_ENABLED(CONFIG_WONDER_RX_DEBUG)) {
		wonder_debug("%s(): signal %d\n", __func__, status->signal);
		wonder_debug("%s(): antenna %d\n", __func__, status->antenna);
		wonder_debug("%s(): freq %d\n", __func__, status->freq);
		wonder_debug("%s(): band %d\n", __func__, status->band);
		wonder_debug("%s(): mactime %llu\n", __func__, status->mactime);
		wonder_debug("%s(): flag %d\n", __func__, status->flag);
		wonder_debug("%s(): encoding %d, enc_flags %d\n",
			__func__, status->encoding, status->enc_flags);
		wonder_debug("%s(): nss %d, rate %d, bw %d\n",
			__func__, status->nss, status->rate_idx, status->bw);
	}
	return 0;
}

static void syna_rx_handler(struct wonder_data *wonder, struct sk_buff *skb)
{
	struct ieee80211_hdr_3addr *hdr = (struct ieee80211_hdr_3addr *)(skb->data);
	char *mgmt_frame;
	char *qos;

	if (ieee80211_is_data(hdr->frame_control)) {
		/* TODO: Workaround to remove 4 byte tailer for syna in legacy data frame. */
		if (!ieee80211_is_data_qos(hdr->frame_control)) {
			skb->len -= 4;
		} else {
			/* TODO: Workaround to remove 2 byte tailer for syna in QoS AMSDU frame. */
			qos = ieee80211_get_qos_ctl((struct ieee80211_hdr *)hdr);
			if (qos[0] & IEEE80211_QOS_CTL_A_MSDU_PRESENT) {
				skb->len -= 2;
			}
		}
	}

	/*
	 * TODO: Workaround to handle extra 2 byte padding between header and payload for syna
	 * The possible frame type are management and Non QoS data frames.
	 */
	if (!ieee80211_is_data_qos(hdr->frame_control)) {
		mgmt_frame = skb->data + sizeof(struct ieee80211_hdr_3addr) + 2;
		skb->len -= 2;
		memcpy(skb->data + sizeof(struct ieee80211_hdr_3addr), mgmt_frame, skb->len);
	}
}

static rx_handler_result_t wonder_rx_adhoc_handler(struct wonder_data *wonder,
	struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;
	struct ieee80211_hdr_3addr *hdr;
	struct ieee80211_radiotap_header *radhdr;
	struct net_device *vdev;
	struct ieee80211_rx_status *rx_status = IEEE80211_SKB_RXCB(skb);

	if (!wonder || !wonder->vdev) {
		wonder_error("RX received but interface not active. Dropping.\n");
		goto drop;
	}

	vdev = wonder->vdev;
	/* --- Packet Filtering & Validation (mac80211/Wonder Driver responsibility) --- */
	if (skb->len < 24) { /* Basic check for 802.11 header length */
		wonder_error("Dropping short RX frame.\n");
		goto drop;
	}

	radhdr = (struct ieee80211_radiotap_header *)skb->data;
	/* Pull radotap since this frame is preparing forwarded to mac80211. */
	skb_pull(skb, le16_to_cpu(radhdr->it_len));
	hdr = (struct ieee80211_hdr_3addr *)(skb->data);

	/* Filtering */
	if (!wonder_80211_common_filter(wonder, hdr))
		goto drop;

	/*
		 * Need to change pkt_type since the default type from montor mode is
		 * PACKET_OTHERHOST.
		 */
	if (ieee80211_is_data(hdr->frame_control)) {
		skb->pkt_type = PACKET_HOST;
	}

	/* Vendor specific RX handler */
	if (IS_ENABLED(CONFIG_WONDER_SYNA_SUPPORT)) {
		syna_rx_handler(wonder, skb);
	}

	/* Fill RX status for mac80211 operation */
	memset(rx_status, 0, sizeof(*rx_status));
	wonder_fill_rx_status(radhdr, rx_status);

	/* Populate necessary metadata for mac80211 */
	skb->dev = vdev;

	if (IS_ENABLED(CONFIG_WONDER_RX_DEBUG)) {
		wonder_error("%s(): receiv packet from %s, send to mac80211, skb->protocol: %x,"
			"radhdr_len: %d\n", __func__, skb->dev->name, skb->protocol, radhdr->it_len);
		hexdump("wonder_rx_header: ", (u8 *)hdr, sizeof(struct ieee80211_hdr_3addr));
		hexdump("wonder_rx_frame: ", skb->data, skb->len);
	}
	ieee80211_rx_ni(wonder->hw, skb);
	return RX_HANDLER_CONSUMED;
drop:
	/* Drop Frames */
	kfree_skb(skb);
	return RX_HANDLER_CONSUMED;
}

/*
 * RX handler to process packets from the physical device
 */
static rx_handler_result_t wonder_rx_handler(struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;
	struct wonder_data *wonder = rcu_dereference(skb->dev->rx_handler_data);
	struct cnss_wonder_lease lease;
	rx_handler_result_t ret = RX_HANDLER_PASS;

	/* The network core owns skb/dev at entry. Closed admission leaves skb
	 * untouched for its caller, and reads no borrowed Wonder pdev/vdev.
	 */
	if (!wonder || READ_ONCE(wonder->quiescing) ||
	    wondertap_call_get(&wonder->wondertap_data, &lease))
		return RX_HANDLER_PASS;
	wonder_txs_dequeue(wonder->hw);
	switch (wonder->iftype) {
	case NL80211_IFTYPE_MONITOR:
		ret = wonder_rx_monitor_handler(wonder, pskb);
		break;
	case NL80211_IFTYPE_ADHOC:
		ret = wonder_rx_adhoc_handler(wonder, pskb);
		break;
	default:
		break;
	}
	cnss_wonder_call_put(&lease);
	return ret;
}

static int wonder_rx_setup(struct wonder_data *wonder)
{
	struct net_device *vdev;
	int ret;

	ASSERT_RTNL();
	if (!wonder->pdev)
		return -ENODEV;
	if (wonder->rx_registered)
		return -EALREADY;
	vdev = dev_get_by_name(&init_net, VDEV_NAME);
	if (!vdev)
		return -ENODEV;
	wonder->rx_vdev = vdev;
	wonder->vdev = vdev;
	ret = netdev_rx_handler_register(wonder->pdev, wonder_rx_handler, wonder);
	if (ret) {
		wonder->vdev = NULL;
		wonder->rx_vdev = NULL;
		dev_put(vdev);
		return ret;
	}
	wonder->rx_registered = true;
	return 0;
}

static void wonder_rx_reset(struct wonder_data *wonder)
{
	ASSERT_RTNL();
	if (wonder->rx_registered) {
		netdev_rx_handler_unregister(wonder->pdev);
		wonder->rx_registered = false;
	}
	/* unregister synchronizes RX readers before their instance/vdev changes. */
	wonder->vdev = NULL;
	if (wonder->rx_vdev) {
		dev_put(wonder->rx_vdev);
		wonder->rx_vdev = NULL;
	}
}

static void wonder_vendor_stop(struct wonder_data *wonder)
{
	int ret;

	ASSERT_RTNL();
	if (!wonder->vendor_started)
		return;
	if (READ_ONCE(wonder->provider_lost)) {
		wonder->vendor_started = false;
		return; /* HDD retirement owns backend cleanup. */
	}
	ret = wondertap_deinit(&wonder->wondertap_data);
	if (!ret)
		wonder->vendor_started = false;
	else
		wonder_error("Vendor deinit not completed: %d; residual ownership remains\n", ret);
	/* A non-actor worker may be denied during detach. Keep started set so
	 * the sole normal-remove actor can attempt its regular lease afterwards.
	 */
}

/* No pointer borrowed by TX may be read before this admission succeeds. */
static bool wonder_tx_enter(struct wonder_data *wonder)
{
	unsigned long flags;
	bool admitted;

	spin_lock_irqsave(&wonder->lifecycle_lock, flags);
	admitted = wonder->tx_open && !wonder->quiescing;
	if (admitted)
		wonder->tx_active++;
	spin_unlock_irqrestore(&wonder->lifecycle_lock, flags);
	return admitted;
}

static void wonder_tx_reject_provider(struct wonder_data *wonder)
{
	unsigned long flags;

	spin_lock_irqsave(&wonder->lifecycle_lock, flags);
	WRITE_ONCE(wonder->tx_open, false);
	spin_unlock_irqrestore(&wonder->lifecycle_lock, flags);
	/* A wake callback may hold an outer lease. Closing its loop hint here
	 * prevents unbounded dequeue after nested TX observes provider closure.
	 */
}

static void wonder_tx_leave(struct wonder_data *wonder)
{
	unsigned long flags;

	spin_lock_irqsave(&wonder->lifecycle_lock, flags);
	if (WARN_ON(!wonder->tx_active))
		goto out;
	wonder->tx_active--;
	/* Keep the last wake inside the lock observed by wonder_tx_idle(). */
	if (!wonder->tx_active)
		wake_up_all(&wonder->tx_wait);
out:
	spin_unlock_irqrestore(&wonder->lifecycle_lock, flags);
}

static bool wonder_tx_idle(struct wonder_data *wonder)
{
	unsigned long flags;
	bool idle;

	spin_lock_irqsave(&wonder->lifecycle_lock, flags);
	idle = !wonder->tx_active;
	spin_unlock_irqrestore(&wonder->lifecycle_lock, flags);
	return idle;
}

static void wonder_tx_close_and_wait(struct wonder_data *wonder)
{
	unsigned long flags;

	spin_lock_irqsave(&wonder->lifecycle_lock, flags);
	WRITE_ONCE(wonder->tx_open, false);
	spin_unlock_irqrestore(&wonder->lifecycle_lock, flags);
	/* No lifecycle/component lock here. TX must obey its atomic TX contract. */
	wait_event(wonder->tx_wait, wonder_tx_idle(wonder));
}

static int wonder_tx_open(struct wonder_data *wonder)
{
	unsigned long flags;
	int ret = 0;

	ASSERT_RTNL();
	spin_lock_irqsave(&wonder->lifecycle_lock, flags);
	if (wonder->quiescing)
		ret = -ESHUTDOWN;
	else if (wonder->tx_open || wonder->tx_active)
		ret = -EBUSY;
	else if (!wonder->pdev || !wonder->vdev)
		ret = -ENODEV;
	else
		WRITE_ONCE(wonder->tx_open, true);
	spin_unlock_irqrestore(&wonder->lifecycle_lock, flags);
	return ret;
}

#define WONDER_TX_ROOM (sizeof(struct ieee80211_radiotap_header) + 1 + sizeof(struct wonder_txd))
/* --- mac80211 Operation Implementations (The Core Middleware Logic) --- */
static void wonder_tx(struct ieee80211_hw *hw,
					  struct ieee80211_tx_control *control,
					  struct sk_buff *skb) {
	struct wonder_data *wonder = hw->priv;
	struct cnss_wonder_lease lease;
	struct net_device *pdev;
	struct net_device *vdev;
	struct ieee80211_radiotap_header *radhdr;
	struct ieee80211_hdr *hdr;
	struct wonder_txd *txd;
	unsigned int room;

	if (!wonder_tx_enter(wonder)) {
		/* Closed admission owns only skb, never a borrowed netdev/stat pointer. */
		dev_kfree_skb_any(skb);
		return;
	}
	if (wondertap_call_get(&wonder->wondertap_data, &lease)) {
		wonder_tx_reject_provider(wonder);
		dev_kfree_skb_any(skb);
		wonder_tx_leave(wonder);
		return;
	}
	pdev = wonder->pdev;
	vdev = wonder->vdev;
	room = skb_headroom(skb);
	if (unlikely(!pdev) || unlikely(!vdev)) {
		wonder_error("Physical device is not exist, dropping packet.\n");
		goto drop;
	}

	if (room < WONDER_TX_ROOM) {
		vdev->stats.tx_errors++;
		wonder_error("Not enough headroom, dropping packet.\n");
		goto drop;
	}

	if (IS_ENABLED(CONFIG_WONDER_TX_DEBUG)) {
		wonder_error("Forward packet to pdev %s, len %d\n", pdev->name, skb->len);
		hexdump("wonder_tx: ", skb->data, skb->len);
	}

	hdr = (struct ieee80211_hdr *)skb->data;
	/* The mac80211 probe request my using Broadcast BSSID correct it in here. */
	if (wonder->iftype == NL80211_IFTYPE_ADHOC && ieee80211_is_probe_req(hdr->frame_control)) {
		struct ieee80211_bss_conf *bss_conf = &wonder->vif->bss_conf;
		memcpy(hdr->addr3, bss_conf->bssid, ETH_ALEN);
	}

	/* Require head room for radiotap and wonder_txd */
	skb_push(skb, WONDER_TX_ROOM);
	txd = (struct wonder_txd *)skb->data;
	/* Assign wonder txd */
	txd->frame_type = le16_to_cpu(hdr->frame_control) & IEEE80211_FCTL_FTYPE;
	txd->is_unicast = !is_multicast_ether_addr(hdr->addr1);
	txd->tid = (ieee80211_is_data_qos(hdr->frame_control)) ? ieee80211_get_tid(hdr) : 0;
	skb_pull(skb, sizeof(struct wonder_txd));
	/* The monitor mode request non-zero length of radiotap. */
	radhdr = (struct ieee80211_radiotap_header *)skb->data;
	radhdr->it_version = 0;
	radhdr->it_pad = 0;
	radhdr->it_len = sizeof(struct ieee80211_radiotap_header) + 1;
	/* Assign the skb to the physical device for transmission */
	skb->dev = pdev;
	/* Report Fake TX status to adjust Rate and AMSDU length */
	if (1)
		wonder_txs_direct_report(hw, control->sta, skb);
	else
		wonder_txs_enqueue(control->sta, skb);

	if (unlikely(!netif_running(pdev) || !netif_device_present(pdev))) {
		vdev->stats.tx_dropped++;
		goto drop;
	} else {
		dev_sw_netstats_tx_add(vdev, 1, skb->len);
		vdev->stats.tx_packets++;
		vdev->stats.tx_bytes += skb->len;
		/* Call the physical device's transmit handler */
		dev_queue_xmit(skb);
	}
	wonder_tx_leave(wonder);
	cnss_wonder_call_put(&lease);
	return;
drop:
	if (vdev)
		dev_core_stats_tx_dropped_inc(vdev);
	dev_kfree_skb_any(skb);
	wonder_tx_leave(wonder);
	cnss_wonder_call_put(&lease);
}

static int wonder_start(struct ieee80211_hw *hw)
{
	struct wonder_data *wonder = hw->priv;
	struct cnss_wonder_lease lease;
	struct wondertap_init_params wondertap_init_params;
	struct wondertap_capability wondertap_capabilities;
	const char* pdev_name = physical_name;
	int ret;

	ASSERT_RTNL();
	if (READ_ONCE(wonder->quiescing))
		return -ESHUTDOWN;
	ret = wondertap_call_get(&wonder->wondertap_data, &lease);
	if (ret)
		return ret;
	/* This lease also covers pdev/RX/TX-open and state after vendor init. */
	/* This should turn on the hardware and frame reception. */
	wonder_info("HW started.\n");
	ret = wondertap_get_capabilities(&wonder->wondertap_data, &wondertap_capabilities);
	if (ret) {
		wonder_error("Failed to get wondertap capabilities, error: %d\n", ret);
		goto out;
	}

	wonder_info("wondertap version: %u\n", wondertap_capabilities.version);
	wonder_info("wondertap capabilities: 0x%X\n", wondertap_capabilities.raw_bits);
	ret = wondertap_init(&wonder->wondertap_data, &wondertap_init_params);
	if (ret) {
		wonder_error("Failed to initialize wondertap0, error: %d\n", ret);
		goto out;
	}
	wonder->vendor_started = true;
	/*
	 * The vendor-specific init() operation, called within wondertap_init(),
	 * may be responsible for creating the underlying physical network device.
	 * Therefore, we retrieve the device only after wondertap_init() has
	 * been called.
	 */
	ret = wonder_pdev_get(wonder, pdev_name);
	if (ret) {
		wonder_error("Failed to get physical device %s\n", pdev_name);
		goto err_vendor;
	}
	if (READ_ONCE(wonder->quiescing)) {
		ret = -ESHUTDOWN;
		goto err_pdev;
	}
	ret = wonder_rx_setup(wonder);
	if (ret)
		goto err_pdev;
	ret = wonder_tx_open(wonder);
	if (!ret)
		goto out;
	/* Admission never opened on failure; undo the newly owned RX reference. */
	wonder_rx_reset(wonder);
err_pdev:
	wonder_pdev_put(wonder);
err_vendor:
	wonder_vendor_stop(wonder);
out:
	cnss_wonder_call_put(&lease);
	return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
static void wonder_stop(struct ieee80211_hw *hw, bool suspended)
#else
static void wonder_stop(struct ieee80211_hw *hw)
#endif
{
	struct wonder_data *wonder = hw->priv;
	/* This should turn off the hardware. */
	wonder_info("HW stopped.\n");
	/* stop is under RTNL; counted TX/wake paths must not acquire RTNL. */
	wonder_tx_close_and_wait(wonder);
	wonder_rx_reset(wonder);
	wonder_pdev_put(wonder);
	wonder_vendor_stop(wonder);
}

static int wonder_config(struct ieee80211_hw *hw, u32 changed)
{
	/* Handle configuration changes (rate control, power, etc.) */
	wonder_debug(DRV_NAME ": HW configuration changed (0x%X).\n", changed);

	return 0;
}

static void wonder_configure_filter(struct ieee80211_hw *hw,
					unsigned int changed_flags,
					unsigned int *total_flags,
					u64 multicast)
{
	struct wonder_data *wonder = hw->priv;
	/* Configure the device's RX filter (e.g., monitor mode flags). */
	wonder_info("RX filter configured (changed=0x%X).\n", changed_flags);

	/* For a Soft-MAC driver, we often acknowledge all flags requested by mac80211. */
	/* We assume the vendor FMAC handles the actual low-level filtering. */
	wonder->config_filters = changed_flags;
	changed_flags |= (1 << 31);
	*total_flags &= ~changed_flags; /* Clear the flags we received */
}

static void wonder_handle_tx_queue(struct ieee80211_hw *hw,
								struct ieee80211_txq *txq)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0)
	ieee80211_handle_wake_tx_queue(hw, txq);
	return;
#else
	struct wonder_data *wonder = hw->priv;
	struct ieee80211_txq *queue = NULL;
	struct sk_buff *skb;
	struct ieee80211_tx_control control;

	ieee80211_txq_schedule_start(hw, txq->ac);
	while (READ_ONCE(wonder->tx_open) &&
	       (queue = ieee80211_next_txq(hw, txq->ac))) {
		memset(&control, 0, sizeof(control));
		control.sta = queue->sta;
		while (READ_ONCE(wonder->tx_open)) {
			skb = ieee80211_tx_dequeue(hw, queue);
			if (!skb)
				break;

			wonder_tx(hw, &control, skb);
		}
		ieee80211_return_txq(hw, queue, false);
	}
	ieee80211_txq_schedule_end(hw, txq->ac);
	return;
#endif /* LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 0) */
}

static void wonder_wake_tx_queue(struct ieee80211_hw *hw,
							struct ieee80211_txq *txq)
{
	struct wonder_data *wonder = hw->priv;
	struct cnss_wonder_lease lease;
	unsigned long frame_count = 0;
	unsigned long byte_count = 0;

	/* Count the whole queue callback, including gaps between individual TXs. */
	if (!wonder_tx_enter(wonder))
		return;
	if (wondertap_call_get(&wonder->wondertap_data, &lease)) {
		wonder_tx_reject_provider(wonder);
		wonder_tx_leave(wonder);
		return;
	}
	/* Called when mac80211 is ready to transmit frames on a previously stopped queue. */
	if (IS_ENABLED(CONFIG_WONDER_TX_DEBUG))
		wonder_info("TX queue woken up.\n");

	/* Get tx_queue length */
	ieee80211_txq_get_depth(txq, &frame_count, &byte_count);
	/* frame_count and byte_count */
	if (IS_ENABLED(CONFIG_WONDER_TX_DEBUG)) {
		wonder_info("Waking up TXQ for AC %d, mac80211 has %lu frames (%lu bytes) pending\n",
			txq->ac, frame_count, byte_count);
	}
	/* Airtime fairness support. */
	if (READ_ONCE(wonder->tx_open) && !wonder->tx_stop)
		wonder_handle_tx_queue(hw, txq);
	wonder_tx_leave(wonder);
	cnss_wonder_call_put(&lease);
}

static void wonder_channel_switch(struct ieee80211_hw *hw,
						struct ieee80211_vif *vif,
						struct ieee80211_channel_switch *ch_switch)
{
}

/* --- NAN Operation Implementations (Mandatory for NL80211_IFTYPE_NAN support) --- */

static int wonder_start_nan(struct ieee80211_hw *hw,
						struct ieee80211_vif *vif,
						struct cfg80211_nan_conf *conf)
{
	wonder_info("START NAN operation on VIF (Type: %d).\n", vif->type);
	/* In a real driver, this would configure the hardware to start the NAN cluster. */
	return 0;
}

static int wonder_stop_nan(struct ieee80211_hw *hw,
						struct ieee80211_vif *vif)
{
	wonder_info("STOP NAN operation on VIF (Type: %d).\n", vif->type);
	/* In a real driver, this would configure the hardware to stop the NAN cluster. */
	return 0;
}

static int wonder_add_nan_func(struct ieee80211_hw *hw,
						struct ieee80211_vif *vif,
						const struct cfg80211_nan_func *nan_func)
{
	wonder_info("ADD NAN function (VIF: %d, Instance ID: %u).\n",
			vif->type, nan_func->instance_id);
	/* In a real driver, this registers a NAN service function with the hardware/firmware. */
	return 0;
}

static void wonder_del_nan_func(struct ieee80211_hw *hw,
					struct ieee80211_vif *vif,
					u8 instance_id)
{
	wonder_info("DELETE NAN function (VIF: %d, Instance ID: %u).\n",
			vif->type, instance_id);
	/* In a real driver, this would remove the registered NAN service function. */
	return;
}

static int wonder_force_set_mac(struct wonder_data *wonder, struct ieee80211_vif *vif)
{
	struct net_device *pdev = wonder->pdev;
	struct net_device *vdev = wonder->vdev;

	if (!pdev || !vdev) {
		return -ENODEV;
	}

	eth_hw_addr_set(vdev, (void *)pdev->dev_addr);
	memcpy(vif->addr, (void *)pdev->dev_addr, ETH_ALEN);
	ether_addr_copy(vif->bss_conf.addr, vif->addr);
	wonder_info("Set physical mac address %pM to virtual interface %s\n",
			 pdev->dev_addr, vdev->name);
	return 0;
}

static int wonder_add_interface(struct ieee80211_hw *hw,
						struct ieee80211_vif *vif)
{
	struct wonder_data *wonder = hw->priv;
	struct wireless_dev *wdev;
	struct net_device *vdev;

	if (wonder->vif) {
		/*
		 * Allow STATION mode to be added even if other modes are present,
		 * assuming the underlying hardware supports concurrent STA/P2P/NAN operations.
		 */
		if (vif->type != NL80211_IFTYPE_STATION) {
			wonder_error("Only one virtual interface other than STATION is supported.\n");
			return -EOPNOTSUPP;
		}
	}

	/* Accept supported interface types */
	if (vif->type != NL80211_IFTYPE_MONITOR &&
		vif->type != NL80211_IFTYPE_ADHOC &&
		vif->type != NL80211_IFTYPE_STATION &&
		vif->type != NL80211_IFTYPE_NAN) {
		wonder_error("Interface type %d not supported.\n", vif->type);
		return -EOPNOTSUPP;
	}
	/* Populate necessary metadata for mac80211 */
	wdev = ieee80211_vif_to_wdev(vif);
	vdev = wdev->netdev;
	wonder->vif = vif;
	wonder->iftype = wdev->iftype;
	wonder->vdev = vdev;
	wonder_info("Added virtual interface %s (Type: %d), name %s\n",
			wiphy_name(hw->wiphy), vif->type, vdev->name);
	/* Configure mac address to phyiscal interface address */
	return wonder_force_set_mac(wonder, vif);
}

static void wonder_remove_interface(struct ieee80211_hw *hw,
							struct ieee80211_vif *vif)
{
	struct wonder_data *wonder = hw->priv;

	/* Clean up interface data */
	if (wonder->vif == vif) {
		wonder->vif = NULL;
		wonder->iftype = NL80211_IFTYPE_MONITOR;
	}
	wonder_info("Removed virtual interface.\n");
}

static bool wonder_amsdu_sanity(struct ieee80211_hw *hw,
					     struct sk_buff *head,
					     struct sk_buff *skb)
{
	wonder_info("TX AMSDU sanity check.\n");
	return true;
}

static int wonder_ampdu_action(struct ieee80211_hw *hw,
			    struct ieee80211_vif *vif,
			    struct ieee80211_ampdu_params *params)
{
	wonder_info("%s().\n", __func__);
	return 0;
}

static void wonder_flush(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		      u32 queues, bool drop)
{
	wonder_info("%s().\n", __func__);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
static void wonder_flush_sta(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			  struct ieee80211_sta *sta)
{
	wonder_info("%s().\n", __func__);
}
#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0) */

static int wonder_sta_add(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			struct ieee80211_sta *sta)
{
	wonder_info("%s().\n", __func__);
	return 0;
}

static int wonder_sta_remove(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			struct ieee80211_sta *sta)
{
	wonder_info("%s().\n", __func__);
	return 0;
}

static void wonder_sta_notify(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		enum sta_notify_cmd cmd, struct ieee80211_sta *sta)
{
	wonder_info("%s().\n", __func__);
	return;
}

static int wonder_sta_set_txpwr(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif,
				struct ieee80211_sta *sta)
{
	wonder_info("%s().\n", __func__);
	return 0;
}

static void wonder_sta_pre_rcu_remove(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif,
				struct ieee80211_sta *sta)
{
	wonder_info("%s().\n", __func__);
}

static void wonder_sta_rate_tbl_update(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif,
				struct ieee80211_sta *sta)
{
	struct ieee80211_sta_rates *sta_rates = rcu_dereference(sta->rates);
	int i;

	if (!sta_rates)
		return;

	for (i = 0; i < ARRAY_SIZE(sta_rates->rate); i++) {
		if (sta_rates->rate[i].idx < 0 || !sta_rates->rate[i].count)
			break;
	}
}

static void wonder_sta_statistics(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif,
				struct ieee80211_sta *sta,
				struct station_info *sinfo)
{
	wonder_info("%s().\n", __func__);
}

static int wonder_tx_last_beacon(struct ieee80211_hw *hw)
{
	struct wonder_data *wonder = hw->priv;

	return wonder->iftype == NL80211_IFTYPE_ADHOC;
}

static const struct ieee80211_ops wonder_mac80211_ops = {
	.tx                 = wonder_tx,
	.start              = wonder_start,
	.stop               = wonder_stop,
	.config             = wonder_config,
	.add_interface      = wonder_add_interface,
	.remove_interface   = wonder_remove_interface,
	.configure_filter   = wonder_configure_filter,
	.wake_tx_queue      = wonder_wake_tx_queue,
	.channel_switch     = wonder_channel_switch,
	/* --- Mandatory NAN Hooks --- */
	.start_nan          = wonder_start_nan,
	.stop_nan           = wonder_stop_nan,
	.add_nan_func       = wonder_add_nan_func,
	.del_nan_func       = wonder_del_nan_func,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
	/* --- Mandatory Channel Context Hooks (emulated path) --- */
	.add_chanctx        = ieee80211_emulate_add_chanctx,
	.remove_chanctx     = ieee80211_emulate_remove_chanctx,
	.change_chanctx     = ieee80211_emulate_change_chanctx,
#endif
	/* --- AMSDU Support -- */
	.can_aggregate_in_amsdu = wonder_amsdu_sanity,
	/* --- AMPDU Support --- */
	.ampdu_action = wonder_ampdu_action,
	/* --- Station Support --- */
	.sta_add = wonder_sta_add,
	.sta_remove = wonder_sta_remove,
	.sta_notify = wonder_sta_notify,
	.sta_set_txpwr = wonder_sta_set_txpwr,
	.sta_pre_rcu_remove = wonder_sta_pre_rcu_remove,
	.sta_rate_tbl_update = wonder_sta_rate_tbl_update,
	.sta_statistics = wonder_sta_statistics,
	/* --- TXQ Support --- */
	.flush = wonder_flush,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	.flush_sta = wonder_flush_sta,
#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0) */
	/* -- ADHOC Support -- */
	.tx_last_beacon = wonder_tx_last_beacon,
};

int wonder_features_init(struct wonder_data *wonder)
{
	int ret;

	wonder_set_custom_regulator(wonder->hw);
	ret = ieee80211_register_hw(wonder->hw);
	if (ret)
		return ret;
	wonder->hw_registered = true;
	wonder_get_regulator_domain(wonder->hw);
	return 0;
}

void wonder_features_exit(struct wonder_data *wonder)
{
	/* Worker and broker actor can both reach here; never take this mutex
	 * before cancel_work_sync or from a component callback / RTNL owner.
	 */
	mutex_lock(&wonder->quiesce_lock);
	wonder_tx_close_and_wait(wonder);
	rtnl_lock();
	wonder_rx_reset(wonder);
	rtnl_unlock();
	if (wonder->hw_registered) {
		ieee80211_unregister_hw(wonder->hw);
		wonder->hw_registered = false;
	}
	/* Also cover partially started/no-stop framework paths. */
	rtnl_lock();
	wonder_pdev_put(wonder);
	wonder_vendor_stop(wonder);
	rtnl_unlock();
	mutex_unlock(&wonder->quiesce_lock);
}

char *physical_name = PDEV_NAME;
extern struct ieee80211_supported_band wonder_band_2ghz;
extern struct ieee80211_supported_band wonder_band_5ghz;
struct wonder_data *wonder_mac80211_init(void)
{
	struct ieee80211_hw *hw;
	struct wonder_data *wonder = NULL;
	u8 random_mac_addr[ETH_ALEN];
	int ret;

	if (atomic_cmpxchg(&wonder_instance_claimed, 0, 1))
		return ERR_PTR(-EBUSY);
	/* Allocate the mac80211 hardware structure */
	hw = ieee80211_alloc_hw_nm(sizeof(*wonder), &wonder_mac80211_ops, DRV_NAME);
	if (!hw) {
		wonder_error("Failed to allocate mac80211 hardware.\n");
		ret = -ENOMEM;
		goto release_instance;
	}

	/* Initialize private data */
	wonder = hw->priv;
	wonder->hw = hw;
	mutex_init(&wonder->quiesce_lock);
	spin_lock_init(&wonder->lifecycle_lock);
	init_waitqueue_head(&wonder->tx_wait);
	wonder->tx_active = 0;
	wonder->tx_open = false;
	wondertap_prep(&wonder->wondertap_data);
	wonder->pdev = NULL;
	wonder->data_version = WONDER_DATA_80211_RADIOTAP;
	wonder->iftype = NL80211_IFTYPE_MONITOR;
	wonder->config_filters = 0;
	wonder->tx_stop = false;
	/* Set Band Capabilities */
	hw->wiphy->bands[NL80211_BAND_2GHZ] = &wonder_band_2ghz;
	hw->wiphy->bands[NL80211_BAND_5GHZ] = &wonder_band_5ghz;

	/* Set Vendor Command Capabilities for cfg80211 */
	hw->wiphy->vendor_commands = wonder_get_wiphy_vendor_command();
	hw->wiphy->n_vendor_commands = wonder_get_wiphy_vendor_command_array_size();

	/* To support WMM (QoS), the queues must larger than 4 */
	hw->queues = 4;

	/* Set Hardware Capabilities */
	ieee80211_hw_set(hw, SUPPORTS_PS);
	ieee80211_hw_set(hw, SINGLE_SCAN_ON_ALL_BANDS);
	ieee80211_hw_set(hw, SIGNAL_DBM);
	/* Support AMSDU */
	ieee80211_hw_set(hw, TX_AMSDU);
	ieee80211_hw_set(hw, SUPPORT_FAST_XMIT);
	/*
	 * NO_AUTO_VIF is set, so the kernel won't create a default interface.
	 * Interfaces must now be created manually.
	 */
	ieee80211_hw_set(hw, NO_AUTO_VIF);

	eth_random_addr(random_mac_addr);
	SET_IEEE80211_PERM_ADDR(hw, random_mac_addr);
	wonder_info("Set MAC addrs from wlan0: %pM\n", random_mac_addr);

	hw->extra_tx_headroom = 0; /* The frame is fully formed by mac80211 */

	/* Tell mac80211 which interface types we support by setting the bits in the wiphy structure. */
	hw->wiphy->interface_modes |=
		BIT(NL80211_IFTYPE_ADHOC) |
		BIT(NL80211_IFTYPE_MONITOR) |
		BIT(NL80211_IFTYPE_NAN) |           /* Added NAN mode support */
		BIT(NL80211_IFTYPE_STATION);        /* Added STATION mode support */

	/* Set the band that supports NAN, mandatory if NL80211_IFTYPE_NAN is supported */
	hw->wiphy->nan_supported_bands |= BIT(NL80211_BAND_2GHZ);
	/* Support BW80 for IBSS Mode */
	wiphy_ext_feature_set(hw->wiphy, NL80211_EXT_FEATURE_VHT_IBSS);
	/* Prepare private resources before register_hw exposes callbacks. */
	ret = wonder_txs_queue_init();
	if (ret)
		goto free_hw;
	ret = wonder_ssr_init(wonder);
	if (ret)
		goto free_txs;
	ret = wonder_features_init(wonder);
	if (ret)
		goto stop_notifier;
	return wonder;
stop_notifier:
	wonder_ssr_exit(wonder);
free_txs:
	wonder_txs_queue_exit();
free_hw:
	ieee80211_free_hw(hw);
release_instance:
	atomic_set_release(&wonder_instance_claimed, 0);
	return ERR_PTR(ret);
}

void wonder_mac80211_quiesce(struct wonder_data *wonder)
{
	/* No instance/component/RTNL lock may be held across these waits. */
	wonder_ssr_exit(wonder);
	wonder_features_exit(wonder);
}

void wonder_mac80211_free(struct wonder_data *wonder)
{
	if (WARN_ON(wonder->hw_registered || wonder->notifier_registered ||
		    wonder->rx_registered))
		return;
	wonder_txs_queue_exit();
	ieee80211_free_hw(wonder->hw);
	atomic_set_release(&wonder_instance_claimed, 0);
}

void wonder_mac80211_exit(struct wonder_data *wonder)
{
	wonder_mac80211_quiesce(wonder);
	wonder_mac80211_free(wonder);
}
