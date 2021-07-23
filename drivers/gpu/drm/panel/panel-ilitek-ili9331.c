// SPDX-License-Identifier: GPL-2.0+
/*
 * DRM driver for Ilitek ILI9331 panels
 *
 * Copyright 2018 David Lechner <david@lechnology.com>
 * Copyright 2020 Paul Cercueil <paul@crapouillou.net>
 * Copyright 2021 Christophe Branchereau <cbranchereau@gmail.com>
 *
 * Based on mi0283qt.c:
 * Copyright 2016 Noralf Trønnes
 */

#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of_graph.h>
#include <linux/property.h>
#include <drm/drm_atomic_helper.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <video/mipi_display.h>

struct ili9331_pdata {
	struct drm_display_mode mode;
	unsigned int width_mm;
	unsigned int height_mm;
	unsigned int bus_type;
	unsigned int lanes;
};

struct ili9331 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	const struct ili9331_pdata *pdata;

	struct gpio_desc	*reset_gpiod;
};

#define mipi_dcs_command(dsi, cmd, seq...) \
({ \
	u8 d[] = { seq }; \
	mipi_dsi_dcs_write(dsi, cmd, d, ARRAY_SIZE(d)); \
})

static inline struct ili9331 *panel_to_ili9331(struct drm_panel *panel)
{
	return container_of(panel, struct ili9331, panel);
}

static int ili9331_prepare(struct drm_panel *panel)
{
	struct ili9331 *priv = panel_to_ili9331(panel);
	struct mipi_dsi_device *dsi = priv->dsi;
	u8 addr_mode;
	int ret;

	gpiod_set_value_cansleep(priv->reset_gpiod, 0);
	usleep_range(20, 1000);
	gpiod_set_value_cansleep(priv->reset_gpiod, 1);
	msleep(120);

	ret = mipi_dcs_command(dsi, MIPI_DCS_SOFT_RESET);
	if (ret) {
		dev_err(panel->dev, "Failed to send reset command: %d\n", ret);
		return ret;
	}

	/* Wait 5ms after soft reset per MIPI DCS spec */
	usleep_range(5000, 20000);

	mipi_dcs_command(dsi, MIPI_DCS_SET_DISPLAY_OFF);

	mipi_dcs_command(dsi, 0x01, 0x0000);
	mipi_dcs_command(dsi, 0x02, 0x0200); 
        mipi_dcs_command(dsi, 0x03, 0x1048);
        mipi_dcs_command(dsi, 0x08, 0x0202);
        mipi_dcs_command(dsi, 0x09, 0x0000);
        mipi_dcs_command(dsi, 0x0A, 0x0000);
        mipi_dcs_command(dsi, 0x0C, 0x0000);
        mipi_dcs_command(dsi, 0x0D, 0x0000);
        mipi_dcs_command(dsi, 0x0F, 0x0000);
        mipi_dcs_command(dsi, 0x10, 0x0000);
        mipi_dcs_command(dsi, 0x11, 0x0007);
        mipi_dcs_command(dsi, 0x12, 0x0000);
        mipi_dcs_command(dsi, 0x13, 0x0000);
        mipi_dcs_command(dsi, 0x10, 0x1690);
        mipi_dcs_command(dsi, 0x11, 0x0224);
        mipi_dcs_command(dsi, 0x12, 0x0500);
        mipi_dcs_command(dsi, 0x13, 0x0500);
        mipi_dcs_command(dsi, 0x29, 0x000C);
        mipi_dcs_command(dsi, 0x2B, 0x000D);
        mipi_dcs_command(dsi, 0x30, 0x0000);
        mipi_dcs_command(dsi, 0x31, 0x0106);
        mipi_dcs_command(dsi, 0x32, 0x0000);
        mipi_dcs_command(dsi, 0x35, 0x0204);
        mipi_dcs_command(dsi, 0x36, 0x160A);
        mipi_dcs_command(dsi, 0x37, 0x0707);
        mipi_dcs_command(dsi, 0x38, 0x0106);
        mipi_dcs_command(dsi, 0x39, 0x0706);
        mipi_dcs_command(dsi, 0x3C, 0x0402):
        mipi_dcs_command(dsi, 0x3D, 0x0C0F);
        mipi_dcs_command(dsi, 0x50, 0x0000);
        mipi_dcs_command(dsi, 0x51, 0x00EF);
        mipi_dcs_command(dsi, 0x52, 0x0000);
        mipi_dcs_command(dsi, 0x53, 0x013F);
        mipi_dcs_command(dsi, 0x20, 0x0000);
        mipi_dcs_command(dsi, 0x21, 0x0000);
        mipi_dcs_command(dsi, 0x60, 0x2700);
        mipi_dcs_command(dsi, 0x61, 0x0001);
        mipi_dcs_command(dsi, 0x6A, 0x0000);
        mipi_dcs_command(dsi, 0x80, 0x0000);
        mipi_dcs_command(dsi, 0x81, 0x0000);
        mipi_dcs_command(dsi, 0x82, 0x0000);
        mipi_dcs_command(dsi, 0x83, 0x0000);
        mipi_dcs_command(dsi, 0x84, 0x0000);
        mipi_dcs_command(dsi, 0x85, 0x0000);
        mipi_dcs_command(dsi, 0x20, 0x00EF);
        mipi_dcs_command(dsi, 0x21, 0x0190);
        mipi_dcs_command(dsi, 0x90, 0x0010);
        mipi_dcs_command(dsi, 0x92, 0x0600);

	mipi_dcs_command(dsi, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(100);

	mipi_dcs_command(dsi, MIPI_DCS_SET_DISPLAY_ON);
	msleep(100);

	return 0;
}

static int ili9331_unprepare(struct drm_panel *panel)
{
	struct ili9331 *priv = panel_to_ili9331(panel);

	mipi_dcs_command(priv->dsi, MIPI_DCS_SET_DISPLAY_OFF);

	return 0;
}

static int ili9331_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	struct ili9331 *priv = panel_to_ili9331(panel);
	struct drm_display_mode *mode;
	u32 format = MEDIA_BUS_FMT_RGB565_1X16;

	mode = drm_mode_duplicate(connector->dev, &priv->pdata->mode);
	if (!mode) {
		dev_err(panel->dev, "failed to add mode %ux%u\n",
			priv->pdata->mode.hdisplay, priv->pdata->mode.vdisplay);
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	connector->display_info.bpc = 8;
	connector->display_info.width_mm = priv->pdata->width_mm;
	connector->display_info.height_mm = priv->pdata->height_mm;

	drm_display_info_set_bus_formats(&connector->display_info, &format, 1);
	connector->display_info.bus_flags = DRM_BUS_FLAG_PIXDATA_SAMPLE_POSEDGE;

	return 1;
}

static const struct drm_panel_funcs ili9331_funcs = {
	.prepare	= ili9331_prepare,
	.unprepare	= ili9331_unprepare,
	.get_modes	= ili9331_get_modes,
};

static int ili9331_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct ili9331 *priv;
	int ret;

	/* See comment for mipi_dbi_spi_init() */
	if (!dev->coherent_dma_mask) {
		ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_warn(dev, "Failed to set dma mask %d\n", ret);
			return ret;
		}
	}

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, priv);
	priv->dsi = dsi;

	priv->pdata = device_get_match_data(dev);
	if (!priv->pdata)
		return -EINVAL;

	drm_panel_init(&priv->panel, dev, &ili9331_funcs,
		       DRM_MODE_CONNECTOR_DPI);

	priv->reset_gpiod = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->reset_gpiod)) {
		dev_err(dev, "Couldn't get our reset GPIO\n");
		return PTR_ERR(priv->reset_gpiod);
	}

	ret = drm_panel_of_backlight(&priv->panel);
	if (ret < 0) {
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get backlight handle\n");
		return ret;
	}

	drm_panel_add(&priv->panel);

	dsi->bus_type = priv->pdata->bus_type;
	dsi->lanes = priv->pdata->lanes;
	dsi->format = MIPI_DSI_FMT_RGB565;

	ret = mipi_dsi_attach(dsi);
	if (ret) {
		dev_err(dev, "Failed to attach DSI panel\n");
		goto err_panel_remove;
	}

	ret = mipi_dsi_maybe_register_tiny_driver(dsi);
	if (ret) {
		dev_err(dev, "Failed to init TinyDRM driver\n");
		goto err_mipi_dsi_detach;
	}

	return 0;

err_mipi_dsi_detach:
	mipi_dsi_detach(dsi);
err_panel_remove:
	drm_panel_remove(&priv->panel);
	return ret;
}

static int ili9331_remove(struct mipi_dsi_device *dsi)
{
	struct ili9331 *priv = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&priv->panel);

	drm_panel_disable(&priv->panel);
	drm_panel_unprepare(&priv->panel);

	return 0;
}

static const struct ili9331_pdata yx240qv29_pdata = {
	.mode = { DRM_SIMPLE_MODE(240, 320, 37, 49) },
	.width_mm = 0, // TODO
	.height_mm = 0, // TODO
	.bus_type = MIPI_DCS_BUS_TYPE_DBI_SPI_C3,
	.lanes = 1,
};

static const struct of_device_id ili9331_of_match[] = {
	{ .compatible = "adafruit,yx240qv29", .data = &yx240qv29_pdata },
	{ }
};
MODULE_DEVICE_TABLE(of, ili9331_of_match);

static struct mipi_dsi_driver ili9331_dsi_driver = {
	.probe		= ili9331_probe,
	.remove		= ili9331_remove,
	.driver = {
		.name		= "ili9331-dsi",
		.of_match_table	= ili9331_of_match,
	},
};
module_mipi_dsi_driver(ili9331_dsi_driver);

MODULE_DESCRIPTION("Ilitek ILI9331 DRM panel driver");
MODULE_AUTHOR("David Lechner <david@lechnology.com>");
MODULE_AUTHOR("Paul Cercueil <paul@crapouillou.net>");
MODULE_LICENSE("GPL");
