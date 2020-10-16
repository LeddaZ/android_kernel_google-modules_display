// SPDX-License-Identifier: GPL-2.0-only
/* exynos_drm_decon.c
 *
 * Copyright (C) 2018 Samsung Electronics Co.Ltd
 * Authors:
 *	Hyung-jun Kim <hyungjun07.kim@samsung.com>
 *	Seong-gyu Park <seongyu.park@samsung.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_vblank.h>
#include <drm/exynos_drm.h>
#include <drm/exynos_display_common.h>

#include <linux/clk.h>
#include <linux/component.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/irq.h>
#include <linux/pm_runtime.h>
#include <linux/console.h>
#include <linux/iommu.h>
#include <uapi/linux/sched/types.h>

#include <video/videomode.h>

#include <decon_cal.h>
#include <regs-decon.h>

#include "exynos_drm_crtc.h"
#include "exynos_drm_decon.h"
#include "exynos_drm_dpp.h"
#include "exynos_drm_drv.h"
#include "exynos_drm_dsim.h"
#include "exynos_drm_fb.h"
#include "exynos_drm_plane.h"

struct decon_device *decon_drvdata[MAX_DECON_CNT];

#define decon_info(decon, fmt, ...)	\
pr_info("%s[%d]: "fmt, decon->dev->driver->name, decon->id, ##__VA_ARGS__)

#define decon_warn(decon, fmt, ...)	\
pr_warn("%s[%d]: "fmt, decon->dev->driver->name, decon->id, ##__VA_ARGS__)

#define decon_err(decon, fmt, ...)	\
pr_err("%s[%d]: "fmt, decon->dev->driver->name, decon->id, ##__VA_ARGS__)

#define decon_debug(decon, fmt, ...)	\
pr_debug("%s[%d]: "fmt, decon->dev->driver->name, decon->id, ##__VA_ARGS__)

#define SHADOW_UPDATE_TIMEOUT_US	(300 * USEC_PER_MSEC) /* 300ms */

static const struct of_device_id decon_driver_dt_match[] = {
	{.compatible = "samsung,exynos-decon"},
	{},
};
MODULE_DEVICE_TABLE(of, decon_driver_dt_match);

static void decon_mode_update_bts(struct decon_device *decon, const struct drm_display_mode *mode);
static void decon_seamless_mode_set(struct exynos_drm_crtc *exynos_crtc,
				    struct drm_crtc_state *old_crtc_state);

void decon_dump(struct decon_device *decon)
{
	int i;
	int acquired = console_trylock();
	struct decon_device *d;

	for (i = 0; i < REGS_DECON_ID_MAX; ++i) {
		d = get_decon_drvdata(i);
		if (!d)
			continue;

		if (d->state != DECON_STATE_ON) {
			decon_info(decon, "DECON disabled(%d)\n", decon->state);
			continue;
		}

		__decon_dump(d->id, &d->regs, d->config.dsc.enabled);
	}

	for (i = 0; i < decon->dpp_cnt; ++i)
		dpp_dump(decon->dpp[i]);

	if (acquired)
		console_unlock();
}

static inline u32 win_start_pos(int x, int y)
{
	return (WIN_STRPTR_Y_F(y) | WIN_STRPTR_X_F(x));
}

static inline u32 win_end_pos(int x, int y,  u32 xres, u32 yres)
{
	return (WIN_ENDPTR_Y_F(y + yres - 1) | WIN_ENDPTR_X_F(x + xres - 1));
}

static inline bool is_tui(const struct drm_crtc_state *crtc_state)
{
	if (crtc_state && (crtc_state->adjusted_mode.private_flags &
				EXYNOS_DISPLAY_MODE_FLAG_TUI))
		return true;

	return false;
}

/* ARGB value */
#define COLOR_MAP_VALUE			0x00340080

/*
 * This function can be used in cases where all windows are disabled
 * but need something to be rendered for display. This will make a black
 * frame via decon using a single window with color map enabled.
 */
static void decon_set_color_map(struct decon_device *decon, u32 win_id,
						u32 hactive, u32 vactive)
{
	struct decon_window_regs win_info;

	decon_debug(decon, "%s +\n", __func__);

	memset(&win_info, 0, sizeof(struct decon_window_regs));
	win_info.start_pos = win_start_pos(0, 0);
	win_info.end_pos = win_end_pos(0, 0, hactive, vactive);
	win_info.start_time = 0;
	win_info.colormap = 0x000000; /* black */
	win_info.blend = DECON_BLENDING_NONE;
	decon_reg_set_window_control(decon->id, win_id, &win_info, true);
	decon_reg_update_req_window(decon->id, win_id);

	decon_debug(decon, "%s -\n", __func__);
}

static int decon_enable_vblank(struct exynos_drm_crtc *crtc)
{
	struct decon_device *decon = crtc->ctx;

	/* TODO : need to write code completely */
	decon_debug(decon, "%s\n", __func__);

	return 0;
}

static void decon_disable_vblank(struct exynos_drm_crtc *crtc)
{
	struct decon_device *decon = crtc->ctx;

	/* TODO : need to write code completely */
	decon_debug(decon, "%s\n", __func__);
}

static bool has_writeback_job(struct drm_crtc_state *new_crtc_state)
{
	int i;
	struct drm_atomic_state *state = new_crtc_state->state;
	struct drm_connector_state *conn_state;
	struct drm_connector *conn;

	for_each_new_connector_in_state(state, conn, conn_state, i) {
		if (!(new_crtc_state->connector_mask &
					drm_connector_mask(conn)))
			continue;

		if (wb_check_job(conn_state))
			return true;
	}
	return false;
}

static void decon_update_config(struct decon_config *config,
				const struct drm_display_mode *mode,
				const struct exynos_display_mode *exynos_mode)
{
	bool is_vid_mode;

	config->image_width = mode->hdisplay;
	config->image_height = mode->vdisplay;

	if (!exynos_mode) {
		pr_debug("%s: no private mode config\n", __func__);

		/* valid defaults (ex. for writeback) */
		config->dsc.enabled = false;
		config->out_bpc = 8;
		return;
	}

	config->dsc.enabled = exynos_mode->dsc.enabled;
	if (exynos_mode->dsc.enabled) {
		config->dsc.dsc_count = exynos_mode->dsc.dsc_count;
		config->dsc.slice_count = exynos_mode->dsc.slice_count;
		config->dsc.slice_height = exynos_mode->dsc.slice_height;
		config->dsc.slice_width = DIV_ROUND_UP(config->image_width,
						       config->dsc.slice_count);
	}

	is_vid_mode = (exynos_mode->mode_flags & MIPI_DSI_MODE_VIDEO) != 0;
	config->mode.op_mode = is_vid_mode ? DECON_VIDEO_MODE : DECON_COMMAND_MODE;

	config->out_bpc = exynos_mode->bpc;
}

static bool decon_is_seamless_possible(const struct decon_device *decon,
				       const struct drm_display_mode *mode,
				       const struct exynos_display_mode *exynos_mode)
{
	struct decon_config new_config = decon->config;

	decon_update_config(&new_config, mode, exynos_mode);

	/* don't allow any changes in decon config */
	return !memcmp(&new_config, &decon->config, sizeof(new_config));
}

static int decon_check_modeset(struct exynos_drm_crtc *exynos_crtc,
			       struct drm_crtc_state *crtc_state)
{
	const struct drm_atomic_state *state = crtc_state->state;
	const struct decon_device *decon = exynos_crtc->ctx;
	struct exynos_drm_crtc_state *exynos_crtc_state;
	const struct exynos_drm_connector_state *exynos_conn_state;

	exynos_conn_state = crtc_get_exynos_connector_state(state, crtc_state);
	if (!exynos_conn_state)
		return 0;

	if (!(exynos_conn_state->exynos_mode.mode_flags & MIPI_DSI_MODE_VIDEO)) {
		if (!decon->irq_te || !decon->res.pinctrl) {
			decon_err(decon, "TE error: irq_te %d, te_pinctrl %p\n",
				  decon->irq_te, decon->res.pinctrl);

			return -EINVAL;
		}
	}

	if (exynos_conn_state->seamless_possible && !crtc_state->connectors_changed &&
	    !crtc_state->active_changed && crtc_state->active) {
		if (!decon_is_seamless_possible(decon, &crtc_state->adjusted_mode,
						&exynos_conn_state->exynos_mode)) {
			decon_warn(decon, "seamless not possible for mode %s\n",
				   crtc_state->adjusted_mode.name);
		} else {
			exynos_crtc_state = to_exynos_crtc_state(crtc_state);
			exynos_crtc_state->seamless_mode_changed = true;
			crtc_state->mode_changed = false;

			decon_debug(decon, "switch to mode %s can be seamless\n",
				    crtc_state->adjusted_mode.name);
		}
	}

	return 0;
}

static int decon_atomic_check(struct exynos_drm_crtc *exynos_crtc,
			      struct drm_crtc_state *crtc_state)
{
	const struct decon_device *decon = exynos_crtc->ctx;
	const bool is_wb = has_writeback_job(crtc_state);
	const bool is_swb = decon->config.out_type == DECON_OUT_WB;
	struct exynos_drm_crtc_state *exynos_crtc_state = to_exynos_crtc_state(crtc_state);
	int ret = 0;

	if (is_wb) {
		exynos_crtc_state->wb_type =
			is_swb ? EXYNOS_WB_SWB : EXYNOS_WB_CWB;
	} else {
		exynos_crtc_state->wb_type = EXYNOS_WB_NONE;
	}

	if (is_swb)
		crtc_state->no_vblank = true;

	if (crtc_state->mode_changed)
		ret = decon_check_modeset(exynos_crtc, crtc_state);

	return ret;
}

static void decon_atomic_begin(struct exynos_drm_crtc *crtc)
{
	struct decon_device *decon = crtc->ctx;

	decon_debug(decon, "%s +\n", __func__);
	DPU_EVENT_LOG(DPU_EVT_ATOMIC_BEGIN, decon->id, NULL);
	decon_reg_wait_update_done_and_mask(decon->id, &decon->config.mode,
			SHADOW_UPDATE_TIMEOUT_US);
	decon_debug(decon, "%s -\n", __func__);
}

static void decon_disable_win(struct decon_device *decon, int win_id)
{
	const struct drm_crtc *crtc = &decon->crtc->base;
	const unsigned int num_planes = hweight32(crtc->state->plane_mask);

	decon_debug(decon, "winid:%d/%d\n", win_id, num_planes);

	/*
	 * When disabling the plane, previously connected window(zpos) should be
	 * disabled not newly requested zpos(window). Only disable window if it
	 * was previously connected and it's not going to be used by any other
	 * plane, by using normalized zpos as win_id we know that any win_id
	 * beyond the number of planes will not be used.
	 */
	if (win_id < MAX_PLANE && win_id >= num_planes)
		decon_reg_set_win_enable(decon->id, win_id, 0);
}

static void _dpp_disable(struct dpp_device *dpp)
{
	if (dpp->is_win_connected) {
		dpp->disable(dpp);
		dpp->is_win_connected = false;
	}
}

static void decon_update_plane(struct exynos_drm_crtc *crtc,
			       struct exynos_drm_plane *plane)
{
	struct exynos_drm_plane_state *state =
				to_exynos_plane_state(plane->base.state);
	struct dpp_device *dpp = plane_to_dpp(plane);
	struct decon_device *decon = crtc->ctx;
	struct decon_window_regs win_info;
	unsigned int zpos;
	bool is_colormap = false;
	u16 hw_alpha;

	decon_debug(decon, "%s +\n", __func__);

	memset(&win_info, 0, sizeof(struct decon_window_regs));

	is_colormap = state->base.fb &&
			exynos_drm_fb_is_colormap(state->base.fb);
	if (is_colormap)
		win_info.colormap = state->colormap;

	win_info.start_pos = win_start_pos(state->crtc.x, state->crtc.y);
	win_info.end_pos = win_end_pos(state->crtc.x, state->crtc.y,
			state->crtc.w, state->crtc.h);
	win_info.start_time = 0;

	win_info.ch = dpp->id; /* DPP's id is DPP channel number */

	hw_alpha = DIV_ROUND_CLOSEST(state->base.alpha * EXYNOS_PLANE_ALPHA_MAX,
			DRM_BLEND_ALPHA_OPAQUE);
	win_info.plane_alpha = hw_alpha;
	win_info.blend = state->base.pixel_blend_mode;

	zpos = state->base.normalized_zpos;
	if (zpos == 0 && hw_alpha == EXYNOS_PLANE_ALPHA_MAX)
		win_info.blend = DRM_MODE_BLEND_PIXEL_NONE;

	/* disable previous window if zpos has changed */
	if (dpp->win_id != zpos)
		decon_disable_win(decon, dpp->win_id);

	decon_reg_set_window_control(decon->id, zpos, &win_info, is_colormap);

	dpp->decon_id = decon->id;
	if (!is_colormap) {
		dpp->update(dpp, state);
		dpp->is_win_connected = true;
	} else {
		_dpp_disable(dpp);
	}

	dpp->win_id = zpos;

	DPU_EVENT_LOG(DPU_EVT_PLANE_UPDATE, decon->id, dpp);
	decon_debug(decon, "plane idx[%d]: alpha(0x%x) hw alpha(0x%x)\n",
			drm_plane_index(&plane->base), state->base.alpha,
			hw_alpha);
	decon_debug(decon, "blend_mode(%d) color(%s:0x%x)\n", win_info.blend,
			is_colormap ? "enable" : "disable", win_info.colormap);
	decon_debug(decon, "%s -\n", __func__);
}

static void decon_disable_plane(struct exynos_drm_crtc *exynos_crtc,
				struct exynos_drm_plane *exynos_plane)
{
	struct decon_device *decon = exynos_crtc->ctx;
	struct dpp_device *dpp = plane_to_dpp(exynos_plane);

	decon_debug(decon, "%s +\n", __func__);

	decon_disable_win(decon, dpp->win_id);

	_dpp_disable(dpp);

	DPU_EVENT_LOG(DPU_EVT_PLANE_DISABLE, decon->id, dpp);
	decon_debug(decon, "%s -\n", __func__);
}

static void decon_atomic_flush(struct exynos_drm_crtc *exynos_crtc,
		struct drm_crtc_state *old_crtc_state)
{
	struct decon_device *decon = exynos_crtc->ctx;
	struct drm_crtc_state *new_crtc_state = exynos_crtc->base.state;
	struct exynos_drm_crtc_state *new_exynos_crtc_state =
					to_exynos_crtc_state(new_crtc_state);
	struct exynos_drm_crtc_state *old_exynos_crtc_state =
					to_exynos_crtc_state(old_crtc_state);
	struct exynos_dqe *dqe = decon->dqe;
	unsigned long flags;

	decon_debug(decon, "%s +\n", __func__);

	if (new_exynos_crtc_state->wb_type == EXYNOS_WB_NONE &&
			decon->config.out_type == DECON_OUT_WB)
		return;

	if (new_exynos_crtc_state->wb_type == EXYNOS_WB_CWB)
		decon_reg_set_cwb_enable(decon->id, true);
	else if (old_exynos_crtc_state->wb_type == EXYNOS_WB_CWB)
		decon_reg_set_cwb_enable(decon->id, false);

	/* if there are no planes attached, enable colormap as fallback */
	if (new_crtc_state->plane_mask == 0) {
		decon_debug(decon, "no planes, enable color map\n");

		decon_set_color_map(decon, 0, decon->config.image_width,
				decon->config.image_height);
	}

	decon->config.in_bpc = new_exynos_crtc_state->in_bpc;
	decon_reg_set_bpc_and_dither_path(decon->id, &decon->config);
	decon_debug(decon, "in/out/force bpc(%d/%d/%d)\n",
			new_exynos_crtc_state->in_bpc, decon->config.out_bpc,
			new_exynos_crtc_state->force_bpc);

	if (dqe)
		exynos_dqe_update(dqe, &new_exynos_crtc_state->dqe,
				decon->config.image_width,
				decon->config.image_height);

	decon_reg_all_win_shadow_update_req(decon->id);

	if (atomic_add_unless(&decon->bts.delayed_update, -1, 0))
		decon_mode_update_bts(decon, &new_crtc_state->mode);

	if (new_exynos_crtc_state->seamless_mode_changed)
		decon_seamless_mode_set(exynos_crtc, old_crtc_state);

	spin_lock_irqsave(&decon->slock, flags);
	decon_reg_start(decon->id, &decon->config);
	reinit_completion(&decon->framestart_done);
	spin_unlock_irqrestore(&decon->slock, flags);

	if (!new_crtc_state->no_vblank)
		exynos_crtc_handle_event(exynos_crtc);

	DPU_EVENT_LOG(DPU_EVT_ATOMIC_FLUSH, decon->id, NULL);

	decon_debug(decon, "%s -\n", __func__);
}

static void decon_print_config_info(struct decon_device *decon)
{
	char *str_output = NULL;
	char *str_trigger = NULL;

	if (decon->config.mode.trig_mode == DECON_HW_TRIG)
		str_trigger = "hw trigger.";
	else if (decon->config.mode.trig_mode == DECON_SW_TRIG)
		str_trigger = "sw trigger.";
	if (decon->config.mode.op_mode == DECON_VIDEO_MODE)
		str_trigger = "";

	if (decon->config.out_type == DECON_OUT_DSI)
		str_output = "Dual DSI";
	else if (decon->config.out_type & DECON_OUT_DSI0)
		str_output = "DSI0";
	else if  (decon->config.out_type & DECON_OUT_DSI1)
		str_output = "DSI1";
	else if  (decon->config.out_type & DECON_OUT_DP0)
		str_output = "DP0";
	else if  (decon->config.out_type & DECON_OUT_DP1)
		str_output = "DP1";
	else if  (decon->config.out_type & DECON_OUT_WB)
		str_output = "WB";

	decon_info(decon, "%s mode. %s %s output.(%dx%d@%dhz)\n",
			decon->config.mode.op_mode ? "command" : "video",
			str_trigger, str_output,
			decon->config.image_width, decon->config.image_height,
			decon->bts.fps);
}

static void decon_set_te_pinctrl(struct decon_device *decon, bool en)
{
	int ret;

	if ((decon->config.mode.op_mode != DECON_COMMAND_MODE) ||
			(decon->config.mode.trig_mode != DECON_HW_TRIG))
		return;

	if (!decon->res.pinctrl || !decon->res.te_on)
		return;

	ret = pinctrl_select_state(decon->res.pinctrl,
			en ? decon->res.te_on : decon->res.te_off);
	if (ret)
		decon_err(decon, "failed to control decon TE(%d)\n", en);
}

static void decon_enable_irqs(struct decon_device *decon)
{
	decon_reg_set_interrupts(decon->id, 1);

	enable_irq(decon->irq_fs);
	enable_irq(decon->irq_fd);
	enable_irq(decon->irq_ext);
	if ((decon->config.mode.op_mode == DECON_COMMAND_MODE) &&
			(decon->config.mode.trig_mode == DECON_HW_TRIG))
		enable_irq(decon->irq_te);
}

static void _decon_enable(struct decon_device *decon)
{
	decon_reg_init(decon->id, &decon->config);
	decon_enable_irqs(decon);
}

static void decon_mode_update_bts(struct decon_device *decon, const struct drm_display_mode *mode)
{
	struct videomode vm;
	int i;

	drm_display_mode_to_videomode(mode, &vm);

	decon->bts.vbp = vm.vback_porch;
	decon->bts.vfp = vm.vfront_porch;
	decon->bts.vsa = vm.vsync_len;
	decon->bts.fps = drm_mode_vrefresh(mode);

	for (i = 0; i < MAX_WIN_PER_DECON; i++)
		decon->bts.win_config[i].state = DPU_WIN_STATE_DISABLED;
}

static void decon_mode_set(struct exynos_drm_crtc *crtc,
			   const struct drm_display_mode *mode,
			   const struct drm_display_mode *adjusted_mode)
{
	struct decon_device *decon = crtc->ctx;

	decon_mode_update_bts(decon, adjusted_mode);
}

#if defined(CONFIG_EXYNOS_BTS)
static void decon_seamless_mode_bts_update(struct decon_device *decon,
					   const struct drm_display_mode *mode)
{
	/*
	 * when going from high->low refresh rate need to run with the higher fps while the
	 * switch takes effect in display, this could happen within 2 vsyncs in the worst case
	 */
	if (decon->bts.fps > drm_mode_vrefresh(mode)) {
		atomic_set(&decon->bts.delayed_update, 2);
		return;
	}

	decon_mode_update_bts(decon, mode);
	atomic_set(&decon->bts.delayed_update, 0);

	decon->bts.ops->calc_bw(decon);
	decon->bts.ops->update_bw(decon, false);
}
#else
static inline void
decon_seamless_mode_bts_update(struct decon_device *decon,
			       const struct drm_display_mode *mode) { }
#endif

static void decon_seamless_mode_set(struct exynos_drm_crtc *exynos_crtc,
				    struct drm_crtc_state *old_crtc_state)
{
	struct drm_crtc *crtc = &exynos_crtc->base;
	struct decon_device *decon = exynos_crtc->ctx;
	struct drm_crtc_state *crtc_state = crtc->state;
	struct drm_atomic_state *old_state = old_crtc_state->state;
	struct drm_connector *conn;
	struct drm_connector_state *conn_state;
	struct drm_display_mode *mode, *adjusted_mode;
	int i;

	mode = &crtc_state->mode;
	adjusted_mode = &crtc_state->adjusted_mode;

	decon_debug(decon, "seamless mode set to %s\n", mode->name);

	decon_seamless_mode_bts_update(decon, adjusted_mode);

	for_each_new_connector_in_state(old_state, conn, conn_state, i) {
		const struct drm_encoder_helper_funcs *funcs;
		struct drm_encoder *encoder;
		struct drm_bridge *bridge;

		if (!(crtc_state->connector_mask & drm_connector_mask(conn)))
			continue;

		if (!conn_state->best_encoder)
			continue;

		encoder = conn_state->best_encoder;
		funcs = encoder->helper_private;

		if (funcs && funcs->atomic_mode_set)
			funcs->atomic_mode_set(encoder, crtc_state, conn_state);
		else if (funcs && funcs->mode_set)
			funcs->mode_set(encoder, mode, adjusted_mode);

		bridge = drm_bridge_chain_get_first_bridge(encoder);
		drm_bridge_chain_mode_set(bridge, mode, adjusted_mode);
	}
}

static void decon_enable(struct exynos_drm_crtc *crtc, struct drm_crtc_state *old_crtc_state)
{
	const struct drm_crtc_state *crtc_state = crtc->base.state;
	struct decon_device *decon = crtc->ctx;
	int i;

	if (crtc_state->mode_changed) {
		const struct drm_atomic_state *state = old_crtc_state->state;
		const struct exynos_drm_connector_state *exynos_conn_state =
			crtc_get_exynos_connector_state(state, crtc_state);
		const struct exynos_display_mode *exynos_mode = NULL;

		if (exynos_conn_state)
			exynos_mode = &exynos_conn_state->exynos_mode;

		decon_update_config(&decon->config, &crtc_state->adjusted_mode, exynos_mode);
	}

	if (decon->state == DECON_STATE_ON) {
		decon_info(decon, "already enabled(%d)\n", decon->state);
		return;
	}

	decon_info(decon, "%s +\n", __func__);

	if (is_tui(crtc_state)) {
		decon_debug(decon, "tui_state : skip power enable\n");
	} else {
		pm_runtime_get_sync(decon->dev);
		decon_set_te_pinctrl(decon, true);
	}

	_decon_enable(decon);

	for (i = 0; i < decon->dpp_cnt; i++) {
		struct dpp_device *dpp = decon->dpp[i];

		if ((dpp->win_id < MAX_WIN_PER_DECON) &&
		    ((dpp->decon_id < 0) || (dpp->decon_id == decon->id))) {
			dpp->win_id = 0xFF;
			dpp->dbg_dma_addr = 0;
		}
	}

	/*
	 * Make sure all window connections are disabled when getting enabled, in case there are any
	 * stale mappings. New mappings will happen later before atomic flush
	 */
	for (i = 0; i < MAX_WIN_PER_DECON; ++i)
		decon_reg_set_win_enable(decon->id, i, 0);

	decon_print_config_info(decon);

	decon->state = DECON_STATE_ON;

	DPU_EVENT_LOG(DPU_EVT_DECON_ENABLED, decon->id, decon);

	decon_info(decon, "%s -\n", __func__);
}

void decon_exit_hibernation(struct decon_device *decon)
{
	if (decon->state != DECON_STATE_HIBERNATION)
		return;

	decon_debug(decon, "%s +\n", __func__);

	_decon_enable(decon);

	decon->state = DECON_STATE_ON;

	decon_debug(decon, "%s -\n", __func__);
}

static void decon_disable_irqs(struct decon_device *decon)
{
	disable_irq(decon->irq_fs);
	disable_irq(decon->irq_fd);
	disable_irq(decon->irq_ext);
	decon_reg_set_interrupts(decon->id, 0);
	if ((decon->config.mode.op_mode == DECON_COMMAND_MODE) &&
			(decon->config.mode.trig_mode == DECON_HW_TRIG))
		disable_irq(decon->irq_te);
}

static void _decon_disable(struct decon_device *decon)
{
	const struct drm_crtc_state *crtc_state = decon->crtc->base.state;

	decon_reg_stop(decon->id, &decon->config, crtc_state->active_changed, decon->bts.fps);
	decon_disable_irqs(decon);
}

void decon_enter_hibernation(struct decon_device *decon)
{
	decon_debug(decon, "%s +\n", __func__);

	if (decon->state != DECON_STATE_ON)
		return;

	_decon_disable(decon);

	decon->state = DECON_STATE_HIBERNATION;
	decon_debug(decon, "%s -\n", __func__);
}

static void decon_disable(struct exynos_drm_crtc *crtc)
{
	struct decon_device *decon = crtc->ctx;

	if (decon->state == DECON_STATE_OFF)
		return;

	decon_info(decon, "%s +\n", __func__);

	_decon_disable(decon);

	if (is_tui(crtc->base.state)) {
		decon_debug(decon, "tui_state : skip power disable\n");
	} else {
		decon_set_te_pinctrl(decon, false);
		pm_runtime_put_sync(decon->dev);
	}

	decon->state = DECON_STATE_OFF;

	DPU_EVENT_LOG(DPU_EVT_DECON_DISABLED, decon->id, decon);

	decon_info(decon, "%s -\n", __func__);
}

static const struct exynos_drm_crtc_ops decon_crtc_ops = {
	.enable = decon_enable,
	.disable = decon_disable,
	.enable_vblank = decon_enable_vblank,
	.disable_vblank = decon_disable_vblank,
	.mode_set = decon_mode_set,
	.atomic_check = decon_atomic_check,
	.atomic_begin = decon_atomic_begin,
	.update_plane = decon_update_plane,
	.disable_plane = decon_disable_plane,
	.atomic_flush = decon_atomic_flush,
};

static int dpu_sysmmu_fault_handler(struct iommu_fault *fault, void *data)
{
	struct decon_device *decon = data;

	if (!decon)
		return 0;

	decon_warn(decon, "%s +\n", __func__);

	decon_dump_all(decon);

	return 0;
}

static int decon_bind(struct device *dev, struct device *master, void *data)
{
	struct decon_device *decon = dev_get_drvdata(dev);
	struct drm_device *drm_dev = data;
	struct exynos_drm_private *priv = drm_dev->dev_private;
	struct drm_plane *default_plane;
	int i;

	decon->drm_dev = drm_dev;

	default_plane = &decon->dpp[decon->id]->plane.base;

	decon->crtc = exynos_drm_crtc_create(drm_dev, default_plane,
			decon->con_type, &decon_crtc_ops, decon);
	if (IS_ERR(decon->crtc))
		return PTR_ERR(decon->crtc);

	for (i = 0; i < decon->dpp_cnt; ++i) {
		struct dpp_device *dpp = decon->dpp[i];
		struct drm_plane *plane = &dpp->plane.base;

		plane->possible_crtcs |=
			drm_crtc_mask(&decon->crtc->base);
		decon_debug(decon, "plane possible_crtcs = 0x%x\n",
				plane->possible_crtcs);
	}

	priv->iommu_client = dev;

	iommu_register_device_fault_handler(dev, dpu_sysmmu_fault_handler, decon);

#if IS_ENABLED(CONFIG_EXYNOS_ITMON)
	decon->itmon_nb.notifier_call = dpu_itmon_notifier;
	itmon_notifier_chain_register(&decon->itmon_nb);
#endif

	if (IS_ENABLED(CONFIG_EXYNOS_BTS)) {
		decon->bts.ops = &dpu_bts_control;
		decon->bts.ops->init(decon);
	}

	decon_debug(decon, "%s -\n", __func__);
	return 0;
}

static void decon_unbind(struct device *dev, struct device *master,
			void *data)
{
	struct decon_device *decon = dev_get_drvdata(dev);

	decon_debug(decon, "%s +\n", __func__);
	if (IS_ENABLED(CONFIG_EXYNOS_BTS))
		decon->bts.ops->deinit(decon);

	decon_disable(decon->crtc);
	decon_debug(decon, "%s -\n", __func__);
}

static const struct component_ops decon_component_ops = {
	.bind	= decon_bind,
	.unbind = decon_unbind,
};

static irqreturn_t decon_irq_handler(int irq, void *dev_data)
{
	struct decon_device *decon = dev_data;
	u32 irq_sts_reg;
	u32 ext_irq = 0;

	spin_lock(&decon->slock);
	if (decon->state != DECON_STATE_ON)
		goto irq_end;

	irq_sts_reg = decon_reg_get_interrupt_and_clear(decon->id, &ext_irq);
	decon_debug(decon, "%s: irq_sts_reg = %x, ext_irq = %x\n",
			__func__, irq_sts_reg, ext_irq);

	if (irq_sts_reg & DPU_FRAME_DONE_INT_PEND) {
		DPU_EVENT_LOG(DPU_EVT_DECON_FRAMEDONE, decon->id, decon);
		decon->busy = false;
		wake_up_interruptible_all(&decon->framedone_wait);
		decon_debug(decon, "%s: frame done\n", __func__);
	}

	if (irq_sts_reg & DPU_FRAME_START_INT_PEND) {
		decon->busy = true;
		complete(&decon->framestart_done);
		DPU_EVENT_LOG(DPU_EVT_DECON_FRAMESTART, decon->id, decon);
		decon_debug(decon, "%s: frame start\n", __func__);
	}

	if (ext_irq & DPU_RESOURCE_CONFLICT_INT_PEND)
		decon_debug(decon, "%s: resource conflict\n", __func__);

	if (ext_irq & DPU_TIME_OUT_INT_PEND) {
		decon_err(decon, "%s: timeout irq occurs\n", __func__);
		decon_dump(decon);
		WARN_ON(1);
	}

irq_end:
	spin_unlock(&decon->slock);
	return IRQ_HANDLED;
}

static int decon_parse_dt(struct decon_device *decon, struct device_node *np)
{
	struct device_node *dpp_np = NULL;
	struct property *prop;
	const __be32 *cur;
	u32 val;
	int ret = 0, i;
	int dpp_id;

	of_property_read_u32(np, "decon,id", &decon->id);

	ret = of_property_read_u32(np, "max_win", &decon->win_cnt);
	if (ret) {
		decon_err(decon, "failed to parse max windows count\n");
		return ret;
	}

	ret = of_property_read_u32(np, "op_mode", &decon->config.mode.op_mode);
	if (ret) {
		decon_err(decon, "failed to parse operation mode(%d)\n", ret);
		return ret;
	}

	ret = of_property_read_u32(np, "trig_mode",
			&decon->config.mode.trig_mode);
	if (ret) {
		decon_err(decon, "failed to parse trigger mode(%d)\n", ret);
		return ret;
	}

	ret = of_property_read_u32(np, "rd_en", &decon->config.urgent.rd_en);
	if (ret)
		decon_warn(decon, "failed to parse urgent rd_en(%d)\n", ret);

	ret = of_property_read_u32(np, "rd_hi_thres",
			&decon->config.urgent.rd_hi_thres);
	if (ret) {
		decon_warn(decon, "failed to parse urgent rd_hi_thres(%d)\n",
				ret);
	}

	ret = of_property_read_u32(np, "rd_lo_thres",
			&decon->config.urgent.rd_lo_thres);
	if (ret) {
		decon_warn(decon, "failed to parse urgent rd_lo_thres(%d)\n",
				ret);
	}

	ret = of_property_read_u32(np, "rd_wait_cycle",
			&decon->config.urgent.rd_wait_cycle);
	if (ret) {
		decon_warn(decon, "failed to parse urgent rd_wait_cycle(%d)\n",
				ret);
	}

	ret = of_property_read_u32(np, "wr_en", &decon->config.urgent.wr_en);
	if (ret)
		decon_warn(decon, "failed to parse urgent wr_en(%d)\n", ret);

	ret = of_property_read_u32(np, "wr_hi_thres",
			&decon->config.urgent.wr_hi_thres);
	if (ret) {
		decon_warn(decon, "failed to parse urgent wr_hi_thres(%d)\n",
				ret);
	}

	ret = of_property_read_u32(np, "wr_lo_thres",
			&decon->config.urgent.wr_lo_thres);
	if (ret) {
		decon_warn(decon, "failed to parse urgent wr_lo_thres(%d)\n",
				ret);
	}

	decon->config.urgent.dta_en = of_property_read_bool(np, "dta_en");
	if (decon->config.urgent.dta_en) {
		ret = of_property_read_u32(np, "dta_hi_thres",
				&decon->config.urgent.dta_hi_thres);
		if (ret) {
			decon_err(decon, "failed to parse dta_hi_thres(%d)\n",
					ret);
		}
		ret = of_property_read_u32(np, "dta_lo_thres",
				&decon->config.urgent.dta_lo_thres);
		if (ret) {
			decon_err(decon, "failed to parse dta_lo_thres(%d)\n",
					ret);
		}
	}

	ret = of_property_read_u32(np, "out_type", &decon->config.out_type);
	if (ret) {
		decon_err(decon, "failed to parse output type(%d)\n", ret);
		return ret;
	}

	if (decon->config.mode.trig_mode == DECON_HW_TRIG) {
		ret = of_property_read_u32(np, "te_from",
				&decon->config.te_from);
		if (ret) {
			decon_err(decon, "failed to get TE from DDI\n");
			return ret;
		}
		if (decon->config.te_from >= MAX_DECON_TE_FROM_DDI) {
			decon_err(decon, "TE from DDI is wrong(%d)\n",
					decon->config.te_from);
			return ret;
		}
		decon_info(decon, "TE from DDI%d\n", decon->config.te_from);
	} else {
		decon->config.te_from = MAX_DECON_TE_FROM_DDI;
		decon_info(decon, "TE from NONE\n");
	}

	if (of_property_read_u32(np, "ppc", (u32 *)&decon->bts.ppc))
		decon->bts.ppc = 2UL;
	decon_info(decon, "PPC(%llu)\n", decon->bts.ppc);

	if (of_property_read_u32(np, "line_mem_cnt",
				(u32 *)&decon->bts.line_mem_cnt)) {
		decon->bts.line_mem_cnt = 4UL;
		decon_warn(decon, "line memory cnt is not defined in DT.\n");
	}
	decon_info(decon, "line memory cnt(%d)\n", decon->bts.line_mem_cnt);

	if (of_property_read_u32(np, "cycle_per_line",
				(u32 *)&decon->bts.cycle_per_line)) {
		decon->bts.cycle_per_line = 8UL;
		decon_warn(decon, "cycle per line is not defined in DT.\n");
	}
	decon_info(decon, "cycle per line(%d)\n", decon->bts.cycle_per_line);

	if (decon->config.out_type == DECON_OUT_DSI)
		decon->config.mode.dsi_mode = DSI_MODE_DUAL_DSI;
	else if (decon->config.out_type & (DECON_OUT_DSI0 | DECON_OUT_DSI1))
		decon->config.mode.dsi_mode = DSI_MODE_SINGLE;
	else
		decon->config.mode.dsi_mode = DSI_MODE_NONE;

	decon->dpp_cnt = of_count_phandle_with_args(np, "dpps", NULL);
	for (i = 0; i < decon->dpp_cnt; ++i) {
		dpp_np = of_parse_phandle(np, "dpps", i);
		if (!dpp_np) {
			decon_err(decon, "can't find dpp%d node\n", i);
			return -EINVAL;
		}

		decon->dpp[i] = of_find_dpp_by_node(dpp_np);
		if (!decon->dpp[i]) {
			decon_err(decon, "can't find dpp%d structure\n", i);
			return -EINVAL;
		}

		dpp_id = decon->dpp[i]->id;
		decon_info(decon, "found dpp%d\n", dpp_id);

		if (dpp_np)
			of_node_put(dpp_np);
	}

	of_property_for_each_u32(np, "connector", prop, cur, val)
		decon->con_type |= val;

	return 0;
}

static int decon_remap_regs(struct decon_device *decon)
{
	struct device *dev = decon->dev;
	struct device_node *np = dev->of_node;
	int i, ret = 0;

	i = of_property_match_string(np, "reg-names", "main");
	decon->regs.regs = of_iomap(np, i);
	if (IS_ERR(decon->regs.regs)) {
		decon_err(decon, "failed decon ioremap\n");
		ret = PTR_ERR(decon->regs.regs);
		goto err;
	}
	decon_regs_desc_init(decon->regs.regs, "decon", REGS_DECON,
			decon->id);

	np = of_find_compatible_node(NULL, NULL, "samsung,exynos9-disp_ss");
	if (IS_ERR_OR_NULL(np)) {
		decon_err(decon, "failed to find disp_ss node");
		ret = PTR_ERR(np);
		goto err_main;
	}
	i = of_property_match_string(np, "reg-names", "sys");
	decon->regs.ss_regs = of_iomap(np, i);
	if (!decon->regs.ss_regs) {
		decon_err(decon, "failed to map sysreg-disp address.");
		ret = -ENOMEM;
		goto err_main;
	}
	decon_regs_desc_init(decon->regs.ss_regs, "decon-ss", REGS_DECON_SYS,
			decon->id);

	return ret;

err_main:
	iounmap(decon->regs.regs);
err:
	return ret;
}

static irqreturn_t decon_te_irq_handler(int irq, void *dev_id)
{
	struct decon_device *decon = dev_id;
	struct exynos_hibernation *hibernation;

	if (!decon || decon->state != DECON_STATE_ON)
		goto end;

	DPU_EVENT_LOG(DPU_EVT_TE_INTERRUPT, decon->id, NULL);

	hibernation = decon->hibernation;

	if (hibernation && !is_hibernaton_blocked(hibernation))
		kthread_queue_work(&decon->worker, &hibernation->work);

	if (decon->config.mode.op_mode == DECON_COMMAND_MODE)
		drm_crtc_handle_vblank(&decon->crtc->base);

end:
	return IRQ_HANDLED;
}

static int decon_register_irqs(struct decon_device *decon)
{
	struct device *dev = decon->dev;
	struct device_node *np = dev->of_node;
	struct platform_device *pdev;
	int ret = 0;
	int gpio;

	pdev = container_of(dev, struct platform_device, dev);

	/* 1: FRAME START */
	decon->irq_fs = of_irq_get_byname(np, "frame_start");
	ret = devm_request_irq(dev, decon->irq_fs, decon_irq_handler,
			0, pdev->name, decon);
	if (ret) {
		decon_err(decon, "failed to install FRAME START irq\n");
		return ret;
	}
	disable_irq(decon->irq_fs);

	/* 2: FRAME DONE */
	decon->irq_fd = of_irq_get_byname(np, "frame_done");
	ret = devm_request_irq(dev, decon->irq_fd, decon_irq_handler,
			0, pdev->name, decon);
	if (ret) {
		decon_err(decon, "failed to install FRAME DONE irq\n");
		return ret;
	}
	disable_irq(decon->irq_fd);

	/* 3: EXTRA: resource conflict, timeout and error irq */
	decon->irq_ext = of_irq_get_byname(np, "extra");
	ret = devm_request_irq(dev, decon->irq_ext, decon_irq_handler,
			0, pdev->name, decon);
	if (ret) {
		decon_err(decon, "failed to install EXTRA irq\n");
		return ret;
	}
	disable_irq(decon->irq_ext);

	/*
	 * Get IRQ resource and register IRQ handler. Only enabled in command
	 * mode.
	 */
	if (of_get_property(dev->of_node, "gpios", NULL) != NULL) {
		gpio = of_get_gpio(dev->of_node, 0);
		if (gpio < 0) {
			decon_err(decon, "failed to get TE gpio\n");
			return -ENODEV;
		}
	} else {
		decon_debug(decon, "failed to find TE gpio node\n");
		return 0;
	}

	decon->irq_te = gpio_to_irq(gpio);

	decon_info(decon, "TE irq number(%d)\n", decon->irq_te);
	irq_set_status_flags(decon->irq_te, IRQ_DISABLE_UNLAZY);
	ret = devm_request_irq(dev, decon->irq_te, decon_te_irq_handler,
			IRQF_TRIGGER_RISING, pdev->name, decon);
	disable_irq(decon->irq_te);

	return ret;
}

static int decon_get_pinctrl(struct decon_device *decon)
{
	int ret = 0;

	decon->res.pinctrl = devm_pinctrl_get(decon->dev);
	if (IS_ERR(decon->res.pinctrl)) {
		decon_debug(decon, "failed to get pinctrl\n");
		ret = PTR_ERR(decon->res.pinctrl);
		decon->res.pinctrl = NULL;
		/* optional in video mode */
		return 0;
	}

	decon->res.te_on = pinctrl_lookup_state(decon->res.pinctrl, "hw_te_on");
	if (IS_ERR(decon->res.te_on)) {
		decon_err(decon, "failed to get hw_te_on pin state\n");
		ret = PTR_ERR(decon->res.te_on);
		decon->res.te_on = NULL;
		goto err;
	}
	decon->res.te_off = pinctrl_lookup_state(decon->res.pinctrl,
			"hw_te_off");
	if (IS_ERR(decon->res.te_off)) {
		decon_err(decon, "failed to get hw_te_off pin state\n");
		ret = PTR_ERR(decon->res.te_off);
		decon->res.te_off = NULL;
		goto err;
	}

err:
	return ret;
}

#ifndef CONFIG_BOARD_EMULATOR
static int decon_get_clock(struct decon_device *decon)
{
	decon->res.aclk = devm_clk_get(decon->dev, "aclk");
	if (IS_ERR_OR_NULL(decon->res.aclk)) {
		decon_info(decon, "failed to get aclk(optional)\n");
		decon->res.aclk = NULL;
	}

	decon->res.aclk_disp = devm_clk_get(decon->dev, "aclk-disp");
	if (IS_ERR_OR_NULL(decon->res.aclk_disp)) {
		decon_info(decon, "failed to get aclk_disp(optional)\n");
		decon->res.aclk_disp = NULL;
	}

	return 0;
}
#else
static inline int decon_get_clock(struct decon_device *decon) { return 0; }
#endif

static int decon_init_resources(struct decon_device *decon)
{
	int ret = 0;

	ret = decon_remap_regs(decon);
	if (ret)
		goto err;

	ret = decon_register_irqs(decon);
	if (ret)
		goto err;

	ret = decon_get_pinctrl(decon);
	if (ret)
		goto err;

	ret = decon_get_clock(decon);
	if (ret)
		goto err;

	ret = __decon_init_resources(decon);
	if (ret)
		goto err;

err:
	return ret;
}

static int decon_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct decon_device *decon;
	struct device *dev = &pdev->dev;
	struct sched_param param = {
		.sched_priority = 20
	};

	decon = devm_kzalloc(dev, sizeof(struct decon_device), GFP_KERNEL);
	if (!decon)
		return -ENOMEM;

	decon->dev = dev;

	ret = decon_parse_dt(decon, dev->of_node);
	if (ret)
		goto err;

	decon_drvdata[decon->id] = decon;

	spin_lock_init(&decon->slock);
	init_completion(&decon->framestart_done);
	init_waitqueue_head(&decon->framedone_wait);

	decon->state = DECON_STATE_OFF;
	pm_runtime_enable(decon->dev);

	ret = decon_init_resources(decon);
	if (ret)
		goto err;

	/* set drvdata */
	platform_set_drvdata(pdev, decon);

	kthread_init_worker(&decon->worker);
	decon->thread = kthread_run(kthread_worker_fn, &decon->worker,
				    "decon%d_kthread", decon->id);
	if (IS_ERR(decon->thread)) {
		decon_err(decon, "failed to run display thread\n");
		ret = PTR_ERR(decon->thread);
		goto err;
	}
	sched_setscheduler_nocheck(decon->thread, SCHED_FIFO, &param);

	decon->hibernation = exynos_hibernation_register(decon);

	decon->dqe = exynos_dqe_register(decon);

	ret = component_add(dev, &decon_component_ops);
	if (ret)
		goto err;

	decon_info(decon, "successfully probed");

err:
	return ret;
}

static int decon_remove(struct platform_device *pdev)
{
	struct decon_device *decon = platform_get_drvdata(pdev);

	if (decon->thread)
		kthread_stop(decon->thread);

	exynos_hibernation_destroy(decon->hibernation);

	component_del(&pdev->dev, &decon_component_ops);

	__decon_unmap_regs(decon);
	iounmap(decon->regs.regs);

	return 0;
}

#ifdef CONFIG_PM
static int decon_suspend(struct device *dev)
{
	struct decon_device *decon = dev_get_drvdata(dev);

	if (decon->res.aclk)
		clk_disable_unprepare(decon->res.aclk);

	if (decon->res.aclk_disp)
		clk_disable_unprepare(decon->res.aclk_disp);

	if (decon->dqe)
		exynos_dqe_reset(decon->dqe);

	decon_debug(decon, "suspended\n");

	return 0;
}

static int decon_resume(struct device *dev)
{
	struct decon_device *decon = dev_get_drvdata(dev);

	if (decon->res.aclk)
		clk_prepare_enable(decon->res.aclk);

	if (decon->res.aclk_disp)
		clk_prepare_enable(decon->res.aclk_disp);

	decon_debug(decon, "resumed\n");

	return 0;
}
#endif

static const struct dev_pm_ops decon_pm_ops = {
	SET_RUNTIME_PM_OPS(decon_suspend, decon_resume, NULL)
};

struct platform_driver decon_driver = {
	.probe		= decon_probe,
	.remove		= decon_remove,
	.driver		= {
		.name	= "exynos-decon",
		.pm	= &decon_pm_ops,
		.of_match_table = decon_driver_dt_match,
	},
};

MODULE_AUTHOR("Hyung-jun Kim <hyungjun07.kim@samsung.com>");
MODULE_AUTHOR("Seong-gyu Park <seongyu.park@samsung.com>");
MODULE_DESCRIPTION("Samsung SoC Display and Enhancement Controller");
MODULE_LICENSE("GPL v2");
