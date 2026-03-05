// SPDX-License-Identifier: GPL-2.0-only
/*
 * Chipone ICNA35XX Driver IC panels driver
 *
 * Copyright (c) 2025 Teguh Sobirin <teguh@sobir.in>
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct panel_info {
	struct drm_panel panel;
	struct drm_connector *connector;
	struct mipi_dsi_device *dsi;
	struct panel_desc *desc;
	enum drm_panel_orientation orientation;

	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data *supplies;
};

struct panel_desc {
	unsigned int width_mm;
	unsigned int height_mm;

	unsigned int bpc;
	unsigned int lanes;
	unsigned long mode_flags;
	enum mipi_dsi_pixel_format format;

	const struct drm_display_mode *modes;
	unsigned int num_modes;
	int (*init_sequence)(struct panel_info *pinfo);

	struct drm_dsc_config dsc;
};

static const struct regulator_bulk_data panel_supplies[] = {
	{ .supply = "vdd" },
	{ .supply = "vddio" },
	{ .supply = "vci" },
	{ .supply = "disp" },
	{ .supply = "blvdd" },
};

static inline struct panel_info *to_panel_info(struct drm_panel *panel)
{
	return container_of(panel, struct panel_info, panel);
}

static int icna35xx_get_current_mode(struct panel_info *pinfo)
{
	struct drm_connector *connector = pinfo->connector;
	struct drm_crtc_state *crtc_state;
	int i;

	/* Return the default (first) mode if no info available yet */
	if (!connector->state || !connector->state->crtc)
		return 0;

	crtc_state = connector->state->crtc->state;

	for (i = 0; i < pinfo->desc->num_modes; i++) {
		if (drm_mode_match(&crtc_state->mode,
				   &pinfo->desc->modes[i],
				   DRM_MODE_MATCH_TIMINGS | DRM_MODE_MATCH_CLOCK))
			return i;
	}

	return 0;
}

static int icna3512_init_sequence(struct panel_info *pinfo)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = pinfo->dsi };
	struct drm_dsc_picture_parameter_set pps;

	int cur_mode = icna35xx_get_current_mode(pinfo);
	int cur_vrefresh = drm_mode_vrefresh(&pinfo->desc->modes[cur_mode]);

	pinfo->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x9C, 0xA5, 0xA5); // Manufacture Command Access
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xFD, 0x5A, 0x5A); // Manufacture Command Access Unlock

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x9F, 0x07); // REG SEL - Group 7
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xCE, 0x22);

	mipi_dsi_msleep(&dsi_ctx, 120);

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x9F, 0x01); // REG SEL - Group 1
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xC6, 0x11, 0x88); // DSC CFG
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xC7, 0x12, 0x00, 0x00, 0xAB, 0x10, 0xA0, 0x07, 0x80, 0x04, 0x38, 0x00, 0x14, 0x04, 0x38, 0x05, 0x46, 0x01, 0x9A, 0x02, 0xD4, 0x00, 0x19, 0x02, 0x40, 0x00, 0x15, 0x00, 0x0D, 0x05, 0x7A, 0x03, 0x1D); // DSC PPS 1-1
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xC8, 0x16, 0x00, 0x10, 0xEC, 0x07, 0x10, 0x20, 0x00, 0x06, 0x0F, 0x0F, 0x33, 0x0E, 0x1C, 0x2A, 0x38, 0x46, 0x54, 0x62, 0x69, 0x70, 0x77, 0x79, 0x7B, 0x7D, 0x7E, 0x01, 0xC2, 0x22, 0x00, 0x2A, 0x40); // DSC PPS 1-2
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xC9, 0x32, 0xBE, 0x3A, 0xFC, 0x3A, 0xFA, 0x3A, 0xF8, 0x3B, 0x38, 0x3B, 0x78, 0x3B, 0x76, 0x4B, 0xB6, 0x4B, 0xB6, 0x4B, 0xF4, 0x5B, 0xF4, 0x7C, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00); // DSC PPS 1-3

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x9F, 0x07); // REG SEL - Group 7
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xE5, 0x01, 0x85, 0x17, 0x85, 0x17, 0x85, 0x17, 0x85, 0x17, 0x85, 0x17, 0x85, 0x17, 0x85, 0x17, 0x85, 0x17); // PWM HBM Area Control

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x9F, 0x05); // REG SEL - Group 5
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xB3, 0x82, 0x00, 0x00, 0x99, 0x99, 0x09, 0x99, 0x00, 0x3E, 0xFE); // BC Control 2

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x9F, 0x0E); // REG SEL - Group 14
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xB2, 0x70, 0x76, 0x04); // CE Control
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xB3, 0x41, 0xA4, 0x0A, 0x17, 0x14); // EDGE Control
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xB4, 0x31); // Contrast Control
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xB5, 0x61); // HDR Control
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xB6, 0x01); // HDR Dimming Control
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xB7, 0x61, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20); // SLR Control
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xD6, 0x14, 0x24, 0x08); // PAPERMODE

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x9F, 0x0F); // REG SEL - Group 15
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xCE, 0x52);

	if (cur_vrefresh == 165) {
		mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x48, 0x20); // Write DSI MODE - HFR_MODE = HF2 Mode, DSI_MODE = Command Mode - Through GRAM
	} else {
		mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x9F, 0x01); // REG SEL - Group 1
		mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xB3, // Timing Control1
										 0x00, 0xE0, 0xA0, 0x10, 0xC8, 0x00);

		mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x9F, 0x07); // REG SEL - Group 7
		mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xB2,		// PWM Control 1
										 0x04, 0x18, 0x08, 0x0C, 0x02, 0x00, 0xC4);
		mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xD3, // PWM HMD WD1 MODE0
										 0x88, 0x4A, 0x4A, 0x88, 0x4A, 0x4A, 0x00, 0xEB,
										 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
		mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xCB, // PWM HMD Control 1
										 0x01, 0x01, 0x01, 0x01, 0x04, 0x09, 0x2C);
		mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x48, 0x00); // Write DSI MODE - HFR_MODE = Normal Mode, DSI_MODE = Command Mode - Through GRAM
	}

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x53, 0xE0); // Write Control Display
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x35, 0x00); // Write Tearing Effect Line On

	drm_dsc_pps_payload_pack(&pps, &pinfo->desc->dsc);
	mipi_dsi_picture_parameter_set_multi(&dsi_ctx, &pps);

	mipi_dsi_msleep(&dsi_ctx, 20);

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	return dsi_ctx.accum_err;
}

static int icna3520_init_sequence(struct panel_info *pinfo)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = pinfo->dsi };
	struct drm_dsc_picture_parameter_set pps;

	int cur_mode = icna35xx_get_current_mode(pinfo);
	int cur_vrefresh = drm_mode_vrefresh(&pinfo->desc->modes[cur_mode]);

	pinfo->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x9C, 0xA5, 0xA5);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xFD, 0x5A, 0x5A);

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x53, 0xE0);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x35, 0x00);

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);

	mipi_dsi_msleep(&dsi_ctx, 120);

	if (cur_vrefresh == 165) {
		mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x48, 0x00);
		mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x9F, 0x00);
		mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xB3,
			0x00, 0xD8, 0x00, 0x1C, 0x00, 0x4C);
	} else {
		mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x48, 0x10);
		mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x9F, 0x00);
		mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xB3,
			0x00, 0xDB, 0x00, 0x1C, 0x00, 0x1C, 0x00, 0x00,
			0xDB, 0x00, 0x1C, 0x07, 0xD6, 0x00);
	}

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x9F, 0x01);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xB2, 0x00);

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x9F, 0x0D);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xB2, 0x27);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xB6, 0x03);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xBB, 0x01);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xB2, 0x24);

	drm_dsc_pps_payload_pack(&pps, &pinfo->desc->dsc);
	mipi_dsi_picture_parameter_set_multi(&dsi_ctx, &pps);

	mipi_dsi_msleep(&dsi_ctx, 20);

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	return dsi_ctx.accum_err;
}

static const struct drm_display_mode odin2portal_modes[] = {
	{
		/* 165Hz */
		.clock = (1080 + 98 + 1 + 23) * (1920 + 20 + 1 + 15) * 165 / 1000,
		.hdisplay = 1080,
		.hsync_start = 1080 + 98,
		.hsync_end = 1080 + 98 + 1,
		.htotal = 1080 + 98 + 1 + 23,
		.vdisplay = 1920,
		.vsync_start = 1920 + 20,
		.vsync_end = 1920 + 20 + 1,
		.vtotal = 1920 + 20 + 1 + 15,
	},
	{
		/* 60Hz */
		.clock = (1080 + 156 + 1 + 23) * (1920 + 2760 + 1 + 15) * 60 / 1000,
		.hdisplay = 1080,
		.hsync_start = 1080 + 156,
		.hsync_end = 1080 + 156 + 1,
		.htotal = 1080 + 156 + 1 + 23,
		.vdisplay = 1920,
		.vsync_start = 1920 + 2760,
		.vsync_end = 1920 + 2760 + 1,
		.vtotal = 1920 + 2760 + 1 + 15,
	}
};

static const struct drm_display_mode thor_top_modes[] = {
	{
		/* 120Hz */
		.clock = (1080 + 24 + 1 + 24) * (1920 + 28 + 1 + 28) * 120 / 1000,
		.hdisplay = 1080,
		.hsync_start = 1080 + 24,
		.hsync_end = 1080 + 24 + 1,
		.htotal = 1080 + 24 + 1 + 24,
		.vdisplay = 1920,
		.vsync_start = 1920 + 28,
		.vsync_end = 1920 + 28 + 1,
		.vtotal = 1920 + 28 + 1 + 28,
	},
	{
		/* 60Hz */
		.clock = (1080 + 24 + 1 + 24) * (1920 + 2006 + 1 + 28) * 60 / 1000,
		.hdisplay = 1080,
		.hsync_start = 1080 + 24,
		.hsync_end = 1080 + 24 + 1,
		.htotal = 1080 + 24 + 1 + 24,
		.vdisplay = 1920,
		.vsync_start = 1920 + 2006,
		.vsync_end = 1920 + 2006 + 1,
		.vtotal = 1920 + 2006 + 1 + 28,
	}
};

static struct panel_desc odin2portal_desc = {
	.modes = odin2portal_modes,
	.num_modes = ARRAY_SIZE(odin2portal_modes),
	.width_mm = 160,
	.height_mm = 89,
	.bpc = 8,
	.lanes = 4,
	.format = MIPI_DSI_FMT_RGB888,
	.mode_flags = MIPI_DSI_MODE_NO_EOT_PACKET | MIPI_DSI_CLOCK_NON_CONTINUOUS |
			MIPI_DSI_MODE_LPM,
	.init_sequence = icna3512_init_sequence,
	.dsc = {
		.dsc_version_major = 0x1,
		.dsc_version_minor = 0x2,
		.slice_height = 20,
		.slice_width = 1080,
		.slice_count = 2,
		.bits_per_component = 10,
		.bits_per_pixel = 10 << 4,
		.block_pred_enable = false,
	},
};

static struct panel_desc thor_top_desc = {
	.modes = thor_top_modes,
	.num_modes = ARRAY_SIZE(thor_top_modes),
	.width_mm = 136,
	.height_mm = 68,
	.bpc = 8,
	.lanes = 4,
	.format = MIPI_DSI_FMT_RGB888,
	.mode_flags =  MIPI_DSI_MODE_NO_EOT_PACKET | MIPI_DSI_CLOCK_NON_CONTINUOUS |
			MIPI_DSI_MODE_LPM,
	.init_sequence = icna3520_init_sequence,
	.dsc = {
		.dsc_version_major = 0x1,
		.dsc_version_minor = 0x1,
		.slice_height = 12,
		.slice_width = 540,
		.slice_count = 2,
		.bits_per_component = 8,
		.bits_per_pixel = 8 << 4,
		.block_pred_enable = true,
	},
};

static void icna35xx_reset(struct panel_info *pinfo)
{
	gpiod_set_value_cansleep(pinfo->reset_gpio, 0);
	usleep_range(20000, 21000);
	gpiod_set_value_cansleep(pinfo->reset_gpio, 1);
	usleep_range(20000, 21000);
	gpiod_set_value_cansleep(pinfo->reset_gpio, 0);
	usleep_range(20000, 21000);
}

static int icna35xx_prepare(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(panel_supplies), pinfo->supplies);
	if (ret < 0) {
		dev_err(panel->dev, "failed to enable regulators: %d\n", ret);
		return ret;
	}

	icna35xx_reset(pinfo);

	ret = pinfo->desc->init_sequence(pinfo);
	if (ret < 0) {
		regulator_bulk_disable(ARRAY_SIZE(panel_supplies), pinfo->supplies);
		dev_err(panel->dev, "failed to initialize panel: %d\n", ret);
		return ret;
	}

	return 0;
}

static int icna35xx_disable(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = pinfo->dsi };

	pinfo->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 50);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	return dsi_ctx.accum_err;
}

static int icna35xx_unprepare(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);

	gpiod_set_value_cansleep(pinfo->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(panel_supplies), pinfo->supplies);

	return 0;
}

static void icna35xx_remove(struct mipi_dsi_device *dsi)
{
	struct panel_info *pinfo = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(pinfo->dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&pinfo->panel);
}

static int icna35xx_get_modes(struct drm_panel *panel,
			       struct drm_connector *connector)
{
	struct panel_info *pinfo = to_panel_info(panel);
	int i;

	for (i = 0; i < pinfo->desc->num_modes; i++) {
		const struct drm_display_mode *m = &pinfo->desc->modes[i];
		struct drm_display_mode *mode;

		mode = drm_mode_duplicate(connector->dev, m);
		if (!mode) {
			dev_err(panel->dev, "failed to add mode %ux%u@%u\n",
				m->hdisplay, m->vdisplay, drm_mode_vrefresh(m));
			return -ENOMEM;
		}

		mode->type = DRM_MODE_TYPE_DRIVER;
		if (i == 0)
			mode->type |= DRM_MODE_TYPE_PREFERRED;

		drm_mode_set_name(mode);
		drm_mode_probed_add(connector, mode);
	}

	connector->display_info.width_mm = pinfo->desc->width_mm;
	connector->display_info.height_mm = pinfo->desc->height_mm;
	connector->display_info.bpc = pinfo->desc->bpc;
	pinfo->connector = connector;

	return pinfo->desc->num_modes;
}

static enum drm_panel_orientation icna35xx_get_orientation(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);

	return pinfo->orientation;
}

static const struct drm_panel_funcs icna35xx_panel_funcs = {
	.disable = icna35xx_disable,
	.prepare = icna35xx_prepare,
	.unprepare = icna35xx_unprepare,
	.get_modes = icna35xx_get_modes,
	.get_orientation = icna35xx_get_orientation,
};

static int icna35xx_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return 0;
}

static int icna35xx_bl_get_brightness(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_get_display_brightness_large(dsi, &brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return brightness;
}

static const struct backlight_ops icna35xx_bl_ops = {
	.update_status = icna35xx_bl_update_status,
	.get_brightness = icna35xx_bl_get_brightness,
};

static struct backlight_device *icna35xx_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 4096,
		.max_brightness = 4096,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &icna35xx_bl_ops, &props);
}

static int icna35xx_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct panel_info *pinfo;
	int ret;

	pinfo = devm_kzalloc(dev, sizeof(*pinfo), GFP_KERNEL);
	if (!pinfo)
		return -ENOMEM;

	ret = devm_regulator_bulk_get_const(dev, ARRAY_SIZE(panel_supplies),
	panel_supplies, &pinfo->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get regulators\n");

	pinfo->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(pinfo->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(pinfo->reset_gpio), "failed to get reset gpio\n");

	pinfo->desc = (struct panel_desc *)of_device_get_match_data(dev);
	if (!pinfo->desc)
		return -ENODEV;

	pinfo->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, pinfo);
	drm_panel_init(&pinfo->panel, dev, &icna35xx_panel_funcs, DRM_MODE_CONNECTOR_DSI);

	ret = of_drm_get_panel_orientation(dev->of_node, &pinfo->orientation);
	if (ret < 0) {
		dev_err(dev, "%pOF: failed to get orientation %d\n", dev->of_node, ret);
		return ret;
	}

	pinfo->panel.prepare_prev_first = true;

	pinfo->panel.backlight = icna35xx_create_backlight(dsi);
	if (IS_ERR(pinfo->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(pinfo->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&pinfo->panel);

	pinfo->dsi->lanes = pinfo->desc->lanes;
	pinfo->dsi->format = pinfo->desc->format;
	pinfo->dsi->mode_flags = pinfo->desc->mode_flags;
	pinfo->dsi->dsc = &pinfo->desc->dsc;

	ret = mipi_dsi_attach(pinfo->dsi);
	if (ret < 0) {
		dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
		drm_panel_remove(&pinfo->panel);
		return ret;
	}

	return 0;
}

static const struct of_device_id icna35xx_of_match[] = {
	{ .compatible = "ayntec,odin2portal-panel", .data = &odin2portal_desc },
	{ .compatible = "ayntec,thor-panel-top", .data = &thor_top_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, icna35xx_of_match);

static struct mipi_dsi_driver icna35xx_driver = {
	.probe = icna35xx_probe,
	.remove = icna35xx_remove,
	.driver = {
		.name = "panel-chipone-icna35xx",
		.of_match_table = icna35xx_of_match,
	},
};
module_mipi_dsi_driver(icna35xx_driver);

MODULE_AUTHOR("Teguh Sobirin <teguh@sobir.in>");
MODULE_DESCRIPTION("DRM driver for Chipone ICNA35XX based MIPI DSI panels");
MODULE_LICENSE("GPL");
