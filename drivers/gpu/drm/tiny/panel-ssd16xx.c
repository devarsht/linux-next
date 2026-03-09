// SPDX-License-Identifier: GPL-2.0-only
/*
 * DRM driver for e-paper display panels using Solomon SSD16xx family controllers
 *
 * Copyright (C) 2025 Texas Instruments Incorporated - https://www.ti.com/
 *
 * Author: Devarsh Thakkar <devarsht@ti.com>
 *
 * References: https://github.com/Lesords/epaper
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/spi/spi.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

static int rotation = -1;
module_param(rotation, int, 0644);
MODULE_PARM_DESC(rotation,
		 "Display rotation (-1=use DT, 0=landscape, 90=portrait CW)");

static int refresh_mode = -1;
module_param(refresh_mode, int, 0644);
MODULE_PARM_DESC(refresh_mode,
		 "Refresh mode (-1=panel default, 0=partial ~300-500ms, 1=full ~1.5-2s, 2=fast ~1.0-1.5s)");

static int border_waveform_init_lut = -1;
module_param(border_waveform_init_lut, int, 0644);
MODULE_PARM_DESC(border_waveform_init_lut,
		 "Border waveform index during clear/init (-1=panel default, 0-9=enum index)");

static int border_waveform_lut = -1;
module_param(border_waveform_lut, int, 0644);
MODULE_PARM_DESC(border_waveform_lut,
		 "Border waveform index during display updates (-1=panel default, 0-9=enum index)");

static bool border_refresh_on_every_update;
module_param(border_refresh_on_every_update, bool, 0644);
MODULE_PARM_DESC(border_refresh_on_every_update,
		 "Re-send border waveform command before each display update (default: false)");

static int clear_on_init = -1;
module_param(clear_on_init, int, 0644);
MODULE_PARM_DESC(clear_on_init,
		 "Clear display on first app launch (-1=disabled, 0=partial, 1=full, 2=fast)");

static int clear_on_close = -1;
module_param(clear_on_close, int, 0644);
MODULE_PARM_DESC(clear_on_close,
		 "Clear display on app close/CRTC disable (-1=disabled, 0=partial, 1=full, 2=fast)");

static int refresh_mode_init = -1;
module_param(refresh_mode_init, int, 0644);
MODULE_PARM_DESC(refresh_mode_init,
		 "Skip baseline establishment on first enable (-1=disabled, 0=partial, 1=full, 2=fast)");

#define SSD16XX_CMD_DRIVER_OUTPUT_CONTROL		0x01
#define SSD16XX_CMD_DATA_ENTRY_MODE			0x11
#define SSD16XX_CMD_SW_RESET				0x12
#define SSD16XX_CMD_TEMPERATURE_SENSOR_CONTROL		0x18
#define SSD16XX_CMD_MASTER_ACTIVATION			0x20
#define SSD16XX_CMD_DISPLAY_UPDATE_CONTROL1		0x21
#define SSD16XX_CMD_DISPLAY_UPDATE_CONTROL2		0x22
#define SSD16XX_CMD_WRITE_RAM_BW			0x24
#define SSD16XX_CMD_WRITE_RAM_RED			0x26
#define SSD16XX_CMD_BORDER_WAVEFORM_CONTROL		0x3C
#define SSD16XX_CMD_SET_RAM_X_ADDRESS_START_END		0x44
#define SSD16XX_CMD_SET_RAM_Y_ADDRESS_START_END		0x45
#define SSD16XX_CMD_SET_RAM_X_ADDRESS_COUNTER		0x4E
#define SSD16XX_CMD_SET_RAM_Y_ADDRESS_COUNTER		0x4F

/*
 * Data Entry Mode (command 0x11) AM/IDY/IDX bit encoding.
 *
 * Bit 2 (AM): Address update direction: 0 = X direction, 1 = Y direction
 * ID[1:0] when AM=0 (X-direction modes, address counter advances in X):
 *   00 = X decrement, Y decrement   01 = X increment, Y decrement
 *   10 = X decrement, Y increment   11 = X increment, Y increment (default)
 * ID[1:0] when AM=1 (Y-direction modes, address counter advances in Y):
 *   00 = Y decrement, X decrement   01 = Y increment, X decrement
 *   10 = Y decrement, X increment   11 = Y increment, X increment
 *
 * Rotation to data entry mode mapping:
 *   0°  → 0x03 (X++, Y++)         Landscape left-to-right, top-to-bottom
 *   90° → 0x00 (X--, Y--)         Portrait CW, right-to-left rows
 */
#define SSD16XX_DATA_ENTRY_XDEC_YDEC		0x00  /* X--, Y-- (X-mode) */
#define SSD16XX_DATA_ENTRY_XINC_YINC		0x03  /* X++, Y++ (X-mode, default) */

/* POR reset value: GD=0 (G0 first), SM=0 (interlaced), TB=0 (G0->G299) */
#define SSD16XX_DRIVER_OUTPUT_CTRL_DEFAULT	0x00

/*
 * Border Waveform Control (command 0x3C) byte values - from SSD1683 datasheet.
 * LUT0-3: GS transition waveforms. FixLvl: fixed voltage levels.
 * VCOM: border follows VCOM voltage. HiZ: high-impedance (floating, default).
 */
#define SSD16XX_BORDER_WAVEFORM_LUT0		0x00  /* GS Transition LUT0 (black) */
#define SSD16XX_BORDER_WAVEFORM_LUT1		0x01  /* GS Transition LUT1 (white) */
#define SSD16XX_BORDER_WAVEFORM_LUT2		0x02  /* GS Transition LUT2 (black) */
#define SSD16XX_BORDER_WAVEFORM_LUT3		0x03  /* GS Transition LUT3 (gray) */
#define SSD16XX_BORDER_WAVEFORM_FIXLVL_VSS	0x40  /* Fix Level VSS (0V, black) */
#define SSD16XX_BORDER_WAVEFORM_FIXLVL_VSH1	0x50  /* Fix Level VSH1 (+15V, black) */
#define SSD16XX_BORDER_WAVEFORM_FIXLVL_VSL	0x60  /* Fix Level VSL (-15V, white) */
#define SSD16XX_BORDER_WAVEFORM_FIXLVL_VSH2	0x70  /* Fix Level VSH2 (+15V alt, black) */
#define SSD16XX_BORDER_WAVEFORM_VCOM		0x80  /* Follow VCOM (-2V~-3V, preserve) */
#define SSD16XX_BORDER_WAVEFORM_HIZ		0xC0  /* HiZ (floating, default) */

/*
 * Temperature Sensor Control (command 0x18) - use internal temperature sensor.
 * Bit 7 set = use internal temperature sensor.
 */
#define SSD16XX_TEMP_SENSOR_INTERNAL		0x80

/*
 * Display Update Control 1 (0x21) byte 1 definitions - from SSD1683 datasheet
 */
#define SSD16XX_CTRL1_NORMAL			0x00  /* Both RAMs enabled */
#define SSD16XX_CTRL1_BYPASS_RED_RAM		0x40  /* Bypass RED RAM (force RED=0) */
#define SSD16XX_CTRL1_BYTE2_DEFAULT		0x00

/* Display Update Control 2 (0x22) bit definitions - from SSD1683 datasheet */
#define SSD16XX_CTRL2_ENABLE_CLK		BIT(7)
#define SSD16XX_CTRL2_ENABLE_ANALOG		BIT(6)
#define SSD16XX_CTRL2_LOAD_TEMPERATURE		BIT(5)
#define SSD16XX_CTRL2_LOAD_LUT			BIT(4)
#define SSD16XX_CTRL2_MODE2			BIT(3)  /* Display Mode 2 (partial/BW) */
#define SSD16XX_CTRL2_DISPLAY			BIT(2)
#define SSD16XX_CTRL2_DISABLE_ANALOG		BIT(1)
#define SSD16XX_CTRL2_DISABLE_CLK		BIT(0)

/* Full refresh: temperature + LUT load, ~1.5-2s */
#define SSD1683_CTRL2_FULL_REFRESH (SSD16XX_CTRL2_ENABLE_CLK | \
				    SSD16XX_CTRL2_ENABLE_ANALOG | \
				    SSD16XX_CTRL2_LOAD_TEMPERATURE | \
				    SSD16XX_CTRL2_LOAD_LUT | \
				    SSD16XX_CTRL2_DISPLAY | \
				    SSD16XX_CTRL2_DISABLE_ANALOG | \
				    SSD16XX_CTRL2_DISABLE_CLK)  /* 0xF7 */

/* Fast refresh: skip temperature load, ~1.0-1.5s */
#define SSD1683_CTRL2_FAST_REFRESH (SSD16XX_CTRL2_ENABLE_CLK | \
				    SSD16XX_CTRL2_ENABLE_ANALOG | \
				    SSD16XX_CTRL2_DISPLAY | \
				    SSD16XX_CTRL2_DISABLE_ANALOG | \
				    SSD16XX_CTRL2_DISABLE_CLK)  /* 0xC7 */

/* Partial refresh: BW mode, temperature + LUT load, ~300-500ms */
#define SSD1683_CTRL2_PARTIAL_REFRESH (SSD16XX_CTRL2_ENABLE_CLK | \
				       SSD16XX_CTRL2_ENABLE_ANALOG | \
				       SSD16XX_CTRL2_LOAD_TEMPERATURE | \
				       SSD16XX_CTRL2_LOAD_LUT | \
				       SSD16XX_CTRL2_MODE2 | \
				       SSD16XX_CTRL2_DISPLAY | \
				       SSD16XX_CTRL2_DISABLE_ANALOG | \
				       SSD16XX_CTRL2_DISABLE_CLK)  /* 0xFF */

/*
 * 0x91 = ENABLE_CLK | LOAD_LUT | DISABLE_CLK
 * Used to pre-load LUT during hw_init without triggering a display update.
 * Required for FAST refresh mode which omits LOAD_LUT from each update cycle.
 */
#define SSD16XX_CTRL2_LOAD_TEMP_LUT		(SSD16XX_CTRL2_ENABLE_CLK | \
						 SSD16XX_CTRL2_LOAD_LUT | \
						 SSD16XX_CTRL2_DISABLE_CLK)  /* 0x91 */

#define SSD16XX_SPI_BITS_PER_WORD		8
#define SSD16XX_SPI_SPEED_DEFAULT		1000000

MODULE_IMPORT_NS("DMA_BUF");

enum ssd16xx_controller {
	SSD1683 = 1,
};

enum ssd16xx_model {
	GDEY042T81 = 1,
};

enum ssd16xx_refresh_mode {
	SSD16XX_REFRESH_PARTIAL = 0,  /* Partial refresh (~300-500ms) */
	SSD16XX_REFRESH_FULL,         /* Full refresh (~1.5-2s) */
	SSD16XX_REFRESH_FAST,         /* Fast refresh, skip temp load (~1.0-1.5s) */
};

/* Border waveform enum indices (0-9); map to HW bytes via ssd16xx_border_waveform_values[] */
enum ssd16xx_border_waveform {
	SSD16XX_BORDER_LUT0 = 0,  /* GS Transition LUT0 (black) */
	SSD16XX_BORDER_LUT1,      /* GS Transition LUT1 (white) */
	SSD16XX_BORDER_LUT2,      /* GS Transition LUT2 (black) */
	SSD16XX_BORDER_LUT3,      /* GS Transition LUT3 (gray) */
	SSD16XX_BORDER_VSS,       /* Fix Level VSS (black) */
	SSD16XX_BORDER_VSH1,      /* Fix Level VSH1 (black) */
	SSD16XX_BORDER_VSL,       /* Fix Level VSL (white) */
	SSD16XX_BORDER_VSH2,      /* Fix Level VSH2 (black) */
	SSD16XX_BORDER_VCOM,      /* Follow VCOM (preserve) */
	SSD16XX_BORDER_HIZ,       /* HiZ (floating, default) */
};

static const u8 ssd16xx_border_waveform_values[] = {
	[SSD16XX_BORDER_LUT0] = SSD16XX_BORDER_WAVEFORM_LUT0,
	[SSD16XX_BORDER_LUT1] = SSD16XX_BORDER_WAVEFORM_LUT1,
	[SSD16XX_BORDER_LUT2] = SSD16XX_BORDER_WAVEFORM_LUT2,
	[SSD16XX_BORDER_LUT3] = SSD16XX_BORDER_WAVEFORM_LUT3,
	[SSD16XX_BORDER_VSS]  = SSD16XX_BORDER_WAVEFORM_FIXLVL_VSS,
	[SSD16XX_BORDER_VSH1] = SSD16XX_BORDER_WAVEFORM_FIXLVL_VSH1,
	[SSD16XX_BORDER_VSL]  = SSD16XX_BORDER_WAVEFORM_FIXLVL_VSL,
	[SSD16XX_BORDER_VSH2] = SSD16XX_BORDER_WAVEFORM_FIXLVL_VSH2,
	[SSD16XX_BORDER_VCOM] = SSD16XX_BORDER_WAVEFORM_VCOM,
	[SSD16XX_BORDER_HIZ]  = SSD16XX_BORDER_WAVEFORM_HIZ,
};

struct ssd16xx_controller_config {
	u16 max_width;
	u16 max_height;
	u8 ram_x_address_bits;
	u8 ram_y_address_bits;
};

struct ssd16xx_panel_config {
	/* Data Entry Mode - controls X/Y increment direction for landscape (0°) */
	u8 data_entry_mode;

	/* Driver Output Control - third byte (scan direction) */
	u8 driver_output_ctrl_byte3;

	/* Default refresh mode for this panel */
	enum ssd16xx_refresh_mode default_refresh_mode;

	/* Default border waveform during clear/init (enum index 0-9) */
	enum ssd16xx_border_waveform default_border_waveform_init;

	/* Default border waveform during display updates (enum index 0-9) */
	enum ssd16xx_border_waveform default_border_waveform_update;

	/* Whether to re-send border waveform command before each display update */
	bool default_border_refresh_on_every_update;

	/*
	 * Default clear-on-init behaviour.
	 * -1=disabled, 0=partial, 1=full, 2=fast (matches enum ssd16xx_refresh_mode)
	 */
	int default_clear_on_init;

	/* Default clear-on-close behaviour (-1=disabled, 0=partial, 1=full, 2=fast) */
	int default_clear_on_close;

	/*
	 * Default refresh-mode-init: -1=disabled, else skip baseline establishment
	 * and start directly in this refresh mode.
	 */
	int default_refresh_mode_init;

	/*
	 * Whether this panel has a physical red colour plane (3-colour BWR).
	 * false: 2-colour black/white only; the RED RAM is always bypassed.
	 * true:  3-colour panel; full-refresh writes to the RED RAM so that
	 *        red pixels are driven through the red waveform.
	 */
	bool red_supported;

	/* Panel-specific display mode (resolution and physical dimensions) */
	const struct drm_display_mode *mode;
};

struct ssd16xx_error_ctx {
	int errno_code;
};

struct ssd16xx_panel {
	struct drm_device drm;

	struct drm_plane primary_plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;

	struct spi_device *spi;
	struct gpio_desc *reset;
	struct gpio_desc *busy;
	struct gpio_desc *dc;

	enum ssd16xx_model model;
	enum ssd16xx_controller controller;
	const struct ssd16xx_controller_config *controller_cfg;
	const struct ssd16xx_panel_config *panel_cfg;
	struct drm_display_mode *mode;
	u32 width;
	u32 height;

	bool initialized;
	bool reinit_pending;      /* HW re-init required after orientation change */
	bool init_refresh_pending; /* First frame after refresh_mode_init enable */
	bool first_clear_done;    /* clear_on_init has already fired once */

	int orientation;      /* Display orientation in degrees: 0/90 */
	enum ssd16xx_refresh_mode refresh_mode; /* Active refresh mode */

	/* Border waveform (as enum indices) */
	int border_waveform_init_idx;   /* Border waveform during clear/init */
	int border_waveform_update_idx; /* Border waveform during display updates */
	bool border_refresh_on_every_update; /* Re-send border cmd each display update */

	/* Display control */
	int clear_on_init;   /* -1=disabled, 0=partial, 1=full, 2=fast */
	int clear_on_close;  /* -1=disabled, 0=partial, 1=full, 2=fast */
	int refresh_mode_init; /* -1=disabled, else use this mode for the first frame */

	struct drm_framebuffer *last_fb;        /* Last drawn FB for reinit redraws */
	struct drm_property *rotation_property;
	struct drm_property *refresh_mode_property;
	struct drm_property *border_waveform_init_property;
	struct drm_property *border_waveform_update_property;
	struct drm_property *border_refresh_on_every_update_property;
	struct drm_property *clear_on_init_property;
	struct drm_property *clear_on_close_property;
	struct drm_property *refresh_mode_init_property;
};

static inline struct ssd16xx_panel *to_ssd16xx_panel(struct drm_device *drm)
{
	return container_of(drm, struct ssd16xx_panel, drm);
}

static inline struct ssd16xx_panel *crtc_to_ssd16xx_panel(struct drm_crtc *crtc)
{
	return container_of(crtc, struct ssd16xx_panel, crtc);
}

static inline struct ssd16xx_panel *plane_to_ssd16xx_panel(struct drm_plane *plane)
{
	return container_of(plane, struct ssd16xx_panel, primary_plane);
}

static const struct ssd16xx_controller_config ssd16xx_controller_configs[] = {
	[SSD1683] = {
		.max_width = 400,
		.max_height = 300,
		.ram_x_address_bits = 8,
		.ram_y_address_bits = 16,
	},
};

/* GDEY042T81: 4.2" 400x300 panel, 84.8x63.6mm active area */
static const struct drm_display_mode gdey042t81_mode = {
	DRM_SIMPLE_MODE(400, 300, 85, 64),
};

static const struct ssd16xx_panel_config ssd16xx_panel_configs[] = {
	[GDEY042T81] = {
		.data_entry_mode = SSD16XX_DATA_ENTRY_XINC_YINC,
		.driver_output_ctrl_byte3 = SSD16XX_DRIVER_OUTPUT_CTRL_DEFAULT,
		.default_refresh_mode = SSD16XX_REFRESH_PARTIAL,
		.default_border_waveform_init   = SSD16XX_BORDER_LUT1,  /* white, clean clear */
		.default_border_waveform_update = SSD16XX_BORDER_HIZ,   /* floating, preserve */
		.default_border_refresh_on_every_update = false,
		.default_clear_on_init   = -1,
		.default_clear_on_close  = -1,
		.default_refresh_mode_init = SSD16XX_REFRESH_FULL,
		.red_supported = false,  /* 2-colour black/white panel */
		.mode = &gdey042t81_mode,
	},
};

static void ssd16xx_wait_for_panel(struct ssd16xx_panel *panel)
{
	unsigned int timeout_ms = 10000;
	unsigned long timeout_jiffies = jiffies + msecs_to_jiffies(timeout_ms);
	unsigned long start_ms = jiffies_to_msecs(jiffies);
	int busy_val;

	busy_val = gpiod_get_value_cansleep(panel->busy);
	drm_dbg(&panel->drm, "BUSY initial value: %d\n", busy_val);

	while (gpiod_get_value_cansleep(panel->busy) == 1) {
		if (time_after(jiffies, timeout_jiffies)) {
			drm_err(&panel->drm, "Busy wait timed out after %lums\n",
				jiffies_to_msecs(jiffies) - start_ms);
			return;
		}
		usleep_range(100, 200);
	}

	drm_dbg(&panel->drm, "BUSY became ready after %lums\n",
		jiffies_to_msecs(jiffies) - start_ms);
}

static void ssd16xx_spi_sync(struct spi_device *spi, struct spi_message *msg,
			     struct ssd16xx_error_ctx *err)
{
	int ret;

	if (err->errno_code)
		return;

	ret = spi_sync(spi, msg);
	if (ret < 0)
		err->errno_code = ret;
}

static void ssd16xx_send_cmd(struct ssd16xx_panel *panel, u8 cmd,
			     struct ssd16xx_error_ctx *err)
{
	struct spi_transfer xfer = {
		.tx_buf = &cmd,
		.len = 1,
	};
	struct spi_message msg;

	if (err->errno_code)
		return;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	gpiod_set_value_cansleep(panel->dc, 0);
	ssd16xx_spi_sync(panel->spi, &msg, err);
}

static void ssd16xx_send_data(struct ssd16xx_panel *panel, u8 data,
			      struct ssd16xx_error_ctx *err)
{
	struct spi_transfer xfer = {
		.tx_buf = &data,
		.len = 1,
	};
	struct spi_message msg;

	if (err->errno_code)
		return;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	gpiod_set_value_cansleep(panel->dc, 1);
	ssd16xx_spi_sync(panel->spi, &msg, err);
}

static void ssd16xx_send_x_param(struct ssd16xx_panel *panel, u16 x,
				 struct ssd16xx_error_ctx *err)
{
	if (panel->controller_cfg->ram_x_address_bits == 8) {
		ssd16xx_send_data(panel, (u8)x, err);
	} else {
		ssd16xx_send_data(panel, x & 0xFF, err);
		ssd16xx_send_data(panel, (x >> 8) & 0xFF, err);
	}
}

static void ssd16xx_send_y_param(struct ssd16xx_panel *panel, u16 y,
				 struct ssd16xx_error_ctx *err)
{
	if (panel->controller_cfg->ram_y_address_bits == 8) {
		ssd16xx_send_data(panel, (u8)y, err);
	} else {
		ssd16xx_send_data(panel, y & 0xFF, err);
		ssd16xx_send_data(panel, (y >> 8) & 0xFF, err);
	}
}

static void ssd16xx_send_data_bulk(struct ssd16xx_panel *panel,
				   const u8 *data, size_t len,
				   struct ssd16xx_error_ctx *err)
{
	struct spi_transfer xfer = {
		.tx_buf = data,
		.len = len,
	};
	struct spi_message msg;

	if (err->errno_code)
		return;

	if (!data || !len)
		return;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	gpiod_set_value_cansleep(panel->dc, 1);
	ssd16xx_spi_sync(panel->spi, &msg, err);
}

static void ssd16xx_display_update(struct ssd16xx_panel *panel,
				   u8 ctrl1_byte1, u8 ctrl1_byte2, u8 ctrl2_mode,
				   struct ssd16xx_error_ctx *err)
{
	int busy_before;

	busy_before = gpiod_get_value_cansleep(panel->busy);
	drm_dbg(&panel->drm,
		"display_update: Setting ctrl1=0x%02x,0x%02x mode=0x%02x (BUSY=%d)\n",
		ctrl1_byte1, ctrl1_byte2, ctrl2_mode, busy_before);

	ssd16xx_send_cmd(panel, SSD16XX_CMD_DISPLAY_UPDATE_CONTROL1, err);
	ssd16xx_send_data(panel, ctrl1_byte1, err);
	ssd16xx_send_data(panel, ctrl1_byte2, err);

	ssd16xx_send_cmd(panel, SSD16XX_CMD_DISPLAY_UPDATE_CONTROL2, err);
	ssd16xx_send_data(panel, ctrl2_mode, err);
	ssd16xx_send_cmd(panel, SSD16XX_CMD_MASTER_ACTIVATION, err);

	busy_before = gpiod_get_value_cansleep(panel->busy);
	drm_dbg(&panel->drm,
		"display_update: Master activation sent (BUSY now=%d), waiting...\n",
		busy_before);

	if (!err->errno_code)
		ssd16xx_wait_for_panel(panel);
}

static void ssd16xx_hw_reset(struct ssd16xx_panel *panel)
{
	gpiod_set_value_cansleep(panel->reset, 0);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(panel->reset, 1);
	usleep_range(10000, 11000);
}

static int ssd16xx_hw_init(struct ssd16xx_panel *panel)
{
	struct ssd16xx_error_ctx err = { .errno_code = 0 };
	u16 ram_width = panel->controller_cfg->max_width;
	u16 ram_height = panel->controller_cfg->max_height;
	u8 data_entry_mode;
	u16 cursor_x_start;
	u16 cursor_y_start;

	ssd16xx_hw_reset(panel);

	/* Software reset */
	ssd16xx_send_cmd(panel, SSD16XX_CMD_SW_RESET, &err);
	ssd16xx_wait_for_panel(panel);

	/* Driver output control (0x01): MUX ratio and scan direction. */
	ssd16xx_send_cmd(panel, SSD16XX_CMD_DRIVER_OUTPUT_CONTROL, &err);
	ssd16xx_send_y_param(panel, ram_height - 1, &err);
	ssd16xx_send_data(panel, panel->panel_cfg->driver_output_ctrl_byte3, &err);

	/* Internal temperature sensor */
	ssd16xx_send_cmd(panel, SSD16XX_CMD_TEMPERATURE_SENSOR_CONTROL, &err);
	ssd16xx_send_data(panel, SSD16XX_TEMP_SENSOR_INTERNAL, &err);

	/*
	 * For FAST refresh mode, pre-load the LUT once here during initialization.
	 * FAST mode ctrl2 (0xC7) omits LOAD_LUT on every update for speed, so the
	 * LUT must be loaded upfront. FULL (0xF7) and PARTIAL (0xFF) load LUT on
	 * every update, so no preload is needed for those modes.
	 */
	if (panel->refresh_mode == SSD16XX_REFRESH_FAST) {
		ssd16xx_send_cmd(panel, SSD16XX_CMD_DISPLAY_UPDATE_CONTROL1, &err);
		ssd16xx_send_data(panel, SSD16XX_CTRL1_BYPASS_RED_RAM, &err);
		ssd16xx_send_data(panel, SSD16XX_CTRL1_BYTE2_DEFAULT, &err);

		ssd16xx_send_cmd(panel, SSD16XX_CMD_DISPLAY_UPDATE_CONTROL2, &err);
		ssd16xx_send_data(panel, SSD16XX_CTRL2_LOAD_TEMP_LUT, &err);

		ssd16xx_send_cmd(panel, SSD16XX_CMD_MASTER_ACTIVATION, &err);
		ssd16xx_wait_for_panel(panel);
	}

	/* Select data entry mode and cursor start based on orientation */
	switch (panel->orientation) {
	case 90:
		/* Portrait 90° CW: X-mode X-- Y--, start at (max, 0). */
		data_entry_mode = SSD16XX_DATA_ENTRY_XDEC_YDEC;
		cursor_x_start = (ram_width / 8) - 1;
		cursor_y_start = 0;
		drm_dbg(&panel->drm, "HW_INIT: 90° portrait CW, data_entry=0x%02x, cursor=(%u,%u)\n",
			data_entry_mode, cursor_x_start, cursor_y_start);
		break;
	default:
		data_entry_mode = SSD16XX_DATA_ENTRY_XINC_YINC;
		cursor_x_start = 0;
		cursor_y_start = 0;
		drm_dbg(&panel->drm, "HW_INIT: 0° landscape, data_entry=0x%02x, cursor=(%u,%u)\n",
			data_entry_mode, cursor_x_start, cursor_y_start);
		break;
	}

	ssd16xx_send_cmd(panel, SSD16XX_CMD_DATA_ENTRY_MODE, &err);
	ssd16xx_send_data(panel, data_entry_mode, &err);

	/* Set RAM X address window (full physical width) */
	ssd16xx_send_cmd(panel, SSD16XX_CMD_SET_RAM_X_ADDRESS_START_END, &err);
	ssd16xx_send_x_param(panel, 0, &err);
	ssd16xx_send_x_param(panel, (ram_width / 8) - 1, &err);

	/* Set RAM Y address window (full physical height) */
	ssd16xx_send_cmd(panel, SSD16XX_CMD_SET_RAM_Y_ADDRESS_START_END, &err);
	ssd16xx_send_y_param(panel, 0, &err);
	ssd16xx_send_y_param(panel, ram_height - 1, &err);

	/* Set RAM address counters to start position */
	ssd16xx_send_cmd(panel, SSD16XX_CMD_SET_RAM_X_ADDRESS_COUNTER, &err);
	ssd16xx_send_x_param(panel, cursor_x_start, &err);

	ssd16xx_send_cmd(panel, SSD16XX_CMD_SET_RAM_Y_ADDRESS_COUNTER, &err);
	ssd16xx_send_y_param(panel, cursor_y_start, &err);

	ssd16xx_wait_for_panel(panel);

	if (err.errno_code)
		drm_err(&panel->drm, "Hardware initialization failed: %d\n", err.errno_code);

	return err.errno_code;
}

/*
 * Clear display by writing all-white to both BW and RED RAM.
 * The ctrl2 argument selects the waveform (full/partial/fast refresh).
 * Border waveform is set to init value before clearing, then restored
 * to the update value to preserve the border during subsequent updates.
 */
static void ssd16xx_clear_display(struct ssd16xx_panel *panel, u8 ctrl2)
{
	struct ssd16xx_error_ctx err = { .errno_code = 0 };
	unsigned int data_size = (panel->width * panel->height) / 8;
	u8 *white_buffer;

	white_buffer = kmalloc(data_size, GFP_KERNEL);
	if (!white_buffer)
		return;
	memset(white_buffer, 0xFF, data_size);

	ssd16xx_send_cmd(panel, SSD16XX_CMD_SET_RAM_X_ADDRESS_COUNTER, &err);
	ssd16xx_send_x_param(panel, 0x00, &err);

	ssd16xx_send_cmd(panel, SSD16XX_CMD_SET_RAM_Y_ADDRESS_COUNTER, &err);
	ssd16xx_send_y_param(panel, 0x00, &err);

	ssd16xx_send_cmd(panel, SSD16XX_CMD_WRITE_RAM_BW, &err);
	ssd16xx_send_data_bulk(panel, white_buffer, data_size, &err);

	ssd16xx_send_cmd(panel, SSD16XX_CMD_WRITE_RAM_RED, &err);
	ssd16xx_send_data_bulk(panel, white_buffer, data_size, &err);

	/* Set border waveform for the clear operation */
	drm_dbg(&panel->drm, "clear_display: Set border init waveform: 0x%02x\n",
		ssd16xx_border_waveform_values[panel->border_waveform_init_idx]);
	ssd16xx_send_cmd(panel, SSD16XX_CMD_BORDER_WAVEFORM_CONTROL, &err);
	ssd16xx_send_data(panel,
			  ssd16xx_border_waveform_values[panel->border_waveform_init_idx],
			  &err);

	/* 3-colour: CTRL1_NORMAL (read both RAMs); BW: bypass RED. */
	ssd16xx_display_update(panel,
			       panel->panel_cfg->red_supported
					? SSD16XX_CTRL1_NORMAL
					: SSD16XX_CTRL1_BYPASS_RED_RAM,
			       SSD16XX_CTRL1_BYTE2_DEFAULT, ctrl2, &err);

	/* Restore border waveform to update/preservation value */
	drm_dbg(&panel->drm, "clear_display: Restored border update waveform: 0x%02x\n",
		ssd16xx_border_waveform_values[panel->border_waveform_update_idx]);
	ssd16xx_send_cmd(panel, SSD16XX_CMD_BORDER_WAVEFORM_CONTROL, &err);
	ssd16xx_send_data(panel,
			  ssd16xx_border_waveform_values[panel->border_waveform_update_idx],
			  &err);

	if (err.errno_code)
		drm_err(&panel->drm, "Clear display failed: %d\n", err.errno_code);

	kfree(white_buffer);
}

static u8 ssd16xx_refresh_mode_to_ctrl2(int mode)
{
	switch (mode) {
	case 0: return SSD1683_CTRL2_PARTIAL_REFRESH;
	case 2: return SSD1683_CTRL2_FAST_REFRESH;
	default: return SSD1683_CTRL2_FULL_REFRESH;
	}
}

/*
 * Clear display on first DRM app launch (if clear_on_init >= 0).
 * Guarded by panel->first_clear_done so it fires only once per module load.
 */
static void ssd16xx_clear_display_on_init(struct ssd16xx_panel *panel)
{
	if (panel->clear_on_init < 0 || panel->first_clear_done)
		return;

	drm_dbg(&panel->drm, "clear_on_init: running, mode=%d\n",
		       panel->clear_on_init);
	ssd16xx_clear_display(panel,
			      ssd16xx_refresh_mode_to_ctrl2(panel->clear_on_init));
	panel->first_clear_done = true;
}

/*
 * Clear display on CRTC disable (if clear_on_close >= 0).
 * Called at the start of ssd16xx_crtc_atomic_disable() before deep sleep.
 */
static void ssd16xx_clear_display_on_exit(struct ssd16xx_panel *panel)
{
	if (panel->clear_on_close < 0)
		return;

	drm_dbg(&panel->drm, "clear_on_close: running, mode=%d\n",
		       panel->clear_on_close);
	ssd16xx_clear_display(panel,
			      ssd16xx_refresh_mode_to_ctrl2(panel->clear_on_close));
}

/*
 * Read a single pixel from the framebuffer and return true if it should be
 * rendered as white on the monochrome e-paper panel.
 *
 * For colour formats, luminance is calculated using ITU-R BT.601 coefficients
 * scaled by 1000 to avoid floating point: (299*R + 587*G + 114*B) > 127000.
 */
static bool ssd16xx_pixel_is_white(struct iosys_map *src,
				   struct drm_framebuffer *fb,
				   unsigned int x, unsigned int y)
{
	u32 format = fb->format->format;

	switch (format) {
	case DRM_FORMAT_XRGB8888: {
		u32 *line = (u32 *)(src->vaddr + y * fb->pitches[0]);
		u32 px = line[x];
		u8 r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;

		return (299u * r + 587u * g + 114u * b) > 127000u;
	}
	case DRM_FORMAT_RGB888: {
		u8 *line = (u8 *)(src->vaddr + y * fb->pitches[0]);
		u8 r = line[x * 3], g = line[x * 3 + 1], b = line[x * 3 + 2];

		return (299u * r + 587u * g + 114u * b) > 127000u;
	}
	case DRM_FORMAT_RGB565: {
		u16 *line = (u16 *)(src->vaddr + y * fb->pitches[0]);
		u16 px = line[x];
		u8 r = ((px >> 11) & 0x1F) << 3;
		u8 g = ((px >> 5) & 0x3F) << 2;
		u8 b = (px & 0x1F) << 3;

		return (299u * r + 587u * g + 114u * b) > 127000u;
	}
	case DRM_FORMAT_R8: {
		u8 *line = (u8 *)(src->vaddr + y * fb->pitches[0]);

		return line[x] > 127;
	}
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV16: {
		u8 *y_plane = (u8 *)(src->vaddr);

		return y_plane[y * fb->pitches[0] + x] > 127;
	}
	case DRM_FORMAT_YUYV: {
		u8 *line = (u8 *)(src->vaddr + y * fb->pitches[0]);

		return line[x * 2] > 127;
	}
	case DRM_FORMAT_UYVY: {
		u8 *line = (u8 *)(src->vaddr + y * fb->pitches[0]);

		return line[x * 2 + 1] > 127;
	}
	case DRM_FORMAT_R1: {
		u8 *line = (u8 *)(src->vaddr + y * fb->pitches[0]);

		return !!(line[x / 8] & (1 << (7 - (x % 8))));
	}
	default:
		return false;
	}
}

/*
 * ssd16xx_pixel_is_red() - test whether a pixel is dominated by the red channel.
 *
 * Only meaningful for formats that carry RGB information (XRGB8888, RGB888,
 * RGB565).  For luma-only and monochrome formats there is no red channel, so
 * the function always returns false; callers should use ssd16xx_pixel_is_white()
 * to obtain the BW value for those formats.
 *
 * Returns true when the red component exceeds 50% intensity AND is strictly
 * greater than both green and blue (dominant red hue).
 */
static bool ssd16xx_pixel_is_red(struct iosys_map *src,
				 struct drm_framebuffer *fb,
				 unsigned int x, unsigned int y)
{
	u32 format = fb->format->format;

	switch (format) {
	case DRM_FORMAT_XRGB8888: {
		u32 *line = (u32 *)(src->vaddr + y * fb->pitches[0]);
		u32 px = line[x];
		u8 r = (px >> 16) & 0xFF;
		u8 g = (px >> 8) & 0xFF;
		u8 b = px & 0xFF;

		return r > 127 && r > g && r > b;
	}
	case DRM_FORMAT_RGB888: {
		u8 *line = (u8 *)(src->vaddr + y * fb->pitches[0]);
		u8 r = line[x * 3];
		u8 g = line[x * 3 + 1];
		u8 b = line[x * 3 + 2];

		return r > 127 && r > g && r > b;
	}
	case DRM_FORMAT_RGB565: {
		u16 *line = (u16 *)(src->vaddr + y * fb->pitches[0]);
		u16 px = line[x];
		u8 r = ((px >> 11) & 0x1F) << 3;
		u8 g = ((px >> 5) & 0x3F) << 2;
		u8 b = (px & 0x1F) << 3;

		return r > 127 && r > g && r > b;
	}
	default:
		return false; /* No colour channel information */
	}
}

/*
 * ssd16xx_convert_fb_to_3color() - split a framebuffer into BW and RED planes.
 * @bw_dst:  output buffer for the black/white RAM plane (1=white, 0=black)
 * @red_dst: output buffer for the red RAM plane        (1=red,   0=not red)
 * @src:     mapped framebuffer memory
 * @fb:      DRM framebuffer descriptor
 * @rect:    region to convert (must be aligned to 8-pixel boundaries)
 *
 * Each output buffer must be at least rect_width/8 * rect_height bytes.
 * Pixels are classified as:
 *   - red:   written to red_dst as 1, bw_dst as 0 (black)
 *   - white: written to bw_dst as 1, red_dst as 0
 *   - black: written to both as 0
 *
 * For monochrome formats (R1) where no colour information is available the
 * source data is copied verbatim to bw_dst and red_dst is cleared to 0xFF
 * (all-white = no red pixels).
 */
static void ssd16xx_convert_fb_to_3color(u8 *bw_dst, u8 *red_dst,
					 struct iosys_map *src,
					 struct drm_framebuffer *fb,
					 struct drm_rect *rect)
{
	unsigned int x, y;
	u8 bw_byte = 0, red_byte = 0;
	unsigned int bit_pos = 0;
	unsigned int dst_idx = 0;

	drm_dbg(fb->dev,
		       "convert_3color: fmt=%p4cc rect=(%d,%d)-(%d,%d) path=%s\n",
		       &fb->format->format,
		       rect->x1, rect->y1, rect->x2, rect->y2,
		       fb->format->format == DRM_FORMAT_R1 ? "R1-direct" : "color-pixel");

	/*
	 * R1 is already monochrome — no colour channel exists.
	 * Copy BW data directly and leave the red plane all-white (transparent).
	 */
	if (fb->format->format == DRM_FORMAT_R1) {
		unsigned int src_pitch = fb->pitches[0];
		unsigned int width_bytes = drm_rect_width(rect) / 8;
		unsigned int data_size = width_bytes * drm_rect_height(rect);

		for (y = rect->y1; y < rect->y2; y++) {
			u8 *line = src->vaddr + y * src_pitch + (rect->x1 / 8);

			memcpy(bw_dst + dst_idx, line, width_bytes);
			dst_idx += width_bytes;
		}
		memset(red_dst, 0xFF, data_size); /* 0xFF = all white: no red pixels */
		return;
	}

	for (y = rect->y1; y < rect->y2; y++) {
		for (x = rect->x1; x < rect->x2; x++) {
			bool is_red = ssd16xx_pixel_is_red(src, fb, x, y);

			if (is_red)
				red_byte |= (1 << (7 - bit_pos));
			else if (ssd16xx_pixel_is_white(src, fb, x, y))
				bw_byte |= (1 << (7 - bit_pos));
			/* else: black pixel — both bits remain 0 */
			if (++bit_pos == 8) {
				bw_dst[dst_idx] = bw_byte;
				red_dst[dst_idx] = red_byte;
				dst_idx++;
				bw_byte = 0;
				red_byte = 0;
				bit_pos = 0;
			}
		}

		/* Flush any partial byte at the end of each row */
		if (bit_pos > 0) {
			bw_dst[dst_idx] = bw_byte;
			red_dst[dst_idx] = red_byte;
			dst_idx++;
			bw_byte = 0;
			red_byte = 0;
			bit_pos = 0;
		}
	}
}

/*
 * Convert a framebuffer region to 1bpp monochrome for the SSD16xx RAM.
 * Each output byte holds 8 pixels, MSB-first, 1=white 0=black.
 *
 * Traversal order varies by orientation to match the hardware data entry mode:
 *   0°  : row-major, left→right, top→bottom
 *   90° : column-major, rightmost column first, top→bottom within each column
 *
 * R1 (native 1bpp) has a fast memcpy path for 0° only; all other orientations
 * go through ssd16xx_pixel_is_white() for proper transposition.
 */
static void ssd16xx_convert_fb_to_1bpp(u8 *dst, struct iosys_map *src,
					struct drm_framebuffer *fb,
					struct drm_rect *rect,
					unsigned int orientation)
{
	u32 format = fb->format->format;
	unsigned int x, y;
	u8 byte = 0;
	unsigned int bit_pos = 0;
	unsigned int dst_idx = 0;

	drm_dbg(fb->dev,
		       "convert_1bpp: fmt=%p4cc rect=(%d,%d)-(%d,%d) orient=%u° path=%s\n",
		       &fb->format->format,
		       rect->x1, rect->y1, rect->x2, rect->y2,
		       orientation,
		       (format == DRM_FORMAT_R1 && orientation == 0) ? "R1-fast" :
		       orientation == 90 ? "90deg" : "0deg");

	/* R1 fast path: 0° landscape only — rows map directly to output bytes */
	if (format == DRM_FORMAT_R1 && orientation == 0) {
		unsigned int src_pitch = fb->pitches[0];
		unsigned int width_bytes = drm_rect_width(rect) / 8;

		for (y = rect->y1; y < rect->y2; y++) {
			u8 *src_line = src->vaddr + y * src_pitch + (rect->x1 / 8);

			memcpy(dst + dst_idx, src_line, width_bytes);
			dst_idx += width_bytes;
		}
		return;
	}

	if (orientation == 90) {
		/*
		 * 90° CW portrait: column-major, rightmost column first.
		 * Each portrait source column becomes one physical RAM row.
		 */
		int x_signed;

		for (x_signed = rect->x2 - 1; x_signed >= (int)rect->x1; x_signed--) {
			x = x_signed;
			for (y = rect->y1; y < rect->y2; y++) {
				if (ssd16xx_pixel_is_white(src, fb, x, y))
					byte |= (1 << (7 - bit_pos));
				if (++bit_pos == 8) {
					dst[dst_idx++] = byte;
					byte = 0;
					bit_pos = 0;
				}
			}
			if (bit_pos > 0) {
				dst[dst_idx++] = byte;
				byte = 0;
				bit_pos = 0;
			}
		}
	} else {
		/* 0° landscape: row-major, left to right, top to bottom */
		for (y = rect->y1; y < rect->y2; y++) {
			for (x = rect->x1; x < rect->x2; x++) {
				if (ssd16xx_pixel_is_white(src, fb, x, y))
					byte |= (1 << (7 - bit_pos));
				if (++bit_pos == 8) {
					dst[dst_idx++] = byte;
					byte = 0;
					bit_pos = 0;
				}
			}
			if (bit_pos > 0) {
				dst[dst_idx++] = byte;
				byte = 0;
				bit_pos = 0;
			}
		}
	}
}

static void ssd16xx_fb_dirty(struct drm_framebuffer *fb, struct drm_rect *rect,
			     struct ssd16xx_panel *panel)
{
	struct drm_gem_dma_object *dma_obj = drm_fb_dma_get_gem_obj(fb, 0);
	struct iosys_map map;
	struct ssd16xx_error_ctx err = { .errno_code = 0 };
	unsigned int data_size = (panel->width * panel->height) / 8;
	u8 *mono_buffer;
	u8 *red_buffer = NULL;
	u16 ram_x_start, ram_x_end, ram_y_start, ram_y_end;

	/* Process full display area; convert handles orientation traversal. */
	rect->x1 = 0;
	rect->y1 = 0;
	rect->x2 = panel->width;
	rect->y2 = panel->height;

	drm_dbg(&panel->drm,
		"fb_dirty: fb=%dx%d, refresh_mode=%d, orientation=%d\n",
		fb->width, fb->height, panel->refresh_mode, panel->orientation);

	mono_buffer = kzalloc(data_size, GFP_KERNEL);
	if (!mono_buffer)
		return;

	/* 3-colour + FULL: allocate RED buffer (not needed during baseline). */
	if (panel->panel_cfg->red_supported &&
	    panel->refresh_mode == SSD16XX_REFRESH_FULL) {
		red_buffer = kzalloc(data_size, GFP_KERNEL);
		if (!red_buffer) {
			kfree(mono_buffer);
			return;
		}
	}

	iosys_map_set_vaddr(&map, dma_obj->vaddr);

	if (red_buffer)
		ssd16xx_convert_fb_to_3color(mono_buffer, red_buffer, &map, fb, rect);
	else
		ssd16xx_convert_fb_to_1bpp(mono_buffer, &map, fb, rect, panel->orientation);

	/* Set RAM window and cursor for current orientation. */
	ram_x_start = 0;
	ram_x_end = (panel->controller_cfg->max_width / 8) - 1;
	ram_y_start = 0;
	ram_y_end = panel->controller_cfg->max_height - 1;

	switch (panel->orientation) {
	case 90:
		/* XDEC_YDEC: send end-before-start; cursor at (max, max). */
		ssd16xx_send_cmd(panel, SSD16XX_CMD_SET_RAM_X_ADDRESS_START_END, &err);
		ssd16xx_send_x_param(panel, ram_x_end, &err);
		ssd16xx_send_x_param(panel, ram_x_start, &err);

		ssd16xx_send_cmd(panel, SSD16XX_CMD_SET_RAM_Y_ADDRESS_START_END, &err);
		ssd16xx_send_y_param(panel, ram_y_end, &err);
		ssd16xx_send_y_param(panel, ram_y_start, &err);

		ssd16xx_send_cmd(panel, SSD16XX_CMD_SET_RAM_X_ADDRESS_COUNTER, &err);
		ssd16xx_send_x_param(panel, ram_x_end, &err);

		ssd16xx_send_cmd(panel, SSD16XX_CMD_SET_RAM_Y_ADDRESS_COUNTER, &err);
		ssd16xx_send_y_param(panel, ram_y_end, &err);
		break;

	default: /* 0° landscape */
		/* XINC_YINC: cursor at (0, 0). */
		ssd16xx_send_cmd(panel, SSD16XX_CMD_SET_RAM_X_ADDRESS_START_END, &err);
		ssd16xx_send_x_param(panel, ram_x_start, &err);
		ssd16xx_send_x_param(panel, ram_x_end, &err);

		ssd16xx_send_cmd(panel, SSD16XX_CMD_SET_RAM_Y_ADDRESS_START_END, &err);
		ssd16xx_send_y_param(panel, ram_y_start, &err);
		ssd16xx_send_y_param(panel, ram_y_end, &err);

		ssd16xx_send_cmd(panel, SSD16XX_CMD_SET_RAM_X_ADDRESS_COUNTER, &err);
		ssd16xx_send_x_param(panel, ram_x_start, &err);

		ssd16xx_send_cmd(panel, SSD16XX_CMD_SET_RAM_Y_ADDRESS_COUNTER, &err);
		ssd16xx_send_y_param(panel, ram_y_start, &err);
		break;
	}

	ssd16xx_send_cmd(panel, SSD16XX_CMD_WRITE_RAM_BW, &err);
	ssd16xx_send_data_bulk(panel, mono_buffer, data_size, &err);


	/* Optionally re-send border waveform before each display update */
	if (panel->border_refresh_on_every_update || panel->init_refresh_pending) {
		u8 idx = panel->border_waveform_update_idx;
		u8 border = ssd16xx_border_waveform_values[idx];

		drm_dbg(&panel->drm, "fb_dirty: Refreshing border waveform: 0x%02x\n",
			border);
		ssd16xx_send_cmd(panel, SSD16XX_CMD_BORDER_WAVEFORM_CONTROL, &err);
		ssd16xx_send_data(panel, border, &err);
	}

	switch (panel->refresh_mode) {
	case SSD16XX_REFRESH_FULL:
		/*
		 * BW full refresh: write RED RAM BEFORE display_update
		 * to avoid a post-BUSY write timing issue on some
		 * controller revisions that silently corrupts RED RAM.
		 * RED RAM is then bypassed (CTRL1_BYPASS_RED_RAM) so
		 * stale RED RAM content does not affect the output.
		 */
		if (red_buffer) {
			/* 3-colour: write red channel before activating */
			ssd16xx_send_cmd(panel, SSD16XX_CMD_WRITE_RAM_RED, &err);
			ssd16xx_send_data_bulk(panel, red_buffer, data_size, &err);
			ssd16xx_display_update(panel, SSD16XX_CTRL1_NORMAL,
					       SSD16XX_CTRL1_BYTE2_DEFAULT,
					       SSD1683_CTRL2_FULL_REFRESH, &err);
		} else {
			if (panel->orientation == 90) {
				ssd16xx_send_cmd(panel, SSD16XX_CMD_SET_RAM_X_ADDRESS_COUNTER, &err);
				ssd16xx_send_x_param(panel, ram_x_end, &err);
				ssd16xx_send_cmd(panel, SSD16XX_CMD_SET_RAM_Y_ADDRESS_COUNTER, &err);
				ssd16xx_send_y_param(panel, ram_y_end, &err);
			} else {
				ssd16xx_send_cmd(panel, SSD16XX_CMD_SET_RAM_X_ADDRESS_COUNTER, &err);
				ssd16xx_send_x_param(panel, ram_x_start, &err);
				ssd16xx_send_cmd(panel, SSD16XX_CMD_SET_RAM_Y_ADDRESS_COUNTER, &err);
				ssd16xx_send_y_param(panel, ram_y_start, &err);
			}
			ssd16xx_send_cmd(panel, SSD16XX_CMD_WRITE_RAM_RED, &err);
			ssd16xx_send_data_bulk(panel, mono_buffer, data_size, &err);
			ssd16xx_display_update(panel, SSD16XX_CTRL1_BYPASS_RED_RAM,
					       SSD16XX_CTRL1_BYTE2_DEFAULT,
					       SSD1683_CTRL2_FULL_REFRESH, &err);
		}
		break;
	case SSD16XX_REFRESH_FAST:
		/*
		 * Fast refresh: LUT pre-loaded during hw_init; BYPASS_RED_RAM
		 * so RED RAM does not affect the current output.
		 * Write RED RAM BEFORE display_update (same reasoning as FULL)
		 * so it holds the just-displayed frame as a valid reference for
		 * any subsequent PARTIAL refresh.
		 */
		if (!red_buffer) {
			if (panel->orientation == 90) {
				ssd16xx_send_cmd(panel, SSD16XX_CMD_SET_RAM_X_ADDRESS_COUNTER, &err);
				ssd16xx_send_x_param(panel, ram_x_end, &err);
				ssd16xx_send_cmd(panel, SSD16XX_CMD_SET_RAM_Y_ADDRESS_COUNTER, &err);
				ssd16xx_send_y_param(panel, ram_y_end, &err);
			} else {
				ssd16xx_send_cmd(panel, SSD16XX_CMD_SET_RAM_X_ADDRESS_COUNTER, &err);
				ssd16xx_send_x_param(panel, ram_x_start, &err);
				ssd16xx_send_cmd(panel, SSD16XX_CMD_SET_RAM_Y_ADDRESS_COUNTER, &err);
				ssd16xx_send_y_param(panel, ram_y_start, &err);
			}
			ssd16xx_send_cmd(panel, SSD16XX_CMD_WRITE_RAM_RED, &err);
			ssd16xx_send_data_bulk(panel, mono_buffer, data_size, &err);
		}
		ssd16xx_display_update(panel, SSD16XX_CTRL1_BYPASS_RED_RAM,
				       SSD16XX_CTRL1_BYTE2_DEFAULT,
				       SSD1683_CTRL2_FAST_REFRESH, &err);
		break;
	case SSD16XX_REFRESH_PARTIAL:
	default:
		/*
		 * Partial refresh: both RAMs used for transition waveforms.
		 * RED RAM must hold the PREVIOUS frame (= current display
		 * content) so the controller can compute pixel transitions.
		 * Write RED RAM AFTER display_update so it captures the
		 * just-displayed frame as the reference for the next partial.
		 */
		ssd16xx_display_update(panel, SSD16XX_CTRL1_NORMAL,
				       SSD16XX_CTRL1_BYTE2_DEFAULT,
				       SSD1683_CTRL2_PARTIAL_REFRESH, &err);
		if (!red_buffer) {
			if (panel->orientation == 90) {
				ssd16xx_send_cmd(panel, SSD16XX_CMD_SET_RAM_X_ADDRESS_COUNTER, &err);
				ssd16xx_send_x_param(panel, ram_x_end, &err);
				ssd16xx_send_cmd(panel, SSD16XX_CMD_SET_RAM_Y_ADDRESS_COUNTER, &err);
				ssd16xx_send_y_param(panel, ram_y_end, &err);
			} else {
				ssd16xx_send_cmd(panel, SSD16XX_CMD_SET_RAM_X_ADDRESS_COUNTER, &err);
				ssd16xx_send_x_param(panel, ram_x_start, &err);
				ssd16xx_send_cmd(panel, SSD16XX_CMD_SET_RAM_Y_ADDRESS_COUNTER, &err);
				ssd16xx_send_y_param(panel, ram_y_start, &err);
			}
			ssd16xx_send_cmd(panel, SSD16XX_CMD_WRITE_RAM_RED, &err);
			ssd16xx_send_data_bulk(panel, mono_buffer, data_size, &err);
		}
		break;
	}

	if (err.errno_code)
		drm_err(&panel->drm, "Display update failed: %d\n", err.errno_code);

	kfree(red_buffer);
	kfree(mono_buffer);
}

/* -----------------------------------------------------------------------------
 * Plane Functions
 */

static void ssd16xx_plane_destroy(struct drm_plane *plane)
{
	drm_plane_cleanup(plane);
}

static void ssd16xx_plane_reset(struct drm_plane *plane)
{
	drm_atomic_helper_plane_reset(plane);
}

static const struct drm_plane_funcs ssd16xx_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = ssd16xx_plane_destroy,
	.reset = ssd16xx_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static int ssd16xx_plane_atomic_check(struct drm_plane *plane,
				      struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc_state;

	if (!new_plane_state->crtc)
		return 0;

	crtc_state = drm_atomic_get_new_crtc_state(state, new_plane_state->crtc);

	return drm_atomic_helper_check_plane_state(new_plane_state, crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING,
						   false, false);
}

static void ssd16xx_plane_atomic_update(struct drm_plane *plane,
					struct drm_atomic_state *state)
{
	struct drm_plane_state *old_state = drm_atomic_get_old_plane_state(state, plane);
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct ssd16xx_panel *panel = plane_to_ssd16xx_panel(plane);
	enum ssd16xx_refresh_mode saved_mode;
	u8 saved_border_waveform_idx;
	struct ssd16xx_error_ctx err = { .errno_code = 0 };
	struct drm_framebuffer *fb = new_state->fb;
	struct drm_rect rect;

	drm_dbg(&panel->drm, "plane_atomic_update: fb=%p, initialized=%d\n",
		fb, panel->initialized);

	if (!fb || !panel->initialized)
		return;

	/*
	 * If a rotation change is pending, skip the update here — crtc_atomic_flush
	 * will re-init the hardware for the new orientation and redraw.
	 */
	if (panel->reinit_pending) {
		drm_dbg(&panel->drm, "plane_atomic_update: skipping (reinit pending)\n");
		return;
	}

	if (!drm_atomic_helper_damage_merged(old_state, new_state, &rect)) {
		rect.x1 = 0;
		rect.y1 = 0;
		rect.x2 = fb->width;
		rect.y2 = fb->height;
		drm_dbg(&panel->drm, "plane_atomic_update: no damage, using full screen\n");
	}

	drm_dbg(&panel->drm, "plane_atomic_update: calling fb_dirty rect=(%d,%d)-(%d,%d)\n",
		rect.x1, rect.y1, rect.x2, rect.y2);
	/*
	 * When refresh_mode_init was set, use the specified mode for this first
	 * frame only, then restore the user-configured refresh_mode so
	 * subsequent updates continue with the configured mode.
	 */
	saved_mode = panel->refresh_mode;
	saved_border_waveform_idx = panel->border_waveform_update_idx;
	if (panel->init_refresh_pending) {
		panel->refresh_mode = panel->refresh_mode_init;
		panel->border_waveform_update_idx = panel->border_waveform_init_idx;
	}

	ssd16xx_fb_dirty(fb, &rect, panel);
	panel->last_fb = fb;
	panel->refresh_mode = saved_mode;
	panel->border_waveform_update_idx = saved_border_waveform_idx;

	/*
	 * If this was the init frame (which used border_waveform_init_idx
	 * inside fb_dirty), restore the border waveform to the normal
	 * (non-init) value now that the init refresh is complete.
	 */
	if (panel->init_refresh_pending) {
		drm_dbg(&panel->drm,
			"plane_atomic_update: restoring border waveform to 0x%02x\n",
			ssd16xx_border_waveform_values[saved_border_waveform_idx]);
		ssd16xx_send_cmd(panel, SSD16XX_CMD_BORDER_WAVEFORM_CONTROL, &err);
		ssd16xx_send_data(panel,
				  ssd16xx_border_waveform_values[saved_border_waveform_idx],
				  &err);
		if (err.errno_code)
			drm_err(&panel->drm, "Border waveform restore failed: %d\n",
				err.errno_code);
		panel->init_refresh_pending = false;
	}
}


static const struct drm_plane_helper_funcs ssd16xx_plane_helper_funcs = {
	.atomic_check = ssd16xx_plane_atomic_check,
	.atomic_update = ssd16xx_plane_atomic_update,
};

/* -----------------------------------------------------------------------------
 * CRTC Functions
 */

static void ssd16xx_crtc_destroy(struct drm_crtc *crtc)
{
	drm_crtc_cleanup(crtc);
}

static const struct drm_crtc_funcs ssd16xx_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = ssd16xx_crtc_destroy,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static enum drm_mode_status ssd16xx_crtc_mode_valid(struct drm_crtc *crtc,
						    const struct drm_display_mode *mode)
{
	struct ssd16xx_panel *panel = crtc_to_ssd16xx_panel(crtc);

	/* Accept only our panel's native mode (landscape or portrait) */
	if ((mode->hdisplay == panel->mode->hdisplay &&
	     mode->vdisplay == panel->mode->vdisplay) ||
	    (mode->hdisplay == panel->mode->vdisplay &&
	     mode->vdisplay == panel->mode->hdisplay))
		return MODE_OK;

	return MODE_BAD;
}

static int ssd16xx_crtc_atomic_check(struct drm_crtc *crtc,
				     struct drm_atomic_state *state)
{
	return 0;
}

static void ssd16xx_crtc_atomic_enable(struct drm_crtc *crtc,
				       struct drm_atomic_state *state)
{
	struct ssd16xx_panel *panel = crtc_to_ssd16xx_panel(crtc);
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	struct drm_plane_state *plane_state;
	int ret, idx;

	if (!drm_dev_enter(&panel->drm, &idx))
		return;

	drm_dbg(&panel->drm, "atomic_enable: %dx%d\n",
		       crtc_state->mode.hdisplay, crtc_state->mode.vdisplay);

	panel->width = crtc_state->mode.hdisplay;
	panel->height = crtc_state->mode.vdisplay;

	ret = ssd16xx_hw_init(panel);
	if (ret) {
		drm_err(&panel->drm, "crtc_atomic_enable: HW init failed: %d\n", ret);
		goto out;
	}

	panel->initialized = true;

	/* Clear display on first app launch if configured */
	ssd16xx_clear_display_on_init(panel);

	/*
	 * If refresh_mode_init is set, skip baseline establishment and use the
	 * configured refresh mode directly (e.g. to avoid a full refresh flash
	 * when resuming with preserved RAM content).
	 */
	if (panel->refresh_mode_init >= 0) {
		drm_dbg(&panel->drm,
			       "atomic_enable: refresh_mode_init=%d, skipping baseline\n",
			       panel->refresh_mode_init);
		panel->init_refresh_pending = true;
	}

	/* Draw FB if already attached (plane_atomic_update may have run first). */
	plane_state = drm_atomic_get_new_plane_state(state, crtc->primary);
	if (plane_state && plane_state->fb) {
		struct drm_rect rect = {
			.x1 = 0,
			.y1 = 0,
			.x2 = plane_state->fb->width,
			.y2 = plane_state->fb->height,
		};
		ssd16xx_fb_dirty(plane_state->fb, &rect, panel);
		panel->last_fb = plane_state->fb;
	}

out:
	drm_dev_exit(idx);
}

static void ssd16xx_crtc_atomic_disable(struct drm_crtc *crtc,
					struct drm_atomic_state *state)
{
	struct ssd16xx_panel *panel = crtc_to_ssd16xx_panel(crtc);
	int idx;

	if (!drm_dev_enter(&panel->drm, &idx))
		return;

	panel->init_refresh_pending = false;
	panel->initialized = false;

	/* Clear display on close if configured (before entering deep sleep) */
	ssd16xx_clear_display_on_exit(panel);

	drm_dev_exit(idx);
}

/*
 * Re-initialize hardware and redraw the current framebuffer when the
 * display orientation changes at runtime via the rotation connector property.
 * Called by the DRM atomic helper after atomic_enable/disable have run.
 */
static void ssd16xx_crtc_atomic_flush(struct drm_crtc *crtc,
				      struct drm_atomic_state *state)
{
	struct ssd16xx_panel *panel = crtc_to_ssd16xx_panel(crtc);
	struct drm_framebuffer *fb;
	struct drm_rect full;
	int ret, idx;

	if (!panel->reinit_pending || !panel->initialized)
		return;

	if (!drm_dev_enter(&panel->drm, &idx))
		return;

	panel->reinit_pending = false;
	panel->init_refresh_pending = false;

	drm_dbg(&panel->drm, "atomic_flush: reinit, orientation=%u°\n",
		panel->orientation);

	ret = ssd16xx_hw_init(panel);
	if (ret) {
		drm_err(&panel->drm, "Orientation re-init failed: %d\n", ret);
		drm_dev_exit(idx);
		return;
	}

	fb = panel->primary_plane.state ? panel->primary_plane.state->fb
					: panel->last_fb;
	if (fb) {
		full.x1 = 0;
		full.y1 = 0;
		full.x2 = fb->width;
		full.y2 = fb->height;
		ssd16xx_fb_dirty(fb, &full, panel);
		panel->last_fb = fb;
	}

	drm_dev_exit(idx);
}

static const struct drm_crtc_helper_funcs ssd16xx_crtc_helper_funcs = {
	.mode_valid = ssd16xx_crtc_mode_valid,
	.atomic_check = ssd16xx_crtc_atomic_check,
	.atomic_enable = ssd16xx_crtc_atomic_enable,
	.atomic_disable = ssd16xx_crtc_atomic_disable,
	.atomic_flush = ssd16xx_crtc_atomic_flush,
};

/* -----------------------------------------------------------------------------
 * Connector Functions
 */

static int ssd16xx_connector_get_modes(struct drm_connector *connector)
{
	struct ssd16xx_panel *panel = to_ssd16xx_panel(connector->dev);
	bool mode_is_portrait = (panel->mode->hdisplay < panel->mode->vdisplay);
	bool orient_is_portrait = (panel->orientation == 90);

	drm_dbg(&panel->drm, "connector_get_modes: orientation=%u°\n",
		panel->orientation);

	/* For portrait, swap dimensions so clients see logical size. */
	if (mode_is_portrait != orient_is_portrait) {
		struct drm_display_mode *mode;

		mode = drm_mode_duplicate(&panel->drm, panel->mode);
		if (!mode)
			return 0;
		swap(mode->hdisplay, mode->vdisplay);
		swap(mode->hsync_start, mode->vsync_start);
		swap(mode->hsync_end, mode->vsync_end);
		swap(mode->htotal, mode->vtotal);
		swap(mode->width_mm, mode->height_mm);
		mode->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_set_name(mode);
		drm_mode_probed_add(connector, mode);
		return 1;
	}

	return drm_connector_helper_get_modes_fixed(connector, panel->mode);
}

static const struct drm_connector_helper_funcs ssd16xx_connector_helper_funcs = {
	.get_modes = ssd16xx_connector_get_modes,
};

/* Enum values for the rotation connector property (degrees clockwise) */
static const struct drm_prop_enum_list ssd16xx_rotation_enum[] = {
	{ 0,   "0"   },
	{ 90,  "90"  },
};

/* Enum values for the refresh_mode connector property */
static const struct drm_prop_enum_list ssd16xx_refresh_mode_enum[] = {
	{ SSD16XX_REFRESH_PARTIAL, "partial" },
	{ SSD16XX_REFRESH_FULL,    "full"    },
	{ SSD16XX_REFRESH_FAST,    "fast"    },
};

/*
 * Enum for clear_on_init, clear_on_close, refresh_mode_init properties.
 * Value 0 = disabled; values 1-3 = partial/full/fast (refresh mode + 1).
 * The +1 offset allows a single enum to represent both "disabled" and the
 * three refresh modes without sign-extending the DRM property value.
 */
static const struct drm_prop_enum_list ssd16xx_init_refresh_enum[] = {
	{ 0, "disabled" },
	{ 1, "partial"  },
	{ 2, "full"     },
	{ 3, "fast"     },
};

/* Enum values for border_waveform connector properties (one per HW mode) */
static const struct drm_prop_enum_list ssd16xx_border_waveform_enum[] = {
	{ SSD16XX_BORDER_LUT0, "lut0_black"    },
	{ SSD16XX_BORDER_LUT1, "lut1_white"    },
	{ SSD16XX_BORDER_LUT2, "lut2_black"    },
	{ SSD16XX_BORDER_LUT3, "lut3_gray"     },
	{ SSD16XX_BORDER_VSS,  "vss_black"     },
	{ SSD16XX_BORDER_VSH1, "vsh1_black"    },
	{ SSD16XX_BORDER_VSL,  "vsl_white"     },
	{ SSD16XX_BORDER_VSH2, "vsh2_black"    },
	{ SSD16XX_BORDER_VCOM, "vcom_preserve" },
	{ SSD16XX_BORDER_HIZ,  "hiz_float"     },
};

static int ssd16xx_connector_create_properties(struct ssd16xx_panel *panel)
{
	struct drm_device *drm = &panel->drm;
	struct drm_connector *connector = &panel->connector;

	panel->rotation_property =
		drm_property_create_enum(drm, 0, "rotation",
					 ssd16xx_rotation_enum,
					 ARRAY_SIZE(ssd16xx_rotation_enum));
	if (!panel->rotation_property)
		return -ENOMEM;
	drm_object_attach_property(&connector->base,
				   panel->rotation_property, panel->orientation);

	panel->refresh_mode_property =
		drm_property_create_enum(drm, 0, "refresh_mode",
					 ssd16xx_refresh_mode_enum,
					 ARRAY_SIZE(ssd16xx_refresh_mode_enum));
	if (!panel->refresh_mode_property)
		return -ENOMEM;
	drm_object_attach_property(&connector->base,
				   panel->refresh_mode_property, panel->refresh_mode);

	panel->border_waveform_init_property =
		drm_property_create_enum(drm, 0, "border_waveform_init",
					 ssd16xx_border_waveform_enum,
					 ARRAY_SIZE(ssd16xx_border_waveform_enum));
	if (!panel->border_waveform_init_property)
		return -ENOMEM;
	drm_object_attach_property(&connector->base,
				   panel->border_waveform_init_property,
				   panel->border_waveform_init_idx);

	panel->border_waveform_update_property =
		drm_property_create_enum(drm, 0, "border_waveform_update",
					 ssd16xx_border_waveform_enum,
					 ARRAY_SIZE(ssd16xx_border_waveform_enum));
	if (!panel->border_waveform_update_property)
		return -ENOMEM;
	drm_object_attach_property(&connector->base,
				   panel->border_waveform_update_property,
				   panel->border_waveform_update_idx);

	panel->border_refresh_on_every_update_property =
		drm_property_create_bool(drm, 0, "border_refresh_on_every_update");
	if (!panel->border_refresh_on_every_update_property)
		return -ENOMEM;
	drm_object_attach_property(&connector->base,
				   panel->border_refresh_on_every_update_property,
				   panel->border_refresh_on_every_update);

	panel->clear_on_init_property =
		drm_property_create_enum(drm, 0, "clear_on_init",
					 ssd16xx_init_refresh_enum,
					 ARRAY_SIZE(ssd16xx_init_refresh_enum));
	if (!panel->clear_on_init_property)
		return -ENOMEM;
	/* Property value 0=disabled, 1-3=mode; field is -1/0/1/2 → val = field+1 */
	drm_object_attach_property(&connector->base,
				   panel->clear_on_init_property,
				   panel->clear_on_init + 1);

	panel->clear_on_close_property =
		drm_property_create_enum(drm, 0, "clear_on_close",
					 ssd16xx_init_refresh_enum,
					 ARRAY_SIZE(ssd16xx_init_refresh_enum));
	if (!panel->clear_on_close_property)
		return -ENOMEM;
	drm_object_attach_property(&connector->base,
				   panel->clear_on_close_property,
				   panel->clear_on_close + 1);

	panel->refresh_mode_init_property =
		drm_property_create_enum(drm, 0, "refresh_mode_init",
					 ssd16xx_init_refresh_enum,
					 ARRAY_SIZE(ssd16xx_init_refresh_enum));
	if (!panel->refresh_mode_init_property)
		return -ENOMEM;
	drm_object_attach_property(&connector->base,
				   panel->refresh_mode_init_property,
				   panel->refresh_mode_init + 1);

	return 0;
}

static int ssd16xx_connector_atomic_get_property(
		struct drm_connector *connector,
		const struct drm_connector_state *state,
		struct drm_property *property,
		uint64_t *val)
{
	struct ssd16xx_panel *panel = to_ssd16xx_panel(connector->dev);

	drm_dbg(&panel->drm, "get_property: %s\n", property->name);

	if (property == panel->rotation_property) {
		*val = panel->orientation;
		return 0;
	}
	if (property == panel->refresh_mode_property) {
		*val = panel->refresh_mode;
		return 0;
	}
	if (property == panel->border_waveform_init_property) {
		*val = panel->border_waveform_init_idx;
		return 0;
	}
	if (property == panel->border_waveform_update_property) {
		*val = panel->border_waveform_update_idx;
		return 0;
	}
	if (property == panel->border_refresh_on_every_update_property) {
		*val = panel->border_refresh_on_every_update;
		return 0;
	}
	if (property == panel->clear_on_init_property) {
		*val = panel->clear_on_init + 1;  /* field -1/0/1/2 → val 0/1/2/3 */
		return 0;
	}
	if (property == panel->clear_on_close_property) {
		*val = panel->clear_on_close + 1;
		return 0;
	}
	if (property == panel->refresh_mode_init_property) {
		*val = panel->refresh_mode_init + 1;
		return 0;
	}

	return -EINVAL;
}

static int ssd16xx_connector_atomic_set_property(
		struct drm_connector *connector,
		struct drm_connector_state *state,
		struct drm_property *property,
		uint64_t val)
{
	struct ssd16xx_panel *panel = to_ssd16xx_panel(connector->dev);

	drm_dbg(&panel->drm, "set_property: %s = %llu\n", property->name, val);

	if (property == panel->rotation_property) {
		if (val != 0 && val != 90)
			return -EINVAL;
		panel->orientation = val;
		/*
		 * Flag hardware re-init needed. crtc_atomic_flush will call
		 * ssd16xx_hw_init() with the new orientation and redraw.
		 */
		panel->reinit_pending = true;
		return 0;
	}
	if (property == panel->refresh_mode_property) {
		if (val > SSD16XX_REFRESH_FAST)
			return -EINVAL;
		panel->refresh_mode = val;
		return 0;
	}
	if (property == panel->border_waveform_init_property) {
		if (val >= ARRAY_SIZE(ssd16xx_border_waveform_values))
			return -EINVAL;
		panel->border_waveform_init_idx = val;
		return 0;
	}
	if (property == panel->border_waveform_update_property) {
		if (val >= ARRAY_SIZE(ssd16xx_border_waveform_values))
			return -EINVAL;
		panel->border_waveform_update_idx = val;
		return 0;
	}
	if (property == panel->border_refresh_on_every_update_property) {
		panel->border_refresh_on_every_update = !!val;
		return 0;
	}
	if (property == panel->clear_on_init_property) {
		if (val > 3)
			return -EINVAL;
		panel->clear_on_init = (int)val - 1;  /* val 0/1/2/3 → field -1/0/1/2 */
		panel->first_clear_done = false;  /* allow re-fire on next enable */
		return 0;
	}
	if (property == panel->clear_on_close_property) {
		if (val > 3)
			return -EINVAL;
		panel->clear_on_close = (int)val - 1;
		return 0;
	}
	if (property == panel->refresh_mode_init_property) {
		if (val > 3)
			return -EINVAL;
		panel->refresh_mode_init = (int)val - 1;
		return 0;
	}

	return -EINVAL;
}

static const struct drm_connector_funcs ssd16xx_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
	.atomic_get_property = ssd16xx_connector_atomic_get_property,
	.atomic_set_property = ssd16xx_connector_atomic_set_property,
};

static const u32 ssd16xx_formats[] = {
	DRM_FORMAT_XRGB8888,  /* 32-bit RGB with padding (preferred) */
	DRM_FORMAT_RGB888,    /* 24-bit packed RGB */
	DRM_FORMAT_RGB565,    /* 16-bit RGB (5:6:5) */
	DRM_FORMAT_R8,        /* 8-bit grayscale */
	DRM_FORMAT_NV12,      /* YUV 4:2:0 planar */
	DRM_FORMAT_NV16,      /* YUV 4:2:2 planar */
	DRM_FORMAT_YUYV,      /* Packed YUV 4:2:2 (Y0 U0 Y1 V0) */
	DRM_FORMAT_UYVY,      /* Packed YUV 4:2:2 (U0 Y0 V0 Y1) */
	DRM_FORMAT_R1,        /* 1-bit monochrome (native, 8 pixels/byte) */
};

DEFINE_DRM_GEM_FOPS(ssd16xx_fops);

static struct drm_driver ssd16xx_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &ssd16xx_fops,
	.name = "ssd16xx",
	.desc = "DRM driver for SSD16xx e-paper controller family",
	.major = 1,
	.minor = 0,
	DRM_GEM_DMA_DRIVER_OPS,
	DRM_FBDEV_DMA_DRIVER_OPS,
};

static const struct drm_mode_config_funcs ssd16xx_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static int ssd16xx_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct ssd16xx_panel *panel;
	struct drm_device *drm;
	const struct spi_device_id *spi_id;
	struct drm_display_mode *mode;
	const void *match;
	enum ssd16xx_model model;
	u32 dt_rotation = 0;
	int ret;

	match = device_get_match_data(dev);
	if (match) {
		model = (enum ssd16xx_model)(uintptr_t)match;
	} else {
		spi_id = spi_get_device_id(spi);
		model = (enum ssd16xx_model)spi_id->driver_data;
	}

	if (!dev->coherent_dma_mask) {
		ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(64));
		if (ret) {
			dev_warn(dev, "Failed to set DMA mask: %d\n", ret);
			return ret;
		}
	}

	panel = devm_drm_dev_alloc(dev, &ssd16xx_drm_driver,
				   struct ssd16xx_panel, drm);
	if (IS_ERR(panel))
		return PTR_ERR(panel);

	drm = &panel->drm;
	panel->spi = spi;
	panel->model = model;
	spi_set_drvdata(spi, panel);

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = SSD16XX_SPI_BITS_PER_WORD;

	if (!spi->max_speed_hz) {
		drm_warn(drm, "spi-max-frequency not specified, using %u Hz\n",
			 SSD16XX_SPI_SPEED_DEFAULT);
		spi->max_speed_hz = SSD16XX_SPI_SPEED_DEFAULT;
	}

	ret = spi_setup(spi);
	if (ret < 0) {
		drm_err(drm, "SPI setup failed: %d\n", ret);
		return ret;
	}

	switch (model) {
	case GDEY042T81:
		panel->controller = SSD1683;
		break;
	default:
		drm_err(drm, "Unknown panel model: %d\n", model);
		return -EINVAL;
	}

	if (panel->controller >= ARRAY_SIZE(ssd16xx_controller_configs) ||
	    !ssd16xx_controller_configs[panel->controller].max_width)
		return -EINVAL;
	panel->controller_cfg = &ssd16xx_controller_configs[panel->controller];

	if (model >= ARRAY_SIZE(ssd16xx_panel_configs))
		return -EINVAL;
	panel->panel_cfg = &ssd16xx_panel_configs[model];

	mode = devm_kmemdup(dev, panel->panel_cfg->mode,
			    sizeof(*panel->panel_cfg->mode), GFP_KERNEL);
	if (!mode)
		return -ENOMEM;

	panel->refresh_mode = panel->panel_cfg->default_refresh_mode;
	panel->border_waveform_init_idx   = panel->panel_cfg->default_border_waveform_init;
	panel->border_waveform_update_idx = panel->panel_cfg->default_border_waveform_update;
	panel->border_refresh_on_every_update =
		panel->panel_cfg->default_border_refresh_on_every_update;
	panel->clear_on_init   = panel->panel_cfg->default_clear_on_init;
	panel->clear_on_close  = panel->panel_cfg->default_clear_on_close;
	panel->refresh_mode_init = panel->panel_cfg->default_refresh_mode_init;

	/* Module parameter overrides for border/display control */
	if (border_waveform_init_lut >= 0 &&
	    border_waveform_init_lut < (int)ARRAY_SIZE(ssd16xx_border_waveform_values))
		panel->border_waveform_init_idx = border_waveform_init_lut;
	if (border_waveform_lut >= 0 &&
	    border_waveform_lut < (int)ARRAY_SIZE(ssd16xx_border_waveform_values))
		panel->border_waveform_update_idx = border_waveform_lut;
	if (border_refresh_on_every_update)
		panel->border_refresh_on_every_update = true;
	if (clear_on_init >= 0 && clear_on_init <= 2)
		panel->clear_on_init = clear_on_init;
	if (clear_on_close >= 0 && clear_on_close <= 2)
		panel->clear_on_close = clear_on_close;
	if (refresh_mode_init >= 0 && refresh_mode_init <= 2)
		panel->refresh_mode_init = refresh_mode_init;

	/* Module parameter overrides panel default refresh mode when set */
	if (refresh_mode >= 0) {
		if (refresh_mode > SSD16XX_REFRESH_FAST)
			drm_warn(drm, "Invalid refresh_mode module param %d, ignored\n",
				 refresh_mode);
		else
			panel->refresh_mode = refresh_mode;
	}

	drm_dbg(drm, "Refresh mode: %s\n",
		panel->refresh_mode == SSD16XX_REFRESH_FULL  ? "full" :
		panel->refresh_mode == SSD16XX_REFRESH_FAST  ? "fast" : "partial");

	/* Parse "rotation" DT property; swap mode dimensions for portrait. */
	device_property_read_u32(dev, "rotation", &dt_rotation);
	if (dt_rotation != 0 && dt_rotation != 90) {
		drm_warn(drm, "Invalid DT rotation %u, defaulting to 0°\n", dt_rotation);
		dt_rotation = 0;
	}
	panel->orientation = dt_rotation;

	/* Module parameter overrides DT rotation when set */
	if (rotation >= 0) {
		if (rotation != 0 && rotation != 90)
			drm_warn(drm, "Invalid rotation module param %d, ignored\n",
				 rotation);
		else
			panel->orientation = rotation;
	}

	drm_dbg(drm, "Using %s orientation (%u°, %ux%u logical)\n",
		panel->orientation == 90 ? "portrait" : "landscape",
		panel->orientation, mode->hdisplay, mode->vdisplay);

	/* Swap mode dimensions for portrait so clients see logical size. */
	if (panel->orientation == 90) {
		swap(mode->hdisplay, mode->vdisplay);
		swap(mode->hsync_start, mode->vsync_start);
		swap(mode->hsync_end, mode->vsync_end);
		swap(mode->htotal, mode->vtotal);
		swap(mode->width_mm, mode->height_mm);
		drm_dbg(drm, "Mode dimensions swapped for portrait: %ux%u\n",
			mode->hdisplay, mode->vdisplay);
	} else {
		drm_dbg(drm, "Mode dimensions unchanged: %ux%u\n",
			mode->hdisplay, mode->vdisplay);
	}
	panel->mode = mode;
	panel->width = mode->hdisplay;
	panel->height = mode->vdisplay;

	/* Acquire GPIOs. */
	panel->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(panel->reset))
		return dev_err_probe(dev, PTR_ERR(panel->reset), "Failed to get RESET GPIO\n");

	panel->busy = devm_gpiod_get(dev, "busy", GPIOD_IN);
	if (IS_ERR(panel->busy))
		return dev_err_probe(dev, PTR_ERR(panel->busy), "Failed to get BUSY GPIO\n");

	panel->dc = devm_gpiod_get(dev, "dc", GPIOD_OUT_LOW);
	if (IS_ERR(panel->dc))
		return dev_err_probe(dev, PTR_ERR(panel->dc), "Failed to get DC GPIO\n");

	ssd16xx_hw_reset(panel);

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.funcs = &ssd16xx_mode_config_funcs;
	drm->mode_config.min_width = min(panel->width, panel->height);
	drm->mode_config.max_width = max(panel->width, panel->height);
	drm->mode_config.min_height = min(panel->width, panel->height);
	drm->mode_config.max_height = max(panel->width, panel->height);

	drm_connector_helper_add(&panel->connector, &ssd16xx_connector_helper_funcs);
	ret = drm_connector_init(drm, &panel->connector, &ssd16xx_connector_funcs,
				 DRM_MODE_CONNECTOR_SPI);
	if (ret)
		return ret;

	ret = drm_universal_plane_init(drm, &panel->primary_plane, 0,
				       &ssd16xx_plane_funcs,
				       ssd16xx_formats, ARRAY_SIZE(ssd16xx_formats),
				       NULL, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&panel->primary_plane, &ssd16xx_plane_helper_funcs);

	ret = drm_crtc_init_with_planes(drm, &panel->crtc, &panel->primary_plane,
					NULL, &ssd16xx_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&panel->crtc, &ssd16xx_crtc_helper_funcs);

	ret = drm_simple_encoder_init(drm, &panel->encoder, DRM_MODE_ENCODER_NONE);
	if (ret)
		return ret;
	panel->encoder.possible_crtcs = drm_crtc_mask(&panel->crtc);

	ret = drm_connector_attach_encoder(&panel->connector, &panel->encoder);
	if (ret)
		return ret;

	/*
	 * Create the rotation connector property so userspace can change the
	 * display orientation at runtime. The initial value reflects the
	 * boot-time DT rotation.
	 */
	ret = ssd16xx_connector_create_properties(panel);
	if (ret)
		return ret;

	/*
	 * Hardware rotation is handled via Data Entry Mode and mode dimension
	 * swapping. Do NOT set panel_orientation here — that would cause double
	 * rotation (hardware + compositor). See MIPI DBI drivers for reference.
	 */

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	drm_dbg(drm, "SSD16xx e-paper display initialized (%dx%d, %d° rotation)\n",
		panel->width, panel->height, panel->orientation);

	drm_client_setup(drm, NULL);

	return 0;
}

static void ssd16xx_remove(struct spi_device *spi)
{
	struct ssd16xx_panel *panel = spi_get_drvdata(spi);

	drm_dev_unplug(&panel->drm);
	drm_atomic_helper_shutdown(&panel->drm);
}

static void ssd16xx_shutdown(struct spi_device *spi)
{
	struct ssd16xx_panel *panel = spi_get_drvdata(spi);

	drm_atomic_helper_shutdown(&panel->drm);
}

static const struct of_device_id ssd16xx_of_match[] = {
	{ .compatible = "gooddisplay,gdey042t81", .data = (void *)GDEY042T81 },
	{ }
};
MODULE_DEVICE_TABLE(of, ssd16xx_of_match);

static const struct spi_device_id ssd16xx_id[] = {
	{ "gdey042t81", GDEY042T81 },
	{ }
};
MODULE_DEVICE_TABLE(spi, ssd16xx_id);

static struct spi_driver ssd16xx_spi_driver = {
	.driver = {
		.name = "ssd16xx",
		.of_match_table = ssd16xx_of_match,
	},
	.probe = ssd16xx_probe,
	.remove = ssd16xx_remove,
	.shutdown = ssd16xx_shutdown,
	.id_table = ssd16xx_id,
};
module_spi_driver(ssd16xx_spi_driver);

MODULE_AUTHOR("Devarsh Thakkar <devarsht@ti.com>");
MODULE_DESCRIPTION("DRM driver for Solomon SSD16xx e-paper display controller family");
MODULE_LICENSE("GPL");
