// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2018, Linaro Limited
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/leds.h>

struct pca9956;

struct pca9956_led {
	struct pca9956 *pca;
	struct led_classdev ldev;
	int id;

	const char *name;
	const char *default_trigger;

	int type;
	u8 brightness;
};

struct pca9956 {
	struct i2c_client *client;
	struct pca9956_led leds[24];

	bool grp_blinking;
};

enum {
	PCA9956_DISABLED,
	PCA9956_LED,
	PCA9956_KEEP,
};

#define to_pca9956(cdev) container_of(cdev, struct pca9956_led, ldev)

#define MODE1_REG		0x0
#define MODE2_REG		0x1

#define LEDOUT_REG(ldr)		(0x2 + (ldr >> 3))
#define LEDOUT_SHIFT(ldr)	((ldr % 4) * 2)
#define LEDOUT_OFF		0
#define LEDOUT_OE		1
#define LEDOUT_PWM_ALL		2
#define LEDOUT_PWM_GRP		3

#define PWM_REG(pwm)		(0xa + pwm)
#define IREFALL_REG		0x40

static int pca9956_set_mode1(struct pca9956 *pca, bool lpm)
{
	u32 val;

	val = i2c_smbus_read_byte_data(pca->client, MODE1_REG);
	if (lpm)
		val |= BIT(4);
	else
		val &= ~BIT(4);
	return i2c_smbus_write_byte_data(pca->client, MODE1_REG, val);
}

static int pca9956_set_mode2(struct pca9956 *pca)
{
	u32 val;

	val = i2c_smbus_read_byte_data(pca->client, MODE2_REG);
	if (pca->grp_blinking)
		val |= BIT(5);
	else
		val &= ~BIT(5);
	return i2c_smbus_write_byte_data(pca->client, MODE2_REG, val);
}

static int pca9956_get_brightness(struct led_classdev *led_cdev)
{
	struct pca9956_led *led = to_pca9956(led_cdev);
	struct pca9956 *pca = led->pca;
	u32 val;

	val = i2c_smbus_read_byte_data(pca->client, 0x2 + (led->id >> 2));
	val = val >> LEDOUT_SHIFT(led->id);
	val &= 0x3;

	if (val == 0)
		return LED_OFF;
	if (val == 1)
		return LED_FULL;

	return i2c_smbus_read_byte_data(pca->client, 0xa + led->id);
}

static int pca9956_set_brightness(struct led_classdev *led_cdev,
				  enum led_brightness brightness)
{
	struct pca9956_led *led = to_pca9956(led_cdev);
	struct pca9956 *pca = led->pca;
	u32 ledout;

	ledout = i2c_smbus_read_byte_data(pca->client, LEDOUT_REG(led->id));
	ledout &= ~3 << LEDOUT_SHIFT(led->id);

	if (brightness != LED_OFF) {
		ledout |= 2 << LEDOUT_SHIFT(led->id);;
		i2c_smbus_write_byte_data(pca->client, PWM_REG(led->id), brightness);
	}

	return i2c_smbus_write_byte_data(pca->client, LEDOUT_REG(led->id), ledout);
}

static int pca9956_probe(struct i2c_client *client,
			 const struct i2c_device_id *i2c_id)
{
	struct device_node *child;
	struct pca9956_led *led;
	struct device_node *np = client->dev.of_node;
	struct device *dev = &client->dev;
	struct pca9956 *pca;
	const char *state;
	int ret;
	u32 val;
	u32 id;
	int i;

	pca = devm_kzalloc(dev, sizeof(*pca), GFP_KERNEL);
	if (!pca)
		return -ENOMEM;

	pca->client = client;

	for (i = 0; i < ARRAY_SIZE(pca->leds); i++) {
		led = &pca->leds[i];
		led->id = id;
		led->pca = pca;
	}

	for_each_child_of_node(np, child) {
		ret = of_property_read_u32(child, "reg", &id);
		if (ret < 0) {
			dev_err(dev, "failed to parse reg of %pOF\n", child);
			of_node_put(child);
			return ret;
		}

		if (id >= ARRAY_SIZE(pca->leds)) {
			dev_err(dev, "invalid reg of %pOF\n", child);
			of_node_put(child);
			return -EINVAL;
		}

		led = &pca->leds[id];
		led->type = PCA9956_LED;

		if (of_property_read_string(child, "label", &led->name))
			led->name = child->name;
		if (!of_property_read_string(child, "default-state", &state)) {
			if (!strcmp(state, "on"))
				led->brightness = LED_FULL;
			else if (!strcmp(state, "keep"))
				led->type = PCA9956_KEEP;
		}
		of_property_read_string(child, "linux,default-trigger",
					&led->default_trigger);
	}

	ret = pca9956_set_mode1(pca, false);
	if (ret) {
		dev_err(dev, "failed to configure mode 1\n");
		return -EINVAL;
	}

	ret = pca9956_set_mode2(pca);
	if (ret) {
		dev_err(dev, "failed to configure mode 2\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(np, "nxp,irefall", &val);
	if (ret < 0 && ret != -EINVAL)
		return -EINVAL;
	else if (!ret)
		i2c_smbus_write_byte_data(client, IREFALL_REG, val);

	for (i = 0; i < ARRAY_SIZE(pca->leds); i++) {
		led = &pca->leds[i];

		switch (led->type) {
		case PCA9956_DISABLED:
			pca9956_set_brightness(&led->ldev, LED_OFF);
			break;
		case PCA9956_KEEP:
			led->brightness = pca9956_get_brightness(&led->ldev);
		case PCA9956_LED:
			led->ldev.name = led->name;
			led->ldev.default_trigger = led->default_trigger;
			led->ldev.brightness = led->brightness;
			led->ldev.brightness_set_blocking = pca9956_set_brightness;

			ret = devm_led_classdev_register(dev, &led->ldev);
			if (ret < 0) {
				dev_err(dev, "couldn't register LED %s\n",
					led->name);
				return ret;
			}

			pca9956_set_brightness(&led->ldev, led->ldev.brightness);
			break;
		}
	}

	return 0;
}

static int pca9956_remove(struct i2c_client *client)
{
	return 0;
}

static const struct i2c_device_id pca9956_id[] = {
	{ "pca9956b" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, pca9956_id);

static const struct of_device_id pca9956_of_match[] = {
	{ .compatible = "nxp,pca9956b" },
	{},
};
MODULE_DEVICE_TABLE(of, pca9956_of_match);

static struct i2c_driver pca9956_driver = {
	.driver = {
		.name = "leds-pca9956",
		.of_match_table = pca9956_of_match,
	},
	.probe = pca9956_probe,
	.remove = pca9956_remove,
	.id_table = pca9956_id,
};
module_i2c_driver(pca9956_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("PCA 9956B LED dimmer");
