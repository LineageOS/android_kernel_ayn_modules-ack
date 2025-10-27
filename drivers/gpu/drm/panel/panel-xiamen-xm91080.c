// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2024, Digi International Inc.
 *
 * Author: Gonzalo Ruiz <gonzalo.ruiz@digi.com>
 */

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#include <linux/bitfield.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#define DSI_CMD_SETMIPI_1LANE		0
#define DSI_CMD_SETMIPI_2LANE		1
#define DSI_CMD_SETMIPI_3LANE		2
#define DSI_CMD_SETMIPI_4LANE		3

struct xm91080;

struct xm91080_panel_desc {
	const struct drm_display_mode *mode;
	unsigned int lanes;
	enum mipi_dsi_pixel_format format;
	unsigned int panel_sleep_delay;
	void (*gip_sequence)(struct xm91080 *xm91080);
};

struct xm91080 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	const struct xm91080_panel_desc *desc;
	struct regulator_bulk_data supplies[5];
	struct gpio_desc *reset;
	unsigned int sleep_delay;
	enum drm_panel_orientation orientation;
};

static inline struct xm91080 *panel_to_xm91080(struct drm_panel *panel)
{
	return container_of(panel, struct xm91080, panel);
}

/* General initialization Packet (GIP) sequence */
static void odin2mini_gip_sequence(struct xm91080 *xm91080)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = xm91080->dsi };

	/* Set XM Command Password 1 */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xFF, 0x10, 0x80, 0x01);

	/* Set XM Command Password 2 */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0x80);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xFF, 0x10, 0x80);

	/* boe 5.5  */
	/* tcon setting ok */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0x81);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb2, 0xa0, 0x00, 0x14, 0x00, 0x14);

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0x86);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb2, 0x04, 0x06, 0x04, 0x04, 0x23, 0x04);

	/* ckv setting */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0x80);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb4, 0x18, 0x0b, 0x07, 0x87, 0x0f, 0x00, 0x00,
						   0x02, 0x00, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0x90);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb4, 0x18, 0x0a, 0x07, 0x88, 0x0f, 0x00, 0x00,
						   0x02, 0x00, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0xa0);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb4, 0x18, 0x09, 0x07, 0x89, 0x0f, 0x00, 0x00,
						   0x02, 0x00, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0xb0);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb4, 0x18, 0x08, 0x07, 0x8a, 0x0f, 0x00, 0x00,
						   0x02, 0x00, 0x00);

	/* vst */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0x80);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb6, 0x83, 0x02, 0x00, 0x00, 0x82, 0x02, 0x00,
						   0x00);

	/* u2d ok */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0x80);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbc, 0x00, 0x00, 0x0e, 0x26, 0x25, 0x02, 0x1d,
						   0x00, 0x08, 0x06, 0x1f, 0x20, 0x21, 0x00, 0x00,
						   0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0x90);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbc, 0x00, 0x00, 0x0d, 0x26, 0x25, 0x02, 0x1d,
						   0x00, 0x07, 0x05, 0x1f, 0x20, 0x21, 0x00, 0x00,
						   0x00);

	/* d2u ok */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0xa0);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbc, 0x00, 0x00, 0x0d, 0x25, 0x26, 0x02, 0x1d,
						   0x00, 0x05, 0x07, 0x1f, 0x20, 0x21, 0x00, 0x00,
						   0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0xb0);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbc, 0x00, 0x00, 0x0e, 0x25, 0x26, 0x02, 0x1d,
						   0x00, 0x06, 0x08, 0x1f, 0x20, 0x21, 0x00, 0x00,
						   0x00);

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0xa0);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb9, 0xc0, 0xc0, 0xe4, 0xe4, 0xe4, 0xea, 0xe6,
						   0xc0, 0xe4, 0xe4, 0x54, 0xe4, 0xe4, 0xc0, 0xc0,
						   0xc0);

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0xb0);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb9, 0xc0, 0xc0, 0xe4, 0xe4, 0xe4, 0xea, 0xe6,
						   0xc0, 0xe4, 0xe4, 0x54, 0xe4, 0xe4, 0xc0, 0xc0,
						   0xc0);

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0x80);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb9, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
						   0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
						   0xff);

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0x90);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb9, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
						   0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
						   0xff);

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0x80);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xba, 0x0a, 0xaa, 0xaa, 0x80, 0x0a, 0xaa, 0xaa,
						   0x80);

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0xd0);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb6, 0x81, 0x00, 0x02, 0x02);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0xe0);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x01,
						   0x01, 0x00, 0x00);

	/* G-swap */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0xA5);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xC0, 0x20);

	/* mirror X2 */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0xA0);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xA5, 0x20); /* 20-0  ˢ  10-  ˢ */

	/* VGH=VGHO/VGL=VGLO */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0xF0);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xA4, 0x00);

	/* VGL/VGH = -8 / 9 */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0x90);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xAB, 0xA8, 0x94);

	/* GVDDP/GVDDN */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0xA0);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xA4, 0x2F, 0x2F);

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0x80);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xD4, 0x03, 0x07, 0x0F, 0x18, 0x1F, 0x23, 0x28,
						   0x2c, 0x30, 0x3c, 0x44, 0x53, 0x5E, 0x6F, 0x7E,
						   0x7E, 0x8F, 0xA2, 0xAF, 0xBE, 0xC8, 0xD4, 0xD8,
						   0xDC, 0xE1, 0xE5, 0xEA, 0xF1, 0xFB, 0xFF, 0x00,
						   0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0x80);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xD5, 0x03, 0x07, 0x0F, 0x18, 0x1F, 0x23, 0x28,
						   0x2C, 0x30, 0x3C, 0x44, 0x53, 0x5E, 0x6F, 0x7E,
						   0x7E, 0x8F, 0xA2, 0xAF, 0xBE, 0xC8, 0xD4, 0xD8,
						   0xDC, 0xE1, 0xE5, 0xEA, 0xF1, 0xFB, 0xFF, 0x00,
						   0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0x80);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xD6, 0x03, 0x07, 0x0F, 0x18, 0x1F, 0x23, 0x28,
						   0x2C, 0x30, 0x3C, 0x44, 0x53, 0x5E, 0x6F, 0x7E,
						   0x7E, 0x8F, 0xA2, 0xAF, 0xBE, 0xC8, 0xD4, 0xD8,
						   0xDC, 0xE1, 0xE5, 0xEA, 0xF1, 0xFB, 0xFF, 0x00,
						   0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0x80);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xD7, 0x03, 0x07, 0x0F, 0x18, 0x1F, 0x23, 0x28,
						   0x2C, 0x30, 0x3C, 0x44, 0x53, 0x5E, 0x6F, 0x7E,
						   0x7E, 0x8F, 0xA2, 0xAF, 0xBE, 0xC8, 0xD4, 0xD8,
						   0xDC, 0xE1, 0xE5, 0xEA, 0xF1, 0xFB, 0xFF, 0x00,
						   0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0x80);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xD8, 0x03, 0x07, 0x0F, 0x18, 0x1F, 0x23, 0x28,
						   0x2C, 0x30, 0x3C, 0x44, 0x53, 0x5E, 0x6F, 0x7E,
						   0x7E, 0x8F, 0xA2, 0xAF, 0xBE, 0xC8, 0xD4, 0xD8,
						   0xDC, 0xE1, 0xE5, 0xEA, 0xF1, 0xFB, 0xFF, 0x00,
						   0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0x80);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xD9, 0x03, 0x07, 0x0F, 0x18, 0x1F, 0x23, 0x28,
						   0x2C, 0x30, 0x3C, 0x44, 0x53, 0x5E, 0x6F, 0x7E,
						   0x7E, 0x8F, 0xA2, 0xAF, 0xBE, 0xC8, 0xD4, 0xD8,
						   0xDC, 0xE1, 0xE5, 0xEA, 0xF1, 0xFB, 0xFF, 0x00,
						   0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00);

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0x9C);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xA6, 0x90);

	/* enmode_sdpch */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0xC2);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xA6, 0x08);

	/* sd_prc */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0x86);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xA5, 0x19);

	/* mipi skew */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x00, 0x90);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xA3, 0x04, 0x04, 0x01, 0x05, 0x06, 0x00);

	/* Sleep Out */
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
}

static int xm91080_prepare(struct drm_panel *panel)
{
	struct xm91080 *xm91080 = panel_to_xm91080(panel);
	int ret;

	gpiod_set_value(xm91080->reset, 1);

	ret = regulator_bulk_enable(ARRAY_SIZE(xm91080->supplies),
				    xm91080->supplies);
	if (ret < 0)
		return ret;
	msleep(20);

	gpiod_set_value(xm91080->reset, 0);
	msleep(150);

	if (xm91080->desc->gip_sequence)
		xm91080->desc->gip_sequence(xm91080);

	return 0;
}

static int xm91080_enable(struct drm_panel *panel)
{
	struct xm91080 *xm91080 = panel_to_xm91080(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = xm91080->dsi };

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	msleep(50);

	return 0;
}

static int xm91080_disable(struct drm_panel *panel)
{
	struct xm91080 *xm91080 = panel_to_xm91080(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = xm91080->dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	msleep(50);

	return 0;
}

static int xm91080_unprepare(struct drm_panel *panel)
{
	struct xm91080 *xm91080 = panel_to_xm91080(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = xm91080->dsi };

	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);

	msleep(xm91080->sleep_delay);

	gpiod_set_value(xm91080->reset, 1);

	msleep(xm91080->sleep_delay);

	regulator_bulk_disable(ARRAY_SIZE(xm91080->supplies), xm91080->supplies);

	return 0;
}

static int xm91080_get_modes(struct drm_panel *panel,
			    struct drm_connector *connector)
{
	struct xm91080 *xm91080 = panel_to_xm91080(panel);
	const struct drm_display_mode *desc_mode = xm91080->desc->mode;
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, desc_mode);
	if (!mode) {
		dev_err(&xm91080->dsi->dev, "failed to add mode %ux%u@%u\n",
			desc_mode->hdisplay, desc_mode->vdisplay,
			drm_mode_vrefresh(desc_mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = desc_mode->width_mm;
	connector->display_info.height_mm = desc_mode->height_mm;

	/*
	 * TODO: Remove once all drm drivers call
	 * drm_connector_set_orientation_from_panel()
	 */
	drm_connector_set_panel_orientation(connector, xm91080->orientation);

	return 1;
}

static enum drm_panel_orientation xm91080_get_orientation(struct drm_panel *panel)
{
	struct xm91080 *xm91080 = panel_to_xm91080(panel);

	return xm91080->orientation;
}

static const struct drm_panel_funcs xm91080_funcs = {
	.disable		= xm91080_disable,
	.unprepare		= xm91080_unprepare,
	.prepare		= xm91080_prepare,
	.enable			= xm91080_enable,
	.get_modes		= xm91080_get_modes,
	.get_orientation	= xm91080_get_orientation,
};

static const struct drm_display_mode odin2mini_mode = {
	.clock          = (1080 + 24 + 4 + 16) * (1920 + 18 + 2 + 16) * 60 / 1000,

	.hdisplay       = 1080,
	.hsync_start    = 1080 + 24,
	.hsync_end      = 1080 + 24 + 4,
	.htotal         = 1080 + 24 + 4 + 16,

	.vdisplay       = 1920,
	.vsync_start    = 1920 + 18,
	.vsync_end      = 1920 + 18 + 2,
	.vtotal         = 1920 + 18 + 2 + 16,

	.width_mm       = 61,
	.height_mm      = 110,

	.type = DRM_MODE_TYPE_DRIVER,
};

static const struct xm91080_panel_desc odin2mini_desc = {
	.mode = &odin2mini_mode,
	.lanes = 4,
	.format = MIPI_DSI_FMT_RGB888,
	.panel_sleep_delay = 0,
	.gip_sequence = odin2mini_gip_sequence,
};

static int xm91080_dsi_probe(struct mipi_dsi_device *dsi)
{
	const struct xm91080_panel_desc *desc;
	struct xm91080 *xm91080;
	int ret;

	xm91080 = devm_kzalloc(&dsi->dev, sizeof(*xm91080), GFP_KERNEL);
	if (!xm91080)
		return -ENOMEM;

	desc = of_device_get_match_data(&dsi->dev);
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_LPM;
	dsi->format = desc->format;
	dsi->lanes = desc->lanes;

	xm91080->supplies[0].supply = "vdd";
	xm91080->supplies[1].supply = "vddio";
	xm91080->supplies[2].supply = "vci";
	xm91080->supplies[3].supply = "disp";
	xm91080->supplies[4].supply = "blvdd";

	ret = devm_regulator_bulk_get(&dsi->dev, ARRAY_SIZE(xm91080->supplies),
				      xm91080->supplies);
	if (ret < 0)
		return ret;

	xm91080->reset = devm_gpiod_get(&dsi->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(xm91080->reset)) {
		dev_err(&dsi->dev, "Couldn't get our reset GPIO\n");
		return PTR_ERR(xm91080->reset);
	}

	ret = of_drm_get_panel_orientation(dsi->dev.of_node, &xm91080->orientation);
	if (ret < 0) {
		dev_err(&dsi->dev, "Failed to get orientation\n");
		return ret;
	}

	drm_panel_init(&xm91080->panel, &dsi->dev, &xm91080_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	/**
	 * Once sleep out has been issued, XM91080 IC required to wait 120ms
	 * before initiating new commands.
	 */
	xm91080->sleep_delay = 120 + desc->panel_sleep_delay;

	ret = drm_panel_of_backlight(&xm91080->panel);
	if (ret)
		return ret;

	drm_panel_add(&xm91080->panel);

	mipi_dsi_set_drvdata(dsi, xm91080);
	xm91080->dsi = dsi;
	xm91080->desc = desc;

	ret = mipi_dsi_attach(dsi);
	if (ret)
		goto err_attach;

	return 0;

err_attach:
	drm_panel_remove(&xm91080->panel);
	return ret;
}

static void xm91080_dsi_remove(struct mipi_dsi_device *dsi)
{
	struct xm91080 *xm91080 = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&xm91080->panel);
}

static const struct of_device_id xm91080_of_match[] = {
	{ .compatible = "ayntec,odin2mini-panel", .data = &odin2mini_desc },
	{ }
};
MODULE_DEVICE_TABLE(of, xm91080_of_match);

static struct mipi_dsi_driver xm91080_dsi_driver = {
	.probe		= xm91080_dsi_probe,
	.remove		= xm91080_dsi_remove,
	.driver = {
		.name		= "xm91080",
		.of_match_table	= xm91080_of_match,
	},
};
module_mipi_dsi_driver(xm91080_dsi_driver);

MODULE_AUTHOR("Gonzalo Ruiz <gonzalo.ruiz@digi.com>");
MODULE_DESCRIPTION("Xiamen XM91080 TFT LCD Panel Driver");
MODULE_LICENSE("GPL");
