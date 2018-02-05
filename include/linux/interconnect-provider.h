// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018, Linaro Ltd.
 * Author: Georgi Djakov <georgi.djakov@linaro.org>
 */

#ifndef _LINUX_INTERCONNECT_PROVIDER_H
#define _LINUX_INTERCONNECT_PROVIDER_H

#include <linux/interconnect-consumer.h>

struct interconnect_node;

/**
 * struct icp_ops - platform specific callback operations for interconnect
 * providers that will be called from drivers
 *
 * @set: set constraints on interconnect
 */
struct icp_ops {
	int (*set)(struct interconnect_node *src, struct interconnect_node *dst,
		   struct interconnect_creq *creq);
};

/**
 * struct icp - interconnect provider (controller) entity that might
 * provide multiple interconnect controls
 *
 * @icp_list: list of the registered interconnect providers
 * @nodes: internal list of the interconnect provider nodes
 * @ops: pointer to device specific struct icp_ops
 * @dev: the device this interconnect provider belongs to
 * @lock: a lock to protect creq and users
 * @creq: the actual state of constraints for this interconnect provider
 * @users: count of active users
 * @data: pointer to private data
 */
struct icp {
	struct list_head	icp_list;
	struct list_head	nodes;
	const struct icp_ops	*ops;
	struct device		*dev;
	struct mutex		lock;
	struct interconnect_creq creq;
	int			users;
	void			*data;
};

/**
 * struct interconnect_node - entity that is part of the interconnect topology
 *
 * @links: a list of targets where we can go next when traversing
 * @num_links: number of links to other interconnect nodes
 * @icp: points to the interconnect provider of this node
 * @icn_list: list of interconnect nodes
 * @search_list: list used when walking the nodes graph
 * @reverse: pointer to previous node when walking the nodes graph
 * @is_traversed: flag that is used when walking the nodes graph
 * @req_list: a list of QoS constraint requests
 * @creq: aggregated values of all constraint requests
 * @id: platform specific node id
 * @name: node name
 * @data: pointer to private data
 */
struct interconnect_node {
	struct interconnect_node **links;
	size_t			num_links;

	struct icp		*icp;
	struct list_head	icn_list;
	struct list_head	orphan_list;
	struct list_head	search_list;
	struct interconnect_node *reverse;
	bool			is_traversed;
	struct hlist_head	req_list;
	struct interconnect_creq creq;
	int			id;
	const char              *name;
	void			*data;
};

/**
 * struct interconnect_req - constraints that are attached to each node
 *
 * @req_node: the linked list node
 * @node: the interconnect node to which this constraint applies
 * @avg_bw: an integer describing the average bandwidth in kbps
 * @peak_bw: an integer describing the peak bandwidth in kbps
 */
struct interconnect_req {
	struct hlist_node req_node;
	struct interconnect_node *node;
	u32 avg_bw;
	u32 peak_bw;
};

#if IS_ENABLED(CONFIG_INTERCONNECT)

struct interconnect_node *interconnect_node_create(const int id);
int interconnect_link_create(const int src_id, const int dst_id);
int interconnect_add_provider(struct icp *icp);
int interconnect_del_provider(struct icp *icp);
void interconnect_has_full_topology(void);

#else

static inline struct interconnect_node *interconnect_node_create(const int id)
{
	return ERR_PTR(-ENOTSUPP);
}

static inline int interconnect_link_create(const int src_id, const int dst_id)
{
	return -ENOTSUPP;
}

static inline int interconnect_add_provider(struct icp *icp)
{
	return -ENOTSUPP;
}

static inline int interconnect_del_provider(struct icp *icp)
{
	return -ENOTSUPP;
}

void interconnect_has_full_topology(void)
{
}

#endif /* CONFIG_INTERCONNECT */

#endif /* _LINUX_INTERCONNECT_PROVIDER_H */
