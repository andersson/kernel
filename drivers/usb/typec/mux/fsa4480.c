// SPDX-License-Identifier: GPL-2.0+
/*
 * 
 *

 */

#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_mux.h>

struct fsa4480 {
	struct i2c_client *client;

	struct typec_switch *sw;
	struct typec_mux *mux;

	struct regmap *regmap;
};

static const struct regmap_config fsa4480_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x1f,
};

static int fsa4480_switch_set(struct typec_switch *sw,
			      enum typec_orientation orientation)
{
	return 0;
}

static int fsa4480_mux_set(struct typec_mux *mux, struct typec_mux_state *state)
{
	return 0;
}

static int fsa4480_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct typec_switch_desc sw_desc = { };
	struct typec_mux_desc mux_desc = { };
	struct fsa4480 *fsa;
	unsigned int val;

	fsa = devm_kzalloc(dev, sizeof(*fsa), GFP_KERNEL);
	if (!fsa)
		return -ENOMEM;

	fsa->client = client;

	fsa->regmap = devm_regmap_init_i2c(client, &fsa4480_regmap_config);
	if (IS_ERR(fsa->regmap)) {
		dev_err(dev, "failed to initialize regmap\n");
		return PTR_ERR(fsa->regmap);
	}

	regmap_write(fsa->regmap, 4, 0x80);
	regmap_write(fsa->regmap, 5, 0x18);
	regmap_write(fsa->regmap, 4, 0xf8);

	sw_desc.drvdata = fsa;
	sw_desc.fwnode = dev->fwnode;
	sw_desc.set = fsa4480_switch_set;

	fsa->sw = typec_switch_register(dev, &sw_desc);
	if (IS_ERR(fsa->sw)) {
		dev_err(dev, "failed to register typec switch: %ld\n", PTR_ERR(fsa->sw));
		return PTR_ERR(fsa->sw);
	}

	mux_desc.drvdata = fsa;
	mux_desc.fwnode = dev->fwnode;
	mux_desc.set = fsa4480_mux_set;

	fsa->mux = typec_mux_register(dev, &mux_desc);
	if (IS_ERR(fsa->mux)) {
		typec_switch_unregister(fsa->sw);
		dev_err(dev, "failed to register typec mux: %ld\n", PTR_ERR(fsa->mux));
		return PTR_ERR(fsa->mux);
	}

	i2c_set_clientdata(client, fsa);
	return 0;
}

static int fsa4480_remove(struct i2c_client *client)
{
	struct fsa4480 *fsa = i2c_get_clientdata(client);

	typec_mux_unregister(fsa->mux);
	typec_switch_unregister(fsa->sw);

	return 0;
}

static const struct i2c_device_id fsa4480_table[] = {
	{ "fsa4480" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, fsa4480_table);

static const struct of_device_id fsa4480_of_table[] = {
	{ .compatible = "onsemi,fsa4480" },
	{ }
};
MODULE_DEVICE_TABLE(of, fsa4480_of_table);

static struct i2c_driver fsa4480_driver = {
	.driver = {
		.name = "fsa4480",
		.of_match_table = fsa4480_of_table,
	},
	.probe_new	= fsa4480_probe,
	.remove		= fsa4480_remove,
	.id_table	= fsa4480_table,
};

module_i2c_driver(fsa4480_driver);

MODULE_DESCRIPTION("ON Semiconductor FSA4480 driver");
MODULE_LICENSE("GPL v2");
