// SPDX-License-Identifier: GPL-2.0
//
// Ingenic JZ47xx KMS driver
//
// Copyright (C) 2019, Paul Cercueil <paul@crapouillou.net>

#include "ingenic-drm.h"

#include <linux/component.h>
#include <linux/clk.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_bridge_connector.h>
#include <drm/drm_color_mgmt.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_encoder.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_irq.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_managed.h>
#include <drm/drm_of.h>
#include <drm/drm_panel.h>
#include <drm/drm_plane.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

struct ingenic_dma_hwdesc {
	u32 next;
	u32 addr;
	u32 id;
	u32 cmd;
} __aligned(16);

struct ingenic_dma_hwdescs {
	struct ingenic_dma_hwdesc hwdesc[2];
	struct ingenic_dma_hwdesc hwdesc_pal;
	u16 palette[256] __aligned(16);
};

struct jz_soc_info {
	bool needs_dev_clk;
	bool has_osd;
	bool map_noncoherent;
	unsigned int max_width, max_height;
	const u32 *formats_f0, *formats_f1;
	unsigned int num_formats_f0, num_formats_f1;
};

struct ingenic_gem_object {
	struct drm_gem_cma_object base;
	struct ingenic_dma_hwdesc *hwdescs;
	dma_addr_t hwdescs_phys;
};

struct ingenic_drm_private_state {
	struct drm_private_state base;

	bool no_vblank;
	bool use_palette;

	/*
	 * A lot of devices with an Ingenic SoC have a weird LCD panel attached,
	 * where the pixels are not square. For instance, the AUO A030JTN01 and
	 * Innolux EJ030NA panels have a resolution of 320x480 with a 4:3 aspect
	 * ratio.
	 *
	 * All userspace applications are built with the assumption that the
	 * pixels are square. To be able to support these devices without too
	 * much effort, add a doublescan feature, which allows the f0 and f1
	 * planes to be used with only half of the screen's vertical resolution,
	 * where each line of the input is displayed twice.
	 *
	 * This is done using a chained list of DMA descriptors, one descriptor
	 * per output line.
	 */
	bool doublescan;
};

struct ingenic_drm {
	struct drm_device drm;
	/*
	 * f1 (aka. foreground1) is our primary plane, on top of which
	 * f0 (aka. foreground0) can be overlayed. Z-order is fixed in
	 * hardware and cannot be changed.
	 */
	struct drm_plane f0, f1, *ipu_plane;
	struct drm_crtc crtc;

	struct device *dev;
	struct regmap *map;
	struct clk *lcd_clk, *pix_clk, *dma_clk;
	struct mipi_dsi_host dsi_host;
	struct delayed_work refresh_work;
	const struct jz_soc_info *soc_info;

	struct dma_chan *dma_slcd;

	struct ingenic_dma_hwdescs *dma_hwdescs;
	dma_addr_t dma_hwdescs_phys;

	bool panel_is_sharp;
	bool panel_is_slcd;

	/*
	 * clk_mutex is used to synchronize the pixel clock rate update with
	 * the VBLANK. When the pixel clock's parent clock needs to be updated,
	 * clock_nb's notifier function will lock the mutex, then wait until the
	 * next VBLANK. At that point, the parent clock's rate can be updated,
	 * and the mutex is then unlocked. If an atomic commit happens in the
	 * meantime, it will lock on the mutex, effectively waiting until the
	 * clock update process finishes. Finally, the pixel clock's rate will
	 * be recomputed when the mutex has been released, in the pending atomic
	 * commit, or a future one.
	 */
	struct mutex clk_mutex;
	bool update_clk_rate;
	struct notifier_block clock_nb;

	struct drm_private_obj private_obj;
};

struct ingenic_drm_bec {
	struct drm_encoder encoder;
	struct drm_bridge bridge, *next_bridge;

	struct drm_bus_cfg bus_cfg;
};

static inline struct ingenic_drm_bec *
to_ingenic_drm_bec(struct drm_encoder *encoder)
{
	return container_of(encoder, struct ingenic_drm_bec, encoder);
}

static inline struct ingenic_drm_private_state *
to_ingenic_drm_priv_state(struct drm_private_state *state)
{
	return container_of(state, struct ingenic_drm_private_state, base);
}

static struct ingenic_drm_private_state *
ingenic_drm_get_priv_state(struct ingenic_drm *priv, struct drm_atomic_state *state)
{
	struct drm_private_state *priv_state;

	priv_state = drm_atomic_get_private_obj_state(state, &priv->private_obj);
	if (IS_ERR(priv_state))
		return ERR_CAST(priv_state);

	return to_ingenic_drm_priv_state(priv_state);
}

static struct ingenic_drm_private_state *
ingenic_drm_get_new_priv_state(struct ingenic_drm *priv, struct drm_atomic_state *state)
{
	struct drm_private_state *priv_state;

	priv_state = drm_atomic_get_new_private_obj_state(state, &priv->private_obj);
	if (!priv_state)
		return NULL;

	return to_ingenic_drm_priv_state(priv_state);
}

static bool ingenic_drm_writeable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case JZ_REG_LCD_IID:
	case JZ_REG_LCD_SA0:
	case JZ_REG_LCD_FID0:
	case JZ_REG_LCD_CMD0:
	case JZ_REG_LCD_SA1:
	case JZ_REG_LCD_FID1:
	case JZ_REG_LCD_CMD1:
		return false;
	default:
		return true;
	}
}

static const struct regmap_config ingenic_drm_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,

	.max_register = JZ_REG_LCD_SIZE1,
	.writeable_reg = ingenic_drm_writeable_reg,
};

static inline struct ingenic_drm *drm_device_get_priv(struct drm_device *drm)
{
	return container_of(drm, struct ingenic_drm, drm);
}

static inline struct ingenic_drm *drm_crtc_get_priv(struct drm_crtc *crtc)
{
	return container_of(crtc, struct ingenic_drm, crtc);
}

static inline struct ingenic_drm *drm_nb_get_priv(struct notifier_block *nb)
{
	return container_of(nb, struct ingenic_drm, clock_nb);
}

static inline struct ingenic_drm *work_struct_get_priv(struct work_struct *work)
{
	return container_of(work, struct ingenic_drm, refresh_work.work);
}

static inline struct ingenic_gem_object *to_ingenic_gem_obj(struct drm_gem_object *gem_obj)
{
	return container_of(gem_obj, struct ingenic_gem_object, base.base);
}

static inline dma_addr_t dma_hwdesc_addr(const struct ingenic_drm *priv, bool use_f1)
{
	u32 offset = offsetof(struct ingenic_dma_hwdescs, hwdesc[use_f1]);

	return priv->dma_hwdescs_phys + offset;
}

static inline dma_addr_t dma_hwdesc_pal_addr(const struct ingenic_drm *priv)
{
	u32 offset = offsetof(struct ingenic_dma_hwdescs, hwdesc_pal);

	return priv->dma_hwdescs_phys + offset;
}

static int ingenic_drm_update_pixclk(struct notifier_block *nb,
				     unsigned long action,
				     void *data)
{
	struct ingenic_drm *priv = drm_nb_get_priv(nb);
	pr_info("drm update_pixclk");
	switch (action) {
	case PRE_RATE_CHANGE:
		mutex_lock(&priv->clk_mutex);
		priv->update_clk_rate = true;
		drm_crtc_wait_one_vblank(&priv->crtc);
		return NOTIFY_OK;
	default:
		mutex_unlock(&priv->clk_mutex);
		return NOTIFY_OK;
	}
}

static void ingenic_drm_crtc_atomic_enable(struct drm_crtc *crtc,
					   struct drm_atomic_state *state)
{
	struct ingenic_drm *priv = drm_crtc_get_priv(crtc);
	struct ingenic_drm_private_state *priv_state;
	unsigned int val;
	int ret;

	pr_info("drm crtc_atomic_enable");
	priv_state = ingenic_drm_get_new_priv_state(priv, state);
	if (WARN_ON(!priv_state))
		return;

	regmap_write(priv->map, JZ_REG_LCD_STATE, 0);

	 /* Set address of our DMA descriptor chain */
        if (priv_state->use_palette) {	
                regmap_write(priv->map, JZ_REG_LCD_DA0, dma_hwdesc_pal_addr(priv));
		pr_info("priv_state->use_palette");
	}
        else {
                regmap_write(priv->map, JZ_REG_LCD_DA0, dma_hwdesc_addr(priv, 0));
		pr_info("priv_state->not_use_palette");
	}
        regmap_write(priv->map, JZ_REG_LCD_DA1, dma_hwdesc_addr(priv, 1));

	if (priv->panel_is_slcd) {
		pr_info("panel_is_slcd");
		ret = regmap_read_poll_timeout(priv->map,
					       JZ_REG_LCD_SLCD_MSTATE, val,
					       !(val & JZ_SLCD_MSTATE_BUSY),
					       4, USEC_PER_MSEC * 100);
		if (ret) {
			dev_err(priv->dev, "CRTC enable timeout");
			return;
		}

		regmap_write(priv->map, JZ_REG_LCD_SLCD_MCTRL,
			     JZ_SLCD_MCTRL_DMATXEN);
	} else {
		pr_info("panel is NOT slcd!");
		regmap_update_bits(priv->map, JZ_REG_LCD_CTRL,
				   JZ_LCD_CTRL_ENABLE | JZ_LCD_CTRL_DISABLE,
				   JZ_LCD_CTRL_ENABLE);
	}

	drm_crtc_vblank_on(crtc);
}

static void ingenic_drm_crtc_atomic_disable(struct drm_crtc *crtc,
					    struct drm_atomic_state *state)
{
	struct ingenic_drm *priv = drm_crtc_get_priv(crtc);
	unsigned int var;

	pr_info("drm atomic_disable");
	drm_crtc_vblank_off(crtc);

	if (priv->panel_is_slcd) {
		cancel_delayed_work(&priv->refresh_work);
	} else {
		regmap_update_bits(priv->map, JZ_REG_LCD_CTRL,
				   JZ_LCD_CTRL_DISABLE, JZ_LCD_CTRL_DISABLE);

	regmap_read_poll_timeout(priv->map, JZ_REG_LCD_STATE, var,
				 var & JZ_LCD_STATE_DISABLED,
				 1000, 0);
	}
}

static void ingenic_drm_crtc_update_timings(struct ingenic_drm *priv,
					    struct drm_display_mode *mode)
{
	unsigned int vpe, vds, vde, vt, hpe, hds, hde, ht;
	pr_info("drm crtc_update_timings");

	vpe = mode->crtc_vsync_end - mode->crtc_vsync_start;
	vds = mode->crtc_vtotal - mode->crtc_vsync_start;
	vde = vds + mode->crtc_vdisplay;
	vt = vde + mode->crtc_vsync_start - mode->crtc_vdisplay;

	hpe = mode->crtc_hsync_end - mode->crtc_hsync_start;
	hds = mode->crtc_htotal - mode->crtc_hsync_start;
	hde = hds + mode->crtc_hdisplay;
	ht = hde + mode->crtc_hsync_start - mode->crtc_hdisplay;

	regmap_write(priv->map, JZ_REG_LCD_VSYNC,
		     0 << JZ_LCD_VSYNC_VPS_OFFSET |
		     vpe << JZ_LCD_VSYNC_VPE_OFFSET);

	regmap_write(priv->map, JZ_REG_LCD_HSYNC,
		     0 << JZ_LCD_HSYNC_HPS_OFFSET |
		     hpe << JZ_LCD_HSYNC_HPE_OFFSET);

	regmap_write(priv->map, JZ_REG_LCD_VAT,
		     ht << JZ_LCD_VAT_HT_OFFSET |
		     vt << JZ_LCD_VAT_VT_OFFSET);

	regmap_write(priv->map, JZ_REG_LCD_DAH,
		     hds << JZ_LCD_DAH_HDS_OFFSET |
		     hde << JZ_LCD_DAH_HDE_OFFSET);
	regmap_write(priv->map, JZ_REG_LCD_DAV,
		     vds << JZ_LCD_DAV_VDS_OFFSET |
		     vde << JZ_LCD_DAV_VDE_OFFSET);

	if (priv->panel_is_sharp) {
		regmap_write(priv->map, JZ_REG_LCD_PS, hde << 16 | (hde + 1));
		regmap_write(priv->map, JZ_REG_LCD_CLS, hde << 16 | (hde + 1));
		regmap_write(priv->map, JZ_REG_LCD_SPL, hpe << 16 | (hpe + 1));
		regmap_write(priv->map, JZ_REG_LCD_REV, mode->htotal << 16);
	}

	regmap_set_bits(priv->map, JZ_REG_LCD_CTRL,
			JZ_LCD_CTRL_OFUP | JZ_LCD_CTRL_BURST_16);

	/*
	 * IPU restart - specify how much time the LCDC will wait before
	 * transferring a new frame from the IPU. The value is the one
	 * suggested in the programming manual.
	 */
	regmap_write(priv->map, JZ_REG_LCD_IPUR, JZ_LCD_IPUR_IPUREN |
		     (ht * vpe / 3) << JZ_LCD_IPUR_IPUR_LSB);
}

static int ingenic_drm_crtc_atomic_check(struct drm_crtc *crtc,
					 struct drm_atomic_state *state)
{
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state,
									  crtc);
	struct ingenic_drm *priv = drm_crtc_get_priv(crtc);
	struct drm_plane_state *f1_state, *f0_state, *ipu_state = NULL;
	struct ingenic_drm_private_state *priv_state;

	pr_info("drm crtc_atomic_check");
	if (crtc_state->gamma_lut &&
	    drm_color_lut_size(crtc_state->gamma_lut) != ARRAY_SIZE(priv->dma_hwdescs->palette)) {
		dev_dbg(priv->dev, "Invalid palette size\n");
		return -EINVAL;
	}

	if (drm_atomic_crtc_needs_modeset(crtc_state) && priv->soc_info->has_osd) {
		f1_state = drm_atomic_get_plane_state(crtc_state->state,
						      &priv->f1);
		if (IS_ERR(f1_state))
			return PTR_ERR(f1_state);

		f0_state = drm_atomic_get_plane_state(crtc_state->state,
						      &priv->f0);
		if (IS_ERR(f0_state))
			return PTR_ERR(f0_state);

		if (IS_ENABLED(CONFIG_DRM_INGENIC_IPU) && priv->ipu_plane) {
			ipu_state = drm_atomic_get_plane_state(crtc_state->state,
							       priv->ipu_plane);
			if (IS_ERR(ipu_state))
				return PTR_ERR(ipu_state);

			/* IPU and F1 planes cannot be enabled at the same time. */
			if (f1_state->fb && ipu_state->fb) {
				dev_dbg(priv->dev, "Cannot enable both F1 and IPU\n");
				return -EINVAL;
			}
		}

		priv_state = ingenic_drm_get_priv_state(priv, state);
		if (IS_ERR(priv_state))
			return PTR_ERR(priv_state);

		/* If all the planes are disabled, we won't get a VBLANK IRQ */
		priv_state->no_vblank = !f1_state->fb && !f0_state->fb &&
					!(ipu_state && ipu_state->fb);
	}

	return 0;
}

static enum drm_mode_status
ingenic_drm_crtc_mode_valid(struct drm_crtc *crtc, const struct drm_display_mode *mode)
{
	struct ingenic_drm *priv = drm_crtc_get_priv(crtc);
	long rate;
	pr_info("drm crtc_mode_valid");

	if (mode->hdisplay > priv->soc_info->max_width)
		return MODE_BAD_HVALUE;
	if (mode->vdisplay > priv->soc_info->max_height)
		return MODE_BAD_VVALUE;

	rate = clk_round_rate(priv->pix_clk, mode->clock * 1000);
	if (rate < 0)
		return MODE_CLOCK_RANGE;

	return MODE_OK;
}

static void ingenic_drm_crtc_atomic_begin(struct drm_crtc *crtc,
					  struct drm_atomic_state *state)
{
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state,
									  crtc);
	struct ingenic_drm *priv = drm_crtc_get_priv(crtc);
	u32 ctrl = 0;

	pr_info("drm crtc_atomic_begin");

	if (priv->soc_info->has_osd &&
	    drm_atomic_crtc_needs_modeset(crtc_state)) {
		/*
		 * If IPU plane is enabled, enable IPU as source for the F1
		 * plane; otherwise use regular DMA.
		 */
		if (priv->ipu_plane && priv->ipu_plane->state->fb)
			ctrl |= JZ_LCD_OSDCTRL_IPU;

		regmap_update_bits(priv->map, JZ_REG_LCD_OSDCTRL,
				   JZ_LCD_OSDCTRL_IPU, ctrl);
	}
}

static void ingenic_drm_crtc_atomic_flush(struct drm_crtc *crtc,
					  struct drm_atomic_state *state)
{
	struct ingenic_drm *priv = drm_crtc_get_priv(crtc);
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state,
									  crtc);
	struct drm_pending_vblank_event *event = crtc_state->event;
	unsigned int cfg;
	pr_info("drm crtc_atomic_flush");

	if (drm_atomic_crtc_needs_modeset(crtc_state)) {
		regmap_read(priv->map, JZ_REG_LCD_CFG, &cfg);
		priv->panel_is_slcd = !!(cfg & JZ_LCD_CFG_SLCD);

		ingenic_drm_crtc_update_timings(priv, &crtc_state->adjusted_mode);
		priv->update_clk_rate = true;
	}

	if (priv->panel_is_slcd)
		schedule_delayed_work(&priv->refresh_work, 0);

	if (priv->update_clk_rate) {
		mutex_lock(&priv->clk_mutex);
		clk_set_rate(priv->pix_clk,
			     crtc_state->adjusted_mode.crtc_clock * 1000);
		priv->update_clk_rate = false;
		mutex_unlock(&priv->clk_mutex);
	}

	if (event) {
		crtc_state->event = NULL;

		spin_lock_irq(&crtc->dev->event_lock);
		if (drm_crtc_vblank_get(crtc) == 0)
			drm_crtc_arm_vblank_event(crtc, event);
		else
			drm_crtc_send_vblank_event(crtc, event);
		spin_unlock_irq(&crtc->dev->event_lock);
	}
}

static int ingenic_drm_plane_atomic_check(struct drm_plane *plane,
					  struct drm_atomic_state *state)
{
	struct drm_plane_state *old_plane_state = drm_atomic_get_old_plane_state(state,
										 plane);
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state,
										 plane);
	struct ingenic_drm *priv = drm_device_get_priv(plane->dev);
	struct ingenic_drm_private_state *priv_state;
	struct drm_crtc_state *crtc_state;
	struct drm_crtc *crtc = new_plane_state->crtc ?: old_plane_state->crtc;
	int ret;
	pr_info("drm plane_atomic_check");

	if (!crtc)
		return 0;

	crtc_state = drm_atomic_get_existing_crtc_state(state,
							crtc);
	if (WARN_ON(!crtc_state))
		return -EINVAL;

	priv_state = ingenic_drm_get_priv_state(priv, state);
	if (IS_ERR(priv_state))
		return PTR_ERR(priv_state);

	ret = drm_atomic_helper_check_plane_state(new_plane_state, crtc_state,
						  0x8000,
						  DRM_PLANE_HELPER_NO_SCALING,
						  priv->soc_info->has_osd,
						  true);
	if (ret)
		return ret;

	/*
	 * If OSD is not available, check that the width/height match.
	 * Note that state->src_* are in 16.16 fixed-point format.
	 */
	if (!priv->soc_info->has_osd &&
	    (new_plane_state->src_x != 0 ||
	     (new_plane_state->src_w >> 16) != new_plane_state->crtc_w ||
	     (new_plane_state->src_h >> 16) != new_plane_state->crtc_h))
		return -EINVAL;

	/* Enable doublescan if the CRTC_H is twice the SRC_H. */
	priv_state->doublescan = (new_plane_state->src_h >> 16) * 2 == new_plane_state->crtc_h;

	/* Otherwise, fail if CRTC_H != SRC_H */
	if (!priv_state->doublescan && (new_plane_state->src_h >> 16) != new_plane_state->crtc_h)
		return -EINVAL;

	/* Fail if CRTC_W != SRC_W */
	if ((new_plane_state->src_w >> 16) != new_plane_state->crtc_w)
		return -EINVAL;

	priv_state->use_palette = new_plane_state->fb &&
		new_plane_state->fb->format->format == DRM_FORMAT_C8;

	/*
	 * Require full modeset if enabling or disabling a plane, or changing
	 * its position, size or depth.
	 */
	if (priv->soc_info->has_osd &&
	    (!old_plane_state->fb || !new_plane_state->fb ||
	     old_plane_state->crtc_x != new_plane_state->crtc_x ||
	     old_plane_state->crtc_y != new_plane_state->crtc_y ||
	     old_plane_state->crtc_w != new_plane_state->crtc_w ||
	     old_plane_state->crtc_h != new_plane_state->crtc_h ||
	     old_plane_state->fb->format->format != new_plane_state->fb->format->format))
		crtc_state->mode_changed = true;

	if (priv->soc_info->map_noncoherent)
		drm_atomic_helper_check_plane_damage(state, new_plane_state);

	return 0;
}

static void ingenic_drm_plane_enable(struct ingenic_drm *priv,
				     struct drm_plane *plane)
{
	unsigned int en_bit;
	pr_info("drm plane_enable");

	if (priv->soc_info->has_osd) {
		if (plane != &priv->f0)
			en_bit = JZ_LCD_OSDC_F1EN;
		else
			en_bit = JZ_LCD_OSDC_F0EN;

		regmap_set_bits(priv->map, JZ_REG_LCD_OSDC, en_bit);
	}
}

void ingenic_drm_plane_disable(struct device *dev, struct drm_plane *plane)
{
	struct ingenic_drm *priv = dev_get_drvdata(dev);
	unsigned int en_bit;
	pr_info("drm plande_disable");

	if (priv->soc_info->has_osd) {
		if (plane != &priv->f0)
			en_bit = JZ_LCD_OSDC_F1EN;
		else
			en_bit = JZ_LCD_OSDC_F0EN;

		regmap_clear_bits(priv->map, JZ_REG_LCD_OSDC, en_bit);
	}
}

static void ingenic_drm_plane_atomic_disable(struct drm_plane *plane,
					     struct drm_atomic_state *state)
{
	struct ingenic_drm *priv = drm_device_get_priv(plane->dev);
	pr_info("drm plane_atomic_disable");

	ingenic_drm_plane_disable(priv->dev, plane);
}

void ingenic_drm_plane_config(struct device *dev,
			      struct drm_plane *plane, u32 fourcc)
{
	struct ingenic_drm *priv = dev_get_drvdata(dev);
	struct drm_plane_state *state = plane->state;
	unsigned int xy_reg, size_reg;
	unsigned int ctrl = 0;
	pr_info("drm plane_config");

	ingenic_drm_plane_enable(priv, plane);

	if (priv->soc_info->has_osd && plane != &priv->f0) {
		switch (fourcc) {
		case DRM_FORMAT_XRGB1555:
			ctrl |= JZ_LCD_OSDCTRL_RGB555;
			fallthrough;
		case DRM_FORMAT_RGB565:
			ctrl |= JZ_LCD_OSDCTRL_BPP_15_16;
			break;
		case DRM_FORMAT_RGB888:
			ctrl |= JZ_LCD_OSDCTRL_BPP_24_COMP;
			break;
		case DRM_FORMAT_XRGB8888:
			ctrl |= JZ_LCD_OSDCTRL_BPP_18_24;
			break;
		case DRM_FORMAT_XRGB2101010:
			ctrl |= JZ_LCD_OSDCTRL_BPP_30;
			break;
		}

		regmap_update_bits(priv->map, JZ_REG_LCD_OSDCTRL,
				   JZ_LCD_OSDCTRL_BPP_MASK, ctrl);
	} else {
		switch (fourcc) {
		case DRM_FORMAT_C8:
			ctrl |= JZ_LCD_CTRL_BPP_8;
			break;
		case DRM_FORMAT_XRGB1555:
			ctrl |= JZ_LCD_CTRL_RGB555;
			fallthrough;
		case DRM_FORMAT_RGB565:
			ctrl |= JZ_LCD_CTRL_BPP_15_16;
			break;
		case DRM_FORMAT_RGB888:
			ctrl |= JZ_LCD_CTRL_BPP_24_COMP;
			break;
		case DRM_FORMAT_XRGB8888:
			ctrl |= JZ_LCD_CTRL_BPP_18_24;
			break;
		case DRM_FORMAT_XRGB2101010:
			ctrl |= JZ_LCD_CTRL_BPP_30;
			break;
		}

		regmap_update_bits(priv->map, JZ_REG_LCD_CTRL,
				   JZ_LCD_CTRL_BPP_MASK, ctrl);
	}

	if (priv->soc_info->has_osd) {
		if (plane != &priv->f0) {
			xy_reg = JZ_REG_LCD_XYP1;
			size_reg = JZ_REG_LCD_SIZE1;
		} else {
			xy_reg = JZ_REG_LCD_XYP0;
			size_reg = JZ_REG_LCD_SIZE0;
		}

		regmap_write(priv->map, xy_reg,
			     state->crtc_x << JZ_LCD_XYP01_XPOS_LSB |
			     state->crtc_y << JZ_LCD_XYP01_YPOS_LSB);
		regmap_write(priv->map, size_reg,
			     state->crtc_w << JZ_LCD_SIZE01_WIDTH_LSB |
			     state->crtc_h << JZ_LCD_SIZE01_HEIGHT_LSB);
	}
}

bool ingenic_drm_map_noncoherent(const struct device *dev)
{
	const struct ingenic_drm *priv = dev_get_drvdata(dev);
	pr_info("drm map_noncoherent");

	return priv->soc_info->map_noncoherent;
}

static void ingenic_drm_update_palette(struct ingenic_drm *priv,
				       const struct drm_color_lut *lut)
{
	unsigned int i;
	pr_info("drm update_palette");

	for (i = 0; i < ARRAY_SIZE(priv->dma_hwdescs->palette); i++) {
		u16 color = drm_color_lut_extract(lut[i].red, 5) << 11
			| drm_color_lut_extract(lut[i].green, 6) << 5
			| drm_color_lut_extract(lut[i].blue, 5);

		priv->dma_hwdescs->palette[i] = color;
	}
}

static void ingenic_drm_plane_atomic_update(struct drm_plane *plane,
					    struct drm_atomic_state *state)
{
	struct ingenic_drm *priv = drm_device_get_priv(plane->dev);
	struct drm_plane_state *newstate = drm_atomic_get_new_plane_state(state, plane);
	struct drm_plane_state *oldstate = drm_atomic_get_old_plane_state(state, plane);
	struct ingenic_drm_private_state *priv_state;
	struct drm_crtc_state *crtc_state;
	struct ingenic_dma_hwdesc *hwdesc;
	unsigned int width, height, cpp, i;
	struct drm_gem_object *gem_obj;
	struct ingenic_gem_object *obj;
	dma_addr_t addr, next_addr;
	bool use_f1;
	u32 fourcc;
	pr_info("drm plane_atomic_update");

	if (newstate && newstate->fb) {
		if (priv->soc_info->map_noncoherent)
			drm_fb_cma_sync_non_coherent(&priv->drm, oldstate, newstate);

		crtc_state = newstate->crtc->state;
		use_f1 = priv->soc_info->has_osd && plane != &priv->f0;

		addr = drm_fb_cma_get_gem_addr(newstate->fb, newstate, 0);
		width = newstate->src_w >> 16;
		height = newstate->src_h >> 16;
		cpp = newstate->fb->format->cpp[0];

		gem_obj = drm_gem_fb_get_obj(newstate->fb, 0);
		obj = to_ingenic_gem_obj(gem_obj);

		priv_state = ingenic_drm_get_new_priv_state(priv, state);
		if (priv_state && priv_state->use_palette)
			next_addr = dma_hwdesc_pal_addr(priv);
		else
			next_addr = dma_hwdesc_addr(priv, use_f1);

		if (priv_state->doublescan) {
			hwdesc = &obj->hwdescs[0];
			/*
			 * Use one DMA descriptor per output line, and display
			 * each input line twice.
			 */
			for (i = 0; i < newstate->crtc_h; i++) {
				hwdesc[i].next = obj->hwdescs_phys
					+ (i + 1) * sizeof(*hwdesc);
				hwdesc[i].addr = addr + (i / 2) * newstate->fb->pitches[0];
				hwdesc[i].cmd = newstate->fb->pitches[0] / 4;
			}

			/* We want the EOF IRQ only on the very last transfer */
			hwdesc[newstate->crtc_h - 1].cmd |= JZ_LCD_CMD_EOF_IRQ;
			hwdesc[newstate->crtc_h - 1].next = next_addr;
			priv->dma_hwdescs->hwdesc[use_f1] = *hwdesc;
		} else {
			/* Use one DMA descriptor for the whole frame. */
			hwdesc = &priv->dma_hwdescs->hwdesc[use_f1];
			hwdesc->addr = addr;
			hwdesc->cmd = JZ_LCD_CMD_EOF_IRQ | (width * height * cpp / 4);
			hwdesc->next = next_addr;
		}

		if (drm_atomic_crtc_needs_modeset(crtc_state)) {
			fourcc = newstate->fb->format->format;

			ingenic_drm_plane_config(priv->dev, plane, fourcc);

			crtc_state->color_mgmt_changed = fourcc == DRM_FORMAT_C8;
		}

		if (crtc_state->color_mgmt_changed)
			ingenic_drm_update_palette(priv, crtc_state->gamma_lut->data);
	}
}

static void ingenic_drm_encoder_atomic_mode_set(struct drm_encoder *encoder,
						struct drm_crtc_state *crtc_state,
						struct drm_connector_state *conn_state)
{
	struct ingenic_drm *priv = drm_device_get_priv(encoder->dev);
	struct drm_display_mode *mode = &crtc_state->adjusted_mode;
	struct ingenic_drm_bec *bec = to_ingenic_drm_bec(encoder);
	unsigned int cfg, rgbcfg = 0;
	pr_info("drm encoder_atomic_mode_set");

	priv->panel_is_sharp = bec->bus_cfg.flags & DRM_BUS_FLAG_SHARP_SIGNALS;

	if (priv->panel_is_sharp) {
		cfg = JZ_LCD_CFG_MODE_SPECIAL_TFT_1 | JZ_LCD_CFG_REV_POLARITY;
	} else {
		cfg = JZ_LCD_CFG_PS_DISABLE | JZ_LCD_CFG_CLS_DISABLE
		    | JZ_LCD_CFG_SPL_DISABLE | JZ_LCD_CFG_REV_DISABLE;
	}

	if (mode->flags & DRM_MODE_FLAG_NHSYNC)
		cfg |= JZ_LCD_CFG_HSYNC_ACTIVE_LOW;
	if (mode->flags & DRM_MODE_FLAG_NVSYNC)
		cfg |= JZ_LCD_CFG_VSYNC_ACTIVE_LOW;
	if (bec->bus_cfg.flags & DRM_BUS_FLAG_DE_LOW)
		cfg |= JZ_LCD_CFG_DE_ACTIVE_LOW;
	if (bec->bus_cfg.flags & DRM_BUS_FLAG_PIXDATA_DRIVE_NEGEDGE)
		cfg |= JZ_LCD_CFG_PCLK_FALLING_EDGE;

	if (!priv->panel_is_sharp) {
		if (conn_state->connector->connector_type == DRM_MODE_CONNECTOR_TV) {
			if (mode->flags & DRM_MODE_FLAG_INTERLACE)
				cfg |= JZ_LCD_CFG_MODE_TV_OUT_I;
			else
				cfg |= JZ_LCD_CFG_MODE_TV_OUT_P;
		} else {
			switch (bec->bus_cfg.format) {
			case MEDIA_BUS_FMT_RGB565_1X16:
				cfg |= JZ_LCD_CFG_MODE_GENERIC_16BIT;
				break;
			case MEDIA_BUS_FMT_RGB666_1X18:
				cfg |= JZ_LCD_CFG_MODE_GENERIC_18BIT;
				break;
			case MEDIA_BUS_FMT_RGB888_1X24:
				cfg |= JZ_LCD_CFG_MODE_GENERIC_24BIT;
				break;
			case MEDIA_BUS_FMT_RGB888_3X8_DELTA:
				rgbcfg = JZ_LCD_RGBC_EVEN_GBR | JZ_LCD_RGBC_ODD_RGB;
				fallthrough;
			case MEDIA_BUS_FMT_RGB888_3X8:
				cfg |= JZ_LCD_CFG_MODE_8BIT_SERIAL;
				break;
			default:
				break;
			}
		}
	}

	//regmap_write(priv->map, JZ_REG_LCD_CFG, cfg);
	regmap_update_bits(priv->map, JZ_REG_LCD_CFG, ~JZ_LCD_CFG_SLCD, cfg);
	regmap_write(priv->map, JZ_REG_LCD_RGBC, rgbcfg);
}

static int ingenic_drm_bridge_attach(struct drm_bridge *bridge,
				     enum drm_bridge_attach_flags flags)
{
	struct ingenic_drm_bec *bec = to_ingenic_drm_bec(bridge->encoder);
	pr_info("drm bridge_attach");

	return drm_bridge_attach(bridge->encoder, bec->next_bridge,
				 &bec->bridge, flags);
}

static int ingenic_drm_bridge_atomic_check(struct drm_bridge *bridge,
					   struct drm_bridge_state *bridge_state,
					   struct drm_crtc_state *crtc_state,
					   struct drm_connector_state *conn_state)
{
	struct drm_display_mode *mode = &crtc_state->adjusted_mode;
	struct ingenic_drm_bec *bec = to_ingenic_drm_bec(bridge->encoder);
	pr_info("drm bridge_atomic_check");

	bec->bus_cfg = bridge_state->output_bus_cfg;

	if (conn_state->connector->connector_type == DRM_MODE_CONNECTOR_TV)
		return 0;

	switch (bridge_state->output_bus_cfg.format) {
	case MEDIA_BUS_FMT_RGB888_3X8:
	case MEDIA_BUS_FMT_RGB888_3X8_DELTA:
		/*
		 * The LCD controller expects timing values in dot-clock ticks,
		 * which is 3x the timing values in pixels when using a 3x8-bit
		 * display; but it will count the display area size in pixels
		 * either way. Go figure.
		 */
		mode->crtc_clock = mode->clock * 3;
		mode->crtc_hsync_start = mode->hsync_start * 3 - mode->hdisplay * 2;
		mode->crtc_hsync_end = mode->hsync_end * 3 - mode->hdisplay * 2;
		mode->crtc_hdisplay = mode->hdisplay;
		mode->crtc_htotal = mode->htotal * 3 - mode->hdisplay * 2;
		break;
	case MEDIA_BUS_FMT_RGB565_1X16:
	case MEDIA_BUS_FMT_RGB666_1X18:
	case MEDIA_BUS_FMT_RGB888_1X24:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void ingenic_drm_atomic_helper_commit_tail(struct drm_atomic_state *old_state)
{
	/*
	 * Just your regular drm_atomic_helper_commit_tail(), but only calls
	 * drm_atomic_helper_wait_for_vblanks() if priv->no_vblank.
	 */
	struct drm_device *dev = old_state->dev;
	struct ingenic_drm *priv = drm_device_get_priv(dev);
	struct ingenic_drm_private_state *priv_state;
	pr_info("drm atomic_helper_commit_tail");

	drm_atomic_helper_commit_modeset_disables(dev, old_state);

	drm_atomic_helper_commit_planes(dev, old_state, 0);

	drm_atomic_helper_commit_modeset_enables(dev, old_state);

	drm_atomic_helper_commit_hw_done(old_state);

	priv_state = ingenic_drm_get_new_priv_state(priv, old_state);

	if (!priv_state || !priv_state->no_vblank)
		drm_atomic_helper_wait_for_vblanks(dev, old_state);

	drm_atomic_helper_cleanup_planes(dev, old_state);
}

static irqreturn_t ingenic_drm_irq_handler(int irq, void *arg)
{
	struct ingenic_drm *priv = drm_device_get_priv(arg);
	unsigned int state;
	pr_info("drm irq_handler");

	regmap_read(priv->map, JZ_REG_LCD_STATE, &state);

	regmap_update_bits(priv->map, JZ_REG_LCD_STATE,
			   JZ_LCD_STATE_EOF_IRQ, 0);

	if (state & JZ_LCD_STATE_EOF_IRQ)
		drm_crtc_handle_vblank(&priv->crtc);

	return IRQ_HANDLED;
}

static int ingenic_drm_enable_vblank(struct drm_crtc *crtc)
{
	struct ingenic_drm *priv = drm_crtc_get_priv(crtc);
	pr_info("drm enable_vblank");

	if (!priv->panel_is_slcd) {
	pr_info("panel is NOT slcd!");
	regmap_update_bits(priv->map, JZ_REG_LCD_CTRL,
			   JZ_LCD_CTRL_EOF_IRQ, JZ_LCD_CTRL_EOF_IRQ);
	}

	return 0;
}

static void ingenic_drm_disable_vblank(struct drm_crtc *crtc)
{
	struct ingenic_drm *priv = drm_crtc_get_priv(crtc);
	pr_info("drm disable_vblank");
	
	if (!priv->panel_is_slcd)
	regmap_update_bits(priv->map, JZ_REG_LCD_CTRL, JZ_LCD_CTRL_EOF_IRQ, 0);
}

static void ingenic_drm_gem_fb_destroy(struct drm_framebuffer *fb)
{
	struct ingenic_drm *priv = drm_device_get_priv(fb->dev);
	struct drm_gem_object *gem_obj = drm_gem_fb_get_obj(fb, 0);
	struct ingenic_gem_object *obj = to_ingenic_gem_obj(gem_obj);
	pr_info("drm gem_fb_destroy");

	dma_free_coherent(priv->dev,
			  sizeof(*obj->hwdescs) * fb->height,
			  obj->hwdescs, obj->hwdescs_phys);
	drm_gem_fb_destroy(fb);
}

static const struct drm_framebuffer_funcs ingenic_drm_gem_fb_funcs = {
	.destroy	= ingenic_drm_gem_fb_destroy,
	.create_handle	= drm_gem_fb_create_handle,
};

static struct drm_framebuffer *
ingenic_drm_gem_fb_create(struct drm_device *drm, struct drm_file *file,
			  const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct ingenic_drm *priv = drm_device_get_priv(drm);
	struct drm_gem_object *gem_obj;
	struct ingenic_gem_object *obj;
	struct drm_framebuffer *fb;
	pr_info("drm framebuffer");

	/* TODO: use ingenic_drm_gem_fb_funcs */
	if (priv->soc_info->map_noncoherent)
		fb = drm_gem_fb_create_with_dirty(drm, file, mode_cmd);
	else
		fb = drm_gem_fb_create(drm, file, mode_cmd);
	if (IS_ERR(fb))
		return fb;

	gem_obj = drm_gem_fb_get_obj(fb, 0);
	obj = to_ingenic_gem_obj(gem_obj);

	/*
	 * Create (fb->height * 2) DMA descriptors, in case we want to use the
	 * doublescan feature.
	 */
	obj->hwdescs = dma_alloc_coherent(priv->dev,
					  sizeof(*obj->hwdescs) * fb->height * 2,
					  &obj->hwdescs_phys,
					  GFP_KERNEL);
	if (!obj->hwdescs) {
		drm_gem_fb_destroy(fb);
		return ERR_PTR(-ENOMEM);
	}

	return fb;
}

static struct drm_gem_object *
ingenic_drm_gem_create_object(struct drm_device *drm, size_t size)
{
	struct ingenic_drm *priv = drm_device_get_priv(drm);
	struct drm_gem_cma_object *obj;
	pr_info("drm gem_create_object");

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return ERR_PTR(-ENOMEM);

	obj->map_noncoherent = priv->soc_info->map_noncoherent;

	return &obj->base;
}

static struct drm_private_state *
ingenic_drm_duplicate_state(struct drm_private_obj *obj)
{
	struct ingenic_drm_private_state *state = to_ingenic_drm_priv_state(obj->state);
	pr_info("drm duplicate_state");

	state = kmemdup(state, sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;

	__drm_atomic_helper_private_obj_duplicate_state(obj, &state->base);

	return &state->base;
}

static void ingenic_drm_destroy_state(struct drm_private_obj *obj,
				      struct drm_private_state *state)
{
	struct ingenic_drm_private_state *priv_state = to_ingenic_drm_priv_state(state);
	pr_info("drm destroy_state");

	kfree(priv_state);
}

static void ingenic_drm_slcd_done(void *d)
{
	struct ingenic_drm *priv = d;
	struct drm_display_mode *mode = &priv->crtc.state->adjusted_mode;
	pr_info("drm slcd_done");

	drm_crtc_handle_vblank(&priv->crtc);

	schedule_delayed_work(&priv->refresh_work, HZ / (mode->crtc_vsync_end - mode->crtc_vsync_start));
}

static void ingenic_drm_refresh_work(struct work_struct *work)
{
	struct ingenic_drm *priv = work_struct_get_priv(work);
	dma_addr_t hwaddr = priv->dma_hwdescs->hwdesc[0].addr;
	struct dma_async_tx_descriptor *desc;
	size_t len;
	pr_info("drm slcd_refresh_work");

	len = (priv->dma_hwdescs->hwdesc[0].cmd &~ JZ_LCD_CMD_EOF_IRQ) * 4;

	desc = dmaengine_prep_slave_single(priv->dma_slcd,
					   hwaddr, len,
					   DMA_MEM_TO_DEV, 0);
	if (IS_ERR(desc)) {
		dev_err(priv->dev, "Unable to prepare DMA: %ld", PTR_ERR(desc));
		return;
	}

	desc->callback_param = priv;
	desc->callback = ingenic_drm_slcd_done;
	dmaengine_submit(desc);

	dma_async_issue_pending(priv->dma_slcd);
}

DEFINE_DRM_GEM_CMA_FOPS(ingenic_drm_fops);

static const struct drm_driver ingenic_drm_driver_data = {
	.driver_features	= DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.name			= "ingenic-drm",
	.desc			= "DRM module for Ingenic SoCs",
	.date			= "20200716",
	.major			= 1,
	.minor			= 1,
	.patchlevel		= 0,

	.fops			= &ingenic_drm_fops,
	.gem_create_object	= ingenic_drm_gem_create_object,
	DRM_GEM_CMA_DRIVER_OPS,

	.gem_create_object	= ingenic_drm_gem_create_object,

	.irq_handler		= ingenic_drm_irq_handler,
};

static const struct drm_plane_funcs ingenic_drm_primary_plane_funcs = {
	.update_plane		= drm_atomic_helper_update_plane,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.reset			= drm_atomic_helper_plane_reset,
	.destroy		= drm_plane_cleanup,

	.atomic_duplicate_state	= drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_plane_destroy_state,
};

static const struct drm_crtc_funcs ingenic_drm_crtc_funcs = {
	.set_config		= drm_atomic_helper_set_config,
	.page_flip		= drm_atomic_helper_page_flip,
	.reset			= drm_atomic_helper_crtc_reset,
	.destroy		= drm_crtc_cleanup,

	.atomic_duplicate_state	= drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_crtc_destroy_state,

	.enable_vblank		= ingenic_drm_enable_vblank,
	.disable_vblank		= ingenic_drm_disable_vblank,
};

static const struct drm_plane_helper_funcs ingenic_drm_plane_helper_funcs = {
	.atomic_update		= ingenic_drm_plane_atomic_update,
	.atomic_check		= ingenic_drm_plane_atomic_check,
	.atomic_disable		= ingenic_drm_plane_atomic_disable,
	.prepare_fb		= drm_gem_plane_helper_prepare_fb,
};

static const struct drm_crtc_helper_funcs ingenic_drm_crtc_helper_funcs = {
	.atomic_enable		= ingenic_drm_crtc_atomic_enable,
	.atomic_disable		= ingenic_drm_crtc_atomic_disable,
	.atomic_begin		= ingenic_drm_crtc_atomic_begin,
	.atomic_flush		= ingenic_drm_crtc_atomic_flush,
	.atomic_check		= ingenic_drm_crtc_atomic_check,
	.mode_valid		= ingenic_drm_crtc_mode_valid,
};

static const struct drm_encoder_helper_funcs ingenic_drm_encoder_helper_funcs = {
	.atomic_mode_set        = ingenic_drm_encoder_atomic_mode_set,
};

static const struct drm_bridge_funcs ingenic_drm_bridge_funcs = {
	.attach			= ingenic_drm_bridge_attach,
	.atomic_check		= ingenic_drm_bridge_atomic_check,
	.atomic_reset		= drm_atomic_helper_bridge_reset,
	.atomic_duplicate_state	= drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_bridge_destroy_state,
	.atomic_get_input_bus_fmts = drm_atomic_helper_bridge_propagate_bus_fmt,
};

static const struct drm_mode_config_funcs ingenic_drm_mode_config_funcs = {
	.fb_create		= ingenic_drm_gem_fb_create,
	.output_poll_changed	= drm_fb_helper_output_poll_changed,
	.atomic_check		= drm_atomic_helper_check,
	.atomic_commit		= drm_atomic_helper_commit,
};

static struct drm_mode_config_helper_funcs ingenic_drm_mode_config_helpers = {
	.atomic_commit_tail = ingenic_drm_atomic_helper_commit_tail,
};

static const struct drm_private_state_funcs ingenic_drm_private_state_funcs = {
	.atomic_duplicate_state = ingenic_drm_duplicate_state,
	.atomic_destroy_state = ingenic_drm_destroy_state,
};

static void ingenic_drm_unbind_all(void *d)
{
	struct ingenic_drm *priv = d;

	component_unbind_all(priv->dev, &priv->drm);
}

static void __maybe_unused ingenic_drm_release_rmem(void *d)
{
	of_reserved_mem_device_release(d);
}

static struct clk * ingenic_drm_get_parent_clk(struct clk *clk)
{
	/*
	 * Return the first clock above the one that will effectively modify
	 * its rate when clk_set_rate(clk) is called.
	 */
	clk = clk_get_first_to_set_rate(clk);

	return clk_get_parent(clk);
}

static void ingenic_drm_atomic_private_obj_fini(struct drm_device *drm, void *private_obj)
{
	drm_atomic_private_obj_fini(private_obj);
}

static void ingenic_drm_dma_release(void *d)
{
	dma_release_channel(d);
}

static int ingenic_drm_bind(struct device *dev, bool has_components)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct ingenic_drm_private_state *private_state;
	const struct jz_soc_info *soc_info;
	struct ingenic_drm *priv;
	struct clk *parent_clk;
	struct drm_plane *primary;
	struct drm_bridge *bridge;
	struct drm_panel *panel;
	struct drm_connector *connector;
	struct drm_encoder *encoder;
	struct ingenic_drm_bec *bec;
	struct drm_device *drm;
	void __iomem *base;
	long parent_rate;
	unsigned int i, clone_mask = 0;
	dma_addr_t dma_hwdesc_phys_f0, dma_hwdesc_phys_f1;
	int ret, irq;
	
	pr_info("drm drm_bind begin");

	soc_info = of_device_get_match_data(dev);
	if (!soc_info) {
		dev_err(dev, "Missing platform data\n");
		return -EINVAL;
	}

	if (IS_ENABLED(CONFIG_OF_RESERVED_MEM)) {
		ret = of_reserved_mem_device_init(dev);

		if (ret && ret != -ENODEV)
			dev_warn(dev, "Failed to get reserved memory: %d\n", ret);

		if (!ret) {
			ret = devm_add_action_or_reset(dev, ingenic_drm_release_rmem, dev);
			if (ret)
				return ret;
		}
	}

	priv = devm_drm_dev_alloc(dev, &ingenic_drm_driver_data,
				  struct ingenic_drm, drm);
	if (IS_ERR(priv))
		return PTR_ERR(priv);

	priv->soc_info = soc_info;
	priv->dev = dev;
	drm = &priv->drm;

	INIT_DELAYED_WORK(&priv->refresh_work, ingenic_drm_refresh_work);

	platform_set_drvdata(pdev, priv);

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.min_width = 0;
	drm->mode_config.min_height = 0;
	drm->mode_config.max_width = soc_info->max_width;
	drm->mode_config.max_height = 4095;
	drm->mode_config.funcs = &ingenic_drm_mode_config_funcs;
	drm->mode_config.helper_private = &ingenic_drm_mode_config_helpers;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base)) {
		dev_err(dev, "Failed to get memory resource\n");
		return PTR_ERR(base);
	}

	priv->map = devm_regmap_init_mmio(dev, base,
					  &ingenic_drm_regmap_config);
	if (IS_ERR(priv->map)) {
		dev_err(dev, "Failed to create regmap\n");
		return PTR_ERR(priv->map);
	}

	ret = regmap_attach_dev(dev, priv->map, &ingenic_drm_regmap_config);
	if (ret) {
		dev_err(dev, "Failed to attach regmap");
		return ret;
	}

	priv->dma_slcd = dma_request_chan(dev, "slcd");
	if (IS_ERR(priv->dma_slcd)) {
		ret = PTR_ERR(priv->dma_slcd);

		if (ret == -ENOENT) {
			dev_notice(dev, "No SLCD DMA found, SLCD won't be used");
			priv->dma_slcd = NULL;
		} else {
			if (ret != -EPROBE_DEFER)
				dev_err(dev, "Failed to get SLCD DMA channel");
			return ret;
		}
	} else {
		struct dma_slave_config dma_conf = {
			.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
			.dst_addr_width = DMA_SLAVE_BUSWIDTH_2_BYTES,
			.src_maxburst = 64,
			.dst_maxburst = 8,
			.direction = DMA_MEM_TO_DEV,
			.dst_addr = CPHYSADDR(base + JZ_REG_LCD_SLCD_MFIFO),
		};

		ret = devm_add_action_or_reset(dev, ingenic_drm_dma_release,
				       priv->dma_slcd);
		if (ret)
			return ret;

		ret = dmaengine_slave_config(priv->dma_slcd, &dma_conf);
		if (ret) {
			dev_err(dev, "Unable to configure DMA");
			return ret;
		}
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(dev, "Failed to get platform irq");
		return ret;
	}

	/*
	 * DMA clock isn't really optional, but some old Device Tree files
	 * don't provide it.
	 */
	priv->dma_clk = devm_clk_get_optional(dev, "dma");
	if (IS_ERR(priv->dma_clk)) {
		dev_err(dev, "Failed to get dma clock\n");
		return PTR_ERR(priv->dma_clk);
	}

	if (soc_info->needs_dev_clk) {
		priv->lcd_clk = devm_clk_get(dev, "lcd");
		if (IS_ERR(priv->lcd_clk)) {
			dev_err(dev, "Failed to get lcd clock\n");
			return PTR_ERR(priv->lcd_clk);
		}
	}

	priv->pix_clk = devm_clk_get(dev, "lcd_pclk");
	if (IS_ERR(priv->pix_clk)) {
		dev_err(dev, "Failed to get pixel clock\n");
		return PTR_ERR(priv->pix_clk);
	}

	if (priv->dma_slcd) {
		ret = devm_ingenic_drm_init_dsi(dev, &priv->dsi_host);
		if (ret) {
			dev_err(dev, "Unable to init DSI host");
			return ret;
		}
	}

	priv->dma_hwdescs = dmam_alloc_coherent(dev,
						sizeof(*priv->dma_hwdescs),
						&priv->dma_hwdescs_phys,
						GFP_KERNEL);
	if (!priv->dma_hwdescs)
		return -ENOMEM;


	/* Configure DMA hwdesc for foreground0 plane */
	dma_hwdesc_phys_f0 = dma_hwdesc_addr(priv, 0);
	priv->dma_hwdescs->hwdesc[0].next = dma_hwdesc_phys_f0;
	priv->dma_hwdescs->hwdesc[0].id = 0xf0;

	/* Configure DMA hwdesc for foreground1 plane */
	dma_hwdesc_phys_f1 = dma_hwdesc_addr(priv, 1);
	priv->dma_hwdescs->hwdesc[1].next = dma_hwdesc_phys_f1;
	priv->dma_hwdescs->hwdesc[1].id = 0xf1;

	/* Configure DMA hwdesc for palette */
	priv->dma_hwdescs->hwdesc_pal.next = dma_hwdesc_phys_f0;
	priv->dma_hwdescs->hwdesc_pal.id = 0xc0;
	priv->dma_hwdescs->hwdesc_pal.addr = priv->dma_hwdescs_phys
		+ offsetof(struct ingenic_dma_hwdescs, palette);
	priv->dma_hwdescs->hwdesc_pal.cmd = JZ_LCD_CMD_ENABLE_PAL
		| (sizeof(priv->dma_hwdescs->palette) / 4);

	primary = priv->soc_info->has_osd ? &priv->f1 : &priv->f0;

	drm_plane_helper_add(primary, &ingenic_drm_plane_helper_funcs);

	ret = drm_universal_plane_init(drm, primary, 1,
				       &ingenic_drm_primary_plane_funcs,
				       priv->soc_info->formats_f1,
				       priv->soc_info->num_formats_f1,
				       NULL, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret) {
		dev_err(dev, "Failed to register plane: %i\n", ret);
		return ret;
	}

	if (soc_info->map_noncoherent)
		drm_plane_enable_fb_damage_clips(&priv->f1);

	drm_crtc_helper_add(&priv->crtc, &ingenic_drm_crtc_helper_funcs);

	ret = drm_crtc_init_with_planes(drm, &priv->crtc, primary,
					NULL, &ingenic_drm_crtc_funcs, NULL);
	if (ret) {
		dev_err(dev, "Failed to init CRTC: %i\n", ret);
		return ret;
	}

	drm_crtc_enable_color_mgmt(&priv->crtc, 0, false,
				   ARRAY_SIZE(priv->dma_hwdescs->palette));

	if (soc_info->has_osd) {
		drm_plane_helper_add(&priv->f0,
				     &ingenic_drm_plane_helper_funcs);

		ret = drm_universal_plane_init(drm, &priv->f0, 1,
					       &ingenic_drm_primary_plane_funcs,
					       priv->soc_info->formats_f0,
					       priv->soc_info->num_formats_f0,
					       NULL, DRM_PLANE_TYPE_OVERLAY,
					       NULL);
		if (ret) {
			dev_err(dev, "Failed to register overlay plane: %i\n",
				ret);
			return ret;
		}

		if (soc_info->map_noncoherent)
			drm_plane_enable_fb_damage_clips(&priv->f0);

		if (IS_ENABLED(CONFIG_DRM_INGENIC_IPU) && has_components) {
			ret = component_bind_all(dev, drm);
			if (ret) {
				if (ret != -EPROBE_DEFER)
					dev_err(dev, "Failed to bind components: %i\n", ret);
				return ret;
			}

			ret = devm_add_action_or_reset(dev, ingenic_drm_unbind_all, priv);
			if (ret)
				return ret;

			priv->ipu_plane = drm_plane_from_index(drm, 2);
			if (!priv->ipu_plane) {
				dev_err(dev, "Failed to retrieve IPU plane\n");
				return -EINVAL;
			}
		}
	}

	for (i = 0; ; i++) {
		ret = drm_of_find_panel_or_bridge(dev->of_node, 0, i, &panel, &bridge);
		if (ret) {
			/*
			 * Workaround for the case where the drivers for the
			 * remote devices are not enabled. When that happens,
			 * drm_of_find_panel_or_bridge() returns -EPROBE_DEFER
			 * endlessly, which prevents the ingenic-drm driver from
			 * working at all.
			 */
			if (ret == -EPROBE_DEFER) {
				ret = driver_deferred_probe_check_state(dev);
				if (ret == -ENODEV || ret == -ETIMEDOUT)
					continue;
			}
			if (ret == -ENODEV)
				break; /* we're done */
			if (ret != -EPROBE_DEFER)
				dev_err(dev, "Failed to get bridge handle\n");
			return ret;
		}

		if (panel)
			bridge = devm_drm_panel_bridge_add_typed(dev, panel,
								 DRM_MODE_CONNECTOR_DPI);

		bec = drmm_encoder_alloc(drm, struct ingenic_drm_bec, encoder,
					 NULL, DRM_MODE_ENCODER_DPI, NULL);
		if (IS_ERR(bec)) {
			ret = PTR_ERR(bec);
			dev_err(dev, "Failed to init encoder: %d\n", ret);
			return ret;
		}

		encoder = &bec->encoder;
		encoder->possible_crtcs = drm_crtc_mask(&priv->crtc);

		drm_encoder_helper_add(encoder, &ingenic_drm_encoder_helper_funcs);

		bec->bridge.funcs = &ingenic_drm_bridge_funcs;
		bec->next_bridge = bridge;

		ret = drm_bridge_attach(encoder, &bec->bridge, NULL,
					DRM_BRIDGE_ATTACH_NO_CONNECTOR);
		if (ret) {
			dev_err(dev, "Unable to attach bridge\n");
			return ret;
		}

		connector = drm_bridge_connector_init(drm, encoder);
		if (IS_ERR(connector)) {
			dev_err(dev, "Unable to init connector\n");
			return PTR_ERR(connector);
		}

		drm_connector_attach_encoder(connector, encoder);
	}

	drm_for_each_encoder(encoder, drm) {
		clone_mask |= BIT(drm_encoder_index(encoder));
	}

	drm_for_each_encoder(encoder, drm) {
		encoder->possible_clones = clone_mask;
	}

	ret = drm_irq_install(drm, irq);
	if (ret) {
		dev_err(dev, "Unable to install IRQ handler\n");
		return ret;
	}

	ret = drm_vblank_init(drm, 1);
	if (ret) {
		dev_err(dev, "Failed calling drm_vblank_init()\n");
		return ret;
	}

	drm_mode_config_reset(drm);

	ret = clk_prepare_enable(priv->pix_clk);
	if (ret) {
		dev_err(dev, "Unable to start pixel clock\n");
		return ret;
	}

	if (priv->lcd_clk) {
		parent_clk = clk_get_parent(priv->lcd_clk);
		parent_rate = clk_get_rate(parent_clk);

		/* LCD Device clock must be 3x the pixel clock for STN panels,
		 * or 1.5x the pixel clock for TFT panels. To avoid having to
		 * check for the LCD device clock everytime we do a mode change,
		 * we set the LCD device clock to the highest rate possible.
		 */
		ret = clk_set_rate(priv->lcd_clk, parent_rate);
		if (ret) {
			dev_err(dev, "Unable to set LCD clock rate\n");
			goto err_pixclk_disable;
		}

		ret = clk_prepare_enable(priv->lcd_clk);
		if (ret) {
			dev_err(dev, "Unable to start lcd clock\n");
			goto err_pixclk_disable;
		}
	}

	if (priv->dma_clk) {
		ret = clk_prepare_enable(priv->dma_clk);
		if (ret) {
			dev_err(dev, "Unable to start dma clock\n");
			goto err_devclk_disable;
		}
	}

	/* Enable OSD if available */
	if (soc_info->has_osd)
		regmap_write(priv->map, JZ_REG_LCD_OSDC, JZ_LCD_OSDC_OSDEN);

	mutex_init(&priv->clk_mutex);
	priv->clock_nb.notifier_call = ingenic_drm_update_pixclk;

	parent_clk = ingenic_drm_get_parent_clk(priv->pix_clk);

	ret = clk_notifier_register(parent_clk, &priv->clock_nb);
	if (ret) {
		dev_err(dev, "Unable to register clock notifier\n");
		goto err_dmaclk_disable;
	}

	private_state = kzalloc(sizeof(*private_state), GFP_KERNEL);
	if (!private_state) {
		ret = -ENOMEM;
		goto err_clk_notifier_unregister;
	}

	drm_atomic_private_obj_init(drm, &priv->private_obj, &private_state->base,
				    &ingenic_drm_private_state_funcs);

	ret = drmm_add_action_or_reset(drm, ingenic_drm_atomic_private_obj_fini,
				       &priv->private_obj);
	if (ret)
		goto err_private_state_free;

	ret = drm_dev_register(drm, 0);
	if (ret) {
		dev_err(dev, "Failed to register DRM driver\n");
		goto err_clk_notifier_unregister;
	}

	drm_fbdev_generic_setup(drm, 32);

	pr_info("drm drm_bind end");

	return 0;

err_private_state_free:
	kfree(private_state);
err_clk_notifier_unregister:
	clk_notifier_unregister(parent_clk, &priv->clock_nb);
err_dmaclk_disable:
	if (priv->dma_clk)
		clk_disable_unprepare(priv->dma_clk);
err_devclk_disable:
	if (priv->lcd_clk)
		clk_disable_unprepare(priv->lcd_clk);
err_pixclk_disable:
	clk_disable_unprepare(priv->pix_clk);
	return ret;
}

static int ingenic_drm_bind_with_components(struct device *dev)
{
	return ingenic_drm_bind(dev, true);
}

static int compare_of(struct device *dev, void *data)
{
	return dev->of_node == data;
}

static void ingenic_drm_unbind(struct device *dev)
{
	struct ingenic_drm *priv = dev_get_drvdata(dev);
	struct clk *parent_clk = ingenic_drm_get_parent_clk(priv->pix_clk);

	clk_notifier_unregister(parent_clk, &priv->clock_nb);
	if (priv->dma_clk)
		clk_disable_unprepare(priv->dma_clk);
	if (priv->lcd_clk)
		clk_disable_unprepare(priv->lcd_clk);
	clk_disable_unprepare(priv->pix_clk);

	drm_dev_unregister(&priv->drm);
	drm_atomic_helper_shutdown(&priv->drm);
}

static const struct component_master_ops ingenic_master_ops = {
	.bind = ingenic_drm_bind_with_components,
	.unbind = ingenic_drm_unbind,
};

static int ingenic_drm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct component_match *match = NULL;
	struct device_node *np;

	if (!IS_ENABLED(CONFIG_DRM_INGENIC_IPU))
		return ingenic_drm_bind(dev, false);

	/* IPU is at port address 8 */
	np = of_graph_get_remote_node(dev->of_node, 8, 0);
	if (!np)
		return ingenic_drm_bind(dev, false);

	drm_of_component_match_add(dev, &match, compare_of, np);
	of_node_put(np);

	return component_master_add_with_match(dev, &ingenic_master_ops, match);
}

static int ingenic_drm_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	if (!IS_ENABLED(CONFIG_DRM_INGENIC_IPU))
		ingenic_drm_unbind(dev);
	else
		component_master_del(dev, &ingenic_master_ops);

	return 0;
}

static int __maybe_unused ingenic_drm_suspend(struct device *dev)
{
	struct ingenic_drm *priv = dev_get_drvdata(dev);

	return drm_mode_config_helper_suspend(&priv->drm);
}

static int __maybe_unused ingenic_drm_resume(struct device *dev)
{
	struct ingenic_drm *priv = dev_get_drvdata(dev);

	return drm_mode_config_helper_resume(&priv->drm);
}

static SIMPLE_DEV_PM_OPS(ingenic_drm_pm_ops, ingenic_drm_suspend, ingenic_drm_resume);

static const u32 jz4740_formats[] = {
	DRM_FORMAT_XRGB1555,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
};

static const u32 jz4725b_formats_f1[] = {
	DRM_FORMAT_XRGB1555,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
};

static const u32 jz4725b_formats_f0[] = {
	DRM_FORMAT_C8,
	DRM_FORMAT_XRGB1555,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
};

static const u32 jz4770_formats_f1[] = {
	DRM_FORMAT_XRGB1555,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_XRGB2101010,
};

static const u32 jz4770_formats_f0[] = {
	DRM_FORMAT_C8,
	DRM_FORMAT_XRGB1555,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_XRGB2101010,
};

static const struct jz_soc_info jz4740_soc_info = {
	.needs_dev_clk = true,
	.has_osd = false,
	.map_noncoherent = false,
	.max_width = 800,
	.max_height = 600,
	.formats_f1 = jz4740_formats,
	.num_formats_f1 = ARRAY_SIZE(jz4740_formats),
	/* JZ4740 has only one plane */
};

static const struct jz_soc_info jz4725b_soc_info = {
	.needs_dev_clk = false,
	.has_osd = true,
	.map_noncoherent = false,
	.max_width = 800,
	.max_height = 600,
	.formats_f1 = jz4725b_formats_f1,
	.num_formats_f1 = ARRAY_SIZE(jz4725b_formats_f1),
	.formats_f0 = jz4725b_formats_f0,
	.num_formats_f0 = ARRAY_SIZE(jz4725b_formats_f0),
};

static const struct jz_soc_info jz4760_soc_info = {
	.needs_dev_clk = false,
	.has_osd = true,
	.map_noncoherent = false,
	.max_width = 1280,
	.max_height = 720,
	.formats_f1 = jz4770_formats_f1,
	.num_formats_f1 = ARRAY_SIZE(jz4770_formats_f1),
	.formats_f0 = jz4770_formats_f0,
	.num_formats_f0 = ARRAY_SIZE(jz4770_formats_f0),
};

static const struct jz_soc_info jz4770_soc_info = {
	.needs_dev_clk = false,
	.has_osd = true,
	.map_noncoherent = true,
	.max_width = 1280,
	.max_height = 720,
	.formats_f1 = jz4770_formats_f1,
	.num_formats_f1 = ARRAY_SIZE(jz4770_formats_f1),
	.formats_f0 = jz4770_formats_f0,
	.num_formats_f0 = ARRAY_SIZE(jz4770_formats_f0),
};

static const struct of_device_id ingenic_drm_of_match[] = {
	{ .compatible = "ingenic,jz4740-lcd", .data = &jz4740_soc_info },
	{ .compatible = "ingenic,jz4725b-lcd", .data = &jz4725b_soc_info },
	{ .compatible = "ingenic,jz4760-lcd", .data = &jz4760_soc_info },
	{ .compatible = "ingenic,jz4770-lcd", .data = &jz4770_soc_info },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, ingenic_drm_of_match);

static struct platform_driver ingenic_drm_driver = {
	.driver = {
		.name = "ingenic-drm",
		.pm = pm_ptr(&ingenic_drm_pm_ops),
		.of_match_table = of_match_ptr(ingenic_drm_of_match),
	},
	.probe = ingenic_drm_probe,
	.remove = ingenic_drm_remove,
};

static int ingenic_drm_init(void)
{
	int err;

	if (IS_ENABLED(CONFIG_DRM_INGENIC_IPU)) {
		err = platform_driver_register(ingenic_ipu_driver_ptr);
		if (err)
			return err;
	}

	return platform_driver_register(&ingenic_drm_driver);
}
module_init(ingenic_drm_init);

static void ingenic_drm_exit(void)
{
	platform_driver_unregister(&ingenic_drm_driver);

	if (IS_ENABLED(CONFIG_DRM_INGENIC_IPU))
		platform_driver_unregister(ingenic_ipu_driver_ptr);
}
module_exit(ingenic_drm_exit);

MODULE_AUTHOR("Paul Cercueil <paul@crapouillou.net>");
MODULE_DESCRIPTION("DRM driver for the Ingenic SoCs\n");
MODULE_LICENSE("GPL v2");
