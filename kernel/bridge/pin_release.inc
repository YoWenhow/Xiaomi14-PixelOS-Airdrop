/* SPDX-License-Identifier: GPL-2.0-only */
/* The broker owns exactly one pin transferred by the live provider. */
static DEFINE_MUTEX(cw_release_lock);
static struct module *cw_release_owner;
static int (*cw_release_prepare)(void *);
static void *cw_release_context;
static bool cw_release_done;
static bool cw_release_running;
static int cw_release_result;

int cnss_wonder_pin_adopt(struct module *owner, int (*prepare)(void *),
			 void *context)
{
	int ret = 0;

	if (!owner || !prepare || !context ||
	    READ_ONCE(owner->state) != MODULE_STATE_LIVE)
		return -EINVAL;
	mutex_lock(&cw_release_lock);
	if (cw_release_owner || cw_release_done)
		ret = -EBUSY;
	else {
		cw_release_owner = owner;
		cw_release_prepare = prepare;
		cw_release_context = context;
	}
	mutex_unlock(&cw_release_lock);
	return ret;
}
EXPORT_SYMBOL(cnss_wonder_pin_adopt);

/* Runs in broker text, without a provider lease or any RTNL/DSC transition.
 * Dropping either pin while prepare() is executing would be unsafe.
 * A failed prepare retains the original pin and callback for a retry.
 */
static int cw_release_set(const char *value, const struct kernel_param *kp)
{
	struct module *owner;
	bool release;
	int ret;

	ret = kstrtobool(value, &release);
	if (ret || !release)
		return ret ? ret : -EPERM;
	mutex_lock(&cw_release_lock);
	owner = cw_release_owner;
	if (!owner) {
		ret = cw_release_done ? 0 : -ENODEV;
		goto out;
	}
	if (!try_module_get(owner)) {
		ret = -ENODEV;
		goto out;
	}
	cw_release_running = true;
	ret = cw_release_prepare(cw_release_context);
	cw_release_running = false;
	if (!ret) {
		cw_release_owner = NULL;
		cw_release_prepare = NULL;
		cw_release_context = NULL;
		cw_release_done = true;
		module_put(owner); /* transferred permanent pin, callback returned */
	}
	module_put(owner); /* external call pin, still in broker text */
out:
	cw_release_result = ret;
	mutex_unlock(&cw_release_lock);
	return ret;
}

static int cw_release_get(char *buf, const struct kernel_param *kp)
{
	int len;

	if (!mutex_trylock(&cw_release_lock))
		return -EBUSY;
	len = scnprintf(buf, PAGE_SIZE,
			"armed=%u releasing=%u released=%u result=%d\n",
			!!cw_release_owner, cw_release_running,
			cw_release_done, cw_release_result);
	mutex_unlock(&cw_release_lock);
	return len;
}

static const struct kernel_param_ops cw_release_ops = {
	.set = cw_release_set,
	.get = cw_release_get,
};
module_param_cb(pt_release, &cw_release_ops, NULL, 0600);
MODULE_PARM_DESC(pt_release, "Write 1 to retire the quiescent provider and release its module pin");
