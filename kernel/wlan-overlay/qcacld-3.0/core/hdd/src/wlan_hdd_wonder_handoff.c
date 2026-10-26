/* SPDX-License-Identifier: ISC */
#include "wlan_hdd_wonder_handoff.h"
#ifndef HOUJI_HANDOFF_HOST_TEST
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/limits.h>
#endif

int hdd_wh_init(struct hdd_wh_gate *g, struct net_device *sink, u64 generation)
{
	if (!g || !sink || !generation)
		return -EINVAL;
	memset(g, 0, sizeof(*g));
	spin_lock_init(&g->lock);
	init_waitqueue_head(&g->changed);
	dev_hold(sink);
	g->sink = sink;
	g->generation = generation;
	g->state = HDD_WH_OPEN;
	return 0;
}

int hdd_wh_enter(struct hdd_wh_gate *g, u64 generation, struct hdd_wh_ticket *ticket)
{
	int ret = 0;
	if (!g || !ticket || ticket->gate)
		return -EINVAL;
	spin_lock_bh(&g->lock);
	if (generation != g->generation)
		ret = -ESTALE;
	else if (g->state != HDD_WH_OPEN)
		ret = -ESHUTDOWN;
	else if (g->users == UINT_MAX)
		ret = -EOVERFLOW;
	else {
		g->users++;
		ticket->gate = g;
		ticket->generation = generation;
	}
	spin_unlock_bh(&g->lock);
	return ret;
}

int hdd_wh_leave(struct hdd_wh_ticket *ticket)
{
	struct hdd_wh_gate *g;
	bool wake = false;
	if (!ticket || !ticket->gate)
		return -EINVAL;
	g = ticket->gate;
	spin_lock_bh(&g->lock);
	if (ticket->generation != g->generation || !g->users) {
		spin_unlock_bh(&g->lock);
		return -ESTALE;
	}
	g->users--;
	wake = !g->users && g->state == HDD_WH_CLOSING;
	ticket->gate = NULL;
	spin_unlock_bh(&g->lock);
	/* The registry/caller still pins g across this wake-up tail. */
	if (wake)
		wake_up_all(&g->changed);
	return 0;
}

int hdd_wh_close(struct hdd_wh_gate *g, u64 generation, const char country[3], unsigned int reason)
{
	int ret = 0;
	if (!g || !country)
		return -EINVAL;
	spin_lock_bh(&g->lock);
	if (generation != g->generation)
		ret = -ESTALE;
	else if (g->state == HDD_WH_RELEASED)
		ret = -ESHUTDOWN;
	else {
		if (g->state == HDD_WH_OPEN) {
			memcpy(g->country, country, sizeof(g->country));
			g->state = HDD_WH_CLOSING;
		}
		g->reasons |= reason;
	}
	spin_unlock_bh(&g->lock);
	if (!ret)
		wake_up_all(&g->changed);
	return ret;
}

bool hdd_wh_work_pending(struct hdd_wh_gate *g)
{
	bool pending;
	spin_lock_bh(&g->lock);
	pending = g->state == HDD_WH_CLOSING && !g->users && !g->actor;
	spin_unlock_bh(&g->lock);
	return pending;
}

int hdd_wh_try_claim(struct hdd_wh_gate *g, u64 generation, struct hdd_wh_claim *claim)
{
	int ret = 0;
	if (!g || !claim || claim->gate)
		return -EINVAL;
	spin_lock_bh(&g->lock);
	if (generation != g->generation)
		ret = -ESTALE;
	else if (g->state != HDD_WH_CLOSING || g->users || g->actor)
		ret = -EBUSY;
	else if (g->next_cookie == ~(u64)0)
		ret = -EOVERFLOW;
	else {
		g->actor = true;
		g->current_cookie = ++g->next_cookie;
		claim->gate = g;
		claim->generation = generation;
		claim->cookie = g->current_cookie;
	}
	spin_unlock_bh(&g->lock);
	return ret;
}

int hdd_wh_finish(struct hdd_wh_claim *claim, bool done)
{
	struct hdd_wh_gate *g;
	if (!claim || !claim->gate)
		return -EINVAL;
	g = claim->gate;
	spin_lock_bh(&g->lock);
	if (claim->generation != g->generation || !g->actor ||
	    claim->cookie != g->current_cookie || g->state != HDD_WH_CLOSING) {
		spin_unlock_bh(&g->lock);
		return -ESTALE;
	}
	g->actor = false;
	if (done)
		g->state = HDD_WH_DRAINED;
	claim->gate = NULL;
	spin_unlock_bh(&g->lock);
	wake_up_all(&g->changed);
	return 0;
}

int hdd_wh_release(struct hdd_wh_gate *g)
{
	struct net_device *sink;
	int ret = 0;
	if (!g)
		return -EINVAL;
	spin_lock_bh(&g->lock);
	if (g->state == HDD_WH_RELEASED)
		ret = -EALREADY;
	else if (g->state != HDD_WH_DRAINED || g->users || g->actor)
		ret = -EBUSY;
	if (ret) {
		spin_unlock_bh(&g->lock);
		return ret;
	}
	sink = g->sink;
	g->sink = NULL;
	g->state = HDD_WH_RELEASED;
	spin_unlock_bh(&g->lock);
	dev_put(sink);
	return 0;
}
