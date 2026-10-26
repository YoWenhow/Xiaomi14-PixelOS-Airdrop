/* SPDX-License-Identifier: GPL-2.0-only */
/* Paired, single-device experimental lifecycle broker; no legacy priv extension. */
#include <linux/component.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include "cnss_wonder_lifecycle.h"

enum cw_provider_state { CW_PUBLISHING, CW_OPEN, CW_CLOSING, CW_RETIRED };
struct cnss_wonder_provider {
	refcount_t refs;
	struct device *device;
	const void *priv;
	struct module *owner;
	u64 generation;
	unsigned int active;
	enum cw_provider_state state;
	bool registered;
	wait_queue_head_t wait;
};
struct cnss_wonder_consumer {
	refcount_t refs;
	struct device *device;
	struct module *owner;
	const struct cnss_wonder_consumer_ops *ops;
	void *ctx;
	unsigned int active;
	bool closing;
	bool quiescing;
	bool done;
	bool exit_pending;
	struct task_struct *actor;
	u64 generation;
	enum cnss_wonder_retire_reason reason;
	wait_queue_head_t wait;
};
static DEFINE_SPINLOCK(cw_lock);
static DECLARE_WAIT_QUEUE_HEAD(cw_transition_wait);
static struct platform_device *cw_device;
static struct cnss_wonder_provider *cw_provider;
static struct cnss_wonder_consumer *cw_consumer;
static struct task_struct *cw_transition_owner;
static bool cw_transitioning;
static bool cw_device_removing;
static u64 cw_generation;

static void cw_provider_ref_put(struct cnss_wonder_provider *p)
{
	if (refcount_dec_and_test(&p->refs)) {
		put_device(p->device);
		kfree(p);
	}
}
static void cw_consumer_ref_put(struct cnss_wonder_consumer *c)
{
	if (refcount_dec_and_test(&c->refs)) {
		put_device(c->device);
		kfree(c);
	}
}
static bool cw_transition_idle(void)
{
	unsigned long flags;
	bool idle;
	spin_lock_irqsave(&cw_lock, flags);
	idle = !cw_transitioning;
	spin_unlock_irqrestore(&cw_lock, flags);
	return idle;
}
static int cw_begin_transition(void)
{
	unsigned long flags;
	for (;;) {
		spin_lock_irqsave(&cw_lock, flags);
		if (!cw_transitioning) {
			cw_transitioning = true;
			cw_transition_owner = current;
			spin_unlock_irqrestore(&cw_lock, flags);
			return 0;
		}
		if (cw_transition_owner == current) {
			spin_unlock_irqrestore(&cw_lock, flags);
			return -EDEADLK;
		}
		spin_unlock_irqrestore(&cw_lock, flags);
		wait_event(cw_transition_wait, cw_transition_idle());
	}
}
static void cw_end_transition(void)
{
	unsigned long flags;
	spin_lock_irqsave(&cw_lock, flags);
	cw_transition_owner = NULL;
	cw_transitioning = false;
	wake_up_all(&cw_transition_wait);
	spin_unlock_irqrestore(&cw_lock, flags);
}
static bool cw_provider_idle(struct cnss_wonder_provider *p)
{
	unsigned long flags;
	bool idle;
	spin_lock_irqsave(&cw_lock, flags);
	idle = !p->active;
	spin_unlock_irqrestore(&cw_lock, flags);
	return idle;
}
static bool cw_consumer_idle(struct cnss_wonder_consumer *c)
{
	unsigned long flags;
	bool idle;
	spin_lock_irqsave(&cw_lock, flags);
	idle = !c->active;
	spin_unlock_irqrestore(&cw_lock, flags);
	return idle;
}
static bool cw_consumer_done(struct cnss_wonder_consumer *c)
{
	unsigned long flags;
	bool done;
	spin_lock_irqsave(&cw_lock, flags);
	done = c->done;
	spin_unlock_irqrestore(&cw_lock, flags);
	return done;
}
/* Short lock held. First close determines the actor reason; a concurrent HDD
 * close still blocks all ordinary leases at the independent provider gate. */
static void cw_close_consumer(struct cnss_wonder_consumer *c, u64 generation,
			      enum cnss_wonder_retire_reason reason)
{
	if (!c->closing) {
		c->closing = true;
		c->generation = generation;
		c->reason = reason;
	}
}
/* Every caller owns an independent record reference throughout this function.
 * No transition token is acquired here. self is only the owning consumer's
 * detach call; that owner guarantees its code/ctx survive the whole call. */
static void cw_quiesce(struct cnss_wonder_consumer *c, u64 generation,
		       enum cnss_wonder_retire_reason reason, bool self)
{
	unsigned long flags;
	struct module *owner = NULL;
	void (*hook)(void *, u64, enum cnss_wonder_retire_reason);
	void *ctx;
	bool pinned = false;

	spin_lock_irqsave(&cw_lock, flags);
	cw_close_consumer(c, generation, reason);
	spin_unlock_irqrestore(&cw_lock, flags);
	wait_event(c->wait, cw_consumer_idle(c));
	spin_lock_irqsave(&cw_lock, flags);
	if (c->done) {
		spin_unlock_irqrestore(&cw_lock, flags);
		return;
	}
	if (c->quiescing)
		goto wait_actor;
	if (!self) {
		owner = c->owner;
		if (!try_module_get(owner)) {
			/* GOING is not an ack. Owner-side detach must perform cleanup. */
			c->exit_pending = true;
			goto wait_actor;
		}
		pinned = true;
	}
	c->quiescing = true;
	c->actor = current;
	hook = c->ops->quiesce;
	ctx = c->ctx;
	generation = c->generation;
	reason = c->reason;
	refcount_inc(&c->refs); /* actor's independent record lifetime */
	spin_unlock_irqrestore(&cw_lock, flags);
	hook(ctx, generation, reason);
	/* A regular consumer-remove lease must have returned before ack. */
	wait_event(c->wait, cw_consumer_idle(c));
	if (pinned)
		module_put(owner);
	/* Only broker code and owned record fields after the module put. */
	spin_lock_irqsave(&cw_lock, flags);
	c->actor = NULL;
	c->quiescing = false;
	c->exit_pending = false;
	c->done = true;
	wake_up_all(&c->wait);
	spin_unlock_irqrestore(&cw_lock, flags);
	cw_consumer_ref_put(c);
	return;
wait_actor:
	spin_unlock_irqrestore(&cw_lock, flags);
	wait_event(c->wait, cw_consumer_done(c));
}
static int cnss_vendor_wonder_comp_bind(struct device *comp,
				      struct device *master, void *data)
{
	dev_info(comp, "lifecycle Wonder bound to %s\n", dev_name(master));
	return 0;
}
static void cnss_vendor_wonder_comp_unbind(struct device *comp,
					 struct device *master, void *data)
{
	dev_info(comp, "lifecycle Wonder unbound\n");
}
static const struct component_ops cnss_wonder_component_ops = {
	.bind = cnss_vendor_wonder_comp_bind,
	.unbind = cnss_vendor_wonder_comp_unbind,
};
/* The caller holds the provider transition token, never component_mutex. */
static void cw_retire(struct cnss_wonder_provider *p,
		      enum cnss_wonder_retire_reason reason)
{
	unsigned long flags;
	struct cnss_wonder_consumer *c = NULL;
	bool registry_ref = false;
	spin_lock_irqsave(&cw_lock, flags);
	if (p->state == CW_RETIRED) {
		spin_unlock_irqrestore(&cw_lock, flags);
		return;
	}
	p->state = CW_CLOSING;
	if (cw_consumer && cw_consumer->device == p->device) {
		c = cw_consumer;
		refcount_inc(&c->refs);
		cw_close_consumer(c, p->generation, reason);
	}
	spin_unlock_irqrestore(&cw_lock, flags);
	wait_event(p->wait, cw_provider_idle(p));
	if (c)
		cw_quiesce(c, p->generation, reason, false);
	if (p->registered)
		component_del(p->device, &cnss_wonder_component_ops);
	spin_lock_irqsave(&cw_lock, flags);
	p->registered = false;
	p->priv = NULL;
	p->owner = NULL;
	p->state = CW_RETIRED;
	if (cw_provider == p) {
		cw_provider = NULL;
		registry_ref = true;
	}
	wake_up_all(&p->wait);
	spin_unlock_irqrestore(&cw_lock, flags);
	if (c)
		cw_consumer_ref_put(c);
	if (registry_ref)
		cw_provider_ref_put(p);
}
int cnss_wonder_provider_publish(const void *priv, struct module *owner,
				struct cnss_wonder_provider **out)
{
	struct cnss_wonder_provider *p;
	unsigned long flags;
	int ret;
	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!priv || !owner)
		return -EINVAL;
	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	refcount_set(&p->refs, 1);
	init_waitqueue_head(&p->wait);
	ret = cw_begin_transition();
	if (ret)
		goto free_unpublished;
	spin_lock_irqsave(&cw_lock, flags);
	if (!cw_device || cw_device_removing)
		ret = -EPROBE_DEFER;
	else if (cw_provider || (cw_consumer && cw_consumer->closing))
		ret = -EBUSY;
	else if (cw_generation == U64_MAX)
		ret = -EOVERFLOW;
	else {
		p->device = get_device(&cw_device->dev);
		p->priv = priv;
		p->owner = owner;
		p->generation = ++cw_generation;
		p->state = CW_PUBLISHING;
		refcount_inc(&p->refs); /* registry */
		cw_provider = p;
		ret = 0;
	}
	spin_unlock_irqrestore(&cw_lock, flags);
	if (ret) {
		cw_end_transition();
		goto free_unpublished;
	}
	/* No raw priv in platform drvdata: old consumers must not bypass leases. */
	ret = component_add(p->device, &cnss_wonder_component_ops);
	if (ret) {
		cw_retire(p, CNSS_WONDER_PROVIDER_LOST);
		cw_end_transition();
		cw_provider_ref_put(p);
		return ret;
	}
	spin_lock_irqsave(&cw_lock, flags);
	p->registered = true;
	p->state = CW_OPEN;
	spin_unlock_irqrestore(&cw_lock, flags);
	*out = p;
	cw_end_transition();
	return 0;
free_unpublished:
	kfree(p);
	return ret;
}
EXPORT_SYMBOL(cnss_wonder_provider_publish);
void cnss_wonder_provider_retire(struct cnss_wonder_provider *p,
				enum cnss_wonder_retire_reason reason)
{
	if (!p)
		return;
	/* Reentry is a contract violation, NOT a successful teardown barrier. */
	if (WARN_ON_ONCE(cw_begin_transition()))
		return;
	refcount_inc(&p->refs);
	cw_retire(p, reason);
	cw_end_transition();
	cw_provider_ref_put(p);
}
EXPORT_SYMBOL(cnss_wonder_provider_retire);
void cnss_wonder_provider_put(struct cnss_wonder_provider *p)
{
	unsigned long flags;
	bool retired;
	if (!p)
		return;
	spin_lock_irqsave(&cw_lock, flags);
	retired = p->state == CW_RETIRED;
	spin_unlock_irqrestore(&cw_lock, flags);
	if (WARN_ON_ONCE(!retired))
		return;
	cw_provider_ref_put(p);
}
EXPORT_SYMBOL(cnss_wonder_provider_put);
int cnss_wonder_consumer_attach(struct device *device, struct module *owner,
				const struct cnss_wonder_consumer_ops *ops,
				void *ctx, struct cnss_wonder_consumer **out)
{
	struct cnss_wonder_consumer *c;
	unsigned long flags;
	int ret;
	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!device || !owner || !ops || !ops->quiesce || !ctx)
		return -EINVAL;
	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	refcount_set(&c->refs, 1);
	init_waitqueue_head(&c->wait);
	spin_lock_irqsave(&cw_lock, flags);
	if (!cw_device || &cw_device->dev != device || cw_device_removing)
		ret = -EPROBE_DEFER;
	else if (cw_consumer)
		ret = -EBUSY;
	else if (cw_provider && cw_provider->state == CW_CLOSING)
		ret = -ESHUTDOWN;
	else {
		c->device = get_device(device);
		c->owner = owner;
		c->ops = ops;
		c->ctx = ctx;
		refcount_inc(&c->refs); /* registry until consumer_put, not detach */
		cw_consumer = c;
		*out = c;
		ret = 0;
	}
	spin_unlock_irqrestore(&cw_lock, flags);
	if (ret)
		kfree(c);
	return ret;
}
EXPORT_SYMBOL(cnss_wonder_consumer_attach);
void cnss_wonder_consumer_close(struct cnss_wonder_consumer *c)
{
	unsigned long flags;
	u64 generation;
	if (!c)
		return;
	spin_lock_irqsave(&cw_lock, flags);
	generation = cw_provider && cw_provider->device == c->device ?
		cw_provider->generation : 0;
	cw_close_consumer(c, generation, CNSS_WONDER_PROVIDER_LOST);
	spin_unlock_irqrestore(&cw_lock, flags);
}
EXPORT_SYMBOL(cnss_wonder_consumer_close);
void cnss_wonder_consumer_detach(struct cnss_wonder_consumer *c)
{
	unsigned long flags;
	u64 generation;
	if (!c)
		return;
	refcount_inc(&c->refs);
	spin_lock_irqsave(&cw_lock, flags);
	generation = cw_provider && cw_provider->device == c->device ?
		cw_provider->generation : 0;
	spin_unlock_irqrestore(&cw_lock, flags);
	cw_quiesce(c, generation, CNSS_WONDER_CONSUMER_REMOVE, true);
	cw_consumer_ref_put(c);
}
EXPORT_SYMBOL(cnss_wonder_consumer_detach);
void cnss_wonder_consumer_put(struct cnss_wonder_consumer *c)
{
	unsigned long flags;
	bool registry_ref = false;
	if (!c)
		return;
	spin_lock_irqsave(&cw_lock, flags);
	if (WARN_ON_ONCE(!c->done || c->active || c->quiescing)) {
		spin_unlock_irqrestore(&cw_lock, flags);
		return;
	}
	if (cw_consumer == c) {
		cw_consumer = NULL;
		registry_ref = true;
	}
	c->owner = NULL;
	c->ops = NULL;
	c->ctx = NULL;
	spin_unlock_irqrestore(&cw_lock, flags);
	if (registry_ref)
		cw_consumer_ref_put(c);
	cw_consumer_ref_put(c); /* caller ownership ends here */
}
EXPORT_SYMBOL(cnss_wonder_consumer_put);
static void cw_fill_lease(struct cnss_wonder_provider *p,
			  struct cnss_wonder_consumer *c,
			  struct cnss_wonder_lease *out)
{
	refcount_inc(&p->refs);
	p->active++;
	if (c) {
		refcount_inc(&c->refs);
		c->active++;
	}
	out->provider = p;
	out->consumer = c;
	out->owner = p->owner;
	out->priv = p->priv;
	out->generation = p->generation;
	out->held = true;
}
int cnss_wonder_snapshot_get(struct device *device, struct cnss_wonder_lease *out)
{
	unsigned long flags;
	struct cnss_wonder_provider *p;
	int ret;
	if (!out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	spin_lock_irqsave(&cw_lock, flags);
	p = cw_provider;
	if (!p || p->device != device)
		ret = -EPROBE_DEFER;
	else if (p->state != CW_PUBLISHING && p->state != CW_OPEN)
		ret = -ESHUTDOWN;
	else if (!try_module_get(p->owner))
		ret = -ENODEV;
	else {
		cw_fill_lease(p, NULL, out);
		ret = 0;
	}
	spin_unlock_irqrestore(&cw_lock, flags);
	return ret;
}
EXPORT_SYMBOL(cnss_wonder_snapshot_get);
int cnss_wonder_call_get(struct cnss_wonder_consumer *c, u64 generation,
			 struct cnss_wonder_lease *out)
{
	unsigned long flags;
	struct cnss_wonder_provider *p;
	int ret;
	if (!out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	spin_lock_irqsave(&cw_lock, flags);
	p = cw_provider;
	if (!c || cw_consumer != c || c->done)
		ret = -ENODEV;
	else if (!p || p->device != c->device || p->state != CW_OPEN)
		ret = -ESHUTDOWN;
	else if (p->generation != generation)
		ret = -ESTALE;
	else if (c->closing && !(c->quiescing && c->actor == current &&
		 c->reason == CNSS_WONDER_CONSUMER_REMOVE))
		ret = -ESHUTDOWN;
	else if (!try_module_get(p->owner))
		ret = -ENODEV;
	else {
		cw_fill_lease(p, c, out);
		ret = 0;
	}
	spin_unlock_irqrestore(&cw_lock, flags);
	return ret;
}
EXPORT_SYMBOL(cnss_wonder_call_get);
const void *cnss_wonder_lease_priv(const struct cnss_wonder_lease *lease)
{
	return lease && lease->held ? lease->priv : NULL;
}
EXPORT_SYMBOL(cnss_wonder_lease_priv);
u64 cnss_wonder_lease_generation(const struct cnss_wonder_lease *lease)
{
	return lease && lease->held ? lease->generation : 0;
}
EXPORT_SYMBOL(cnss_wonder_lease_generation);
void cnss_wonder_call_put(struct cnss_wonder_lease *lease)
{
	struct cnss_wonder_provider *p;
	struct cnss_wonder_consumer *c;
	struct module *owner;
	unsigned long flags;
	if (!lease || !lease->held)
		return;
	p = lease->provider;
	c = lease->consumer;
	owner = lease->owner;
	memset(lease, 0, sizeof(*lease));
	spin_lock_irqsave(&cw_lock, flags);
	p->active--;
	if (!p->active)
		wake_up_all(&p->wait);
	if (c) {
		c->active--;
		if (!c->active)
			wake_up_all(&c->wait);
	}
	spin_unlock_irqrestore(&cw_lock, flags);
	module_put(owner);
	/* Records remain owned through all counter/wake/module-put tails. */
	if (c)
		cw_consumer_ref_put(c);
	cw_provider_ref_put(p);
}
EXPORT_SYMBOL(cnss_wonder_call_put);
/* New profile must never permit a legacy setter to publish unleased ops. */
int cnss_set_vendor_wonder_priv_data(const void *priv)
{
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(cnss_set_vendor_wonder_priv_data);
static int cnss_vendor_wonder_dev_probe(struct platform_device *pdev)
{
	unsigned long flags;
	int ret;
	if (!pdev)
		return -EINVAL;
	ret = cw_begin_transition();
	if (ret)
		return ret;
	spin_lock_irqsave(&cw_lock, flags);
	if (cw_device)
		ret = cw_device == pdev ? 0 : -EEXIST;
	else {
		get_device(&pdev->dev);
		cw_device = pdev;
		ret = 0;
	}
	spin_unlock_irqrestore(&cw_lock, flags);
	cw_end_transition();
	return ret;
}
static int cnss_vendor_wonder_dev_remove(struct platform_device *pdev)
{
	struct cnss_wonder_provider *p = NULL;
	struct cnss_wonder_consumer *c = NULL;
	unsigned long flags;
	int ret = cw_begin_transition();
	if (ret)
		return ret;
	spin_lock_irqsave(&cw_lock, flags);
	if (!cw_device || cw_device != pdev) {
		ret = cw_device ? -ENODEV : 0;
		spin_unlock_irqrestore(&cw_lock, flags);
		goto out;
	}
	cw_device_removing = true;
	if (cw_provider) {
		p = cw_provider;
		refcount_inc(&p->refs);
	}
	if (cw_consumer) {
		c = cw_consumer;
		refcount_inc(&c->refs);
	}
	spin_unlock_irqrestore(&cw_lock, flags);
	if (p) {
		cw_retire(p, CNSS_WONDER_PROVIDER_LOST);
		cw_provider_ref_put(p);
	}
	if (c) {
		cw_quiesce(c, 0, CNSS_WONDER_PROVIDER_LOST, false);
		cw_consumer_ref_put(c);
	}
	spin_lock_irqsave(&cw_lock, flags);
	cw_device = NULL;
	cw_device_removing = false;
	spin_unlock_irqrestore(&cw_lock, flags);
	put_device(&pdev->dev);
out:
	cw_end_transition();
	return ret;
}
