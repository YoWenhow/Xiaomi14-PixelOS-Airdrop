/* SPDX-License-Identifier: ISC */
#ifndef _WLAN_HDD_PASSTHRU_EXPERIMENT_H_
#define _WLAN_HDD_PASSTHRU_EXPERIMENT_H_
#include <linux/types.h>
#ifdef DRIVER_PASSTHRU_RUNTIME
struct hdd_context;
bool hdd_pt_exclusive_mode(void);
bool hdd_pt_framework_netdev_mode(void);
bool hdd_pt_framework_netdev_ready(struct hdd_context *hdd);
#else
struct hdd_context;
static inline bool hdd_pt_exclusive_mode(void) { return false; }
static inline bool hdd_pt_framework_netdev_mode(void) { return false; }
static inline bool hdd_pt_framework_netdev_ready(struct hdd_context *hdd) { return false; }
#endif
#endif
