// SPDX-License-Identifier: GPL-2.0-only
/*
 * DRM driver for e-paper display panels using Solomon SSD16xx family controllers
 *
 * Copyright (C) 2026 Texas Instruments Incorporated - https://www.ti.com/
 *
 * Author: Devarsh Thakkar <devarsht@ti.com>
 *
 * References: https://github.com/Lesords/epaper
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/spi/spi.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_print.h>

/* -----------------------------------------------------------------------
 * SSD16xx family common: commands, data values, and bit definitions.
 * These apply equally to SSD1673, SSD1680, and SSD1683.
 * -----------------------------------------------------------------------
 */

/* SPI command codes (common) */
#define SSD16XX_CMD_DRIVER_OUTPUT_CONTROL		0x01
#define SSD16XX_CMD_BOOSTER_SOFT_START			0x0C
#define SSD16XX_CMD_DEEP_SLEEP_MODE			0x10
#define SSD16XX_CMD_DATA_ENTRY_MODE			0x11
#define SSD16XX_CMD_SW_RESET				0x12
#define SSD16XX_CMD_MASTER_ACTIVATION			0x20
#define SSD16XX_CMD_DISPLAY_UPDATE_CONTROL1		0x21
#define SSD16XX_CMD_DISPLAY_UPDATE_CONTROL2		0x22
#define SSD16XX_CMD_WRITE_RAM_BW			0x24
#define SSD16XX_CMD_BORDER_WAVEFORM_CONTROL		0x3C
#define SSD16XX_CMD_SET_RAM_X_ADDRESS_START_END		0x44
#define SSD16XX_CMD_SET_RAM_Y_ADDRESS_START_END		0x45
#define SSD16XX_CMD_SET_RAM_X_ADDRESS_COUNTER		0x4E
#define SSD16XX_CMD_SET_RAM_Y_ADDRESS_COUNTER		0x4F

#define SSD16XX_PM_AUTOSUSPEND_DELAY_MS			35000

/*
 * Data Entry Mode (command 0x11) AM/IDY/IDX bit encoding (common).
 *
 * Bit 2 (AM): Address update direction: 0 = X direction, 1 = Y direction
 * ID[1:0] when AM=0 (X-direction modes, address counter advances in X):
 *   00 = X decrement, Y decrement   01 = X increment, Y decrement
 *   10 = X decrement, Y increment   11 = X increment, Y increment (default)
 *
 * The driver uses two modes; together with the RAM cursor start position
 * and ssd16xx_convert_fb()'s packing order they implement all rotations:
 *   0°/270° → 0x03 (X++, Y++), cursor at (0, 0)
 *   90°/180° → 0x00 (X--, Y--), cursor at (max, max)
 */
#define SSD16XX_DATA_ENTRY_XDEC_YDEC		0x00  /* X--, Y-- (X-mode) */
#define SSD16XX_DATA_ENTRY_XINC_YINC		0x03  /* X++, Y++ (X-mode, default) */

/* POR reset value: GD=0 (G0 first), SM=0 (interlaced), TB=0 (G0->G299) */
#define SSD16XX_DRIVER_OUTPUT_CTRL_DEFAULT	0x00

/* Display Update Control 1 (0x21) byte 2 default (common) */
#define SSD16XX_CTRL1_BYTE2_DEFAULT		0x00

/*
 * Display Update Control 2 (0x22) individual bit definitions (common).
 * NOTE: BIT(3) is NOT common — see SSD1683_CTRL2_MODE2 in the SSD1683
 * section below; it has a completely different meaning in SSD1673.
 */
#define SSD16XX_CTRL2_ENABLE_CLK		BIT(7)
#define SSD16XX_CTRL2_ENABLE_ANALOG		BIT(6)
#define SSD16XX_CTRL2_LOAD_TEMPERATURE		BIT(5)
#define SSD16XX_CTRL2_LOAD_LUT			BIT(4)
#define SSD16XX_CTRL2_DISPLAY			BIT(2)
#define SSD16XX_CTRL2_DISABLE_ANALOG		BIT(1)
#define SSD16XX_CTRL2_DISABLE_CLK		BIT(0)

#define SSD16XX_SPI_BITS_PER_WORD		8
#define SSD16XX_SPI_SPEED_DEFAULT		1000000

/* Maximum time to wait for the BUSY pin to deassert after a display update */
#define SSD16XX_BUSY_WAIT_TIMEOUT_MS		6000

/* -----------------------------------------------------------------------
 * SSD1683 / SSD1680 specific: commands, data values, and bit definitions.
 * -----------------------------------------------------------------------
 */

/* Deep Sleep Mode values (command 0x10) */
#define SSD1683_DEEP_SLEEP_MODE_1			0x01  /* RAM retained */
#define SSD1683_DEEP_SLEEP_MODE_2			0x03  /* RAM lost (max power) */

/* Temperature Sensor Selection (command 0x18) */
#define SSD1683_CMD_TEMPERATURE_SENSOR_CONTROL		0x18
#define SSD1683_TEMP_SENSOR_INTERNAL			0x80  /* Bit 7: use internal sensor */

#define SSD1683_CMD_WRITE_RAM_RED			0x26

/* Border Waveform Control (command 0x3C) values */
#define SSD1683_BORDER_WAVEFORM_LUT0		0x00  /* GS Transition LUT0 (black) */
#define SSD1683_BORDER_WAVEFORM_LUT1		0x01  /* GS Transition LUT1 (white) */
#define SSD1683_BORDER_WAVEFORM_LUT2		0x02  /* GS Transition LUT2 (black) */
#define SSD1683_BORDER_WAVEFORM_LUT3		0x03  /* GS Transition LUT3 (gray) */
#define SSD1683_BORDER_WAVEFORM_FIXLVL_VSS	0x40  /* Fix Level VSS (0V, black) */
#define SSD1683_BORDER_WAVEFORM_FIXLVL_VSH1	0x50  /* Fix Level VSH1 (+15V, black) */
#define SSD1683_BORDER_WAVEFORM_FIXLVL_VSL	0x60  /* Fix Level VSL (-15V, white) */
#define SSD1683_BORDER_WAVEFORM_FIXLVL_VSH2	0x70  /* Fix Level VSH2 (+15V alt, black) */
#define SSD1683_BORDER_WAVEFORM_VCOM		0x80  /* Follow VCOM (-2V~-3V, preserve) */
#define SSD1683_BORDER_WAVEFORM_HIZ		0xC0  /* HiZ (floating, default) */

/* Display Update Control 1 (0x21) byte 1: RED RAM control */
#define SSD1683_CTRL1_NORMAL			0x00  /* Both BW and RED RAMs enabled */
#define SSD1683_CTRL1_BYPASS_RED_RAM		0x40  /* Bypass RED RAM (force RED=0) */

/* Display Update Control 2 (0x22) BIT(3): Display Mode 2 (partial) */
#define SSD1683_CTRL2_MODE2			BIT(3)

/* Composite CTRL2 sequences for each refresh mode */
#define SSD1683_CTRL2_FULL_REFRESH (SSD16XX_CTRL2_ENABLE_CLK | \
				    SSD16XX_CTRL2_ENABLE_ANALOG | \
				    SSD16XX_CTRL2_LOAD_TEMPERATURE | \
				    SSD16XX_CTRL2_LOAD_LUT | \
				    SSD16XX_CTRL2_DISPLAY | \
				    SSD16XX_CTRL2_DISABLE_ANALOG | \
				    SSD16XX_CTRL2_DISABLE_CLK)  /* 0xF7, ~1.5-2s */

#define SSD1683_CTRL2_FAST_REFRESH (SSD16XX_CTRL2_ENABLE_CLK | \
				    SSD16XX_CTRL2_ENABLE_ANALOG | \
				    SSD16XX_CTRL2_DISPLAY | \
				    SSD16XX_CTRL2_DISABLE_ANALOG | \
				    SSD16XX_CTRL2_DISABLE_CLK)  /* 0xC7, ~1.0-1.5s */

#define SSD1683_CTRL2_PARTIAL_REFRESH (SSD16XX_CTRL2_ENABLE_CLK | \
				       SSD16XX_CTRL2_ENABLE_ANALOG | \
				       SSD16XX_CTRL2_LOAD_TEMPERATURE | \
				       SSD16XX_CTRL2_LOAD_LUT | \
				       SSD1683_CTRL2_MODE2 | \
				       SSD16XX_CTRL2_DISPLAY | \
				       SSD16XX_CTRL2_DISABLE_ANALOG | \
				       SSD16XX_CTRL2_DISABLE_CLK)  /* 0xFF, ~300-500ms */

/* Load temperature and LUT without a display update, for FAST refresh */
#define SSD1683_CTRL2_LOAD_TEMP_LUT		(SSD16XX_CTRL2_ENABLE_CLK | \
						 SSD16XX_CTRL2_LOAD_LUT | \
						 SSD16XX_CTRL2_LOAD_TEMPERATURE | \
						 SSD16XX_CTRL2_DISABLE_CLK)  /* 0xB1 */

enum ssd16xx_controller {
	SSD1683 = 1,
	SSD1677,
};

enum ssd16xx_model {
	GDEY042T81 = 1,
};

enum ssd16xx_refresh_mode {
	SSD16XX_REFRESH_PARTIAL = 0,  /* Partial refresh (~300-500ms) */
	SSD16XX_REFRESH_FULL,         /* Full refresh (~1.5-2s) */
	SSD16XX_REFRESH_FAST,         /* Fast refresh, skip temp load (~1.0-1.5s) */
};

enum ssd16xx_color_mode {
	SSD16XX_COLOR_MODE_BW = 0,     /* Black/white only; RED RAM always bypassed */
	SSD16XX_COLOR_MODE_3COLOR = 1, /* 3-colour BWR; RED RAM used for red pixels */
};

/* Session property values: disabled, or the refresh mode to use */
enum ssd16xx_session_mode {
	SSD16XX_SESSION_DISABLED = 0,
	SSD16XX_SESSION_PARTIAL,
	SSD16XX_SESSION_FULL,
	SSD16XX_SESSION_FAST,
};

/* Mapped to register values by controller_cfg->border_waveform_table */
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

/* SSD1683/SSD1680 border waveform byte encoding for command 0x3C */
static const u8 ssd1683_border_waveform_table[] = {
	[SSD16XX_BORDER_LUT0] = SSD1683_BORDER_WAVEFORM_LUT0,
	[SSD16XX_BORDER_LUT1] = SSD1683_BORDER_WAVEFORM_LUT1,
	[SSD16XX_BORDER_LUT2] = SSD1683_BORDER_WAVEFORM_LUT2,
	[SSD16XX_BORDER_LUT3] = SSD1683_BORDER_WAVEFORM_LUT3,
	[SSD16XX_BORDER_VSS]  = SSD1683_BORDER_WAVEFORM_FIXLVL_VSS,
	[SSD16XX_BORDER_VSH1] = SSD1683_BORDER_WAVEFORM_FIXLVL_VSH1,
	[SSD16XX_BORDER_VSL]  = SSD1683_BORDER_WAVEFORM_FIXLVL_VSL,
	[SSD16XX_BORDER_VSH2] = SSD1683_BORDER_WAVEFORM_FIXLVL_VSH2,
	[SSD16XX_BORDER_VCOM] = SSD1683_BORDER_WAVEFORM_VCOM,
	[SSD16XX_BORDER_HIZ]  = SSD1683_BORDER_WAVEFORM_HIZ,
};

struct ssd16xx_controller_config {
	u16 max_width;
	u16 max_height;
	u8 ram_x_address_bits;
	u8 ram_y_address_bits;

	bool has_temp_sensor_ctrl; /* Supports command 0x18 */

	/* Command 0x10 values: level1 retains RAM, level2 may lose it */
	u8 deep_sleep_mode_level1;
	u8 deep_sleep_mode_level2;

	const u8 *border_waveform_table; /* Indexed by enum ssd16xx_border_waveform */

	/* Command 0x21 byte 1: use RED RAM, or drive from BW RAM only */
	u8 ctrl1_normal;
	u8 ctrl1_bypass_red_ram;

	u8 ctrl2_refresh[3];     /* Command 0x22, indexed by enum ssd16xx_refresh_mode */
	u8 ctrl2_load_temp_lut;  /* Command 0x22 LUT load without display update */
};

struct ssd16xx_device_config {
	u8 data_entry_mode;
	u8 driver_output_ctrl_byte3; /* Command 0x01 third byte: scan direction */

	enum ssd16xx_refresh_mode default_refresh_mode;
	enum ssd16xx_border_waveform default_border_waveform_init;
	enum ssd16xx_border_waveform default_border_waveform_update;
	bool default_border_refresh_on_every_update;
	int default_refresh_mode_init; /* -1 = disabled */

	int default_clear_on_init;    /* -1 = disabled, else enum ssd16xx_refresh_mode */
	int default_clear_on_close;   /* Same encoding as default_clear_on_init */
	int default_clear_on_disable; /* Same encoding as default_clear_on_init */

	bool red_supported; /* Panel has a red colour plane */
	enum ssd16xx_color_mode default_color_mode;

	const u8 *booster_soft_start_data; /* Optional command 0x0C bytes */
	u8        booster_soft_start_len;

	bool x_mirror; /* Controller scans this panel's RAM X in reverse */

	const struct drm_display_mode *mode;
};

struct ssd16xx_device {
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
	const struct ssd16xx_device_config *device_cfg;
	struct drm_display_mode mode; /* Logical mode, swapped for portrait */
	u32 width;
	u32 height;

	bool initialized;
	bool init_refresh_pending; /* First frame after refresh_mode_init enable */
	bool first_clear_done;  /* clear_on_init has already fired once */
	bool new_session; /* Master set, session props not yet seen */
	bool cleared_on_disable; /* Avoid redundant clear in atomic_disable */
	bool cleared_on_close;   /* Avoid redundant clear in master_drop */

	/*
	 * Serializes panel access and the session state between atomic commits
	 * and master_set()/master_drop(), which run outside the commit machinery.
	 */
	struct mutex lock;
	bool pm_force_suspended; /* System suspend used pm_runtime_force_suspend() */

	int orientation;      /* Display orientation in degrees: 0/90/180/270 */
	u8 deep_sleep_mode;   /* Command 0x10 value for the next suspend */
	enum ssd16xx_refresh_mode refresh_mode; /* Active refresh mode */
	enum ssd16xx_color_mode color_mode;     /* Active color mode (BW or 3-color) */
	bool fast_lut_loaded; /* Controller holds the fast-refresh LUT */

	/* Border waveform (as enum indices) */
	int border_waveform_init_idx;   /* Border waveform during clear/init */
	int border_waveform_update_idx; /* Border waveform during display updates */
	bool border_refresh_on_every_update; /* Re-send border cmd each display update */
	bool border_waveform_pending;   /* One-shot: send border cmd on next update */
	bool ram_lost; /* RAM lost or unknown: next frame needs a full refresh */

	/* Display control */
	int clear_on_init;    /* -1 = disabled, else enum ssd16xx_refresh_mode */
	int clear_on_close;
	int clear_on_disable;
	int refresh_mode_init; /* -1=disabled, else use this mode for the first frame */

	u8  *tx_buf;     /* 1bpp frame buffer (mono + white) */
	u8  *tx_red_buf; /* 1bpp red-channel buffer (3-color panels only) */
	u16 *tx_buf9;    /* 9-bit SPI expansion buffer (3-wire mode only) */
	u8  spi_buf[64] ____cacheline_aligned; /* DMA-safe buffer for SPI transfers */

	struct drm_property *refresh_mode_property;
	struct drm_property *color_mode_property;
	struct drm_property *clear_on_init_property;
	struct drm_property *clear_on_close_property;
	struct drm_property *clear_on_disable_property;
	struct drm_property *refresh_mode_init_property;
};

/* Plane state subclass carrying the driver's plane properties. */
struct ssd16xx_plane_state {
	struct drm_shadow_plane_state base;
	enum ssd16xx_refresh_mode refresh_mode; /* Pending refresh mode */
	enum ssd16xx_color_mode color_mode;     /* Pending color mode */
	int clear_on_init;    /* Same encoding as in struct ssd16xx_device */
	int clear_on_close;
	int clear_on_disable;
	int refresh_mode_init;
};

#define to_ssd16xx_plane_state(s) \
	container_of((s), struct ssd16xx_plane_state, base.base)

static inline struct ssd16xx_device *to_ssd16xx_device(struct drm_device *drm)
{
	return container_of(drm, struct ssd16xx_device, drm);
}

static inline struct ssd16xx_device *crtc_to_ssd16xx_device(struct drm_crtc *crtc)
{
	return container_of(crtc, struct ssd16xx_device, crtc);
}

static inline struct ssd16xx_device *plane_to_ssd16xx_device(struct drm_plane *plane)
{
	return container_of(plane, struct ssd16xx_device, primary_plane);
}

static const struct ssd16xx_controller_config ssd16xx_controller_configs[] = {
	[SSD1683] = {
		.max_width = 400,
		.max_height = 300,
		.ram_x_address_bits = 8,
		.ram_y_address_bits = 16,
		.has_temp_sensor_ctrl    = true,
		.deep_sleep_mode_level1  = SSD1683_DEEP_SLEEP_MODE_1,
		.deep_sleep_mode_level2  = SSD1683_DEEP_SLEEP_MODE_2,
		.border_waveform_table   = ssd1683_border_waveform_table,
		.ctrl1_normal            = SSD1683_CTRL1_NORMAL,
		.ctrl1_bypass_red_ram    = SSD1683_CTRL1_BYPASS_RED_RAM,
		.ctrl2_refresh = {
			[SSD16XX_REFRESH_PARTIAL] = SSD1683_CTRL2_PARTIAL_REFRESH,
			[SSD16XX_REFRESH_FULL]    = SSD1683_CTRL2_FULL_REFRESH,
			[SSD16XX_REFRESH_FAST]    = SSD1683_CTRL2_FAST_REFRESH,
		},
		.ctrl2_load_temp_lut     = SSD1683_CTRL2_LOAD_TEMP_LUT,
	},
	[SSD1677] = {
		/* Only deep sleep mode 0x03 exists, with no RAM retention. */
		.max_width  = 960,
		.max_height = 680,
		.ram_x_address_bits = 10,
		.ram_y_address_bits = 10,
		.has_temp_sensor_ctrl    = true,
		.deep_sleep_mode_level1  = SSD1683_DEEP_SLEEP_MODE_2,
		.deep_sleep_mode_level2  = SSD1683_DEEP_SLEEP_MODE_2,
		.border_waveform_table   = ssd1683_border_waveform_table,
		.ctrl1_normal            = SSD1683_CTRL1_NORMAL,
		.ctrl1_bypass_red_ram    = SSD1683_CTRL1_BYPASS_RED_RAM,
		.ctrl2_refresh = {
			[SSD16XX_REFRESH_PARTIAL] = 0xFF,
			[SSD16XX_REFRESH_FULL]    = 0xF4,
			[SSD16XX_REFRESH_FAST]    = SSD1683_CTRL2_FAST_REFRESH,
		},
		.ctrl2_load_temp_lut     = SSD1683_CTRL2_LOAD_TEMP_LUT,
	},
};

/* GDEY042T81: 4.2" 400x300 panel, 84.8x63.6mm active area */
static const struct drm_display_mode gdey042t81_mode = {
	DRM_SIMPLE_MODE(400, 300, 85, 64),
};

static const struct ssd16xx_device_config ssd16xx_device_configs[] = {
	[GDEY042T81] = {
		.data_entry_mode = SSD16XX_DATA_ENTRY_XINC_YINC,
		.driver_output_ctrl_byte3 = SSD16XX_DRIVER_OUTPUT_CTRL_DEFAULT,
		.default_refresh_mode = SSD16XX_REFRESH_PARTIAL,
		.default_border_waveform_init   = SSD16XX_BORDER_LUT1,  /* white, clean clear */
		.default_border_waveform_update = SSD16XX_BORDER_HIZ,   /* floating, preserve */
		.default_border_refresh_on_every_update = false,
		.default_clear_on_init    = -1,
		.default_clear_on_close   = -1,
		.default_clear_on_disable = -1,
		.default_refresh_mode_init = SSD16XX_REFRESH_FULL,
		.red_supported = false,  /* 2-colour black/white panel */
		.x_mirror = false,
		.default_color_mode = SSD16XX_COLOR_MODE_BW,
		.mode = &gdey042t81_mode,
	},
};

static void ssd16xx_wait_for_device(struct ssd16xx_device *device,
				    int *err)
{
	unsigned long timeout_jiffies = jiffies +
		msecs_to_jiffies(SSD16XX_BUSY_WAIT_TIMEOUT_MS);
	unsigned long start_ms = jiffies_to_msecs(jiffies);
	int busy_val;

	if (*err)
		return;

	busy_val = gpiod_get_value_cansleep(device->busy);
	drm_dbg(&device->drm, "BUSY initial value: %d\n", busy_val);

	while (gpiod_get_value_cansleep(device->busy) == 1) {
		if (time_after(jiffies, timeout_jiffies)) {
			drm_err(&device->drm, "Busy wait timed out after %lums\n",
				jiffies_to_msecs(jiffies) - start_ms);
			/* A hung controller only recovers through hw_init's reset. */
			device->initialized = false;
			device->ram_lost = true;
			*err = -ETIMEDOUT;
			return;
		}
		usleep_range(1000, 2000);
	}

	drm_dbg(&device->drm, "BUSY became ready after %lums\n",
		jiffies_to_msecs(jiffies) - start_ms);
}

static void ssd16xx_spi_sync(struct spi_device *spi, struct spi_message *msg,
			     int *err)
{
	int ret;

	if (*err)
		return;

	ret = spi_sync(spi, msg);
	if (ret < 0)
		*err = ret;
}

static void ssd16xx_send_cmd(struct ssd16xx_device *device, u8 cmd,
			     int *err)
{
	struct spi_transfer xfer = {};
	struct spi_message msg;

	if (*err)
		return;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	if (device->dc) {
		/* 4-wire SPI: D/C# GPIO low selects command mode */
		device->spi_buf[0] = cmd;
		xfer.tx_buf = device->spi_buf;
		xfer.len = 1;
		gpiod_set_value_cansleep(device->dc, 0);
	} else {
		/*
		 * 3-wire SPI (9-bit): bit 8 is the D/C# bit.
		 * D/C# = 0 means the following 8 bits are a command.
		 */
		u16 *word = (u16 *)device->spi_buf;

		*word = cmd; /* bit 8 = 0 for command */
		xfer.tx_buf = device->spi_buf;
		xfer.len = sizeof(u16);
		xfer.bits_per_word = 9;
	}

	ssd16xx_spi_sync(device->spi, &msg, err);
}

static void ssd16xx_send_data(struct ssd16xx_device *device, u8 data,
			      int *err)
{
	struct spi_transfer xfer = {};
	struct spi_message msg;

	if (*err)
		return;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	if (device->dc) {
		/* 4-wire SPI: D/C# GPIO high selects data mode */
		device->spi_buf[0] = data;
		xfer.tx_buf = device->spi_buf;
		xfer.len = 1;
		gpiod_set_value_cansleep(device->dc, 1);
	} else {
		/*
		 * 3-wire SPI (9-bit): bit 8 is the D/C# bit.
		 * D/C# = 1 means the following 8 bits are data.
		 */
		u16 *word = (u16 *)device->spi_buf;

		*word = 0x100 | data;
		xfer.tx_buf = device->spi_buf;
		xfer.len = sizeof(u16);
		xfer.bits_per_word = 9;
	}

	ssd16xx_spi_sync(device->spi, &msg, err);
}

static void ssd16xx_send_x_param(struct ssd16xx_device *device, u16 x,
				 int *err)
{
	u8 bits = device->controller_cfg->ram_x_address_bits;
	u16 max_val = (1U << bits) - 1;

	if (*err)
		return;

	if (x > max_val)
		x = max_val;

	if (bits <= 8) {
		ssd16xx_send_data(device, (u8)x, err);
	} else {
		u8 hi_mask = (1U << (bits - 8)) - 1;

		ssd16xx_send_data(device, x & 0xFF, err);
		ssd16xx_send_data(device, (x >> 8) & hi_mask, err);
	}
}

static void ssd16xx_send_y_param(struct ssd16xx_device *device, u16 y,
				 int *err)
{
	u8 bits = device->controller_cfg->ram_y_address_bits;
	u16 max_val = (1U << bits) - 1;

	if (*err)
		return;

	if (y > max_val)
		y = max_val;

	if (bits <= 8) {
		ssd16xx_send_data(device, (u8)y, err);
	} else {
		u8 hi_mask = (1U << (bits - 8)) - 1;

		ssd16xx_send_data(device, y & 0xFF, err);
		ssd16xx_send_data(device, (y >> 8) & hi_mask, err);
	}
}

static void ssd16xx_send_data_bulk(struct ssd16xx_device *device,
				   const u8 *data, size_t len,
				   int *err)
{
	struct spi_transfer xfer = {};
	struct spi_message msg;

	if (*err)
		return;

	if (!data || !len)
		return;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	if (device->dc) {
		/* 4-wire SPI: D/C# GPIO high selects data mode */
		if (len <= sizeof(device->spi_buf)) {
			memcpy(device->spi_buf, data, len);
			xfer.tx_buf = device->spi_buf;
		} else {
			xfer.tx_buf = data;
		}
		xfer.len = len;
		gpiod_set_value_cansleep(device->dc, 1);
		ssd16xx_spi_sync(device->spi, &msg, err);
	} else {
		/* 3-wire (9-bit): expand u8 → u16 with D/C#=1 in bit 8. */
		size_t i;
		u16 *buf = device->tx_buf9;

		for (i = 0; i < len; i++)
			buf[i] = 0x100 | data[i];

		xfer.tx_buf = buf;
		xfer.len = len * sizeof(u16);
		xfer.bits_per_word = 9;
		ssd16xx_spi_sync(device->spi, &msg, err);
	}
}

static void ssd16xx_display_update(struct ssd16xx_device *device,
				   u8 ctrl1_byte1, u8 ctrl1_byte2, u8 ctrl2_mode,
				   int *err)
{
	if (*err)
		return;

	drm_dbg(&device->drm,
		"display_update: Setting ctrl1=0x%02x,0x%02x mode=0x%02x\n",
		ctrl1_byte1, ctrl1_byte2, ctrl2_mode);

	ssd16xx_send_cmd(device, SSD16XX_CMD_DISPLAY_UPDATE_CONTROL1, err);
	ssd16xx_send_data(device, ctrl1_byte1, err);
	ssd16xx_send_data(device, ctrl1_byte2, err);

	ssd16xx_send_cmd(device, SSD16XX_CMD_DISPLAY_UPDATE_CONTROL2, err);
	ssd16xx_send_data(device, ctrl2_mode, err);
	ssd16xx_send_cmd(device, SSD16XX_CMD_MASTER_ACTIVATION, err);

	drm_dbg(&device->drm,
		"display_update: Master activation sent, waiting...\n");

	ssd16xx_wait_for_device(device, err);
}

static void ssd16xx_hw_reset(struct ssd16xx_device *device)
{
	gpiod_set_value_cansleep(device->reset, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(device->reset, 0);
	usleep_range(10000, 11000);
}

/* Record which LUT a refresh of the given mode leaves loaded. */
static void ssd16xx_track_lut(struct ssd16xx_device *device,
			      enum ssd16xx_refresh_mode mode)
{
	if (mode == SSD16XX_REFRESH_FULL)
		device->fast_lut_loaded = true;
	else if (mode == SSD16XX_REFRESH_PARTIAL)
		device->fast_lut_loaded = false;
}

/*
 * Fast refresh (0xC7) uses the full-refresh LUT but does not load it itself.
 * fast_lut_loaded tracks whether it is still loaded: a reset or a partial
 * update drops it, a full update loads it as well.
 */
static int ssd16xx_preload_fast_lut(struct ssd16xx_device *device)
{
	int err = 0;

	if (device->fast_lut_loaded)
		return 0;

	ssd16xx_send_cmd(device, SSD16XX_CMD_DISPLAY_UPDATE_CONTROL1, &err);
	ssd16xx_send_data(device, device->controller_cfg->ctrl1_bypass_red_ram, &err);
	ssd16xx_send_data(device, SSD16XX_CTRL1_BYTE2_DEFAULT, &err);

	ssd16xx_send_cmd(device, SSD16XX_CMD_DISPLAY_UPDATE_CONTROL2, &err);
	ssd16xx_send_data(device, device->controller_cfg->ctrl2_load_temp_lut, &err);

	ssd16xx_send_cmd(device, SSD16XX_CMD_MASTER_ACTIVATION, &err);
	ssd16xx_wait_for_device(device, &err);

	device->fast_lut_loaded = !err;
	return err;
}

static int ssd16xx_hw_init(struct ssd16xx_device *device)
{
	int err = 0;
	u8 data_entry_mode;

	ssd16xx_hw_reset(device);

	ssd16xx_send_cmd(device, SSD16XX_CMD_SW_RESET, &err);
	ssd16xx_wait_for_device(device, &err);

	if (device->controller_cfg->has_temp_sensor_ctrl) {
		ssd16xx_send_cmd(device, SSD1683_CMD_TEMPERATURE_SENSOR_CONTROL, &err);
		ssd16xx_send_data(device, SSD1683_TEMP_SENSOR_INTERNAL, &err);
	}

	if (device->device_cfg->booster_soft_start_data) {
		ssd16xx_send_cmd(device, SSD16XX_CMD_BOOSTER_SOFT_START, &err);
		ssd16xx_send_data_bulk(device,
				       device->device_cfg->booster_soft_start_data,
				       device->device_cfg->booster_soft_start_len,
				       &err);
	}

	/* MUX ratio is the panel's gate line count, not the controller's. */
	ssd16xx_send_cmd(device, SSD16XX_CMD_DRIVER_OUTPUT_CONTROL, &err);
	ssd16xx_send_y_param(device, device->height - 1, &err);
	ssd16xx_send_data(device, device->device_cfg->driver_output_ctrl_byte3, &err);

	/* The reset dropped any fast LUT; it is loaded again on first use. */
	device->fast_lut_loaded = false;

	/* The RAM window and cursor are programmed by fb_dirty. */
	switch (device->orientation) {
	case 90:
	case 180:
		data_entry_mode = SSD16XX_DATA_ENTRY_XDEC_YDEC;
		break;
	default: /* 0°/270° */
		data_entry_mode = SSD16XX_DATA_ENTRY_XINC_YINC;
		break;
	}

	ssd16xx_send_cmd(device, SSD16XX_CMD_DATA_ENTRY_MODE, &err);
	ssd16xx_send_data(device, data_entry_mode, &err);
	drm_dbg(&device->drm, "hw_init: orientation=%u° data_entry=0x%02x\n",
		device->orientation, data_entry_mode);

	ssd16xx_wait_for_device(device, &err);

	if (err)
		drm_err(&device->drm, "Hardware initialization failed: %d\n", err);

	return err;
}

/*
 * Decrementing data entry (90°/180°) takes the window end-before-start and
 * starts the cursor at (max, max).
 */
static void ssd16xx_set_full_ram_window(struct ssd16xx_device *device, int *err)
{
	u16 x_end, y_end = device->height - 1;
	bool dec = device->orientation == 90 || device->orientation == 180;

	if (device->controller_cfg->ram_x_address_bits == 8)
		x_end = DIV_ROUND_UP(device->width, 8) - 1;
	else
		x_end = device->width - 1;

	ssd16xx_send_cmd(device, SSD16XX_CMD_SET_RAM_X_ADDRESS_START_END, err);
	ssd16xx_send_x_param(device, dec ? x_end : 0, err);
	ssd16xx_send_x_param(device, dec ? 0 : x_end, err);

	ssd16xx_send_cmd(device, SSD16XX_CMD_SET_RAM_Y_ADDRESS_START_END, err);
	ssd16xx_send_y_param(device, dec ? y_end : 0, err);
	ssd16xx_send_y_param(device, dec ? 0 : y_end, err);

	ssd16xx_send_cmd(device, SSD16XX_CMD_SET_RAM_X_ADDRESS_COUNTER, err);
	ssd16xx_send_x_param(device, dec ? x_end : 0, err);

	ssd16xx_send_cmd(device, SSD16XX_CMD_SET_RAM_Y_ADDRESS_COUNTER, err);
	ssd16xx_send_y_param(device, dec ? y_end : 0, err);
}

/* Clear to white, following ssd16xx_fb_dirty()'s RAM protocol for @mode. */
static int ssd16xx_clear_display(struct ssd16xx_device *device,
				 enum ssd16xx_refresh_mode mode)
{
	const u8 *bw_tbl = device->controller_cfg->border_waveform_table;
	const struct ssd16xx_controller_config *cfg = device->controller_cfg;
	unsigned int data_size = DIV_ROUND_UP(device->width, 8) * device->height;
	bool three_color = device->color_mode == SSD16XX_COLOR_MODE_3COLOR &&
			   device->tx_red_buf;
	u8 *white_buffer = device->tx_buf;
	u8 *red_buffer = white_buffer;
	int err = 0;

	if (mode >= ARRAY_SIZE(cfg->ctrl2_refresh))
		mode = SSD16XX_REFRESH_FULL;

	/*
	 * Partial needs RED RAM to hold the shown frame: not the case in
	 * 3-colour mode (red plane) or after a RAM-losing deep sleep.
	 */
	if ((three_color || device->ram_lost) && mode == SSD16XX_REFRESH_PARTIAL)
		mode = SSD16XX_REFRESH_FULL;

	if (mode == SSD16XX_REFRESH_FAST)
		err = ssd16xx_preload_fast_lut(device);

	memset(white_buffer, 0xFF, data_size);
	if (three_color) {
		red_buffer = device->tx_red_buf;
		memset(red_buffer, 0x00, data_size);
	}

	ssd16xx_set_full_ram_window(device, &err);
	ssd16xx_send_cmd(device, SSD16XX_CMD_WRITE_RAM_BW, &err);
	ssd16xx_send_data_bulk(device, white_buffer, data_size, &err);

	if (mode != SSD16XX_REFRESH_PARTIAL) {
		ssd16xx_set_full_ram_window(device, &err);
		ssd16xx_send_cmd(device, SSD1683_CMD_WRITE_RAM_RED, &err);
		ssd16xx_send_data_bulk(device, red_buffer, data_size, &err);
	}

	drm_dbg(&device->drm, "clear_display: mode=%d border=0x%02x\n",
		mode, bw_tbl[device->border_waveform_init_idx]);
	ssd16xx_send_cmd(device, SSD16XX_CMD_BORDER_WAVEFORM_CONTROL, &err);
	ssd16xx_send_data(device, bw_tbl[device->border_waveform_init_idx], &err);

	ssd16xx_display_update(device,
			       three_color || mode == SSD16XX_REFRESH_PARTIAL ?
					cfg->ctrl1_normal : cfg->ctrl1_bypass_red_ram,
			       SSD16XX_CTRL1_BYTE2_DEFAULT,
			       cfg->ctrl2_refresh[mode], &err);
	ssd16xx_track_lut(device, mode);

	/* RED RAM becomes the baseline for the next partial update. */
	if (mode == SSD16XX_REFRESH_PARTIAL) {
		ssd16xx_set_full_ram_window(device, &err);
		ssd16xx_send_cmd(device, SSD1683_CMD_WRITE_RAM_RED, &err);
		ssd16xx_send_data_bulk(device, red_buffer, data_size, &err);
	}

	ssd16xx_send_cmd(device, SSD16XX_CMD_BORDER_WAVEFORM_CONTROL, &err);
	ssd16xx_send_data(device, bw_tbl[device->border_waveform_update_idx], &err);

	if (!err)
		device->ram_lost = false;

	return err;
}

/* Clear once per master session; master_set/master_drop re-arm it. */
static int ssd16xx_clear_display_on_init(struct ssd16xx_device *device)
{
	int ret;

	if (device->clear_on_init < 0 || device->first_clear_done)
		return 0;

	drm_dbg(&device->drm, "clear_on_init: running, mode=%d\n",
		device->clear_on_init);
	ret = ssd16xx_clear_display(device, device->clear_on_init);
	if (ret)
		return ret;

	device->first_clear_done = true;
	return 0;
}

static int ssd16xx_clear_display_on_exit(struct ssd16xx_device *device)
{
	if (device->clear_on_close < 0)
		return 0;

	drm_dbg(&device->drm, "clear_on_close: running, mode=%d\n",
		device->clear_on_close);
	return ssd16xx_clear_display(device, device->clear_on_close);
}

/* ITU-R BT.601 luminance (0-255) of an XRGB8888 pixel. */
static u8 ssd16xx_pixel_luma(struct iosys_map *src,
			     struct drm_framebuffer *fb,
			     unsigned int x, unsigned int y)
{
	u32 *line = (u32 *)(src->vaddr + y * fb->pitches[0]);
	u32 px = le32_to_cpup(&line[x]);
	u8 r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;

	return (u8)((299u * r + 587u * g + 114u * b) / 1000u);
}

/* R1: the bit value; XRGB8888: luminance above 127. */
static bool ssd16xx_pixel_is_white(struct iosys_map *src,
				   struct drm_framebuffer *fb,
				   unsigned int x, unsigned int y)
{
	if (fb->format->format == DRM_FORMAT_R1) {
		u8 *line = (u8 *)(src->vaddr + y * fb->pitches[0]);

		return !!(line[x / 8] & (1 << (7 - (x % 8))));
	}
	return ssd16xx_pixel_luma(src, fb, x, y) > 127;
}

/* XRGB8888 pixel whose red channel is above half and dominant. */
static bool ssd16xx_pixel_is_red(struct iosys_map *src,
				 struct drm_framebuffer *fb,
				 unsigned int x, unsigned int y)
{
	u32 *line;
	u32 px;
	u8 r, g, b;

	if (fb->format->format != DRM_FORMAT_XRGB8888)
		return false;

	line = (u32 *)(src->vaddr + y * fb->pitches[0]);
	px = le32_to_cpup(&line[x]);
	r = (px >> 16) & 0xFF;
	g = (px >> 8) & 0xFF;
	b = px & 0xFF;

	return r > 127 && r > g && r > b;
}

/*
 * ssd16xx_convert_fb() - pack a framebuffer into controller RAM layout.
 * @bw_dst:  black/white RAM plane (1=white, 0=black)
 * @red_dst: red RAM plane (1=red, 0=no red), or NULL for black/white output
 *
 * Supports XRGB8888 (thresholded via ITU-R BT.601 luma at 127) and R1.
 *
 * Output layout:
 *   0°/180°  landscape: row-major, left-to-right, top-to-bottom
 *   90°/270° portrait:  column-major, rightmost column first
 * Each row is padded to a byte boundary.
 *
 * With @red_dst, XRGB8888 pixels with a dominant red channel map to red ink,
 * and R1 bits are taken as red ink on a white background.
 */
static void ssd16xx_convert_fb(u8 *bw_dst, u8 *red_dst, struct iosys_map *src,
			       struct drm_framebuffer *fb, struct drm_rect *rect,
			       unsigned int orientation, bool x_mirror)
{
	bool portrait = orientation == 90 || orientation == 270;
	bool r1 = fb->format->format == DRM_FORMAT_R1;
	unsigned int outer_len = portrait ? drm_rect_width(rect) : drm_rect_height(rect);
	unsigned int inner_len = portrait ? drm_rect_height(rect) : drm_rect_width(rect);
	unsigned int i, j, x, y;
	u8 bw_byte = 0, red_byte = 0;
	unsigned int bit_pos = 0;
	unsigned int dst_idx = 0;

	bool r1_fast = r1 && !red_dst && orientation == 0 && rect->x1 % 8 == 0 &&
		       !x_mirror;

	drm_dbg(fb->dev,
		"convert_fb: fmt=%p4cc rect=(%d,%d)-(%d,%d) orient=%u° path=%s\n",
		&fb->format->format, rect->x1, rect->y1, rect->x2, rect->y2,
		orientation, r1_fast ? "R1-fast" : red_dst ? "3color" :
		portrait ? "portrait" : "landscape");

	/* R1 to black/white at 0° with byte-aligned rows is a plain copy. */
	if (r1_fast) {
		unsigned int width_bytes = DIV_ROUND_UP(drm_rect_width(rect), 8);

		for (y = rect->y1; y < rect->y2; y++) {
			memcpy(bw_dst + dst_idx,
			       src->vaddr + y * fb->pitches[0] + rect->x1 / 8,
			       width_bytes);
			dst_idx += width_bytes;
		}
		return;
	}

	for (i = 0; i < outer_len; i++) {
		for (j = 0; j < inner_len; j++) {
			/* The inner loop walks RAM X; reverse it for x_mirror. */
			unsigned int k = x_mirror ? inner_len - 1 - j : j;

			x = portrait ? rect->x2 - 1 - i : rect->x1 + k;
			y = portrait ? rect->y1 + k : rect->y1 + i;

			if (red_dst && r1) {
				/* Set R1 bits are red ink (RED=1, BW=0), clear ones white. */
				if (ssd16xx_pixel_is_white(src, fb, x, y))
					red_byte |= BIT(7 - bit_pos);
				else
					bw_byte |= BIT(7 - bit_pos);
			} else if (red_dst && ssd16xx_pixel_is_red(src, fb, x, y)) {
				red_byte |= BIT(7 - bit_pos);
			} else if (ssd16xx_pixel_is_white(src, fb, x, y)) {
				bw_byte |= BIT(7 - bit_pos);
			}

			if (++bit_pos == 8 || j == inner_len - 1) {
				bw_dst[dst_idx] = bw_byte;
				if (red_dst)
					red_dst[dst_idx] = red_byte;
				dst_idx++;
				bw_byte = 0;
				red_byte = 0;
				bit_pos = 0;
			}
		}
	}
}

static int ssd16xx_fb_dirty(struct drm_framebuffer *fb,
			    const struct drm_plane_state *plane_state,
			    struct ssd16xx_device *device,
			    const struct iosys_map *src_map)
{
	const u8 *ctrl2_tbl = device->controller_cfg->ctrl2_refresh;
	struct iosys_map map = *src_map;
	int err = 0;
	unsigned int data_size = DIV_ROUND_UP(device->width, 8) * device->height;
	u8 *mono_buffer = NULL;
	u8 *red_buffer = NULL;
	struct drm_rect src = drm_plane_state_src(plane_state);
	struct drm_rect full, *rect = &full;

	/*
	 * Always send the full frame.  Walk the plane's source rectangle, which
	 * atomic_check has constrained to the mode size, so the packed output
	 * never exceeds tx_buf even if the framebuffer itself is larger.
	 */
	drm_rect_fp_to_int(&full, &src);

	drm_dbg(&device->drm,
		"fb_dirty: fb=%dx%d, refresh_mode=%d, orientation=%d\n",
		fb->width, fb->height, device->refresh_mode, device->orientation);

	mono_buffer = device->tx_buf;
	memset(mono_buffer, 0, data_size);

	/* 3-colour FULL/FAST: populate red channel. */
	if (device->color_mode == SSD16XX_COLOR_MODE_3COLOR &&
	    (device->refresh_mode == SSD16XX_REFRESH_FULL ||
	     device->refresh_mode == SSD16XX_REFRESH_FAST)) {
		red_buffer = device->tx_red_buf;
		memset(red_buffer, 0, data_size);
	}

	/*
	 * In 3-colour mode R1 bits are red ink on white; otherwise they are
	 * luma (1 = white, 0 = black).
	 */
	ssd16xx_convert_fb(mono_buffer, red_buffer, &map, fb, rect, device->orientation,
			   device->device_cfg->x_mirror);

	drm_dbg(&device->drm,
		"fb_dirty: mono[0..3]=0x%02x 0x%02x 0x%02x 0x%02x (data_size=%u)\n",
		mono_buffer[0], mono_buffer[1], mono_buffer[2], mono_buffer[3],
		data_size);

	ssd16xx_set_full_ram_window(device, &err);

	ssd16xx_send_cmd(device, SSD16XX_CMD_WRITE_RAM_BW, &err);
	ssd16xx_send_data_bulk(device, mono_buffer, data_size, &err);

	drm_dbg(&device->drm,
		"fb_dirty: border check: every_update=%d init_pending=%d border_pending=%d idx=%d hw=0x%02x\n",
		device->border_refresh_on_every_update, device->init_refresh_pending,
		device->border_waveform_pending, device->border_waveform_update_idx,
		device->controller_cfg->border_waveform_table[device->border_waveform_update_idx]);
	if (device->border_refresh_on_every_update || device->init_refresh_pending ||
	    device->border_waveform_pending) {
		u8 idx = device->border_waveform_update_idx;
		u8 border = device->controller_cfg->border_waveform_table[idx];

		drm_dbg(&device->drm, "fb_dirty: Sending border waveform: 0x%02x\n",
			border);
		ssd16xx_send_cmd(device, SSD16XX_CMD_BORDER_WAVEFORM_CONTROL, &err);
		ssd16xx_send_data(device, border, &err);
		device->border_waveform_pending = false;
	}

	switch (device->refresh_mode) {
	case SSD16XX_REFRESH_FULL:
		/*
		 * Write RED RAM before the update: writing it after BUSY clears
		 * corrupts it on some controller revisions.  In black/white mode
		 * it is bypassed, and holds the baseline for a later partial.
		 */
		ssd16xx_send_cmd(device, SSD1683_CMD_WRITE_RAM_RED, &err);
		if (red_buffer) {
			ssd16xx_send_data_bulk(device, red_buffer, data_size, &err);
			ssd16xx_display_update(device, device->controller_cfg->ctrl1_normal,
					       SSD16XX_CTRL1_BYTE2_DEFAULT,
					       ctrl2_tbl[SSD16XX_REFRESH_FULL], &err);
		} else {
			ssd16xx_send_data_bulk(device, mono_buffer, data_size, &err);
			ssd16xx_display_update(device, device->controller_cfg->ctrl1_bypass_red_ram,
					       SSD16XX_CTRL1_BYTE2_DEFAULT,
					       ctrl2_tbl[SSD16XX_REFRESH_FULL], &err);
		}
		break;
	case SSD16XX_REFRESH_FAST:
		/* Same RAM handling as FULL, with the pre-loaded LUT. */
		ssd16xx_send_cmd(device, SSD1683_CMD_WRITE_RAM_RED, &err);
		if (red_buffer) {
			ssd16xx_send_data_bulk(device, red_buffer, data_size, &err);
			ssd16xx_display_update(device, device->controller_cfg->ctrl1_normal,
					       SSD16XX_CTRL1_BYTE2_DEFAULT,
					       ctrl2_tbl[SSD16XX_REFRESH_FAST], &err);
		} else {
			ssd16xx_send_data_bulk(device, mono_buffer, data_size, &err);
			ssd16xx_display_update(device, device->controller_cfg->ctrl1_bypass_red_ram,
					       SSD16XX_CTRL1_BYTE2_DEFAULT,
					       ctrl2_tbl[SSD16XX_REFRESH_FAST], &err);
		}
		break;
	case SSD16XX_REFRESH_PARTIAL:
	default:
		/*
		 * The controller drives the transition from RED RAM (shown
		 * frame) to BW RAM (new frame); RED RAM then becomes the new
		 * baseline.
		 */
		drm_dbg(&device->drm,
			"fb_dirty: partial pre-update: mono[0]=0x%02x (BW=new, RED=prev)\n",
			mono_buffer[0]);
		ssd16xx_display_update(device, device->controller_cfg->ctrl1_normal,
				       SSD16XX_CTRL1_BYTE2_DEFAULT,
				       ctrl2_tbl[SSD16XX_REFRESH_PARTIAL], &err);
		ssd16xx_send_cmd(device, SSD1683_CMD_WRITE_RAM_RED, &err);
		ssd16xx_send_data_bulk(device, mono_buffer, data_size, &err);
		drm_dbg(&device->drm,
			"fb_dirty: partial post-update: wrote RED baseline mono[0]=0x%02x\n",
			mono_buffer[0]);
		break;
	}
	ssd16xx_track_lut(device, device->refresh_mode);

	return err;
}

/* -----------------------------------------------------------------------------
 * Plane Functions
 */

static void ssd16xx_plane_destroy(struct drm_plane *plane)
{
	drm_plane_cleanup(plane);
}

static struct drm_plane_state *ssd16xx_plane_atomic_create_state(struct drm_plane *plane)
{
	struct ssd16xx_device *device = plane_to_ssd16xx_device(plane);
	struct ssd16xx_plane_state *ssd_state;

	ssd_state = kzalloc_obj(*ssd_state, GFP_KERNEL);
	if (!ssd_state)
		return ERR_PTR(-ENOMEM);

	__drm_gem_shadow_plane_state_init(plane, &ssd_state->base);
	ssd_state->refresh_mode = device->refresh_mode;
	ssd_state->color_mode = device->color_mode;
	ssd_state->clear_on_init = device->clear_on_init;
	ssd_state->clear_on_close = device->clear_on_close;
	ssd_state->clear_on_disable = device->clear_on_disable;
	ssd_state->refresh_mode_init = device->refresh_mode_init;

	return &ssd_state->base.base;
}

static struct drm_plane_state *
ssd16xx_plane_atomic_duplicate_state(struct drm_plane *plane)
{
	struct ssd16xx_plane_state *old_ssd_state, *ssd_state;

	if (WARN_ON(!plane->state))
		return NULL;

	old_ssd_state = to_ssd16xx_plane_state(plane->state);
	ssd_state = kmemdup(old_ssd_state, sizeof(*ssd_state), GFP_KERNEL);
	if (!ssd_state)
		return NULL;

	__drm_gem_duplicate_shadow_plane_state(plane, &ssd_state->base);
	return &ssd_state->base.base;
}

static void ssd16xx_plane_atomic_destroy_state(struct drm_plane *plane,
					       struct drm_plane_state *state)
{
	struct ssd16xx_plane_state *ssd_state = to_ssd16xx_plane_state(state);

	__drm_gem_destroy_shadow_plane_state(&ssd_state->base);
	kfree(ssd_state);
}

/* Internal -1 (disabled) / refresh mode <-> enum ssd16xx_session_mode */
static inline unsigned int ssd16xx_internal_to_session_prop(int internal_val)
{
	if (internal_val < 0)
		return SSD16XX_SESSION_DISABLED;
	return (unsigned int)internal_val + 1;
}

static inline int ssd16xx_session_prop_to_internal(uint64_t prop_val)
{
	if (prop_val == SSD16XX_SESSION_DISABLED)
		return -1;
	return (int)prop_val - 1;
}

static int ssd16xx_plane_atomic_get_property(struct drm_plane *plane,
					     const struct drm_plane_state *state,
					     struct drm_property *property,
					     uint64_t *val)
{
	struct ssd16xx_device *device = plane_to_ssd16xx_device(plane);
	const struct ssd16xx_plane_state *ssd_state = to_ssd16xx_plane_state(state);

	if (property == device->refresh_mode_property) {
		*val = ssd_state->refresh_mode;
		return 0;
	}
	if (property == device->color_mode_property) {
		*val = ssd_state->color_mode;
		return 0;
	}
	if (property == device->clear_on_init_property) {
		*val = ssd16xx_internal_to_session_prop(ssd_state->clear_on_init);
		return 0;
	}
	if (property == device->clear_on_close_property) {
		*val = ssd16xx_internal_to_session_prop(ssd_state->clear_on_close);
		return 0;
	}
	if (property == device->clear_on_disable_property) {
		*val = ssd16xx_internal_to_session_prop(ssd_state->clear_on_disable);
		return 0;
	}
	if (property == device->refresh_mode_init_property) {
		*val = ssd16xx_internal_to_session_prop(ssd_state->refresh_mode_init);
		return 0;
	}
	return -EINVAL;
}

static int ssd16xx_plane_atomic_set_property(struct drm_plane *plane,
					     struct drm_plane_state *state,
					     struct drm_property *property,
					     uint64_t val)
{
	struct ssd16xx_device *device = plane_to_ssd16xx_device(plane);
	struct ssd16xx_plane_state *ssd_state = to_ssd16xx_plane_state(state);

	if (property == device->refresh_mode_property) {
		if (val > SSD16XX_REFRESH_FAST)
			return -EINVAL;
		ssd_state->refresh_mode = val;
		return 0;
	}
	if (property == device->color_mode_property) {
		if (val > SSD16XX_COLOR_MODE_3COLOR)
			return -EINVAL;
		ssd_state->color_mode = val;
		return 0;
	}
	if (property == device->clear_on_init_property) {
		if (val > SSD16XX_SESSION_FAST)
			return -EINVAL;
		ssd_state->clear_on_init = ssd16xx_session_prop_to_internal(val);
		return 0;
	}
	if (property == device->clear_on_close_property) {
		if (val > SSD16XX_SESSION_FAST)
			return -EINVAL;
		ssd_state->clear_on_close = ssd16xx_session_prop_to_internal(val);
		return 0;
	}
	if (property == device->clear_on_disable_property) {
		if (val > SSD16XX_SESSION_FAST)
			return -EINVAL;
		ssd_state->clear_on_disable = ssd16xx_session_prop_to_internal(val);
		return 0;
	}
	if (property == device->refresh_mode_init_property) {
		if (val > SSD16XX_SESSION_FAST)
			return -EINVAL;
		ssd_state->refresh_mode_init = ssd16xx_session_prop_to_internal(val);
		return 0;
	}
	return -EINVAL;
}

static const struct drm_plane_funcs ssd16xx_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = ssd16xx_plane_destroy,
	.atomic_create_state = ssd16xx_plane_atomic_create_state,
	.atomic_duplicate_state = ssd16xx_plane_atomic_duplicate_state,
	.atomic_destroy_state = ssd16xx_plane_atomic_destroy_state,
	.atomic_get_property = ssd16xx_plane_atomic_get_property,
	.atomic_set_property = ssd16xx_plane_atomic_set_property,
};

static int ssd16xx_plane_atomic_check(struct drm_plane *plane,
				      struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_plane_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct ssd16xx_plane_state *ssd_state =
		to_ssd16xx_plane_state(new_plane_state);
	struct drm_crtc_state *crtc_state;

	if (!new_plane_state->crtc)
		return 0;

	if (ssd_state->color_mode == SSD16XX_COLOR_MODE_3COLOR &&
	    ssd_state->refresh_mode == SSD16XX_REFRESH_PARTIAL) {
		drm_dbg_atomic(plane->dev,
			       "3-color mode does not support partial refresh\n");
		return -EINVAL;
	}

	crtc_state = drm_atomic_get_new_crtc_state(state, new_plane_state->crtc);

	return drm_atomic_helper_check_plane_state(new_plane_state, crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING,
						   false, false);
}

/*
 * Copy the session properties of a plane state into the device.  Called at
 * the start of every commit callback that uses them, before any early exit,
 * so a property-only commit or one that disables the CRTC is not missed.
 */
static void ssd16xx_sync_session_props(struct ssd16xx_device *device,
				       const struct ssd16xx_plane_state *ssd_state)
{
	lockdep_assert_held(&device->lock);

	device->refresh_mode = ssd_state->refresh_mode;
	device->color_mode = ssd_state->color_mode;
	device->clear_on_init = ssd_state->clear_on_init;
	device->clear_on_close = ssd_state->clear_on_close;
	device->clear_on_disable = ssd_state->clear_on_disable;

	/*
	 * Arm the init frame from the new master's own refresh_mode_init, as
	 * the CRTC may already be enabled and atomic_enable then does not run.
	 * Follow a later change too, so a disabled one never leaves it pending.
	 */
	device->refresh_mode_init = ssd_state->refresh_mode_init;
	if (device->refresh_mode_init < 0)
		device->init_refresh_pending = false;
	else if (device->new_session)
		device->init_refresh_pending = true;
	device->new_session = false;
}

static void ssd16xx_plane_atomic_update(struct drm_plane *plane,
					struct drm_atomic_commit *state)
{
	struct drm_plane_state *old_state = drm_atomic_get_old_plane_state(state, plane);
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow_state = to_drm_shadow_plane_state(new_state);
	struct ssd16xx_device *device = plane_to_ssd16xx_device(plane);
	struct ssd16xx_plane_state *new_ssd = to_ssd16xx_plane_state(new_state);
	enum ssd16xx_refresh_mode saved_mode;
	u8 saved_border_waveform_idx;
	struct drm_framebuffer *fb = new_state->fb;
	struct drm_rect rect;
	int ret, idx;

	if (!drm_dev_enter(&device->drm, &idx))
		return;

	drm_dbg(&device->drm, "plane_atomic_update: fb=%p\n", fb);

	scoped_guard(mutex, &device->lock)
		ssd16xx_sync_session_props(device, new_ssd);

	if (!fb)
		goto out_exit;

	/* Nothing to draw: don't wake the panel up. */
	if (!drm_atomic_helper_damage_merged(old_state, new_state, &rect))
		goto out_exit;

	ret = pm_runtime_resume_and_get(device->drm.dev);
	if (ret < 0) {
		drm_err(&device->drm, "plane_atomic_update: failed to resume: %d\n", ret);
		goto out_exit;
	}

	drm_dbg(&device->drm, "plane_atomic_update: calling fb_dirty rect=(%d,%d)-(%d,%d)\n",
		rect.x1, rect.y1, rect.x2, rect.y2);

	mutex_lock(&device->lock);

	/* Cleared by a BUSY timeout while the device stayed active. */
	if (!device->initialized) {
		ret = ssd16xx_hw_init(device);
		if (ret) {
			drm_err(&device->drm, "plane_atomic_update: hw_init failed: %d\n", ret);
			goto out_unlock;
		}
		device->initialized = true;
		/* The reset restored the border waveform register to its default. */
		device->border_waveform_pending = true;
	}

	/*
	 * A new master may start drawing on a CRTC that is already enabled
	 * (e.g. left on by fbdev), in which case atomic_enable did not run.
	 */
	ret = ssd16xx_clear_display_on_init(device);
	if (ret)
		drm_err(&device->drm, "plane_atomic_update: clear on init failed: %d\n", ret);

	/*
	 * When refresh_mode_init was set, use the specified mode for this first
	 * frame only, then restore the user-configured refresh_mode so
	 * subsequent updates continue with the configured mode.
	 */
	saved_mode = device->refresh_mode;
	saved_border_waveform_idx = device->border_waveform_update_idx;
	if (device->init_refresh_pending) {
		device->refresh_mode = device->refresh_mode_init;
		device->border_waveform_update_idx = device->border_waveform_init_idx;
	}
	/* RED RAM no longer holds the shown frame. */
	if (device->ram_lost && device->refresh_mode == SSD16XX_REFRESH_PARTIAL)
		device->refresh_mode = SSD16XX_REFRESH_FULL;

	/* Fast refresh (0xC7) does not load a LUT itself. */
	if (device->refresh_mode == SSD16XX_REFRESH_FAST) {
		ret = ssd16xx_preload_fast_lut(device);
		if (ret)
			drm_err(&device->drm,
				"plane_atomic_update: fast LUT preload failed: %d\n", ret);
	}

	ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (ret) {
		drm_err(&device->drm, "plane_atomic_update: begin_cpu_access failed: %d\n", ret);
		goto out_restore;
	}

	ret = ssd16xx_fb_dirty(fb, new_state, device, &shadow_state->data[0]);
	if (ret)
		drm_err(&device->drm, "plane_atomic_update: display update failed: %d\n", ret);
	else
		device->ram_lost = false;

	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);

out_restore:
	device->refresh_mode = saved_mode;
	device->border_waveform_update_idx = saved_border_waveform_idx;

	/*
	 * The init frame used the init border waveform: restore the update one
	 * next time.  On failure keep it pending, partial refresh needs the
	 * baseline it establishes.
	 */
	if (device->init_refresh_pending && !ret) {
		device->init_refresh_pending = false;
		device->border_waveform_pending = true;
	}

out_unlock:
	mutex_unlock(&device->lock);

	pm_runtime_mark_last_busy(device->drm.dev);
	pm_runtime_put_autosuspend(device->drm.dev);

out_exit:
	drm_dev_exit(idx);
}

static const struct drm_plane_helper_funcs ssd16xx_plane_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = ssd16xx_plane_atomic_check,
	.atomic_update = ssd16xx_plane_atomic_update,
};

/* -----------------------------------------------------------------------------
 * CRTC Functions
 */

static const struct drm_crtc_funcs ssd16xx_crtc_funcs = {
	.atomic_create_state = drm_atomic_helper_crtc_create_state,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static enum drm_mode_status ssd16xx_crtc_mode_valid(struct drm_crtc *crtc,
						    const struct drm_display_mode *mode)
{
	struct ssd16xx_device *device = crtc_to_ssd16xx_device(crtc);

	return drm_crtc_helper_mode_valid_fixed(crtc, mode, &device->mode);
}

static int ssd16xx_crtc_atomic_check(struct drm_crtc *crtc,
				     struct drm_atomic_commit *state)
{
	struct drm_crtc_state *new_crtc_state = drm_atomic_get_new_crtc_state(state, crtc);

	if (!new_crtc_state->enable)
		return 0;

	return drm_atomic_helper_check_crtc_primary_plane(new_crtc_state);
}

static void ssd16xx_crtc_atomic_disable(struct drm_crtc *crtc,
					struct drm_atomic_commit *state)
{
	struct ssd16xx_device *device = crtc_to_ssd16xx_device(crtc);
	struct drm_plane_state *new_plane_state =
		drm_atomic_get_new_plane_state(state, &device->primary_plane);
	bool clear;
	int ret, idx;

	if (!drm_dev_enter(&device->drm, &idx))
		return;

	/*
	 * Planes of a disabled CRTC are not updated, so pick up session
	 * properties set in this commit here.
	 */
	scoped_guard(mutex, &device->lock) {
		if (new_plane_state)
			ssd16xx_sync_session_props(device,
						   to_ssd16xx_plane_state(new_plane_state));
		clear = device->clear_on_disable >= 0 && !device->cleared_on_disable;
	}

	if (!clear)
		goto out_exit;

	ret = pm_runtime_resume_and_get(device->drm.dev);
	if (ret < 0)
		goto out_exit;

	scoped_guard(mutex, &device->lock) {
		drm_dbg(&device->drm, "clear_on_disable: running, mode=%d\n",
			device->clear_on_disable);
		ret = ssd16xx_clear_display(device, device->clear_on_disable);
		if (ret)
			drm_err(&device->drm, "atomic_disable: clear failed: %d\n", ret);
		else
			device->cleared_on_disable = true;
	}

	if (ret)
		pm_runtime_put_autosuspend(device->drm.dev);
	else
		pm_runtime_put_sync_suspend(device->drm.dev);
out_exit:
	drm_dev_exit(idx);
}

static void ssd16xx_crtc_atomic_enable(struct drm_crtc *crtc,
				       struct drm_atomic_commit *state)
{
	struct ssd16xx_device *device = crtc_to_ssd16xx_device(crtc);
	struct drm_plane_state *new_plane_state =
		drm_atomic_get_new_plane_state(state, &device->primary_plane);
	int ret, idx;

	if (!drm_dev_enter(&device->drm, &idx))
		return;

	/*
	 * Pick up this commit's session properties before the resume below,
	 * whose callback runs hw_init.
	 */
	if (new_plane_state) {
		scoped_guard(mutex, &device->lock)
			ssd16xx_sync_session_props(device,
						   to_ssd16xx_plane_state(new_plane_state));
	}

	ret = pm_runtime_resume_and_get(device->drm.dev);
	if (ret < 0) {
		drm_err(&device->drm, "crtc_atomic_enable: failed to resume: %d\n", ret);
		drm_dev_exit(idx);
		return;
	}

	drm_dbg(&device->drm, "atomic_enable: %dx%d orientation=%u°\n",
		device->width, device->height, device->orientation);

	/* Serialise hw_init and session state against master_drop(). */
	mutex_lock(&device->lock);

	if (!device->initialized) {
		ret = ssd16xx_hw_init(device);
		if (ret) {
			drm_err(&device->drm, "atomic_enable: hw_init failed: %d\n", ret);
			mutex_unlock(&device->lock);
			pm_runtime_put_autosuspend(device->drm.dev);
			drm_dev_exit(idx);
			return;
		}
		device->initialized = true;
	}

	device->cleared_on_disable = false;

	ret = ssd16xx_clear_display_on_init(device);
	if (ret)
		drm_err(&device->drm, "crtc_atomic_enable: clear on init failed: %d\n", ret);

	if (device->refresh_mode_init >= 0) {
		drm_dbg(&device->drm,
			"atomic_enable: refresh_mode_init=%d, using for first frame\n",
			device->refresh_mode_init);
		device->init_refresh_pending = true;
	}

	mutex_unlock(&device->lock);

	pm_runtime_mark_last_busy(device->drm.dev);
	pm_runtime_put_autosuspend(device->drm.dev);
	drm_dev_exit(idx);
}

static const struct drm_crtc_helper_funcs ssd16xx_crtc_helper_funcs = {
	.mode_valid     = ssd16xx_crtc_mode_valid,
	.atomic_check   = ssd16xx_crtc_atomic_check,
	.atomic_disable = ssd16xx_crtc_atomic_disable,
	.atomic_enable  = ssd16xx_crtc_atomic_enable,
};

/* -----------------------------------------------------------------------------
 * Connector Functions
 */

static int ssd16xx_connector_get_modes(struct drm_connector *connector)
{
	struct ssd16xx_device *device = to_ssd16xx_device(connector->dev);

	return drm_connector_helper_get_modes_fixed(connector, &device->mode);
}

static const struct drm_connector_helper_funcs ssd16xx_connector_helper_funcs = {
	.get_modes = ssd16xx_connector_get_modes,
};

static const struct drm_prop_enum_list ssd16xx_refresh_mode_enum[] = {
	{ SSD16XX_REFRESH_PARTIAL, "partial" },
	{ SSD16XX_REFRESH_FULL,    "full"    },
	{ SSD16XX_REFRESH_FAST,    "fast"    },
};

static const struct drm_prop_enum_list ssd16xx_color_mode_enum[] = {
	{ SSD16XX_COLOR_MODE_BW,     "black-white" },
	{ SSD16XX_COLOR_MODE_3COLOR, "3-color"     },
};

static const struct drm_prop_enum_list ssd16xx_session_mode_enum[] = {
	{ SSD16XX_SESSION_DISABLED, "disabled" },
	{ SSD16XX_SESSION_PARTIAL,  "partial"  },
	{ SSD16XX_SESSION_FULL,     "full"     },
	{ SSD16XX_SESSION_FAST,     "fast"     },
};

static int ssd16xx_plane_create_properties(struct ssd16xx_device *device)
{
	struct drm_device *drm = &device->drm;
	struct drm_mode_object *plane_obj = &device->primary_plane.base;

	device->refresh_mode_property =
		drm_property_create_enum(drm, 0, "refresh_mode",
					 ssd16xx_refresh_mode_enum,
					 ARRAY_SIZE(ssd16xx_refresh_mode_enum));
	if (!device->refresh_mode_property)
		return -ENOMEM;
	drm_object_attach_property(plane_obj,
				   device->refresh_mode_property,
				   device->refresh_mode);

	/* No single-value property on black/white panels. */
	if (device->device_cfg->red_supported) {
		device->color_mode_property =
			drm_property_create_enum(drm, 0, "color_mode",
						 ssd16xx_color_mode_enum,
						 ARRAY_SIZE(ssd16xx_color_mode_enum));
		if (!device->color_mode_property)
			return -ENOMEM;
		drm_object_attach_property(plane_obj,
					   device->color_mode_property,
					   device->color_mode);
	}

	device->clear_on_init_property =
		drm_property_create_enum(drm, 0, "clear_on_init",
					 ssd16xx_session_mode_enum,
					 ARRAY_SIZE(ssd16xx_session_mode_enum));
	if (!device->clear_on_init_property)
		return -ENOMEM;
	drm_object_attach_property(plane_obj,
				   device->clear_on_init_property,
				   ssd16xx_internal_to_session_prop(device->clear_on_init));

	device->clear_on_close_property =
		drm_property_create_enum(drm, 0, "clear_on_close",
					 ssd16xx_session_mode_enum,
					 ARRAY_SIZE(ssd16xx_session_mode_enum));
	if (!device->clear_on_close_property)
		return -ENOMEM;
	drm_object_attach_property(plane_obj,
				   device->clear_on_close_property,
				   ssd16xx_internal_to_session_prop(device->clear_on_close));

	device->clear_on_disable_property =
		drm_property_create_enum(drm, 0, "clear_on_disable",
					 ssd16xx_session_mode_enum,
					 ARRAY_SIZE(ssd16xx_session_mode_enum));
	if (!device->clear_on_disable_property)
		return -ENOMEM;
	drm_object_attach_property(plane_obj,
				   device->clear_on_disable_property,
				   ssd16xx_internal_to_session_prop(device->clear_on_disable));

	device->refresh_mode_init_property =
		drm_property_create_enum(drm, 0, "refresh_mode_init",
					 ssd16xx_session_mode_enum,
					 ARRAY_SIZE(ssd16xx_session_mode_enum));
	if (!device->refresh_mode_init_property)
		return -ENOMEM;
	drm_object_attach_property(plane_obj,
				   device->refresh_mode_init_property,
				   ssd16xx_internal_to_session_prop(device->refresh_mode_init));

	return 0;
}

static const struct drm_connector_funcs ssd16xx_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const u32 ssd16xx_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_R1,
};

DEFINE_DRM_GEM_FOPS(ssd16xx_fops);

/* Each new master starts with refresh_mode_init, if set. */
static void ssd16xx_drm_master_set(struct drm_device *drm,
				   struct drm_file *file, bool from_open)
{
	struct ssd16xx_device *device = to_ssd16xx_device(drm);

	guard(mutex)(&device->lock);

	device->cleared_on_close = false;
	device->first_clear_done = false;
	device->new_session = true;
}

static void ssd16xx_drm_master_drop(struct drm_device *drm,
				    struct drm_file *file)
{
	struct ssd16xx_device *device = to_ssd16xx_device(drm);
	bool clear;
	int ret, idx;

	/*
	 * Not serialized against nonblocking commits by the DRM core (e.g. on
	 * DROP_MASTER), so the session state and the panel clear are taken
	 * under device->lock, in the same drm_dev_enter -> runtime PM -> lock
	 * order as the commit callbacks.
	 */
	if (!drm_dev_enter(drm, &idx))
		return;

	scoped_guard(mutex, &device->lock) {
		device->init_refresh_pending = false;
		device->first_clear_done = false;
		clear = device->clear_on_close >= 0 && !device->cleared_on_close &&
			!device->cleared_on_disable;
	}

	if (!clear || pm_runtime_resume_and_get(drm->dev) < 0)
		goto out_exit;

	scoped_guard(mutex, &device->lock) {
		ret = ssd16xx_clear_display_on_exit(device);
		if (ret)
			drm_err(drm, "master_drop: clear on close failed: %d\n", ret);
		device->cleared_on_close = true;
	}

	/* Sleep right after the clear rather than after the autosuspend delay. */
	pm_runtime_put_sync_suspend(drm->dev);
out_exit:
	drm_dev_exit(idx);
}

static struct drm_driver ssd16xx_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &ssd16xx_fops,
	.name = "ssd16xx",
	.desc = "DRM driver for SSD16xx e-paper controller family",
	.major = 1,
	.minor = 0,
	.master_set  = ssd16xx_drm_master_set,
	.master_drop = ssd16xx_drm_master_drop,
	DRM_GEM_SHMEM_DRIVER_OPS,
	DRM_FBDEV_SHMEM_DRIVER_OPS,
};

static const struct drm_mode_config_funcs ssd16xx_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/* Enable the CRTC (and initialise the controller) before updating planes. */
static const struct drm_mode_config_helper_funcs ssd16xx_mode_config_helper_funcs = {
	.atomic_commit_tail = drm_atomic_helper_commit_tail_rpm,
};

static int ssd16xx_alloc_tx_bufs(struct ssd16xx_device *device)
{
	/* Rows are byte padded: size for both landscape and portrait packing. */
	size_t frame_size = max(DIV_ROUND_UP(device->width, 8) * device->height,
				DIV_ROUND_UP(device->height, 8) * device->width);

	device->tx_buf = drmm_kmalloc(&device->drm, frame_size, GFP_KERNEL);
	if (!device->tx_buf)
		return -ENOMEM;

	if (device->device_cfg->red_supported) {
		device->tx_red_buf = drmm_kmalloc(&device->drm, frame_size, GFP_KERNEL);
		if (!device->tx_red_buf)
			return -ENOMEM;
	}

	if (!device->dc) {
		device->tx_buf9 = drmm_kmalloc_array(&device->drm, frame_size,
						     sizeof(u16), GFP_KERNEL);
		if (!device->tx_buf9)
			return -ENOMEM;
	}

	return 0;
}

static int ssd16xx_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct ssd16xx_device *device;
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

	device = devm_drm_dev_alloc(dev, &ssd16xx_drm_driver,
				    struct ssd16xx_device, drm);
	if (IS_ERR(device))
		return PTR_ERR(device);

	drm = &device->drm;
	ret = drmm_mutex_init(drm, &device->lock);
	if (ret)
		return ret;

	device->spi = spi;
	device->model = model;
	spi_set_drvdata(spi, device);

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
		device->controller = SSD1683;
		break;
	default:
		drm_err(drm, "Unknown panel model: %d\n", model);
		return -EINVAL;
	}

	if (device->controller >= ARRAY_SIZE(ssd16xx_controller_configs) ||
	    !ssd16xx_controller_configs[device->controller].max_width)
		return -EINVAL;
	device->controller_cfg = &ssd16xx_controller_configs[device->controller];

	if (model >= ARRAY_SIZE(ssd16xx_device_configs))
		return -EINVAL;
	device->device_cfg = &ssd16xx_device_configs[model];

	mode = &device->mode;
	drm_mode_copy(mode, device->device_cfg->mode);

	device->refresh_mode = device->device_cfg->default_refresh_mode;
	device->color_mode   = device->device_cfg->default_color_mode;
	device->border_waveform_init_idx   = device->device_cfg->default_border_waveform_init;
	device->border_waveform_update_idx = device->device_cfg->default_border_waveform_update;
	device->border_refresh_on_every_update =
		device->device_cfg->default_border_refresh_on_every_update;
	device->clear_on_init    = device->device_cfg->default_clear_on_init;
	device->clear_on_close   = device->device_cfg->default_clear_on_close;
	device->clear_on_disable = device->device_cfg->default_clear_on_disable;
	device->refresh_mode_init = device->device_cfg->default_refresh_mode_init;

	device_property_read_u32(dev, "rotation", &dt_rotation);
	if (dt_rotation != 0 && dt_rotation != 90 && dt_rotation != 180 && dt_rotation != 270) {
		drm_warn(drm, "Invalid DT rotation %u, defaulting to 0°\n", dt_rotation);
		dt_rotation = 0;
	}
	device->orientation = dt_rotation;

	device->width = mode->hdisplay;
	device->height = mode->vdisplay;

	drm_dbg(drm, "Using %s orientation (%u°, physical %ux%u)\n",
		(device->orientation == 90 || device->orientation == 270) ?
		 "portrait" : "landscape", device->orientation, device->width, device->height);

	/* Swap mode dimensions for portrait so clients see logical size. */
	if (device->orientation == 90 || device->orientation == 270) {
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

	if (device->width > device->controller_cfg->max_width ||
	    device->height > device->controller_cfg->max_height) {
		drm_err(drm, "panel %ux%u exceeds controller max %ux%u\n",
			device->width, device->height,
			device->controller_cfg->max_width,
			device->controller_cfg->max_height);
		return -EINVAL;
	}

	device->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(device->reset))
		return dev_err_probe(dev, PTR_ERR(device->reset), "Failed to get RESET GPIO\n");

	device->busy = devm_gpiod_get(dev, "busy", GPIOD_IN);
	if (IS_ERR(device->busy))
		return dev_err_probe(dev, PTR_ERR(device->busy), "Failed to get BUSY GPIO\n");

	device->dc = devm_gpiod_get_optional(dev, "dc", GPIOD_OUT_LOW);
	if (IS_ERR(device->dc))
		return dev_err_probe(dev, PTR_ERR(device->dc), "Failed to get DC GPIO\n");
	if (!device->dc) {
		if (!spi_is_bpw_supported(spi, 9))
			return dev_err_probe(dev, -EINVAL,
					     "3-wire SPI mode requires 9-bit word support\n");
		drm_dbg(drm, "dc-gpios not specified, using 3-wire (9-bit) SPI mode\n");
	}

	ret = ssd16xx_alloc_tx_bufs(device);
	if (ret)
		return ret;

	ssd16xx_hw_reset(device);

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.funcs = &ssd16xx_mode_config_funcs;
	drm->mode_config.helper_private = &ssd16xx_mode_config_helper_funcs;
	drm->mode_config.min_width = mode->hdisplay;
	drm->mode_config.max_width = mode->hdisplay;
	drm->mode_config.min_height = mode->vdisplay;
	drm->mode_config.max_height = mode->vdisplay;

	drm_connector_helper_add(&device->connector, &ssd16xx_connector_helper_funcs);
	ret = drm_connector_init(drm, &device->connector, &ssd16xx_connector_funcs,
				 DRM_MODE_CONNECTOR_SPI);
	if (ret)
		return ret;

	ret = drm_universal_plane_init(drm, &device->primary_plane, 0,
				       &ssd16xx_plane_funcs,
				       ssd16xx_formats, ARRAY_SIZE(ssd16xx_formats),
				       NULL, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&device->primary_plane, &ssd16xx_plane_helper_funcs);
	drm_plane_enable_fb_damage_clips(&device->primary_plane);

	ret = drm_crtc_init_with_planes(drm, &device->crtc, &device->primary_plane,
					NULL, &ssd16xx_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&device->crtc, &ssd16xx_crtc_helper_funcs);

	ret = drmm_encoder_init(drm, &device->encoder, NULL, DRM_MODE_ENCODER_NONE, NULL);
	if (ret)
		return ret;
	device->encoder.possible_crtcs = drm_crtc_mask(&device->crtc);

	ret = drm_connector_attach_encoder(&device->connector, &device->encoder);
	if (ret)
		return ret;

	ret = ssd16xx_plane_create_properties(device);
	if (ret)
		return ret;
	drm_mode_config_reset(drm);

	device->deep_sleep_mode = device->controller_cfg->deep_sleep_mode_level1;

	/*
	 * Start suspended: the first user's resume runs hw_init.  This also
	 * clears a runtime_error left on the SPI device by a previous load.
	 */
	pm_runtime_set_autosuspend_delay(dev, SSD16XX_PM_AUTOSUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_suspended(dev);
	pm_runtime_enable(dev);

	ret = drm_dev_register(drm, 0);
	if (ret) {
		pm_runtime_disable(dev);
		pm_runtime_dont_use_autosuspend(dev);
		return ret;
	}

	drm_dbg(drm, "SSD16xx e-paper display initialized (%dx%d, %d° rotation)\n",
		device->width, device->height, device->orientation);

	drm_client_setup(drm, NULL);

	return 0;
}

static void ssd16xx_remove(struct spi_device *spi)
{
	struct ssd16xx_device *device = spi_get_drvdata(spi);

	drm_dev_unplug(&device->drm);
	drm_atomic_helper_shutdown(&device->drm);
	pm_runtime_dont_use_autosuspend(&spi->dev);
	/*
	 * Put the panel to sleep regardless of clear_on_disable, which can make
	 * atomic_disable skip touching the hardware.
	 */
	pm_runtime_force_suspend(&spi->dev);
}

static void ssd16xx_shutdown(struct spi_device *spi)
{
	struct ssd16xx_device *device = spi_get_drvdata(spi);

	drm_atomic_helper_shutdown(&device->drm);
	pm_runtime_dont_use_autosuspend(&spi->dev);
	pm_runtime_force_suspend(&spi->dev);
}

static int ssd16xx_pm_suspend(struct device *dev)
{
	struct ssd16xx_device *device = dev_get_drvdata(dev);
	const struct ssd16xx_controller_config *cfg = device->controller_cfg;
	struct drm_device *drm = &device->drm;
	int ret;

	ret = drm_mode_config_helper_suspend(drm);
	if (ret)
		return ret;

	/* Not before: the pipeline suspend above may sleep at level1. */
	device->deep_sleep_mode = cfg->deep_sleep_mode_level2;
	dev_dbg(dev, "system suspend: entering deep sleep mode 0x%02x\n",
		device->deep_sleep_mode);

	/*
	 * Already runtime suspended at level1, so pm_runtime_force_suspend()
	 * would skip the callback: wake the panel (only a reset exits deep
	 * sleep) and enter level2 directly.
	 */
	if (pm_runtime_status_suspended(dev)) {
		if (cfg->deep_sleep_mode_level1 != cfg->deep_sleep_mode_level2) {
			ssd16xx_hw_reset(device);
			ssd16xx_wait_for_device(device, &ret);
			ssd16xx_send_cmd(device, SSD16XX_CMD_DEEP_SLEEP_MODE, &ret);
			ssd16xx_send_data(device, cfg->deep_sleep_mode_level2, &ret);
			if (ret)
				goto err_resume;
		}
		device->initialized = false;
		device->ram_lost = true;
		device->pm_force_suspended = false;
		return 0;
	}

	ret = pm_runtime_force_suspend(dev);
	if (ret)
		goto err_resume;
	device->pm_force_suspended = true;

	return 0;

err_resume:
	device->deep_sleep_mode = cfg->deep_sleep_mode_level1;
	drm_mode_config_helper_resume(drm);
	return ret;
}

static int ssd16xx_pm_resume(struct device *dev)
{
	struct ssd16xx_device *device = dev_get_drvdata(dev);
	struct drm_device *drm = &device->drm;

	dev_dbg(dev, "system resume: restoring state\n");

	if (device->pm_force_suspended) {
		pm_runtime_force_resume(dev);
		device->pm_force_suspended = false;
	}

	device->deep_sleep_mode = device->controller_cfg->deep_sleep_mode_level1;

	return drm_mode_config_helper_resume(drm);
}

static int ssd16xx_pm_runtime_suspend(struct device *dev)
{
	struct ssd16xx_device *device = dev_get_drvdata(dev);
	int err = 0;

	dev_dbg(dev, "runtime suspend: entering deep sleep mode 0x%02x\n",
		device->deep_sleep_mode);

	ssd16xx_send_cmd(device, SSD16XX_CMD_DEEP_SLEEP_MODE, &err);
	ssd16xx_send_data(device, device->deep_sleep_mode, &err);
	device->initialized = false;
	if (device->deep_sleep_mode != SSD1683_DEEP_SLEEP_MODE_1)
		device->ram_lost = true;

	/*
	 * Don't propagate SPI errors: a failure here would latch runtime_error
	 * and block all future resumes; the next resume resets the panel anyway.
	 */
	if (err)
		dev_warn(dev, "failed to enter deep sleep: %d\n", err);

	return 0;
}

static int ssd16xx_pm_runtime_resume(struct device *dev)
{
	struct ssd16xx_device *device = dev_get_drvdata(dev);
	int ret;

	dev_dbg(dev, "runtime resume: initialized=%d%s\n",
		device->initialized,
		!device->initialized ? " (running hw_init)" : "");

	if (!device->initialized) {
		ret = ssd16xx_hw_init(device);
		if (ret)
			return ret;

		device->initialized = true;
		/* The reset restored the border waveform register to its default. */
		device->border_waveform_pending = true;
	}

	return 0;
}

static const struct dev_pm_ops ssd16xx_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(ssd16xx_pm_suspend, ssd16xx_pm_resume)
	RUNTIME_PM_OPS(ssd16xx_pm_runtime_suspend, ssd16xx_pm_runtime_resume, NULL)
};

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
		.pm = pm_ptr(&ssd16xx_pm_ops),
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
