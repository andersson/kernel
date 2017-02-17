/*
 * Copyright (c) 2017, Linaro Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/rpmsg.h>

struct glink_smd {
	struct device *dev;

	struct rpmsg_endpoint *channel;
};

static int glink_smd_callback(struct rpmsg_device *rpdev,
			      void *data, int len, void *priv, u32 addr)
{
//	struct glink_smd *glink = dev_get_drvdata(&rpdev->dev);

	print_hex_dump(KERN_ERR, "[IN] GLINK_CTRL: ", DUMP_PREFIX_OFFSET, 16, 1, data, len, true);

	return 0;
}

static int glink_smd_probe(struct rpmsg_device *rpdev)
{
	struct glink_smd *glink;

	glink = devm_kzalloc(&rpdev->dev, sizeof(*glink), GFP_KERNEL);
	if (!glink)
		return -ENOMEM;

	glink->dev = &rpdev->dev;
	glink->channel = rpdev->ept;

	dev_set_drvdata(&rpdev->dev, glink);

	dev_dbg(&rpdev->dev, "GLINK SMD driver probed\n");

	return 0;
}

static void glink_smd_remove(struct rpmsg_device *rpdev)
{
	struct glink_smd *glink = dev_get_drvdata(&rpdev->dev);

	dev_dbg(glink->dev, "GLINK SMD driver removed\n");
}

static const struct rpmsg_device_id glink_smd_match[] = {
	{ "GLINK_CTRL" },
	{}
};

static struct rpmsg_driver glink_smd_driver = {
	.probe = glink_smd_probe,
	.remove = glink_smd_remove,
	.callback = glink_smd_callback,
	.id_table = glink_smd_match,
	.drv = {
		.name = "glink_smd",
	},
};

module_rpmsg_driver(glink_smd_driver);

MODULE_DESCRIPTION("Qualcomm GLINK SMD driver");
MODULE_LICENSE("GPL v2");

