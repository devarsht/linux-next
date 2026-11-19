# Supporting E-Paper Displays in Linux

**What can be generalised in KMS, and what cannot**

Texas Instruments · 2026-11 · v2 (v1 was presented at the DisplayNext Hackfest, 2026-05)

---

## Contents

1. [What is an e-paper display?](#what-is-an-e-paper-display)
2. [Refresh modes (SSD16xx)](#refresh-modes-ssd16xx)
3. [Why it is application specific](#why-it-is-application-specific)
4. [Userspace landscape](#userspace-landscape)
5. [Refresh control beyond this driver](#refresh-control-beyond-this-driver)
6. [KMS property requirements](#kms-property-requirements)
7. [E-paper behaviour is use-case specific](#e-paper-behaviour-is-use-case-specific)
8. [Classification](#classification)
9. [Proposal: generic e-paper properties](#proposal-generic-e-paper-properties)

---

## What is an e-paper display?

- **Bistable**: the image stays on the panel without power.
- Pixels are moved by an electrophoretic waveform.
- An update takes **hundreds of milliseconds to seconds**.
- Too slow causes flicker; too fast causes ghosting.

LCD and OLED panels scan out every frame at 60 fps. E-paper drivers instead
have to **choose when to refresh and how aggressively**.

## Refresh modes (SSD16xx)

| Mode        | Time       | CTRL2  | Trade-off                             |
|-------------|------------|--------|---------------------------------------|
| **Partial** | 300–500 ms | `0xFF` | Fast, but ghosting accumulates        |
| **Full**    | 1.5–2 s    | `0xF7` | Clean image, but slow and flickers    |
| **Fast**    | 1.0–1.5 s  | `0xC7` | Reuses the full-refresh LUT without reloading it |

Each mode writes a different **Display Update Control 2** byte, which selects
the waveform sequence the controller runs.

### Partial (`0xFF`)

**Best for:** clocks, live data and frequent text updates.

- Drives the transition from RED RAM (the frame on screen) to BW RAM (the new frame).
- RED RAM is then rewritten as the baseline for the next partial update.
- **Needs a valid baseline.** After deep sleep 2 or a BUSY timeout it falls back to a full refresh.
- **Risk:** ghosting builds up over repeated partial refreshes.

```
Typical strategy:   N partial updates  →  1 full refresh to reset
```

### Full (`0xF7`) and Fast (`0xC7`)

**Full** suits documents, page turns and the first frame after boot. It loads
the temperature and the LUT, then runs the complete waveform, which clears
ghosting.

**Fast** suits slideshows and other low-latency updates.

- It uses the **same LUT as full** but does not load it itself.
- The driver tracks whether that LUT is still loaded and preloads it when needed.
- A reset or a partial update (Mode 2 LUT) drops the LUT; a full update reloads it.

```c
/* v4: track the LUT in the controller, not a one-shot flag */
if (mode == FAST && !fast_lut_loaded)
        preload_lut();          /* CTRL2 0xB1, no display update */
```

## Why it is application specific

| Application   | Needs                                       |
|---------------|---------------------------------------------|
| Digital clock | Partial every second                        |
| E-reader      | Full on every page turn                     |
| Stock ticker  | Fast for quick updates, full periodically   |
| Kiosk         | Full on startup, partial during the session |
| Slideshow     | Fast for each slide                         |

**The driver cannot see what the application intends.** The refresh mode is
a per-update policy, so userspace has to control it.

## Userspace landscape

- **No display server or compositor targets e-paper today.**
- Users write **standalone applications** that drive the panel directly through KMS:
  - signage, shelf labels, badges, dashboards and kiosk screens.
- Each update picks a refresh mode based on what it draws.
- Our use cases are built on **LVGL**, an embedded GUI library with a DRM
  backend. Other frameworks such as Slint are also being explored.

> **Open question for upstream:** is an LVGL-based application an acceptable
> open-source user? Support for the generic properties can also be added to
> LVGL's DRM backend.

## Refresh control beyond this driver

The upstream e-paper drivers expose no refresh choice to userspace. Several out-of-tree
drivers offer more than one refresh mode.

### In the kernel today

| Driver | Status | Refresh modes | Exposed to userspace? |
|--------|--------|---------------|-----------------------|
| `drm/tiny/repaper.c` [[1]](#src-1) | Upstream | One update sequence. The first frame after enable clears the panel to white, later frames update from the previous image | No |
| `drm/tiny/pixpaper.c` [[2]](#src-2) | Upstream | One display refresh command | No |
| `fbdev/broadsheetfb.c`, `fbdev/metronomefb.c` [[3]](#src-3) | Upstream (legacy fbdev) | Waveform stored in SPI flash (broadsheet) or loaded as firmware (metronome) | No |
| `drm/solomon/ssd16xx.c` [[4]](#src-4) | Under review (v3) | Partial, full, fast | Driver-private plane properties |

### Outside the kernel

✅ = offered by the driver. — = not offered in the source checked. For grey-level
controllers the waveform names are given with the description from the source.

| Driver | Status | Partial | Full | Fast | Other modes | How userspace chooses |
|--------|--------|:-------:|:----:|:----:|-------------|-----------------------|
| Waveshare 2.13" V4, SSD1680 [[5]](#src-5) | Vendor userspace driver | ✅ | ✅ | ✅ | — | One function per mode |
| Waveshare 4.2" V2, SSD1683 [[5]](#src-5) | Vendor userspace driver | ✅ | ✅ | ✅ | 4-grey | One function per mode |
| Waveshare 2.9" V2 [[5]](#src-5) | Vendor userspace driver | ✅ | ✅ | ✅ | 4-grey | One function per mode |
| Waveshare 7.5" V2, UC8179 [[5]](#src-5) | Vendor userspace driver | ✅ | ✅ | ✅ | 4-grey | One function per mode |
| Waveshare 7.3" E, 6-colour [[5]](#src-5) | Vendor userspace driver | — | ✅ | — | — | One mode only |
| GxEPD2 [[6]](#src-6) | Community library (Arduino) | ✅ | ✅ | — | — | A bool argument; each panel driver declares `hasPartialUpdate` |
| Waveshare IT8951 HAT [[7]](#src-7) | Vendor userspace driver | — | GC16 ("display 16 grayscale image") | A2 ("fast refresh without flash") | INIT ("for every init or some time after A2 mode refresh") | A mode code passed with each area update |
| `rockchip_ebc`, Samuel Holland's branch [[8]](#src-8) | Out-of-tree DRM driver, not merged | ✅ (every frame) | ✅ (only at reset and on CRTC disable) | — | Waveforms RESET, A2, DU, DU4, GC16, GCC16, GL16, GLR16, GLD16 | None per frame; one `default_waveform` module parameter |
| `rockchip_ebc`, PineNote community tree [[9]](#src-9) | Out-of-tree DRM driver, not merged | ✅ (every frame) | ✅ (at probe, on a private ioctl, or automatically after enough partial updates) | — | Same waveforms | None per frame; `default_waveform` / `refresh_waveform` module parameters and a private `GLOBAL_REFRESH` ioctl |
| NXP i.MX EPDC [[10]](#src-10) | Downstream vendor kernel (fbdev) | ✅ (`UPDATE_MODE_PARTIAL`) | ✅ (`UPDATE_MODE_FULL`) | — | A waveform number per update; the app maps INIT, DU, GC4, GC8, GC16 and GC32 to numbers. `GLR16`, `GLD16` and `AUTO` are defined | Per update, through a private fbdev ioctl |

For `rockchip_ebc`, "full" is the driver's global refresh, which redraws the whole
panel (`rockchip_ebc_global_refresh()`); every other frame uses
`rockchip_ebc_partial_refresh()`. Neither tree lets userspace pick the refresh mode for
a given frame. The PineNote tree also has an `OFF_SCREEN` ioctl, which sets the image
drawn when the display is disabled or suspended.

Without a common uAPI, each of these uses its own interface. A generic
`epaper_refresh` property, as proposed below, would give DRM drivers one standard way.

### Sources

All were read on 2026-11-19, at the commits linked below. Line numbers refer to
those commits.

1. <a id="src-1"></a>`drivers/gpu/drm/tiny/repaper.c` at drm-misc-next `c88084027d0e`:
   ["repaper can't do partial updates"](https://gitlab.freedesktop.org/drm/misc/kernel/-/blob/c88084027d0e922dd925b96e5a0d184ba04a8f43/drivers/gpu/drm/tiny/repaper.c#L529) and the
   `partial` / `cleared` first-frame logic
   ([L555–581](https://gitlab.freedesktop.org/drm/misc/kernel/-/blob/c88084027d0e922dd925b96e5a0d184ba04a8f43/drivers/gpu/drm/tiny/repaper.c#L555-581)).
2. <a id="src-2"></a>`drivers/gpu/drm/tiny/pixpaper.c` at `c88084027d0e`:
   [`PIXPAPER_CMD_DISPLAY_REFRESH`](https://gitlab.freedesktop.org/drm/misc/kernel/-/blob/c88084027d0e922dd925b96e5a0d184ba04a8f43/drivers/gpu/drm/tiny/pixpaper.c#L46), sent at
   [L915](https://gitlab.freedesktop.org/drm/misc/kernel/-/blob/c88084027d0e922dd925b96e5a0d184ba04a8f43/drivers/gpu/drm/tiny/pixpaper.c#L915).
3. <a id="src-3"></a>At `c88084027d0e`:
   [`broadsheet_store_waveform_to_spiflash()`](https://gitlab.freedesktop.org/drm/misc/kernel/-/blob/c88084027d0e922dd925b96e5a0d184ba04a8f43/drivers/video/fbdev/broadsheetfb.c#L715)
   in `broadsheetfb.c`, and
   [`request_firmware(..., "metronome.wbf", ...)`](https://gitlab.freedesktop.org/drm/misc/kernel/-/blob/c88084027d0e922dd925b96e5a0d184ba04a8f43/drivers/video/fbdev/metronomefb.c#L623)
   in `metronomefb.c`.
4. <a id="src-4"></a>ssd16xx v3 series, message-id `20261019213726.4145532-1-devarsht@ti.com`.
   <https://lore.kernel.org/all/20261019213726.4145532-1-devarsht@ti.com/>
5. <a id="src-5"></a>Waveshare e-Paper at `d28b3c4cfc78`, `RaspberryPi_JetsonNano/python/lib/waveshare_epd/`:
   - [`epd2in13_V4.py`](https://github.com/waveshareteam/e-Paper/blob/d28b3c4cfc78eef1c6cd6e6e3013654e449d2864/RaspberryPi_JetsonNano/python/lib/waveshare_epd/epd2in13_V4.py#L268-L287): `display()`, `display_fast()`,
     `displayPartial()`, sending CTRL2 [`0xF7`, `0xC7`, `0xFF`](https://github.com/waveshareteam/e-Paper/blob/d28b3c4cfc78eef1c6cd6e6e3013654e449d2864/RaspberryPi_JetsonNano/python/lib/waveshare_epd/epd2in13_V4.py#L104-L126)
   - [`epd4in2_V2.py`](https://github.com/waveshareteam/e-Paper/blob/d28b3c4cfc78eef1c6cd6e6e3013654e449d2864/RaspberryPi_JetsonNano/python/lib/waveshare_epd/epd4in2_V2.py#L396-L446): `display()`, `display_Fast()`,
     `display_Partial()`, `display_4Gray()`
   - [`epd2in9_V2.py`](https://github.com/waveshareteam/e-Paper/blob/d28b3c4cfc78eef1c6cd6e6e3013654e449d2864/RaspberryPi_JetsonNano/python/lib/waveshare_epd/epd2in9_V2.py#L259): `init_Fast()`, plus `display()`,
     `display_Partial()` and `display_4Gray()`
   - [`epd7in5_V2.py`](https://github.com/waveshareteam/e-Paper/blob/d28b3c4cfc78eef1c6cd6e6e3013654e449d2864/RaspberryPi_JetsonNano/python/lib/waveshare_epd/epd7in5_V2.py#L95-L205): `init()`, `init_fast()`,
     `init_part()`, `init_4Gray()`
   - [`epd7in3e.py`](https://github.com/waveshareteam/e-Paper/blob/d28b3c4cfc78eef1c6cd6e6e3013654e449d2864/RaspberryPi_JetsonNano/python/lib/waveshare_epd/epd7in3e.py#L107-L221): only `init()`, `display()`, `Clear()`
     and `sleep()`
6. <a id="src-6"></a>GxEPD2 by Jean-Marc Zingg at `de82887e77a7`:
   [`hasPartialUpdate`, `hasFastPartialUpdate`](https://github.com/ZinggJM/GxEPD2/blob/de82887e77a78528ea386e68bba2cb0291d2c319/src/GxEPD2_EPD.h#L31-L32) and
   [`refresh(bool partial_update_mode)`](https://github.com/ZinggJM/GxEPD2/blob/de82887e77a78528ea386e68bba2cb0291d2c319/src/GxEPD2_EPD.h#L88) in `src/GxEPD2_EPD.h`;
   panel list in [`README.md`](https://github.com/ZinggJM/GxEPD2/blob/de82887e77a78528ea386e68bba2cb0291d2c319/README.md).
7. <a id="src-7"></a>Waveshare IT8951-ePaper at `86406933d8f2`, `Raspberry/lib/e-Paper/`:
   mode descriptions in [`EPD_IT8951.h`](https://github.com/waveshareteam/IT8951-ePaper/blob/86406933d8f22af9fd3f2152b4958610c054b9a8/Raspberry/lib/e-Paper/EPD_IT8951.h#L39-L44), values
   [`INIT_Mode = 0`, `GC16_Mode = 2`, `A2_Mode = 6`](https://github.com/waveshareteam/IT8951-ePaper/blob/86406933d8f22af9fd3f2152b4958610c054b9a8/Raspberry/lib/e-Paper/EPD_IT8951.c#L35-L38) in
   `EPD_IT8951.c`, and the per-area
   [`EPD_IT8951_1bp_Refresh(..., Mode, ...)`](https://github.com/waveshareteam/IT8951-ePaper/blob/86406933d8f22af9fd3f2152b4958610c054b9a8/Raspberry/lib/e-Paper/EPD_IT8951.h#L197).
8. <a id="src-8"></a>Samuel Holland's `rk35/ebc-drm-v5` branch at `e6d78cec7673`:
   - in `drivers/gpu/drm/rockchip/rockchip_ebc.c`: the
     [`default_waveform` module parameter](https://github.com/smaeul/linux/blob/e6d78cec7673f6b00d5b775b9ddc8a53c5c74be2/drivers/gpu/drm/rockchip/rockchip_ebc.c#L157-L158),
     [`rockchip_ebc_global_refresh()`](https://github.com/smaeul/linux/blob/e6d78cec7673f6b00d5b775b9ddc8a53c5c74be2/drivers/gpu/drm/rockchip/rockchip_ebc.c#L308) and
     [`rockchip_ebc_partial_refresh()`](https://github.com/smaeul/linux/blob/e6d78cec7673f6b00d5b775b9ddc8a53c5c74be2/drivers/gpu/drm/rockchip/rockchip_ebc.c#L475), and
     where each is used: [reset](https://github.com/smaeul/linux/blob/e6d78cec7673f6b00d5b775b9ddc8a53c5c74be2/drivers/gpu/drm/rockchip/rockchip_ebc.c#L734),
     [every frame](https://github.com/smaeul/linux/blob/e6d78cec7673f6b00d5b775b9ddc8a53c5c74be2/drivers/gpu/drm/rockchip/rockchip_ebc.c#L738),
     [CRTC disable](https://github.com/smaeul/linux/blob/e6d78cec7673f6b00d5b775b9ddc8a53c5c74be2/drivers/gpu/drm/rockchip/rockchip_ebc.c#L751)
   - [`enum drm_epd_waveform`](https://github.com/smaeul/linux/blob/e6d78cec7673f6b00d5b775b9ddc8a53c5c74be2/include/drm/drm_epd_helper.h#L17-L35) in
     `include/drm/drm_epd_helper.h`
9. <a id="src-9"></a>PineNote community kernel (GitHub `m-weigand/linux`), branch
   `branch_pinenote_6-6-22` at `11a849851a8d`:
   - [`DRM_IOCTL_ROCKCHIP_EBC_GLOBAL_REFRESH` and `OFF_SCREEN`](https://github.com/m-weigand/linux/blob/11a849851a8d31dd8f0eec003d45763dd3212c77/include/uapi/drm/rockchip_ebc_drm.h#L31-L32)
     in `include/uapi/drm/rockchip_ebc_drm.h`
   - in `drivers/gpu/drm/rockchip/rockchip_ebc.c`: the ioctl
     [sets `do_one_full_refresh`](https://github.com/m-weigand/linux/blob/11a849851a8d31dd8f0eec003d45763dd3212c77/drivers/gpu/drm/rockchip/rockchip_ebc.c#L300-L315);
     the refresh thread then runs a
     [global refresh with `refresh_waveform`](https://github.com/m-weigand/linux/blob/11a849851a8d31dd8f0eec003d45763dd3212c77/drivers/gpu/drm/rockchip/rockchip_ebc.c#L1433),
     and otherwise a
     [partial refresh with `default_waveform`](https://github.com/m-weigand/linux/blob/11a849851a8d31dd8f0eec003d45763dd3212c77/drivers/gpu/drm/rockchip/rockchip_ebc.c#L1436);
     the [`auto_refresh` threshold](https://github.com/m-weigand/linux/blob/11a849851a8d31dd8f0eec003d45763dd3212c77/drivers/gpu/drm/rockchip/rockchip_ebc.c#L1322-L1328);
     and the [`OFF_SCREEN` image drawn on disable](https://github.com/m-weigand/linux/blob/11a849851a8d31dd8f0eec003d45763dd3212c77/drivers/gpu/drm/rockchip/rockchip_ebc.c#L1457-L1472)
10. <a id="src-10"></a>NXP linux-imx, branch `lf-5.10.y` at `06213bb0115f`, in
    `include/uapi/linux/mxcfb.h`: [`UPDATE_MODE_PARTIAL` / `FULL`](https://github.com/nxp-imx/linux-imx/blob/06213bb0115f587f55ec1f45fac03544855f97a7/include/uapi/linux/mxcfb.h#L94-L95),
    the [`waveform_mode` and `update_mode` fields](https://github.com/nxp-imx/linux-imx/blob/06213bb0115f587f55ec1f45fac03544855f97a7/include/uapi/linux/mxcfb.h#L131-L134) of
    `struct mxcfb_update_data`, [`struct mxcfb_waveform_modes`](https://github.com/nxp-imx/linux-imx/blob/06213bb0115f587f55ec1f45fac03544855f97a7/include/uapi/linux/mxcfb.h#L153-L158),
    and the [`MXCFB_SEND_UPDATE`](https://github.com/nxp-imx/linux-imx/blob/06213bb0115f587f55ec1f45fac03544855f97a7/include/uapi/linux/mxcfb.h#L190) ioctl.

## KMS property requirements

> "KMS drivers might need to add extra properties to support new features.
> Each new property introduced in a driver needs to meet a few requirements"
>
> — [`Documentation/gpu/drm-kms.rst`](https://docs.kernel.org/gpu/drm-kms.html#requirements)

A new property must:

- be **standardised and documented**: its name, enum values, semantics and interactions
- have a **generic helper in the core** that registers it
- be **decoded by the core** into the object's state structure
- keep the **initial state** the same as the behaviour before the property existed
- come with an **IGT test** where reasonable, and an **open-source userspace** user

**So e-paper behaviour needs generic, documented properties, not driver-private ones.**

## E-paper behaviour is use-case specific

A bistable panel keeps the last image across sessions and power cycles.
Whether that is a feature or a liability depends entirely on the application.

| Behaviour                   | Use case A                                  | Use case B                                         |
|-----------------------------|---------------------------------------------|----------------------------------------------------|
| Refresh per update          | Clock: partial every second                 | E-reader: full on each page turn                   |
| First frame of a session    | Kiosk: full refresh to remove old ghosting  | Clock restart: partial, no visible flash           |
| Image when a new app starts | Patient room sign: blank for a new patient  | Shelf label: keep the old price until the new one is drawn |
| Image after the app exits   | Conference badge: keep the name visible     | Boarding-pass kiosk: erase passenger data          |
| Image on DPMS off           | Shelf label: keep the price                 | Meeting room sign: wipe the schedule               |
| Colour                      | Price tag: red for offers                   | Live dashboard: fast BW updates                    |

- The same panel and driver serve both use cases, and only the application knows which one it needs.
- A fixed driver or per-panel default cannot serve both, so these behaviours need properties.
- The kernel has to act on some of them: when a new DRM master opens, after the app exits or
  crashes, and on DPMS off. Production images often have no fbdev or console, so nothing
  else redraws the panel.

## Classification

Legend: ✅ generalise · ⚠️ candidate / maybe · ❌ no

Each row below gives two use cases on the same panel that need opposite
behaviour. A fixed driver or per-panel default can serve only one of them.

### Generic KMS properties: refresh policy

| Property            | Generalise?  | Example use cases (why it is use-case specific) |
|---------------------|--------------|-------------------------------------------------|
| `refresh_mode`      | ✅ Yes       | **Digital clock / live sensor data:** partial every second, no visible flash.<br>**E-reader:** full on each page turn for clean text.<br>**Slideshow / stock ticker:** fast for each update, full periodically.<br>The right mode depends on what each update draws, which only the app knows. A core enum; the driver declares the subset its panel supports |
| `refresh_mode_init` | ✅ Yes       | **Digital signage starting a new campaign:** full first frame to remove ghosting left by the previous app.<br>**Clock restarted by a watchdog:** partial first frame, so each restart does not flash the panel.<br>A new app cannot know what earlier sessions left on the panel. The init frame also uses the panel's init border waveform, which `refresh_mode` alone does not select |
| `color_mode`        | ⚠️ Candidate | **Shelf price tag (BWR panel):** 3-colour, red for offers; a slow refresh a few times a day is fine.<br>**Live dashboard on the same panel:** BW only, so partial and fast updates are possible.<br>Open point: a property, or a pixel format choice |

### Generic KMS properties: bistable session policy

| Property           | Generalise? | Example use cases (why it is use-case specific) |
|--------------------|-------------|-------------------------------------------------|
| `clear_on_init`    | ✅ Yes      | **Hospital patient room sign:** clean slate when a new DRM master opens for a new admission, so the previous patient's name and warnings never show.<br>**Shelf label:** keep the old price until the restarted app draws the new one, with no blank flash.<br>Enforced for any client, including generic ones that are not e-paper aware |
| `clear_on_close`   | ✅ Yes      | **Boarding-pass kiosk:** erase the passenger's name and booking when the session ends.<br>**Transit departure board:** erase stale departures once its daemon stops.<br>**Conference badge:** keep the attendee's name visible after the app exits, at zero power.<br>Runs on fd close, also when the app crashes or is killed. Without fbdev nothing else redraws the panel |
| `clear_on_disable` | ✅ Yes      | **Meeting room booking display:** wipe the schedule on DPMS off when the room session ends.<br>**E-reader or shelf label:** keep the last page or price on DPMS off, since the panel holds it without power.<br>No userspace runs at that point, so only the kernel can act |

Each value is `disabled` or the refresh mode used for the clear (`partial` / `full` / `fast`).
These apply to any bistable display. The defaults keep today's behaviour (no clear, the
panel's default refresh mode), and the per-panel driver config seeds them.

### Generalisable later

| Setting          | Generalise?            | Example use cases / open point |
|------------------|------------------------|--------------------------------|
| Dynamic rotation | ✅ Yes, reuse core `rotation` | **Handheld reader or rotating signage mount:** switch portrait/landscape at runtime.<br>**Fixed wall sign:** the DT mount rotation is enough.<br>ssd16xx already rotates while packing pixels into controller RAM, so the core plane `rotation` property (90/270) fits without a GPU. Open point: 90/270 swaps the framebuffer's width and height relative to the fixed mode |
| Border waveform (init / update / every update) | ⚠️ Maybe | **Panel in a visible bezel:** drive the border white on full refreshes so it matches the background.<br>**Clock doing partial updates every second:** leave the border floating, so it does not flicker on every update.<br>The SSD16xx border options (VCOM, LUT, fixed levels, HiZ) are controller specific. Whether other controllers can share a generic set has not been checked |

DT `rotation` describes how the panel is mounted and maps to the fixed mode.
It does not need a matching dynamic property; runtime rotation is the case above.

### Stays driver specific

| Setting                        | Generalise?     | Reasoning |
|--------------------------------|-----------------|-----------|
| RAM X mirror                   | ❌ No: hardware | Depends on panel wiring, so the panel compatible / DT describes it, not uAPI |
| Deep sleep level, LUT tracking | ❌ No: kernel   | Sleep with or without RAM retention is a generic idea, but it is a kernel PM decision; fast-LUT tracking is a controller internal |

## Proposal: generic e-paper properties

This grew out of the hackfest `drm_epaper_init()` sketch. To meet the core rules,
it now follows the pattern of existing standard properties.

### Where it belongs: the CRTC

An e-paper refresh runs one waveform over the whole panel, after all planes are
composed. The standard KMS properties split by what they describe (drm-misc-next):

| Object | Standard properties | Describe |
|--------|---------------------|----------|
| Plane | `FB_ID`, `CRTC_*` / `SRC_*`, `alpha`, `pixel blend mode`, `rotation`, `zpos`, `COLOR_ENCODING`, `COLOR_RANGE`, `FB_DAMAGE_CLIPS`, `SCALING_FILTER` | One layer: which buffer, where it goes, how it blends |
| CRTC | `ACTIVE`, `MODE_ID`, `VRR_ENABLED`, `DEGAMMA_LUT`, `CTM`, `GAMMA_LUT`, `BACKGROUND_COLOR`, `OUT_FENCE_PTR`, `SCALING_FILTER`, `SHARPNESS_STRENGTH` | The composed output frame, and how it is presented |
| Connector | `CRTC_ID`, `DPMS`, `link-status`, `max bpc`, `Colorspace`, `content type`, `scaling mode`, `non-desktop`, `panel_type` | The sink and its link |

- **CRTC (proposed).** The refresh mode applies to the composed frame, not to one
  layer. `VRR_ENABLED` is the closest precedent: a standard CRTC hint,
  [decoded by the core](https://gitlab.freedesktop.org/drm/misc/kernel/-/blob/c88084027d0e922dd925b96e5a0d184ba04a8f43/drivers/gpu/drm/drm_atomic_uapi.c#L421) into `crtc_state->vrr_enabled`, about how the output presents the next frames.
  The session properties fit too: `clear_on_disable` acts when the CRTC is disabled,
  and the first-frame mode and `clear_on_init` concern a new master's first commit to
  the output.
- **Not the plane.** With several planes, two planes could ask for different refresh
  modes in the same commit, and the driver would have to choose one or reject the
  commit. ssd16xx v3 used the plane because the driver has one plane.
- **Not the connector.** Connector properties describe the sink and its link, not how
  a frame is presented. ssd16xx v3 also first had these properties on the connector: a
  commit that changed only a connector property never reached the plane update, so the
  change was silently lost.

A CRTC property still arrives with each frame. Adding a plane to an atomic commit also
adds its CRTC's state (`drm_atomic_get_plane_state()` in `drivers/gpu/drm/drm_atomic.c`),
so the driver reads the mode from the new CRTC state in the same commit as `FB_ID`
([drm_atomic.c#L697](https://gitlab.freedesktop.org/drm/misc/kernel/-/blob/c88084027d0e922dd925b96e5a0d184ba04a8f43/drivers/gpu/drm/drm_atomic.c#L697) at drm-misc-next `c88084027d0e`).

### The existing pattern

[`drm_crtc_create_scaling_filter_property()`](https://gitlab.freedesktop.org/drm/misc/kernel/-/blob/c88084027d0e922dd925b96e5a0d184ba04a8f43/drivers/gpu/drm/drm_crtc.c#L948) (`drivers/gpu/drm/drm_crtc.c`) takes a
bitmask of the values the hardware supports and creates a property with a fixed name.
The core [decodes it into the CRTC state](https://gitlab.freedesktop.org/drm/misc/kernel/-/blob/c88084027d0e922dd925b96e5a0d184ba04a8f43/drivers/gpu/drm/drm_atomic_uapi.c#L477) (`drivers/gpu/drm/drm_atomic_uapi.c`), so
drivers only read `new_crtc_state->scaling_filter`:

```c
} else if (property == crtc->scaling_filter_property) {
        state->scaling_filter = val;
```

[`drm_plane_create_rotation_property()`](https://gitlab.freedesktop.org/drm/misc/kernel/-/blob/c88084027d0e922dd925b96e5a0d184ba04a8f43/drivers/gpu/drm/drm_blend.c#L293) (`drivers/gpu/drm/drm_blend.c`) does the same for
planes, and also takes the default value.

### Refresh mode, following that pattern

```c
/* Driver: the refresh modes this panel supports, and the default */
drm_crtc_create_epaper_refresh_property(crtc,
                                        DRM_EPAPER_REFRESH_PARTIAL,   /* default */
                                        BIT(DRM_EPAPER_REFRESH_PARTIAL) |
                                        BIT(DRM_EPAPER_REFRESH_FULL) |
                                        BIT(DRM_EPAPER_REFRESH_FAST));

/* Creates the "epaper_refresh" enum property: "partial", "full", "fast",
 * limited to the supported subset. Decoded by the core into
 * new_crtc_state->epaper_refresh. */
```

The first-frame mode and the session clears would follow as separate CRTC properties.
These names are proposals; no such helper exists in the kernel yet.
