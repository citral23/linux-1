// SPDX-License-Identifier: GPL-2.0
//
// Ingenic Smart LCD driver
//
// Copyright (C) 2019, Paul Cercueil <paul@crapouillou.net>

#include "ingenic-drm.h"

#include <linux/regmap.h>

#include <drm/drm_mipi_dsi.h>

static int ingenic_slcd_send_data(struct regmap *map, u32 data, bool cmd)
{
	unsigned int val;
	int ret;

	ret = regmap_read_poll_timeout(map, JZ_REG_LCD_SLCD_MSTATE, val,
				       !(val & JZ_SLCD_MSTATE_BUSY),
				       4, USEC_PER_MSEC * 100);
	if (ret)
		return ret;

	if (cmd)
		data |= JZ_SLCD_MDATA_COMMAND;

	return regmap_write(map, JZ_REG_LCD_SLCD_MDATA, data);
}

static ssize_t ingenic_slcd_dsi_transfer(struct mipi_dsi_host *host,
					 const struct mipi_dsi_msg *msg)
{
	struct regmap *map = dev_get_regmap(host->dev, NULL);
	const u8 *buf = msg->tx_buf;
	unsigned int i;
	int ret;

	/* We only support sending messages, not receiving */
	if (msg->rx_len)
		return -ENOTSUPP;

	if (*buf == 0xff) {
		msleep(buf[1]);
		return 0;
	}

	ret = ingenic_slcd_send_data(map, *buf, true);
	if (ret) {
		dev_err(host->dev, "Unable to send command: %d", ret);
		return ret;
	}

	for (i = 1; i < msg->tx_len; i += 2) {
		ret = ingenic_slcd_send_data(map, buf[1] << 8 | buf[2], false);
		if (ret) {
			dev_err(host->dev, "Unable to send data: %d", ret);
			return ret;
		}
	}

	return msg->tx_len;
}

static int ingenic_slcd_dsi_attach(struct mipi_dsi_host *host,
				   struct mipi_dsi_device *dsi)
{
	struct regmap *map = dev_get_regmap(host->dev, NULL);

	/* Give control of LCD pins to the SLCD module */
	regmap_update_bits(map, JZ_REG_LCD_CFG,
			   JZ_LCD_CFG_SLCD, JZ_LCD_CFG_SLCD);

	/* Configure for serial transfer, 8-bit commands and 8-bit data */
	regmap_write(map, JZ_REG_LCD_SLCD_MCFG,
		     JZ_SLCD_MCFG_DWIDTH_16BIT | JZ_SLCD_MCFG_CWIDTH_8BIT);

	return 0;
}

static int ingenic_slcd_dsi_detach(struct mipi_dsi_host *host,
				   struct mipi_dsi_device *dsi)
{
	struct regmap *map = dev_get_regmap(host->dev, NULL);

	return regmap_update_bits(map, JZ_REG_LCD_CFG, JZ_LCD_CFG_SLCD, 0);
}

static const struct mipi_dsi_host_ops ingenic_slcd_dsi_ops = {
	.transfer = ingenic_slcd_dsi_transfer,
	.attach = ingenic_slcd_dsi_attach,
	.detach = ingenic_slcd_dsi_detach,
};

static void ingenic_drm_cleanup_dsi(void *d)
{
	mipi_dsi_host_unregister(d);
}

int devm_ingenic_drm_init_dsi(struct device *dev,
			      struct mipi_dsi_host *dsi_host)
{
	int ret;

	dsi_host->dev = dev;
	dsi_host->ops = &ingenic_slcd_dsi_ops;

	ret = mipi_dsi_host_register(dsi_host);
	if (ret) {
		dev_err(dev, "Unable to register DSI host");
		return ret;
	}

	return devm_add_action_or_reset(dev, ingenic_drm_cleanup_dsi, dsi_host);
}
