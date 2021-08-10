// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Paul Cercueil <paul@crapouillou.net>
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/of_device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#include <video/mipi_display.h>

struct ili9331_panel_info {
	const struct drm_display_mode *display_modes;
	unsigned int num_modes;
	unsigned int lanes;
	u16 width_mm, height_mm;
	u32 bus_format, bus_flags;
};

struct ili9331 {
	struct drm_panel	panel;
	struct mipi_dsi_device	*dsi;
	const struct ili9331_panel_info *panel_info;

	struct backlight_device *backlight;
	struct gpio_desc	*reset_gpiod;
	struct gpio_desc	*cs_gpiod;
};

struct ili9331_instr {
	u8 cmd;
	unsigned int payload_size;
	const u8 *payload;
};

#define ILI9331_CMD(_cmd, ...)					\
{ .cmd = _cmd,							\
  .payload_size = ARRAY_SIZE(((const u8 []){__VA_ARGS__})),	\
  .payload = (const u8 []){__VA_ARGS__}				\
}
static const struct ili9331_instr ili9331_init[] = {
	ILI9331_CMD(0xe7, 0x10, 0x14),
	ILI9331_CMD(0x01, 0x00, 0x00),
	ILI9331_CMD(0x02, 0x02, 0x00),
	ILI9331_CMD(0x03, 0x10, 0x48),
	ILI9331_CMD(0x08, 0x02, 0x02),
	ILI9331_CMD(0x09, 0x00, 0x00),
	ILI9331_CMD(0x0a, 0x00, 0x00),
	ILI9331_CMD(0x0c, 0x00, 0x00),
	ILI9331_CMD(0x0d, 0x00, 0x00),
	ILI9331_CMD(0x0f, 0x00, 0x00),
	ILI9331_CMD(0x10, 0x00, 0x00),
	ILI9331_CMD(0x11, 0x00, 0x07),
	ILI9331_CMD(0x12, 0x00, 0x00),
	ILI9331_CMD(0x13, 0x00, 0x00),
	ILI9331_CMD(0xff, 100), // sleep
	ILI9331_CMD(0x10, 0x16, 0x90),
	ILI9331_CMD(0x11, 0x02, 0x24),
	ILI9331_CMD(0xff, 50), // sleep
	ILI9331_CMD(0x12, 0x00, 0x1f),
	ILI9331_CMD(0xff, 50), // sleep
	ILI9331_CMD(0x13, 0x05, 0x00),
	ILI9331_CMD(0x29, 0x00, 0x0c),
	ILI9331_CMD(0x2b, 0x00, 0x0d),
	ILI9331_CMD(0xff, 50), // sleep
	ILI9331_CMD(0x30, 0x00, 0x00),
	ILI9331_CMD(0x31, 0x01, 0x06),
	ILI9331_CMD(0x32, 0x00, 0x00),
	ILI9331_CMD(0x35, 0x02, 0x04),
	ILI9331_CMD(0x36, 0x16, 0x0a),
	ILI9331_CMD(0x37, 0x07, 0x07),
	ILI9331_CMD(0x38, 0x01, 0x06),
	ILI9331_CMD(0x39, 0x07, 0x06),
	ILI9331_CMD(0x3c, 0x04, 0x02),
	ILI9331_CMD(0x3d, 0x0c, 0x0f),
	ILI9331_CMD(0x50, 0x00, 0x00),
	ILI9331_CMD(0x51, 0x00, 0xef),
	ILI9331_CMD(0x52, 0x00, 0x00),
	ILI9331_CMD(0x53, 0x01, 0x3f),
	ILI9331_CMD(0x20, 0x00, 0x00),
	ILI9331_CMD(0x21, 0x00, 0x00),
	ILI9331_CMD(0x60, 0x27, 0x00),
	ILI9331_CMD(0x61, 0x00, 0x01),
	ILI9331_CMD(0x6a, 0x00, 0x00),
	ILI9331_CMD(0x80, 0x00, 0x00),
	ILI9331_CMD(0x81, 0x00, 0x00),
	ILI9331_CMD(0x82, 0x00, 0x00),
	ILI9331_CMD(0x83, 0x00, 0x00),
	ILI9331_CMD(0x84, 0x00, 0x00),
	ILI9331_CMD(0x85, 0x00, 0x00),
	ILI9331_CMD(0x20, 0x00, 0xef),
	ILI9331_CMD(0x21, 0x01, 0x90),
	ILI9331_CMD(0x90, 0x00, 0x10),
	ILI9331_CMD(0x92, 0x06, 0x00),
	ILI9331_CMD(0x07, 0x01, 0x33),
	ILI9331_CMD(0x22),
};

static inline struct ili9331 *panel_to_ili9331(struct drm_panel *panel)
{
	return container_of(panel, struct ili9331, panel);
}

static int ili9331_prepare(struct drm_panel *panel)
{
	struct ili9331 *priv = panel_to_ili9331(panel);
	unsigned int i;
	int ret;
	pr_info("ili9331 prepare");

	gpiod_set_value(priv->reset_gpiod, 0);
	msleep(10);
	gpiod_set_value(priv->reset_gpiod, 1);
	msleep(100);
	gpiod_set_value(priv->cs_gpiod, 0);

	for (i = 0; i < ARRAY_SIZE(ili9331_init); i++) {
		const struct ili9331_instr *instr = &ili9331_init[i];

		ret = mipi_dsi_dcs_write(priv->dsi, instr->cmd,
					 instr->payload, instr->payload_size);
		if (ret < 0)
			goto out_err;
	}

	return 0;

out_err:
	dev_err(&priv->dsi->dev, "Unable to prepare: %i\n", ret);

	return ret;
}

static int ili9331_enable(struct drm_panel *panel)
{
	struct ili9331 *priv = panel_to_ili9331(panel);
	pr_info("ili9331 enable");

	backlight_enable(priv->backlight);

	return 0;
}

static int ili9331_disable(struct drm_panel *panel)
{
	struct ili9331 *priv = panel_to_ili9331(panel);
	pr_info("ili9331 disable");

	backlight_disable(priv->backlight);

	return 0;
}

static int ili9331_unprepare(struct drm_panel *panel)
{
	struct ili9331 *priv = panel_to_ili9331(panel);
	pr_info("ili9331 unprepare");

	gpiod_set_value(priv->reset_gpiod, 0);
	gpiod_set_value(priv->cs_gpiod, 1);

	return 0;
}

static const struct drm_display_mode ili9331_modes[] = {
	{ /* 60 Hz */
		.clock = 12000,
		.hdisplay = 320,
		.hsync_start = 320 + 30,
		.hsync_end = 320 + 30 + 20,
		.htotal = 320 + 30 + 20 + 30,
		.vdisplay = 240,
		.vsync_start = 240 + 20,
		.vsync_end = 240 + 20 + 20,
		.vtotal	= 240 + 20 + 20 + 20,
	},
};

static int ili9331_get_modes(struct drm_panel *panel, 
			     struct drm_connector *connector)
{
	struct ili9331 *priv = panel_to_ili9331(panel);
	const struct ili9331_panel_info *panel_info = priv->panel_info;
	struct drm_display_mode *mode;
	unsigned int i;
	pr_info("ili9331 get_modes");

	for (i = 0; i < panel_info->num_modes; i++) {
		mode = drm_mode_duplicate(connector->dev,
					  &panel_info->display_modes[i]);
		if (!mode) 
			return -ENOMEM;

		drm_mode_set_name(mode);

		mode->type = DRM_MODE_TYPE_DRIVER;
		if (panel_info->num_modes == 1)
			mode->type |= DRM_MODE_TYPE_PREFERRED;

		drm_mode_probed_add(connector, mode);
	}

	connector->display_info.bpc = 8;
	connector->display_info.width_mm = panel_info->width_mm;
	connector->display_info.height_mm = panel_info->height_mm;

	drm_display_info_set_bus_formats(&connector->display_info, 
					 &panel_info->bus_format, 1);
	connector->display_info.bus_flags = panel_info->bus_flags;

	return panel_info->num_modes;
}

static const struct drm_panel_funcs ili9331_funcs = {
	.prepare	= ili9331_prepare,
	.unprepare	= ili9331_unprepare,
	.enable		= ili9331_enable,
	.disable	= ili9331_disable,
	.get_modes	= ili9331_get_modes,
};

static int ili9331_dsi_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct ili9331 *priv;
	int ret;

	priv = devm_kzalloc(&dsi->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	mipi_dsi_set_drvdata(dsi, priv);
	priv->dsi = dsi;

	priv->panel_info = of_device_get_match_data(dev);
        if (!priv->panel_info)
                return -EINVAL;

	priv->reset_gpiod = devm_gpiod_get(&dsi->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->reset_gpiod)) {
		dev_err(&dsi->dev, "Couldn't get our reset GPIO\n");
		return PTR_ERR(priv->reset_gpiod);
	}

	priv->cs_gpiod = devm_gpiod_get(&dsi->dev, "cs", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->cs_gpiod)) {
		dev_err(&dsi->dev, "Couldn't get our cs GPIO\n");
		return PTR_ERR(priv->cs_gpiod);
	}

	priv->backlight = devm_of_find_backlight(&dsi->dev);
	if (IS_ERR(priv->backlight)) {
		ret = PTR_ERR(priv->backlight);
		if (ret != -EPROBE_DEFER)
			dev_err(&dsi->dev, "Failed to get backlight handle");
		return ret;
	}

	drm_panel_init(&priv->panel, dev, &ili9331_funcs,
		       DRM_MODE_CONNECTOR_DPI);

	drm_panel_add(&priv->panel);
	if (ret < 0)
		return ret;

	dsi->mode_flags = MIPI_DSI_MODE_VIDEO_SYNC_PULSE;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->lanes = 4;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0)
		return ret;

	dev_notice(&dsi->dev, "ili9331 probed");

	return 0;
}

static int ili9331_dsi_remove(struct mipi_dsi_device *dsi)
{
	struct ili9331 *priv = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&priv->panel);

	if (priv->backlight)
		put_device(&priv->backlight->dev);

	return 0;
}

static const struct ili9331_panel_info ili9331_panel_info = {
        .display_modes = ili9331_modes,
	.num_modes = ARRAY_SIZE(ili9331_modes),
        .width_mm = 71,
        .height_mm = 53,
	.bus_format = MEDIA_BUS_FMT_RGB565_1X16,
	.bus_flags = 0,
        .lanes = 4,
};


static const struct of_device_id ili9331_of_match[] = {
	{ .compatible = "ilitek,ili9331", .data = &ili9331_panel_info },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ili9331_of_match);

static struct mipi_dsi_driver ili9331_dsi_driver = {
	.probe		= ili9331_dsi_probe,
	.remove		= ili9331_dsi_remove,
	.driver = {
		.name		= "ili9331-dsi",
		.of_match_table	= ili9331_of_match,
	},
};
module_mipi_dsi_driver(ili9331_dsi_driver);

MODULE_AUTHOR("Paul Cercueil <paul@crapouillou.net>");
MODULE_DESCRIPTION("Ilitek ILI9331 Controller Driver");
MODULE_LICENSE("GPL v2");
