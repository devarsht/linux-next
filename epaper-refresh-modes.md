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
9. [Proposal: generic e-paper plane properties](#proposal-generic-e-paper-plane-properties)
10. [Next steps](#next-steps)

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

| Driver | Status | Partial | Full | Fast | Other modes | Related proposed property |
|--------|--------|:-------:|:----:|:----:|-------------|---------------------------|
| Waveshare 2.13" V4, SSD1680 [[5]](#src-5) | Vendor userspace driver | ✅ | ✅ | ✅ | — | `refresh_mode` |
| Waveshare 4.2" V2, SSD1683 [[5]](#src-5) | Vendor userspace driver | ✅ | ✅ | ✅ | 4-grey | `refresh_mode` |
| Waveshare 2.9" V2 [[5]](#src-5) | Vendor userspace driver | ✅ | ✅ | ✅ | 4-grey | `refresh_mode` |
| Waveshare 7.5" V2, UC8179 [[5]](#src-5) | Vendor userspace driver | ✅ | ✅ | ✅ | 4-grey | `refresh_mode` |
| Waveshare 7.3" E, 6-colour [[5]](#src-5) | Vendor userspace driver | — | ✅ | — | — | None: one mode only |
| GxEPD2 [[6]](#src-6) | Community library (Arduino) | ✅ | ✅ | — | — | `refresh_mode`; each panel driver declares `hasPartialUpdate`, like a driver declaring its supported modes |
| Waveshare IT8951 HAT [[7]](#src-7) | Vendor userspace driver | — | GC16 ("display 16 grayscale image") | A2 ("fast refresh without flash") | INIT ("for every init or some time after A2 mode refresh"); every mode can update a rectangle | `refresh_mode` |
| `rockchip_ebc` (PineNote) [[8]](#src-8) [[9]](#src-9) | Out-of-tree DRM driver, not merged | `diff_mode` ("only compute waveforms for changed pixels") | GC16 ("high-quality but flashy") | A2 ("fast transitions between black and white only") | RESET, DU, DU4, GCC16, GL16, GLR16, GLD16 | `refresh_mode`; `clear_on_disable` (the PineNote tree has an `OFF_SCREEN` ioctl for the image shown on disable or suspend) |
| NXP i.MX EPDC [[10]](#src-10) | Downstream vendor kernel (fbdev) | ✅ (`UPDATE_MODE_PARTIAL`) | ✅ (`UPDATE_MODE_FULL`) | — | Waveform chosen per update: INIT, DU, GC4, GC8, GC16, GC32, GLR16, GLD16, AUTO | `refresh_mode` |

Without a common uAPI, each out-of-tree driver uses its own interface: one
function per mode (Waveshare), a bool argument (GxEPD2), a mode code per update
(IT8951), module parameters and private DRM ioctls (`rockchip_ebc`), and private
fbdev ioctls (i.MX EPDC).

### Sources

All were read on 2026-11-18. In-tree files are from drm-misc-next at `c88084027d0e`.

1. <a id="src-1"></a>`drivers/gpu/drm/tiny/repaper.c`: the `cleared` / `partial` logic in the
   framebuffer update path.
   <https://gitlab.freedesktop.org/drm/misc/kernel/-/tree/drm-misc-next/drivers/gpu/drm/tiny/repaper.c>
2. <a id="src-2"></a>`drivers/gpu/drm/tiny/pixpaper.c`: `PIXPAPER_CMD_DISPLAY_REFRESH` (`0x12`).
   <https://gitlab.freedesktop.org/drm/misc/kernel/-/tree/drm-misc-next/drivers/gpu/drm/tiny/pixpaper.c>
3. <a id="src-3"></a>`drivers/video/fbdev/broadsheetfb.c` (`broadsheet_store_waveform_to_spiflash()`) and
   `drivers/video/fbdev/metronomefb.c` (`request_firmware(..., "metronome.wbf", ...)`).
   <https://gitlab.freedesktop.org/drm/misc/kernel/-/tree/drm-misc-next/drivers/video/fbdev>
4. <a id="src-4"></a>ssd16xx v3 series, message-id `20261019213726.4145532-1-devarsht@ti.com`.
   <https://lore.kernel.org/all/20261019213726.4145532-1-devarsht@ti.com/>
5. <a id="src-5"></a>Waveshare e-Paper, `RaspberryPi_JetsonNano/python/lib/waveshare_epd/`, branch `master`:
   [`epd2in13_V4.py`](https://github.com/waveshareteam/e-Paper/blob/master/RaspberryPi_JetsonNano/python/lib/waveshare_epd/epd2in13_V4.py),
   [`epd4in2_V2.py`](https://github.com/waveshareteam/e-Paper/blob/master/RaspberryPi_JetsonNano/python/lib/waveshare_epd/epd4in2_V2.py),
   [`epd2in9_V2.py`](https://github.com/waveshareteam/e-Paper/blob/master/RaspberryPi_JetsonNano/python/lib/waveshare_epd/epd2in9_V2.py),
   [`epd7in5_V2.py`](https://github.com/waveshareteam/e-Paper/blob/master/RaspberryPi_JetsonNano/python/lib/waveshare_epd/epd7in5_V2.py),
   [`epd7in3e.py`](https://github.com/waveshareteam/e-Paper/blob/master/RaspberryPi_JetsonNano/python/lib/waveshare_epd/epd7in3e.py).
6. <a id="src-6"></a>GxEPD2 by Jean-Marc Zingg, branch `master`: panel list in
   [`README.md`](https://github.com/ZinggJM/GxEPD2/blob/master/README.md);
   `hasPartialUpdate`, `hasFastPartialUpdate` and `refresh()` in
   [`src/GxEPD2_EPD.h`](https://github.com/ZinggJM/GxEPD2/blob/master/src/GxEPD2_EPD.h).
7. <a id="src-7"></a>Waveshare IT8951-ePaper, branch `master`:
   [`Raspberry/lib/e-Paper/EPD_IT8951.c`](https://github.com/waveshareteam/IT8951-ePaper/blob/master/Raspberry/lib/e-Paper/EPD_IT8951.c)
   (`INIT_Mode`, `GC16_Mode`, `A2_Mode`) and
   [`EPD_IT8951.h`](https://github.com/waveshareteam/IT8951-ePaper/blob/master/Raspberry/lib/e-Paper/EPD_IT8951.h).
8. <a id="src-8"></a>Samuel Holland, branch `rk35/ebc-drm-v5`:
   [`drivers/gpu/drm/rockchip/rockchip_ebc.c`](https://github.com/smaeul/linux/blob/rk35/ebc-drm-v5/drivers/gpu/drm/rockchip/rockchip_ebc.c)
   (`default_waveform` module parameter) and
   [`include/drm/drm_epd_helper.h`](https://github.com/smaeul/linux/blob/rk35/ebc-drm-v5/include/drm/drm_epd_helper.h)
   (`enum drm_epd_waveform`).
9. <a id="src-9"></a>PineNote community kernel (GitHub `m-weigand/linux`), branch `branch_pinenote_6-6-22`:
   [`drivers/gpu/drm/rockchip/rockchip_ebc.c`](https://github.com/m-weigand/linux/blob/branch_pinenote_6-6-22/drivers/gpu/drm/rockchip/rockchip_ebc.c)
   (module parameters, `OFF_SCREEN` handling on CRTC disable) and
   [`include/uapi/drm/rockchip_ebc_drm.h`](https://github.com/m-weigand/linux/blob/branch_pinenote_6-6-22/include/uapi/drm/rockchip_ebc_drm.h)
   (ioctls).
10. <a id="src-10"></a>NXP linux-imx, branch `lf-5.10.y`:
    [`include/uapi/linux/mxcfb.h`](https://github.com/nxp-imx/linux-imx/blob/lf-5.10.y/include/uapi/linux/mxcfb.h)
    (`struct mxcfb_update_data`, `struct mxcfb_waveform_modes`, `MXCFB_*` ioctls).

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

## Proposal: generic e-paper plane properties

This grew out of the hackfest `drm_epaper_init()` sketch and is shaped to meet the core rules:

```c
/* Driver: which modes this panel supports, and its defaults */
static const struct drm_epaper_caps caps = {
        .refresh_modes   = BIT(DRM_EPAPER_REFRESH_PARTIAL) |
                           BIT(DRM_EPAPER_REFRESH_FULL) |
                           BIT(DRM_EPAPER_REFRESH_FAST),
        .default_refresh = DRM_EPAPER_REFRESH_PARTIAL,
        .default_init    = DRM_EPAPER_REFRESH_FULL,
        .session_clear   = true,  /* clear_on_init/close/disable */
};
drm_plane_create_epaper_properties(plane, &caps);

/* Creates: epaper_refresh, epaper_refresh_init,
 *          epaper_clear_on_init, epaper_clear_on_close,
 *          epaper_clear_on_disable
 * Decoded by the core into new_plane_state->epaper */
```

- They are **plane** properties because they belong to the update that carries the framebuffer.
- Deliverables: a core helper with kernel-doc, an IGT test, and support in the LVGL DRM backend.

## Next steps

**ssd16xx v4 (driver series)**

- Rebase on drm-misc-next.
- Drop the refresh mode, colour mode and session property patches.
- Keep `clear_on_init` / `clear_on_close` / `clear_on_disable` and the first-frame refresh
  as opt-in per-panel defaults (session management patch).

**Separate RFC: generic e-paper plane properties**

- Phase 1: `refresh_mode` + `refresh_mode_init`.
- Phase 2: `clear_on_init` / `clear_on_close` / `clear_on_disable`.
- A core helper, documented uAPI and an IGT test.
- Open-source user: LVGL (an application or the upstream DRM backend).
- Convert ssd16xx. Offer the helper to repaper / pixpaper and to out-of-tree
  drivers such as `rockchip_ebc`, which today use module parameters and private ioctls.

**Later / to discuss:** dynamic rotation via the core `rotation` property, `color_mode`
(property or pixel format), border waveform.
