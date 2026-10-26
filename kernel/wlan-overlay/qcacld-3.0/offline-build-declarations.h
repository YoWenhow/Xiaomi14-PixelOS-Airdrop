/* Build adaptation: declaration matching the existing original function.
 * Origin: components/umac/mlme/connection_mgr/core/src/wlan_cm_vdev_connect.c:1729.
 * This header is included only for that translation unit. No implementation.
 */
#ifdef WLAN_CHIPSET_STATS
struct wlan_objmgr_vdev;
struct wlan_cm_connect_resp;
void cm_cp_stats_cstats_log_connect_event(struct wlan_objmgr_vdev *vdev,
                                        struct wlan_cm_connect_resp *rsp);
#endif
