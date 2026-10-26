/* SPDX-License-Identifier: ISC */
#ifndef _WLAN_HDD_PASSTHRU_VDEV_H_
#define _WLAN_HDD_PASSTHRU_VDEV_H_

#include <linux/types.h>

struct wlan_hdd_link_info;
struct hdd_pt_vdev_record;

#ifdef DRIVER_PASSTHRU_OWNER
enum hdd_pt_vdev_step {
	HDD_PT_VDEV_CREATE_CALLED = 1U << 0,
	HDD_PT_VDEV_OBTAINED = 1U << 1,
	HDD_PT_VDEV_HDD_REF_ACQUIRED = 1U << 2,
	HDD_PT_VDEV_CREATE_RETURNED = 1U << 3,
	HDD_PT_VDEV_DELETE_CALLED = 1U << 4,
	HDD_PT_VDEV_HDD_REF_RELEASED = 1U << 5,
	HDD_PT_VDEV_SME_DELETE_CALLED = 1U << 6,
	HDD_PT_VDEV_DELETE_WAIT_STARTED = 1U << 7,
	HDD_PT_VDEV_DELETE_WAIT_COMPLETED = 1U << 8,
	HDD_PT_VDEV_DELETE_WAIT_TIMED_OUT = 1U << 9,
	HDD_PT_VDEV_DELETE_RETURNED = 1U << 10,
	HDD_PT_VDEV_SME_CREATE_CALLED = 1U << 11,
};

/* One zeroed record per physical iface; set generation before first create.
 * The caller serializes create/destroy/readers and keeps the record, adapter
 * and link alive. With DRIVER_PASSTHRU_MODE, generation must equal
 * adapter->passthru_generation; otherwise the caller owns generation binding.
 * link is an identity token, not an owned reference. No vdev pointer is saved.
 *
 * *_result holds the actual HDD return value once its RETURNED bit is set.
 * sme_delete_status is the raw QDF_STATUS, valid after DELETE_RETURNED if
 * SME_DELETE_CALLED is set. A successful SME call means submission only.
 * WAIT_COMPLETED only records the existing completion wait, not a DP join
 * or proof of hardware/FW deletion. Timeout is never promoted to completion.
 * Missing OBTAINED means HDD received no object; it does not certify that
 * lower-layer creation rollback finished after SME_CREATE_CALLED.
 * This journal does not authorize SSR cleanup or claim resources are gone.
 */
struct hdd_pt_vdev_record {
	u64 generation;
	const struct wlan_hdd_link_info *link;
	u32 steps;
	u8 vdev_id;
	int create_result;
	int delete_result;
	int sme_delete_status;
};

int hdd_pt_vdev_create(struct wlan_hdd_link_info *link,
		       struct hdd_pt_vdev_record *record);
int hdd_pt_vdev_destroy(struct wlan_hdd_link_info *link,
			struct hdd_pt_vdev_record *record);

/* Serialized snapshot only; caller still owns all lifetime/admission gates. */
static inline bool
hdd_pt_vdev_adoptable(const struct hdd_pt_vdev_record *record,
		      const struct wlan_hdd_link_info *link, u64 generation)
{
	const u32 required = HDD_PT_VDEV_CREATE_CALLED |
		HDD_PT_VDEV_SME_CREATE_CALLED |
		HDD_PT_VDEV_OBTAINED | HDD_PT_VDEV_HDD_REF_ACQUIRED |
		HDD_PT_VDEV_CREATE_RETURNED;

	return record && link && generation && record->link == link &&
		record->generation == generation &&
		(record->steps & required) == required &&
		!record->create_result &&
		!(record->steps & (HDD_PT_VDEV_DELETE_CALLED |
				  HDD_PT_VDEV_HDD_REF_RELEASED |
				  HDD_PT_VDEV_SME_DELETE_CALLED));
}
#endif
#endif
