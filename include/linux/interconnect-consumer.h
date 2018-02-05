// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018, Linaro Ltd.
 * Author: Georgi Djakov <georgi.djakov@linaro.org>
 */

#ifndef _LINUX_INTERCONNECT_CONSUMER_H
#define _LINUX_INTERCONNECT_CONSUMER_H

#include <linux/types.h>
#include <linux/mutex.h>

struct interconnect_path;

/**
 * struct creq - interconnect consumer request
 * @avg_bw: the average requested bandwidth (over longer period of time) in kbps
 * @peak_bw: the peak (maximum) bandwidth in kpbs
 */
struct interconnect_creq {
	u32 avg_bw;
	u32 peak_bw;
};

#if IS_ENABLED(CONFIG_INTERCONNECT)

struct interconnect_path *interconnect_get(const int src_id, const int dst_id);

void interconnect_put(struct interconnect_path *path);

/**
 * interconnect_set() - set constraints on a path between two endpoints
 * @path: reference to the path returned by interconnect_get()
 * @creq: request from the consumer, containing its requirements
 *
 * This function is used by an interconnect consumer to express its own needs
 * in term of bandwidth and QoS for a previously requested path between two
 * endpoints. The requests are aggregated and each node is updated accordingly.
 *
 * Returns 0 on success, or an approproate error code otherwise.
 */
int interconnect_set(struct interconnect_path *path,
		     const struct interconnect_creq *creq);

#else

static inline struct interconnect_path *interconnect_get(const int src_id,
							 const int dst_id)
{
	return ERR_PTR(-ENOTSUPP);
}

static inline void interconnect_put(struct interconnect_path *path)
{
}

static inline int interconnect_set(struct interconnect_path *path,
				   const struct interconnect_creq *creq)
{
	return 0;
}

#endif /* CONFIG_INTERCONNECT */

#endif /* _LINUX_INTERCONNECT_CONSUMER_H */
