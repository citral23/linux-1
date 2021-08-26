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
	pr_info("slcd : ingenic_slcd_send_data");

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
	unsigned int i;
	int ret;
	pr_info("slcd : ingenic_slcd_dsi_transfer");

	/* We only support sending messages, not receiving */
	if (msg->rx_len)
		return -ENOTSUPP;

	ret = ingenic_slcd_send_data(map, *(u8 *)msg->tx_buf, true);
	if (ret) {
		dev_err(host->dev, "Unable to send command: %d", ret);
		return ret;
	}

	for (i = 1; i < msg->tx_len; i++) {
		ret = ingenic_slcd_send_data(map, ((u8 *)msg->tx_buf)[i],
					     false);
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
	pr_info("slcd : ingenic_slcd_dsi_attach");

	/* Give control of LCD pins to the SLCD module */
	regmap_update_bits(map, JZ_REG_LCD_CFG,
			   JZ_LCD_CFG_SLCD, JZ_LCD_CFG_SLCD);

	/* Configure for parallel transfer, 8-bit commands and 8-bit data */
	regmap_write(map, JZ_REG_LCD_SLCD_MCFG,
		     JZ_SLCD_MCFG_DWIDTH_8BIT | JZ_SLCD_MCFG_CWIDTH_8BIT);

	/* Enable DMA */
	//regmap_write(map, JZ_REG_LCD_SLCD_MCTRL, JZ_SLCD_MCTRL_DMATXEN);

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
	pr_info("slcd : devm_ingenic_drm_init_dsi");

	dsi_host->dev = dev;
	dsi_host->ops = &ingenic_slcd_dsi_ops;

	pr_info("slcd : mipi_dsi_host_register");
	ret = mipi_dsi_host_register(dsi_host);
	if (ret) {
		dev_err(dev, "Unable to register DSI host");
		return ret;
	}

	return devm_add_action_or_reset(dev, ingenic_drm_cleanup_dsi, dsi_host);
}
