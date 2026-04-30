# SSD16xx Driver Test Results

Driver: `panel-ssd16xx`
Panel: GDEY042T81 (400×300, SSD1683 controller)

---

## Session Info

| Field | Value |
|-------|-------|
| Board | |
| Kernel | |
| Driver commit | |
| Date | |
| Tester | |

---

## Result Key

| Symbol | Meaning |
|--------|---------|
| PASS | Test passed, expected behaviour observed |
| FAIL | Test failed, unexpected behaviour |
| SKIP | Not tested (state reason in Notes) |
| N/A  | Not applicable for this configuration |

---

## 1 — DRM Pixel Formats

| ID | Format | Expected | Result | Notes |
|----|--------|----------|--------|-------|
| F01 | XRGB8888 | Colour bars/fill displays, correct B/W threshold | | |
| F02 | RGB888 | Same as F01 | | |
| F03 | RGB565 | Same as F01 | | |
| F04 | R8 | Grayscale fill, threshold at 127 | | |
| F05 | NV12 | Y-plane luma used, correct B/W output | | |
| F06 | NV16 | Y-plane luma used, correct B/W output | | |
| F07 | YUYV | Y at even bytes, correct B/W output | | |
| F08 | UYVY | Y at odd bytes, correct B/W output | | |
| F09 | R1 | Direct 1-bit passthrough, R1-fast path in dmesg | | |

---

## 2 — DRM Connector Properties

### 2.1 refresh_mode

| ID | Property Value | Expected | Update Time | Result | Notes |
|----|---------------|----------|-------------|--------|-------|
| P01 | refresh_mode=0 (partial) | ~300-500ms, minimal ghosting | | | |
| P02 | refresh_mode=1 (full) | ~1.5-2s, no ghosting | | | |
| P03 | refresh_mode=2 (fast) | ~1.0-1.5s, some ghosting acceptable | | | |

### 2.2 rotation

| ID | Property Value | Expected | Result | Notes |
|----|---------------|----------|--------|-------|
| P04 | rotation=0 | 400×300 mode, landscape output | | |
| P05 | rotation=90 | 300×400 mode, portrait output | | |
| P06 | rotation change at runtime | reinit triggered, correct orientation | | |

### 2.3 border_waveform_init

| ID | Property Value | Expected Border | Result | Notes |
|----|---------------|----------------|--------|-------|
| P07 | border_waveform_init=9 (HiZ) | Minimal border flash | | |
| P08 | border_waveform_init=1 (LUT1) | White border flash on init | | |
| P09 | border_waveform_init=6 (VSL) | White border, no flash | | |
| P10 | border_waveform_init=5 (VSH1) | Black border forced | | |

### 2.4 border_refresh_every_update

| ID | Configuration | Expected | Result | Notes |
|----|--------------|----------|--------|-------|
| P11 | border_refresh=1 + waveform=8 (VCOM) | Border clean after 3 updates, no flash | | |
| P11b | border_refresh=1 + waveform=9 (HiZ) | Border minimal flash per update | | |
| P12 | border_refresh=0 (disabled) | No border overhead, faster updates | | |

---

## 3 — Module Parameters

| ID | Parameter(s) | Expected | Result | Notes |
|----|-------------|----------|--------|-------|
| M01 | rotation=0 | Landscape 400×300, overrides DT | | |
| M02 | rotation=90 | Portrait 300×400, overrides DT | | |
| M03 | refresh_mode=1 | Full refresh as default at load | | |
| M04 | refresh_mode=2 | Fast refresh as default at load | | |
| M05 | clear_on_init=1 | Full clear triggered on first app launch | | |
| M06 | clear_on_init=0 | Partial clear triggered on first app launch | | |
| M07 | clear_on_close=1 | Full clear triggered on CRTC disable | | |
| M08 | clear_on_close=0 | Partial clear triggered on CRTC disable | | |
| M09 | refresh_mode_init=0 | Baseline skipped, starts in partial directly | | |
| M10 | refresh_mode_init=2 | Baseline skipped, starts in fast directly | | |
| M11 | border_waveform_init_lut=1 | White border at module load | | |
| M12 | border_refresh_on_every_update=1 + border_waveform_lut=8 | Border refreshed every update, no flash | | |

---

## 4 — DT Properties

| ID | DT Property | Expected | Result | Notes |
|----|------------|----------|--------|-------|
| D01 | rotation=0 in overlay | Landscape mode at boot, 400×300 | | |
| D02 | rotation=90 in overlay | Portrait mode at boot, 300×400 | | |
| D03 | DT rotation=0, module param rotation=90 | Module param wins, portrait output | | |

---

## 5 — Power Management

| ID | Test | Expected | runtime_status | Result | Notes |
|----|------|----------|----------------|--------|-------|
| PM01 | Runtime suspend (app close) | suspended after CRTC disable | | | |
| PM02 | Runtime resume (app open) | active after CRTC enable | | | |
| PM03 | Runtime resume — Mode 1 path | partial_mode_ready=true, fast resume | | | |
| PM04 | System suspend (echo mem) | Mode 2 set, display off cleanly | N/A | | |
| PM05 | System resume | Display fully restored after wake | N/A | | |
| PM06 | System resume — Mode 2 path | Full hw_init, no partial_mode_ready | N/A | | |

---

## 6 — Integration Tests

| ID | Test | Expected | Result | Notes |
|----|------|----------|--------|-------|
| I01 | clear_on_init=1 + clear_on_close=1 | Clear on launch AND on exit | | |
| I02 | refresh_mode_init=0 + subsequent partials | No baseline, correct partials | | |
| I03 | Rotation change + PM suspend/resume | Correct orientation preserved after resume | | |
| I04 | Full refresh clears ghosting from fast | Ghosting eliminated after refresh_mode=1 | | |

---

## Summary

| Category | Total | PASS | FAIL | SKIP |
|----------|-------|------|------|------|
| DRM Formats | 9 | | | |
| Connector Properties | 12 | | | |
| Module Parameters | 12 | | | |
| DT Properties | 3 | | | |
| Power Management | 6 | | | |
| Integration | 4 | | | |
| **TOTAL** | **46** | | | |

---

## Failures / Issues

| ID | Description | Severity | Status |
|----|-------------|----------|--------|
| | | | |

---

## Additional Observations

(Free-form notes on unexpected behaviour, visual quirks, timing anomalies, etc.)
