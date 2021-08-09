// SPDX-License-Identifier: GPL-2.0-only
/*
 * MIPI-DSI based r66451 AMOLED LCD 5.3 inch panel driver.
 *
 * Copyright (c) 2013 Samsung Electronics Co., Ltd
 *
 * Inki Dae, <inki.dae@visionox.com>
 * Donghwa Lee, <dh09.lee@visionox.com>
 * Joongmock Shin <jmock.shin@visionox.com>
 * Eunchul Kim <chulspro.kim@visionox.com>
 * Tomasz Figa <t.figa@visionox.com>
 * Andrzej Hajda <a.hajda@visionox.com>
*/

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>
#include <video/of_videomode.h>
#include <video/videomode.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#define MAX_DCS_PACKET_LEN	64

static const struct drm_display_mode default_mode = {
	.clock = 353126,
	.hdisplay = 1080,
	.hsync_start = 1080 + 95,
	.hsync_end = 1080 + 95 + 1,
	.htotal = 1080 + 95 + 1 + 40,
	.vdisplay = 2340,
	.vsync_start = 2340 + 75,
	.vsync_end = 2340 + 75 + 1,
	.vtotal = 2340 + 75 + 1 + 4,
};

struct r66451_panel {
	struct device *dev;
	struct drm_panel panel;

	struct regulator_bulk_data supplies[2];
	struct gpio_desc *reset_gpio;
};

static inline struct r66451_panel *panel_to_r66451(struct drm_panel *panel)
{
	return container_of(panel, struct r66451_panel, panel);
}

#define dsi_dcs_write_seq(ctx, seq...) do {						\
		struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);		\
		static const u8 d[] = { seq };						\
		BUILD_BUG_ON_MSG(ARRAY_SIZE(d) > MAX_DCS_PACKET_LEN,			\
				 "DCS sequence too big for stack"); 			\
		ret = mipi_dsi_dcs_write_buffer(dsi, d, ARRAY_SIZE(d));			\
		if (ret < 0)								\
			goto err;							\
	} while (0)

static int r66451_set_maximum_return_packet_size(struct r66451_panel *ctx,
						 u16 size)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int ret;

	ret = mipi_dsi_set_maximum_return_packet_size(dsi, size);
	if (ret < 0) {
		dev_err(ctx->dev,
			"error %d setting maximum return packet size to %d\n",
			ret, size);
		return ret;
	}

	return 0;
}

static int r66451_power_on(struct r66451_panel *ctx)
{
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0)
		return ret;

	/*
	 * Reset sequence of visionox panel requires the panel to be
	 * out of reset for 10ms, followed by being held in reset
	 * for 10ms and then out again
	 */
	gpiod_set_value(ctx->reset_gpio, 1);
	usleep_range(10000, 20000);
	gpiod_set_value(ctx->reset_gpio, 0);
	usleep_range(10000, 20000);
	gpiod_set_value(ctx->reset_gpio, 1);
	usleep_range(10000, 20000);

	return 0;
}

static int r66451_power_off(struct r66451_panel *ctx)
{
	int ret;

	dsi_dcs_write_seq(ctx, MIPI_DCS_ENTER_SLEEP_MODE);

	ret = regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);

err:
	return ret;
}

static int r66451_disable(struct drm_panel *panel)
{
	return 0;
}

static int r66451_unprepare(struct drm_panel *panel)
{
	struct r66451_panel *ctx = panel_to_r66451(panel);
	int ret;

	dsi_dcs_write_seq(ctx, MIPI_DCS_SET_DISPLAY_OFF);
	dsi_dcs_write_seq(ctx, MIPI_DCS_ENTER_SLEEP_MODE);
	msleep(40);

err:
	r66451_power_off(ctx);
	return ret;
}

static int r66451_prepare(struct drm_panel *panel)
{
	struct r66451_panel *ctx = panel_to_r66451(panel);
	int ret;

	ret = r66451_power_on(ctx);
	if (ret < 0)
		return ret;

	ret = r66451_set_maximum_return_packet_size(ctx, MAX_DCS_PACKET_LEN);
	if (ret < 0)
		goto err;

	dsi_dcs_write_seq(ctx, 0xb0, 0x00);
	dsi_dcs_write_seq(ctx, 0xb3, 0x01);
	dsi_dcs_write_seq(ctx, 0xb0, 0x04);
	dsi_dcs_write_seq(ctx, 0xe8, 0x00, 0x02);
	dsi_dcs_write_seq(ctx, 0xe4, 0x00, 0x08);
	dsi_dcs_write_seq(ctx, 0xb0, 0x00);
	dsi_dcs_write_seq(ctx, 0xc4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			       0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x32);
	dsi_dcs_write_seq(ctx, 0xcf, 0x64, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08,
			       0x00, 0x0b, 0x77, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02,
			       0x02, 0x02, 0x02, 0x02, 0x03);
	dsi_dcs_write_seq(ctx, 0xd3, 0x45, 0x00, 0x00, 0x01, 0x13, 0x15, 0x00, 0x15, 0x07,
			       0x0f, 0x77, 0x77, 0x77, 0x37, 0xb2, 0x11, 0x00, 0xa0, 0x3c,
			       0x9c);
	dsi_dcs_write_seq(ctx, 0xd7, 0x00, 0xb9, 0x34, 0x00, 0x40, 0x04, 0x00, 0xa0, 0x0a,
                               0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x19, 0x34,
                               0x00, 0x40, 0x04, 0x00, 0xa0, 0x0a);
	dsi_dcs_write_seq(ctx, 0xd8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x3a, 0x00, 0x3a, 0x00, 0x3a, 0x00, 0x3a, 0x00, 0x3a, 0x05,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a,
			       0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			       0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x32, 0x00, 0x0a,
			       0x00, 0x22);
	dsi_dcs_write_seq(ctx, 0xdf, 0x50, 0x42, 0x58, 0x81, 0x2d, 0x00, 0x00, 0x00, 0x00,
			       0x00, 0x00, 0x6b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			       0x00, 0x01, 0x0f, 0xff, 0xd4, 0x0e, 0x00, 0x00, 0x00, 0x00,
			       0x00, 0x00, 0x0f, 0x53, 0xf1, 0x00, 0x00, 0x00, 0x00, 0x00,
			       0x00, 0x00, 0x00);
	dsi_dcs_write_seq(ctx, 0xf7, 0x01);
	dsi_dcs_write_seq(ctx, 0xb0, 0x80);
	dsi_dcs_write_seq(ctx, 0xe4, 0x34, 0xb4, 0x00, 0x00, 0x00, 0x39, 0x04, 0x09, 0x34);
	dsi_dcs_write_seq(ctx, 0xe6, 0x00);
	dsi_dcs_write_seq(ctx, 0xb0, 0x04);
	dsi_dcs_write_seq(ctx, 0xdf, 0x50, 0x40);
	dsi_dcs_write_seq(ctx, 0xf3, 0x50, 0x00, 0x00, 0x00, 0x00);
	dsi_dcs_write_seq(ctx, 0xf2, 0x11);
	dsi_dcs_write_seq(ctx, 0xf3, 0x01, 0x00, 0x00, 0x00, 0x01);
	dsi_dcs_write_seq(ctx, 0xf4, 0x00, 0x02);
	dsi_dcs_write_seq(ctx, 0xf2, 0x19);
	dsi_dcs_write_seq(ctx, 0xdf, 0x50, 0x42);
	dsi_dcs_write_seq(ctx, 0x2a, 0x00, 0x00, 0x04, 0x37);
	dsi_dcs_write_seq(ctx, 0x2b, 0x00, 0x00, 0x09, 0x23);
	msleep(1);
	dsi_dcs_write_seq(ctx, MIPI_DCS_EXIT_SLEEP_MODE);
	dsi_dcs_write_seq(ctx, MIPI_DCS_SET_DISPLAY_ON);
	msleep(30);

	return 0;

err:
	r66451_unprepare(panel);
	return ret;
}

static int r66451_enable(struct drm_panel *panel)
{
	return 0;
}

static int r66451_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &default_mode);
	if (!mode) {
		dev_err(panel->dev, "failed to add mode %ux%ux@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			drm_mode_vrefresh(&default_mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = 69;
	connector->display_info.height_mm = 149;

	return 1;
}

static const struct drm_panel_funcs r66451_drm_funcs = {
	.disable = r66451_disable,
	.unprepare = r66451_unprepare,
	.prepare = r66451_prepare,
	.enable = r66451_enable,
	.get_modes = r66451_get_modes,
};

static int r66451_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct r66451_panel *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(struct r66451_panel), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_VIDEO_HFP | MIPI_DSI_MODE_VIDEO_HBP |
			  MIPI_DSI_MODE_VIDEO_HSA | MIPI_DSI_MODE_EOT_PACKET |
			  MIPI_DSI_MODE_VSYNC_FLUSH | MIPI_DSI_MODE_VIDEO_AUTO_VERT;

	ctx->supplies[0].supply = "vdd3";
	ctx->supplies[1].supply = "vci";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies),
				      ctx->supplies);
	if (ret < 0) {
		dev_err(dev, "failed to get regulators: %d\n", ret);
		return ret;
	}

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(dev, "cannot get reset-gpios %ld\n",
			PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}

	drm_panel_init(&ctx->panel, dev, &r66451_drm_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0)
		drm_panel_remove(&ctx->panel);

	return ret;
}

static int r66451_remove(struct mipi_dsi_device *dsi)
{
	struct r66451_panel *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);

	return 0;
}

static const struct of_device_id r66451_of_match[] = {
	{ .compatible = "visionox,r66451" },
	{ }
};
MODULE_DEVICE_TABLE(of, r66451_of_match);

static struct mipi_dsi_driver r66451_driver = {
	.probe = r66451_probe,
	.remove = r66451_remove,
	.driver = {
		.name = "panel-visionox-r66451",
		.of_match_table = r66451_of_match,
	},
};
module_mipi_dsi_driver(r66451_driver);

MODULE_AUTHOR("Robert Foss <robert.foss@linaro.org>");
MODULE_DESCRIPTION("MIPI-DSI based r66451 AMOLED Panel Driver");
MODULE_LICENSE("GPL v2");
