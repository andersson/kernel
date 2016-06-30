/*
 * Virtio-based remote processor messaging bus
 *
 * Copyright (C) 2011 Texas Instruments, Inc.
 * Copyright (C) 2011 Google, Inc.
 *
 * Ohad Ben-Cohen <ohad@wizery.com>
 * Brian Swetland <swetland@google.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __RPMSG_INTERNAL_H__
#define __RPMSG_INTERNAL_H__

#define to_rpmsg_device(d) container_of(d, struct rpmsg_device, dev)
#define to_rpmsg_driver(d) container_of(d, struct rpmsg_driver, drv)

/**
 * struct rpmsg_channel_info - internal channel info representation
 * @name: name of service
 * @src: local address
 * @dst: destination address
 */
struct rpmsg_channel_info {
	char name[RPMSG_NAME_SIZE];
	u32 src;
	u32 dst;
};

struct rpmsg_channel {
	struct rpmsg_device rpdev;

	struct rpmsg_endpoint *(*create_ept)(struct rpmsg_device *rpdev,
					    rpmsg_rx_cb_t cb, void *priv, u32 addr);
	void (*destroy_ept)(struct rpmsg_endpoint *ept);

	int (*send)(struct rpmsg_endpoint *ept, void *data, int len);
	int (*sendto)(struct rpmsg_endpoint *ept, void *data, int len, u32 dst);
	int (*send_offchannel)(struct rpmsg_endpoint *ept, u32 src, u32 dst,
				  void *data, int len);

	int (*trysend)(struct rpmsg_endpoint *ept, void *data, int len);
	int (*trysendto)(struct rpmsg_endpoint *ept, void *data, int len, u32 dst);
	int (*trysend_offchannel)(struct rpmsg_endpoint *ept, u32 src, u32 dst,
			     void *data, int len);
	int (*announce_create)(struct rpmsg_device *rpdev);
	int (*announce_destroy)(struct rpmsg_device *rpdev);
};

static inline struct rpmsg_channel *to_rpmsg_channel(struct device *d)
{
	struct rpmsg_device *rpdev = to_rpmsg_device(d);
	return container_of(rpdev, struct rpmsg_channel, rpdev);
}

int rpmsg_register_device(struct rpmsg_device *rpdev);
int rpmsg_unregister_device(struct device *parent,
			    struct rpmsg_channel_info *chinfo);

struct device *rpmsg_find_device(struct device *parent,
				 struct rpmsg_channel_info *chinfo);

#endif
