/* SPDX-License-Identifier: GPL-2.0-only */
/* Paired experimental CNSS/Wonder lifecycle API v1. Legacy priv stays unchanged. */
#ifndef CNSS_WONDER_LIFECYCLE_H
#define CNSS_WONDER_LIFECYCLE_H
#include <linux/types.h>
struct device;
struct module;
struct cnss_wonder_provider;
struct cnss_wonder_consumer;
struct cnss_wonder_lease {
	struct cnss_wonder_provider *provider;
	struct cnss_wonder_consumer *consumer;
	struct module *owner;
	const void *priv;
	u64 generation;
	bool held;
};
enum cnss_wonder_retire_reason {
	CNSS_WONDER_HDD_UNLOAD,
	CNSS_WONDER_HDD_REMOVE,
	CNSS_WONDER_HDD_SSR,
	CNSS_WONDER_PROVIDER_LOST,
	CNSS_WONDER_CONSUMER_REMOVE,
};
struct cnss_wonder_consumer_ops {
	/* Exactly one actor, synchronous and infallible local cleanup, no broker
	 * control reentry. No broker/component/RTNL lock is held across this call.
	 * CONSUMER_REMOVE alone may try normal leases for vendor deinit. Other
	 * reasons prohibit vendor calls; firmware/backend cleanup remains HDD's.
	 */
	void (*quiesce)(void *ctx, u64 generation,
			enum cnss_wonder_retire_reason reason);
};
/* Process context outside component callbacks, RTNL, DSC and cleanup locks.
 * A CNSS Wonder platform device must already be probed; otherwise -EPROBE_DEFER.
 * The current implementation supports one device/publication/endpoint only.
 * The owner must be the actual QCA module, not CNSS device->driver->owner.
 */
int cnss_wonder_provider_publish(const void *priv, struct module *owner,
				struct cnss_wonder_provider **out);
/* No allocation, timeout or supported early failure. Caller serializes its
 * handle with publish/put and obeys the process-context/lock contract above.
 * Retires the borrowed owner/priv, not the caller-owned opaque record.
 */
void cnss_wonder_provider_retire(struct cnss_wonder_provider *provider,
				enum cnss_wonder_retire_reason reason);
void cnss_wonder_provider_put(struct cnss_wonder_provider *provider);
/* Attach never calls the hook. Run after instance allocation, before master_add,
 * outside component callbacks. Endpoint is single-use; no automatic SSR rearm.
 */
int cnss_wonder_consumer_attach(struct device *provider_device,
				struct module *consumer_owner,
				const struct cnss_wonder_consumer_ops *ops,
				void *ctx, struct cnss_wonder_consumer **out);
/* Nonblocking gate close for unexpected unbind/notifier paths. Does not quiesce,
 * wait, acknowledge cleanup or free anything. Later detach remains mandatory.
 */
void cnss_wonder_consumer_close(struct cnss_wonder_consumer *consumer);
/* Barrier only: gate close + sole actor local cleanup + ack. Independent of the
 * provider transition token, and never called with RTNL/component/local cleanup
 * locks. If a broker hook cannot pin a GOING owner, this owner-side detach runs
 * its own hook and acknowledges it. Caller keeps module/ctx alive through return.
 * The record and its hook/ctx remain valid until master_del/callbacks finish.
 */
void cnss_wonder_consumer_detach(struct cnss_wonder_consumer *consumer);
/* Only AFTER detach and component_master_del / all framework callbacks return.
 * No concurrent operation may start through the caller's pointer after put.
 */
void cnss_wonder_consumer_put(struct cnss_wonder_consumer *consumer);
/* Nonblocking inspection, also allowed while PUBLISHING; never invoke ops with
 * this lease. Consume metadata inside the lease, retain generation only.
 */
int cnss_wonder_snapshot_get(struct device *provider_device,
			      struct cnss_wonder_lease *out);
/* Nonblocking regular lease. Always requires OPEN provider, matching generation
 * and successful short QCA module pin. A CLOSING consumer admits only its exact
 * current CONSUMER_REMOVE actor, for ordinary deinit under the same provider
 * gate. All other threads/reasons reject; there is no closed-provider bypass.
 */
int cnss_wonder_call_get(struct cnss_wonder_consumer *consumer,
			 u64 expected_generation, struct cnss_wonder_lease *out);
const void *cnss_wonder_lease_priv(const struct cnss_wonder_lease *lease);
u64 cnss_wonder_lease_generation(const struct cnss_wonder_lease *lease);
void cnss_wonder_call_put(struct cnss_wonder_lease *lease);
/* Transfer ONE already-held live owner pin on success. On failure the caller
 * retains the pin. prepare must synchronously retire every callback/work source;
 * return zero only after it may be safely unloaded. The broker drops the pin
 * after prepare returns in broker text. Single-use per bridge instance.
 */
int cnss_wonder_pin_adopt(struct module *owner, int (*prepare)(void *),
                         void *context);
#endif
