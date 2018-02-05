// SPDX-License-Identifier: GPL-2.0
/*
 * Interconnect framework core driver
 *
 * Copyright (c) 2018, Linaro Ltd.
 * Author: Georgi Djakov <georgi.djakov@linaro.org>
 */

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/interconnect-consumer.h>
#include <linux/interconnect-provider.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

static DEFINE_MUTEX(interconnect_provider_list_mutex);
static LIST_HEAD(interconnect_provider_list);
static DEFINE_MUTEX(interconnect_orphan_mutex);
static LIST_HEAD(interconnect_orphan_list);
static struct dentry *interconnect_debugfs_dir;

/**
 * struct interconnect_path - interconnect path structure
 * @num_nodes: number of hops (nodes)
 * @reqs: array of the requests applicable to this path of nodes
 */
struct interconnect_path {
	size_t num_nodes;
	struct interconnect_req reqs[0];
};

static struct interconnect_node *node_find(const int id)
{
	struct interconnect_node *node = NULL;
	struct icp *icp;

	mutex_lock(&interconnect_provider_list_mutex);

	list_for_each_entry(icp, &interconnect_provider_list, icp_list) {
		struct interconnect_node *n;

		list_for_each_entry(n, &icp->nodes, icn_list) {
			if (n->id == id) {
				node = n;
				goto out;
			}
		}
	}

out:
	mutex_unlock(&interconnect_provider_list_mutex);

	return node;
}

static struct interconnect_node *node_find_orphan(const int id)
{
	struct interconnect_node *node = NULL;
	struct interconnect_node *n;

	mutex_lock(&interconnect_orphan_mutex);

	list_for_each_entry(n, &interconnect_orphan_list, orphan_list) {
		if (n->id == id) {
			node = n;
			goto out;
		}
	}
out:
	mutex_unlock(&interconnect_orphan_mutex);
	return node;
}

static struct interconnect_path *path_allocate(struct interconnect_node *node,
					       ssize_t num_nodes)
{
	struct interconnect_path *path;
	size_t i;

	path = kzalloc(sizeof(*path) + (num_nodes - 1) * sizeof(*path->reqs),
		       GFP_KERNEL);
	if (!path)
		return ERR_PTR(-ENOMEM);

	path->num_nodes = num_nodes;

	for (i = 0; i < num_nodes; i++) {
		hlist_add_head(&path->reqs[i].req_node, &node->req_list);

		path->reqs[i].node = node;
		/* reference to previous node was saved during path traversal */
		node = node->reverse;
	}

	return path;
}

static struct interconnect_path *path_find(struct interconnect_node *src,
					   struct interconnect_node *dst)
{
	struct interconnect_node *node = NULL;
	struct list_head traverse_list;
	struct list_head edge_list;
	struct list_head tmp_list;
	size_t i, number = 1;
	bool found = false;

	INIT_LIST_HEAD(&traverse_list);
	INIT_LIST_HEAD(&edge_list);
	INIT_LIST_HEAD(&tmp_list);

	list_add_tail(&src->search_list, &traverse_list);

	do {
		list_for_each_entry(node, &traverse_list, search_list) {
			if (node == dst) {
				found = true;
				list_add(&node->search_list, &tmp_list);
				break;
			}
			for (i = 0; i < node->num_links; i++) {
				struct interconnect_node *tmp = node->links[i];

				if (!tmp)
					return ERR_PTR(-ENOENT);

				if (tmp->is_traversed)
					continue;

				tmp->is_traversed = true;
				tmp->reverse = node;
				list_add_tail(&tmp->search_list, &edge_list);
			}
		}
		if (found)
			break;

		list_splice_init(&traverse_list, &tmp_list);
		list_splice_init(&edge_list, &traverse_list);

		/* count the number of nodes */
		number++;

	} while (!list_empty(&traverse_list));

	/* reset the traversed state */
	list_for_each_entry(node, &tmp_list, search_list)
		node->is_traversed = false;

	if (found)
		return path_allocate(dst, number);

	return ERR_PTR(-EPROBE_DEFER);
}

static int path_init(struct interconnect_path *path)
{
	struct interconnect_node *node;
	size_t i;

	for (i = 0; i < path->num_nodes; i++) {
		node = path->reqs[i].node;

		mutex_lock(&node->icp->lock);
		node->icp->users++;
		mutex_unlock(&node->icp->lock);
	}

	return 0;
}

static void node_aggregate(struct interconnect_node *node)
{
	struct interconnect_req *r;
	u32 avg_bw = 0;
	u32 peak_bw = 0;

	hlist_for_each_entry(r, &node->req_list, req_node) {
		/* sum(averages) and max(peaks) */
		avg_bw += r->avg_bw;
		peak_bw = max(peak_bw, r->peak_bw);
	}

	node->creq.avg_bw = avg_bw;
	node->creq.peak_bw = peak_bw;
}

static void provider_aggregate(struct icp *icp)
{
	struct interconnect_node *n;
	u32 avg_bw = 0;
	u32 peak_bw = 0;

	/* aggregate for the interconnect provider */
	list_for_each_entry(n, &icp->nodes, icn_list) {
		/* sum the average and max the peak */
		avg_bw += n->creq.avg_bw;
		peak_bw = max(peak_bw, n->creq.peak_bw);
	}

	/* save the aggregated values */
	icp->creq.avg_bw = avg_bw;
	icp->creq.peak_bw = peak_bw;
}

static int constraints_apply(struct interconnect_path *path)
{
	struct interconnect_node *node, *prev = NULL;
	struct icp *icp;
	int i, ret = 0;

	for (i = 0; i < path->num_nodes; i++, prev = node) {
		node = path->reqs[i].node;
		icp = node->icp;

		/*
		 * Both endpoints should be valid master-slave pairs of the
		 * same interconnect provider that will be configured.
		 */
		if (!node || !prev)
			continue;
		if (node->icp != prev->icp)
			continue;

		mutex_lock(&icp->lock);

		/* aggregate requests for the provider */
		provider_aggregate(icp);

		if (icp->ops->set) {
			/* set the constraints */
			ret = icp->ops->set(prev, node, &icp->creq);
		}

		mutex_unlock(&icp->lock);

		if (ret)
			pr_err("interconnect: error applying constraints (%d)", ret);
	}

	return ret;
}

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
		     const struct interconnect_creq *creq)
{
	struct interconnect_node *node;
	struct icp *icp;
	size_t i;
	int ret = 0;

	for (i = 0; i < path->num_nodes; i++) {
		node = path->reqs[i].node;
		icp = node->icp;

		mutex_lock(&icp->lock);

		/* update the consumer request for this path */
		path->reqs[i].avg_bw = creq->avg_bw;
		path->reqs[i].peak_bw = creq->peak_bw;

		/* aggregate requests for this node */
		node_aggregate(node);

		mutex_unlock(&icp->lock);
	}

	ret = constraints_apply(path);
	if (ret)
		pr_err("interconnect: error applying constraints (%d)", ret);

out:
	return ret;
}
EXPORT_SYMBOL_GPL(interconnect_set);

/**
 * interconnect_get() - return a handle for path between two endpoints
 * @sdev: source device identifier
 * @sid: source device port id
 * @ddev: destination device identifier
 * @did: destination device port id
 *
 * This function will search for a path between two endpoints and return an
 * interconnect_path handle on success. Use interconnect_put() to release
 * constraints when the they are not needed anymore.
 *
 * Return: interconnect_path pointer on success, or ERR_PTR() on error
 */
struct interconnect_path *interconnect_get(const int src_id, const int dst_id)
{
	struct interconnect_node *src, *dst;
	struct interconnect_path *path;

	src = node_find(src_id);
	if (!src) {
		path = ERR_PTR(-EPROBE_DEFER);
		goto out;
	}

	dst = node_find(dst_id);
	if (!dst) {
		path = ERR_PTR(-EPROBE_DEFER);
		goto out;
	}

	path = path_find(src, dst);
	if (IS_ERR(path)) {
		pr_err("error finding path between %s and %s (%ld)\n",
		       src->name, dst->name, PTR_ERR(path));
		goto out;
	}

	path_init(path);

out:
	return path;
}
EXPORT_SYMBOL_GPL(interconnect_get);

/**
 * interconnect_put() - release the reference to the interconnect_path
 *
 * @path: interconnect path
 *
 * Use this function to release the path and free the memory when setting
 * constraints on the path is no longer needed.
 */
void interconnect_put(struct interconnect_path *path)
{
	struct interconnect_creq creq = { 0, 0 };
	struct interconnect_node *node;
	size_t i;
	int ret;

	if (!path || WARN_ON_ONCE(IS_ERR(path)))
		return;

	ret = interconnect_set(path, &creq);
	if (ret)
		pr_err("%s: error (%d)\n", __func__, ret);

	for (i = 0; i < path->num_nodes; i++) {
		node = path->reqs[i].node;

		mutex_lock(&node->icp->lock);
		node->icp->users--;
		mutex_unlock(&node->icp->lock);
	}

	kfree(path);
}
EXPORT_SYMBOL_GPL(interconnect_put);

/**
 * interconnect_node_create() - create a node
 * @id: node id
 *
 * Return: interconnect_node pointer on success, or ERR_PTR() on error
 */
struct interconnect_node *interconnect_node_create(const int id)
{
	struct interconnect_node *node;

	node = node_find(id);
	if (node)
		/* node already exists */
		return node;

	node = node_find_orphan(id);
	if (node) {
		/* orphan node already exists */
		mutex_lock(&interconnect_orphan_mutex);
		list_del(&node->orphan_list);
		mutex_unlock(&interconnect_orphan_mutex);
		return node;
	}

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return ERR_PTR(-ENOMEM);

	node->id = id;

	return node;
}
EXPORT_SYMBOL_GPL(interconnect_node_create);

/**
 * interconnect_link_create() - create a link between two nodes
 * @src_id: source node id
 * @dst_id: destination node id
 *
 * Return: 0 on success, or an error code otherwise
 */
int interconnect_link_create(const int src_id, const int dst_id)
{
	struct interconnect_node *src, *dst;
	struct interconnect_node **old;

	src = node_find(src_id);
	if (!src)
		return -ENODEV;

	if (node_find_orphan(src_id)) {
		mutex_lock(&interconnect_orphan_mutex);
		list_del(&src->orphan_list);
		mutex_unlock(&interconnect_orphan_mutex);
	}

	dst = node_find(dst_id);
	if (!dst) {
		dst = node_find_orphan(dst_id);
		if (!dst) {
			/* node does not exist yet, create it */
			dst = interconnect_node_create(dst_id);
			mutex_lock(&interconnect_orphan_mutex);
			list_add_tail(&dst->orphan_list, &interconnect_orphan_list);
			mutex_unlock(&interconnect_orphan_mutex);
		}
	}

	old = src->links;
	src->links = krealloc(old, (src->num_links + 1) * sizeof(*src->links), GFP_KERNEL);
	if (!src->links)
		return -ENOMEM;

	src->links[src->num_links++] = dst;

	return 0;
}
EXPORT_SYMBOL_GPL(interconnect_link_create);


/**
 * interconnect_add_provider() - add a new interconnect provider
 * @icp: the interconnect provider that will be added into topology
 *
 * Return: 0 on success, or an error code otherwise
 */
int interconnect_add_provider(struct icp *icp)
{
	if (!icp)
		return -EINVAL;

	if (!icp->ops->set) {
		dev_err(icp->dev, "%s: .set is not implemented\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&interconnect_provider_list_mutex);
	mutex_init(&icp->lock);
	list_add(&icp->icp_list, &interconnect_provider_list);
	mutex_unlock(&interconnect_provider_list_mutex);

	dev_dbg(icp->dev, "interconnect provider added to topology\n");

	return 0;
}
EXPORT_SYMBOL_GPL(interconnect_add_provider);

/**
 * interconnect_del_provider() - delete previously added interconnect provider
 * @icp: the interconnect provider that will be removed from topology
 *
 * Return: 0 on success, or an error code otherwise
 */
int interconnect_del_provider(struct icp *icp)
{
	mutex_lock(&icp->lock);
	if (icp->users) {
		mutex_unlock(&icp->lock);
		return -EBUSY;
	}
	mutex_unlock(&icp->lock);

	mutex_lock(&interconnect_provider_list_mutex);
	list_del(&icp->icp_list);
	mutex_unlock(&interconnect_provider_list_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(interconnect_del_provider);

MODULE_AUTHOR("Georgi Djakov <georgi.djakov@linaro.org");
MODULE_DESCRIPTION("Interconnect Driver Core");
MODULE_LICENSE("GPL v2");

#ifdef CONFIG_DEBUG_FS

static void interconnect_summary_show_one(struct seq_file *s,
					  struct interconnect_node *n)
{
	if (!n)
		return;

	seq_printf(s, "%-30s %12d %12d\n",
		   n->name, n->creq.avg_bw, n->creq.peak_bw);
}

static int interconnect_summary_show(struct seq_file *s, void *data)
{
	struct icp *icp;

	seq_puts(s, " node                                   avg         peak\n");
	seq_puts(s, "--------------------------------------------------------\n");

	mutex_lock(&interconnect_provider_list_mutex);

	list_for_each_entry(icp, &interconnect_provider_list, icp_list) {
		struct interconnect_node *n;

		list_for_each_entry(n, &icp->nodes, icn_list) {
			interconnect_summary_show_one(s, n);
		}
	}

	mutex_unlock(&interconnect_provider_list_mutex);

	return 0;
}


static int interconnect_summary_open(struct inode *inode, struct file *file)
{
	return single_open(file, interconnect_summary_show, inode->i_private);
}

static const struct file_operations interconnect_summary_fops = {
	.open		= interconnect_summary_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init interconnect_debugfs_init(void)
{
	struct dentry *file;

	interconnect_debugfs_dir = debugfs_create_dir("interconnect", NULL);
	if (!interconnect_debugfs_dir) {
		pr_err("interconnect: error creating debugfs directory\n");
		return -ENODEV;
	}

	file = debugfs_create_file("interconnect_summary", 0444,
				   interconnect_debugfs_dir, NULL,
				   &interconnect_summary_fops);
	if (!file)
		return -ENODEV;

	return 0;
}
late_initcall(interconnect_debugfs_init);
#endif
