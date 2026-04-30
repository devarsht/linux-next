# SSD16xx Driver Test Commands

Driver: `panel-ssd16xx`
Panel: GDEY042T81 (400×300, SSD1683 controller)

---

## Step 0 — Environment Setup

Run once per session to discover DRM IDs and set variables.

```bash
# Auto-discover all DRM IDs
_MT=$(./modetest -M ssd16xx 2>/dev/null)

C=$(echo "$_MT"    | awk '/^Connectors:/{f=1} f && /\tconnected\t/{print $1; exit}')
MODE=$(echo "$_MT" | awk '/^Connectors:/{cf=1} cf && /\tconnected\t/{mf=1} mf && /^  [0-9]+x[0-9]+/{print $1; exit}')
CRTC=$(echo "$_MT" | awk '/^CRTCs:/{f=1;next}  f && /^[0-9]+\t/{print $1; exit}')
P=$(echo "$_MT"    | awk '/^Planes:/{f=1;next}  f && /^[0-9]+\t/{print $1; exit}')

BASE=$(find /lib/modules/$(uname -r) -name "panel-ssd16xx.ko" 2>/dev/null | head -1)
DEV=$(ls /sys/bus/spi/devices/*/power/runtime_status 2>/dev/null | head -1 | sed 's|/power.*||')

echo "Connector=$C  CRTC=$CRTC  Plane=$P  Mode=$MODE  Module=$BASE  Device=$DEV"
```

---

## 1 — DRM Pixel Format Tests

Test all supported pixel formats using kmstest. Each format should display
correctly without corruption.

```bash
# F01: XRGB8888 (32bpp, default)
./kmstest --device="/dev/dri/card0" -f XR24
dmesg | tail -5 | grep "convert_1bpp.*XR24"

# F02: RGB888 (24bpp packed)
./kmstest --device="/dev/dri/card0" -f RG24
dmesg | tail -5 | grep "convert_1bpp.*RG24"

# F03: RGB565 (16bpp)
./kmstest --device="/dev/dri/card0" -f RG16
dmesg | tail -5 | grep "convert_1bpp.*RG16"

# F04: R8 (8-bit grayscale, threshold at 127)
./kmstest --device="/dev/dri/card0" -f R8
dmesg | tail -5 | grep "convert_1bpp.*R8"

# F05: NV12 (YUV 4:2:0 planar, Y-plane for luma)
./kmstest --device="/dev/dri/card0" -f NV12
dmesg | tail -5 | grep "convert_1bpp.*NV12"

# F06: NV16 (YUV 4:2:2 planar)
./kmstest --device="/dev/dri/card0" -f NV16
dmesg | tail -5 | grep "convert_1bpp.*NV16"

# F07: YUYV (packed YUV 4:2:2, Y at even bytes)
./kmstest --device="/dev/dri/card0" -f YUYV
dmesg | tail -5 | grep "convert_1bpp.*YUYV"

# F08: UYVY (packed YUV 4:2:2, Y at odd bytes)
./kmstest --device="/dev/dri/card0" -f UYVY
dmesg | tail -5 | grep "convert_1bpp.*UYVY"

# F09: R1 (1-bit mono, native format)
# Note: kmstest may not support R1 - use specialized R1 test app
# Verify fast path via dmesg:
dmesg | grep "R1-fast\|R1-direct"
```

---

## 2 — Rotation Tests (4-Way)

Test all four rotation angles via runtime DRM property.

### 2.1 Runtime Rotation Property

```bash
# R01: 0° landscape (default)
./modetest -M ssd16xx -w $C:rotation:0
./kmstest --device="/dev/dri/card0"
./modetest -M ssd16xx -c | grep "400x300"
dmesg | tail -5 | grep "orientation=0"

# R02: 90° portrait CW
./modetest -M ssd16xx -w $C:rotation:90
./kmstest --device="/dev/dri/card0"
./modetest -M ssd16xx -c | grep "300x400"
dmesg | tail -5 | grep "orientation=90"

# R03: 180° landscape inverted
./modetest -M ssd16xx -w $C:rotation:180
./kmstest --device="/dev/dri/card0"
./modetest -M ssd16xx -c | grep "400x300"
dmesg | tail -5 | grep "orientation=180"

# R04: 270° portrait CCW
./modetest -M ssd16xx -w $C:rotation:270
./kmstest --device="/dev/dri/card0"
./modetest -M ssd16xx -c | grep "300x400"
dmesg | tail -5 | grep "orientation=270"

# R05: Runtime rotation change sequence (triggers reinit)
./modetest -M ssd16xx -w $C:rotation:0
./kmstest --device="/dev/dri/card0"
./modetest -M ssd16xx -w $C:rotation:90
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "reinit\|atomic_flush"

# Restore default
./modetest -M ssd16xx -w $C:rotation:0
```

### 2.2 Rotation via Device Tree Property

Requires DT overlay edit and reboot.

```bash
# R06: Test rotation=<0> in DT
# Edit k3-am62l3-evm-epaper-gdey042t81.dtso:
#   epaper@0 {
#       ...
#       rotation = <0>;
#   };
# Rebuild overlay, deploy, reboot, then:
dmesg | grep "rotation.*from DT\|orientation"
./modetest -M ssd16xx -c | grep "400x300"

# R07: Test rotation=<90> in DT
# Edit overlay: rotation = <90>;
# Rebuild, reboot, then:
dmesg | grep "rotation.*from DT\|orientation"
./modetest -M ssd16xx -c | grep "300x400"

# R08: Test rotation=<180> in DT
# Edit overlay: rotation = <180>;
# Rebuild, reboot, verify

# R09: Test rotation=<270> in DT
# Edit overlay: rotation = <270>;
# Rebuild, reboot, verify
```

---

## 3 — Module Parameter Tests

Each test requires module reload. Test all module parameters systematically.

### 3.1 rotation Parameter

```bash
# M01: rotation=0 (landscape)
rmmod panel_ssd16xx 2>/dev/null; sleep 1
modprobe $BASE rotation=0
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "orientation=0"
./modetest -M ssd16xx -c | grep "400x300"
rmmod panel_ssd16xx; sleep 1

# M02: rotation=90 (portrait CW)
modprobe $BASE rotation=90
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "orientation=90"
./modetest -M ssd16xx -c | grep "300x400"
rmmod panel_ssd16xx; sleep 1

# M03: rotation=180 (landscape inverted)
modprobe $BASE rotation=180
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "orientation=180"
./modetest -M ssd16xx -c | grep "400x300"
rmmod panel_ssd16xx; sleep 1

# M04: rotation=270 (portrait CCW)
modprobe $BASE rotation=270
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "orientation=270"
./modetest -M ssd16xx -c | grep "300x400"
rmmod panel_ssd16xx; sleep 1

# M05: Module param overrides DT
# With rotation=<0> in DT overlay, boot, then:
modprobe $BASE rotation=90
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "orientation=90"  # param wins over DT
rmmod panel_ssd16xx; sleep 1
```

### 3.2 refresh_mode Parameter

```bash
# M06: refresh_mode=0 (partial as default)
modprobe $BASE refresh_mode=0
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "refresh_mode=0\|PARTIAL"
rmmod panel_ssd16xx; sleep 1

# M07: refresh_mode=1 (full as default)
modprobe $BASE refresh_mode=1
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "refresh_mode=1\|FULL"
rmmod panel_ssd16xx; sleep 1

# M08: refresh_mode=2 (fast as default)
modprobe $BASE refresh_mode=2
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "refresh_mode=2\|FAST"
rmmod panel_ssd16xx; sleep 1
```

### 3.3 clear_on_init Parameter

```bash
# M09: clear_on_init=-1 (disabled, default)
modprobe $BASE clear_on_init=-1
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "clear_on_init"  # should not trigger
rmmod panel_ssd16xx; sleep 1

# M10: clear_on_init=0 (partial clear)
modprobe $BASE clear_on_init=0
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "clear_display"
rmmod panel_ssd16xx; sleep 1

# M11: clear_on_init=1 (full clear)
modprobe $BASE clear_on_init=1
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "clear_display.*full"
rmmod panel_ssd16xx; sleep 1
```

### 3.4 clear_on_close Parameter

```bash
# M12: clear_on_close=-1 (disabled, default)
modprobe $BASE clear_on_close=-1
./kmstest --device="/dev/dri/card0"
rmmod panel_ssd16xx
dmesg | tail -10 | grep "clear_on_close"  # should not trigger
sleep 1

# M13: clear_on_close=0 (partial clear)
modprobe $BASE clear_on_close=0
./kmstest --device="/dev/dri/card0"
rmmod panel_ssd16xx
dmesg | tail -10 | grep "clear_display"
sleep 1

# M14: clear_on_close=1 (full clear)
modprobe $BASE clear_on_close=1
./kmstest --device="/dev/dri/card0"
rmmod panel_ssd16xx
dmesg | tail -10 | grep "clear_display.*full"
sleep 1
```

### 3.5 refresh_mode_init Parameter

```bash
# M15: refresh_mode_init=-1 (disabled)
modprobe $BASE refresh_mode_init=-1
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "init_refresh_pending"  # should not be armed
rmmod panel_ssd16xx; sleep 1

# M16: refresh_mode_init=0 (partial for first frame)
modprobe $BASE refresh_mode_init=0
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "refresh_mode_init=0"
rmmod panel_ssd16xx; sleep 1

# M17: refresh_mode_init=1 (full for first frame, default)
modprobe $BASE refresh_mode_init=1
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "refresh_mode_init=1"
rmmod panel_ssd16xx; sleep 1

# M18: refresh_mode_init=2 (fast for first frame)
modprobe $BASE refresh_mode_init=2
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "refresh_mode_init=2"
rmmod panel_ssd16xx; sleep 1
```

### 3.6 Border Waveform Parameters

```bash
# M19: border_waveform_init_lut=9 (HiZ, default)
modprobe $BASE border_waveform_init_lut=9
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "border_waveform"
rmmod panel_ssd16xx; sleep 1

# M20: border_waveform_init_lut=1 (LUT1, white flash)
modprobe $BASE border_waveform_init_lut=1
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "border_waveform"
rmmod panel_ssd16xx; sleep 1

# M21: border_waveform_lut=8 (VCOM)
modprobe $BASE border_waveform_lut=8
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "border_waveform"
rmmod panel_ssd16xx; sleep 1

# M22: border_refresh_on_every_update=1
modprobe $BASE border_refresh_on_every_update=1 border_waveform_lut=8
./kmstest --device="/dev/dri/card0"
./kmstest --device="/dev/dri/card0"
./kmstest --device="/dev/dri/card0"  # Border should update on each frame
dmesg | tail -20 | grep "border.*every_update"
rmmod panel_ssd16xx; sleep 1

# Restore default
modprobe $BASE
```

### 3.7 Combined Module Parameters

```bash
# M23: Multiple params together
modprobe $BASE rotation=90 refresh_mode=1 clear_on_init=1 clear_on_close=1
./kmstest --device="/dev/dri/card0"
./modetest -M ssd16xx -c | grep "300x400"
dmesg | tail -20 | grep "orientation=90\|refresh_mode=1\|clear_display"
rmmod panel_ssd16xx; sleep 1

# Restore default
modprobe $BASE
```

### 3.8 clear_on_disable Parameter

Clears the display when CRTC is disabled (DPMS off), triggered via `kmsblank`.
Values: `-1` = disabled (default), `0` = partial, `1` = full, `2` = fast.

**How it works:** `kmsblank` does DPMS off → `crtc_atomic_disable` fires → if
`clear_on_disable >= 0`, the display is cleared before deep sleep.

**Test sequence:** kmstest renders something, then exits (fbdev re-enables CRTC
and resets the `display_cleared_on_deinit` guard), then `kmsblank` triggers a
fresh DPMS off → clear fires.

> **Note:** The pattern `"clear_on_disable: running"` is used (not just
> `"clear_on_disable"`) because `get_property: clear_on_disable` is logged on
> every DRM atomic state read even when no clear fires.

```bash
# Setup — run once
rmmod panel_ssd16xx 2>/dev/null; sleep 1
BASE=$(find /lib/modules/$(uname -r) -name "panel-ssd16xx.ko" | head -1)
C=$(./modetest -M ssd16xx 2>/dev/null | awk '/^Connectors:/{f=1} f && /connected/{print $1; exit}')

# M24: clear_on_disable=1 (full clear on DPMS off / kmsblank)
insmod $BASE clear_on_disable=1
echo 'module panel_ssd16xx +p' > /sys/kernel/debug/dynamic_debug/control
./kmstest &>/dev/null &; KMS=$!; sleep 2; kill $KMS; wait $KMS  # render then exit
sleep 4                        # wait for fbdev to re-enable CRTC (resets guard)
dmesg -C                       # fresh log window
./kmsblank -c "@$C"            # DPMS off → crtc_atomic_disable → clear fires
sleep 3                        # wait for e-ink full clear (~2s)
dmesg | grep "clear_on_disable: running"  # expect: mode=1
rmmod panel_ssd16xx; sleep 1

# M25: clear_on_disable=0 (partial clear on DPMS off / kmsblank)
insmod $BASE clear_on_disable=0
echo 'module panel_ssd16xx +p' > /sys/kernel/debug/dynamic_debug/control
./kmstest &>/dev/null &; KMS=$!; sleep 2; kill $KMS; wait $KMS
sleep 4
dmesg -C
./kmsblank -c "@$C"
sleep 3
dmesg | grep "clear_on_disable: running"  # expect: mode=0
rmmod panel_ssd16xx; sleep 1

# M26: clear_on_disable default (-1) — no clear on kmsblank
insmod $BASE                   # no param → default -1
echo 'module panel_ssd16xx +p' > /sys/kernel/debug/dynamic_debug/control
./kmstest &>/dev/null &; KMS=$!; sleep 2; kill $KMS; wait $KMS
sleep 4
dmesg -C
./kmsblank -c "@$C"
sleep 1
dmesg | grep "clear_on_disable: running"  # expect: nothing (no clear)
rmmod panel_ssd16xx; sleep 1

# Restore default
insmod $BASE 2>/dev/null || modprobe panel_ssd16xx
```

---

## 4 — DRM Connector Property Tests

Properties are adjusted at runtime without module reload. Use `./modetest` to
set properties, `./kmstest` to trigger display updates.

### 4.1 refresh_mode Property

```bash
# P01: Set partial mode (~300-500ms)
./modetest -M ssd16xx -w $C:refresh_mode:0
./kmstest --device="/dev/dri/card0"
dmesg | tail -5 | grep "refresh_mode=0\|mode=0xff"

# P02: Set full mode (~1.5-2s, with flash)
./modetest -M ssd16xx -w $C:refresh_mode:1
./kmstest --device="/dev/dri/card0"
dmesg | tail -5 | grep "refresh_mode=1\|mode=0xf7"

# P03: Set fast mode (~1.0-1.5s)
./modetest -M ssd16xx -w $C:refresh_mode:2
./kmstest --device="/dev/dri/card0"
dmesg | tail -5 | grep "refresh_mode=2\|mode=0xc7"

# P04: Verify fast LUT preload happens
dmesg | tail -15 | grep "fast_lut_pending\|LOAD_LUT"

# Restore default
./modetest -M ssd16xx -w $C:refresh_mode:0
```

### 4.2 rotation Property (All 4 Angles)

```bash
# P05: Rotate to 0° (landscape, default)
./modetest -M ssd16xx -w $C:rotation:0
./kmstest --device="/dev/dri/card0"
./modetest -M ssd16xx -c | grep "400x300"
dmesg | tail -5 | grep "orientation=0"

# P06: Rotate to 90° (portrait CW)
./modetest -M ssd16xx -w $C:rotation:90
./kmstest --device="/dev/dri/card0"
./modetest -M ssd16xx -c | grep "300x400"
dmesg | tail -5 | grep "orientation=90"

# P07: Rotate to 180° (landscape inverted)
./modetest -M ssd16xx -w $C:rotation:180
./kmstest --device="/dev/dri/card0"
./modetest -M ssd16xx -c | grep "400x300"
dmesg | tail -5 | grep "orientation=180"

# P08: Rotate to 270° (portrait CCW)
./modetest -M ssd16xx -w $C:rotation:270
./kmstest --device="/dev/dri/card0"
./modetest -M ssd16xx -c | grep "300x400"
dmesg | tail -5 | grep "orientation=270"

# P09: Full rotation cycle (0→90→180→270→0)
for angle in 90 180 270 0; do
    echo "=== Testing rotation=$angle ==="
    ./modetest -M ssd16xx -w $C:rotation:$angle
    ./kmstest --device="/dev/dri/card0"
    dmesg | tail -3 | grep "orientation=$angle"
    sleep 1
done

# P10: Rapid rotation changes (triggers crtc_atomic_flush)
./modetest -M ssd16xx -w $C:rotation:0
./kmstest --device="/dev/dri/card0"
./modetest -M ssd16xx -w $C:rotation:90
./kmstest --device="/dev/dri/card0"
./modetest -M ssd16xx -w $C:rotation:0
./kmstest --device="/dev/dri/card0"
dmesg | tail -20 | grep "reinit\|atomic_flush"
```

### 4.3 border_waveform_init Property

```bash
# P11: Set to HiZ (9, default)
./modetest -M ssd16xx -w $C:border_waveform_init:9
./modetest -M ssd16xx -w $C:rotation:90
./kmstest --device="/dev/dri/card0"
./modetest -M ssd16xx -w $C:rotation:0
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "border.*0xc0"

# P12: Set to LUT1 (1, white flash)
./modetest -M ssd16xx -w $C:border_waveform_init:1
./modetest -M ssd16xx -w $C:rotation:90
./kmstest --device="/dev/dri/card0"
./modetest -M ssd16xx -w $C:rotation:0
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "border.*0x03"

# P13: Set to VSH1 (5, black border)
./modetest -M ssd16xx -w $C:border_waveform_init:5
./modetest -M ssd16xx -w $C:rotation:90
./kmstest --device="/dev/dri/card0"
dmesg | tail -5 | grep "border.*0x33"

# Restore
./modetest -M ssd16xx -w $C:border_waveform_init:9
```

### 4.4 border_waveform_refresh Property

```bash
# P14: Set border_waveform_refresh to VCOM (8)
./modetest -M ssd16xx -w $C:border_waveform_refresh:8
./kmstest --device="/dev/dri/card0"
dmesg | tail -5 | grep "border"

# P15: Set border_waveform_refresh to HiZ (9, default)
./modetest -M ssd16xx -w $C:border_waveform_refresh:9
./kmstest --device="/dev/dri/card0"
dmesg | tail -5 | grep "border"
```

### 4.5 border_refresh_on_every_update Property

```bash
# P16: Enable border refresh every update
./modetest -M ssd16xx -w $C:border_refresh_on_every_update:1
./modetest -M ssd16xx -w $C:border_waveform_refresh:8
./kmstest --device="/dev/dri/card0"
./kmstest --device="/dev/dri/card0"
./kmstest --device="/dev/dri/card0"
dmesg | tail -20 | grep "border.*every_update\|Sending border"

# P17: Disable border refresh (default)
./modetest -M ssd16xx -w $C:border_refresh_on_every_update:0
./kmstest --device="/dev/dri/card0"
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "border"  # Should not send border cmd
```

### 4.6 clear_on_init Property

```bash
# P18: Set clear_on_init to disabled (0)
./modetest -M ssd16xx -w $C:clear_on_init:0
./kmstest --device="/dev/dri/card0"
dmesg | tail -5 | grep "clear_on_init"

# P19: Set clear_on_init to partial (1)
./modetest -M ssd16xx -w $C:clear_on_init:1
./kmstest --device="/dev/dri/card0"  # Should trigger partial clear
dmesg | tail -10 | grep "clear_display"

# P20: Set clear_on_init to full (2)
./modetest -M ssd16xx -w $C:clear_on_init:2
./kmstest --device="/dev/dri/card0"  # Should trigger full clear
dmesg | tail -10 | grep "clear_display.*full"
```

### 4.7 clear_on_close Property

```bash
# P21: Set clear_on_close to disabled (0)
./modetest -M ssd16xx -w $C:clear_on_close:0
./kmstest --device="/dev/dri/card0" &
PID=$!
sleep 2
kill $PID
dmesg | tail -5 | grep "clear_on_close"  # Should not trigger

# P22: Set clear_on_close to partial (1)
./modetest -M ssd16xx -w $C:clear_on_close:1
./kmstest --device="/dev/dri/card0" &
PID=$!
sleep 2
kill $PID
sleep 1
dmesg | tail -10 | grep "clear_display"

# P23: Set clear_on_close to full (2)
./modetest -M ssd16xx -w $C:clear_on_close:2
./kmstest --device="/dev/dri/card0" &
PID=$!
sleep 2
kill $PID
sleep 1
dmesg | tail -10 | grep "clear_display.*full"
```

### 4.8 refresh_mode_init Property

```bash
# P24: Set refresh_mode_init to disabled (0)
./modetest -M ssd16xx -w $C:refresh_mode_init:0
rmmod panel_ssd16xx 2>/dev/null; sleep 1
modprobe $BASE
./kmstest --device="/dev/dri/card0"
dmesg | tail -5 | grep "init_refresh_pending"  # Should not arm

# P25: Set refresh_mode_init to partial (1)
./modetest -M ssd16xx -w $C:refresh_mode_init:1
rmmod panel_ssd16xx 2>/dev/null; sleep 1
modprobe $BASE
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "refresh_mode_init.*0"

# P26: Set refresh_mode_init to full (2, default)
./modetest -M ssd16xx -w $C:refresh_mode_init:2
rmmod panel_ssd16xx 2>/dev/null; sleep 1
modprobe $BASE
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "refresh_mode_init.*1"

# P27: Set refresh_mode_init to fast (3)
./modetest -M ssd16xx -w $C:refresh_mode_init:3
rmmod panel_ssd16xx 2>/dev/null; sleep 1
modprobe $BASE
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "refresh_mode_init.*2"
```

### 4.9 List All Properties

```bash
# P28: Display all connector properties and their current values
./modetest -M ssd16xx -c | grep -A50 "props:"
```

### 4.10 clear_on_disable Property

Controls whether the display is cleared on DPMS off (CRTC disable via `kmsblank`).
Set at runtime via the connector property — no module reload needed.

Property values (enum, with +1 offset from internal field):
- `0` = disabled (internal -1, default)
- `1` = partial clear
- `2` = full clear
- `3` = fast clear

**Test sequence:** Set property, render with kmstest, kmstest exits (fbdev
re-enables CRTC, resets `display_cleared_on_deinit` guard), then `kmsblank`
triggers a second DPMS off → the property-controlled clear fires.

> **Note:** Check for `"clear_on_disable: running"` specifically. The string
> `"clear_on_disable"` alone appears on every DRM atomic state read (property
> getter log) and is not a reliable indicator.

```bash
# Setup — run once
C=$(./modetest -M ssd16xx 2>/dev/null | awk '/^Connectors:/{f=1} f && /connected/{print $1; exit}')
echo 'module panel_ssd16xx +p' > /sys/kernel/debug/dynamic_debug/control

# P29: clear_on_disable property = 2 (full clear) via kmsblank
./modetest -M ssd16xx -w $C:clear_on_disable:2
./kmstest &>/dev/null &; KMS=$!; sleep 2; kill $KMS; wait $KMS  # render then exit
sleep 4                          # fbdev re-enables CRTC (resets guard)
dmesg -C
./kmsblank -c "@$C"              # DPMS off → crtc_atomic_disable → full clear
sleep 3
dmesg | grep "clear_on_disable: running"  # expect: mode=1 (full)

# P30: clear_on_disable property = 1 (partial clear) via kmsblank
./modetest -M ssd16xx -w $C:clear_on_disable:1
./kmstest &>/dev/null &; KMS=$!; sleep 2; kill $KMS; wait $KMS
sleep 4
dmesg -C
./kmsblank -c "@$C"              # DPMS off → partial clear
sleep 3
dmesg | grep "clear_on_disable: running"  # expect: mode=0 (partial)

# P31: clear_on_disable property = 0 (disabled) — no clear on kmsblank
./modetest -M ssd16xx -w $C:clear_on_disable:0
./kmstest &>/dev/null &; KMS=$!; sleep 2; kill $KMS; wait $KMS
sleep 4
dmesg -C
./kmsblank -c "@$C"
sleep 1
dmesg | grep "clear_on_disable: running"  # expect: nothing (disabled)

# Restore to disabled
./modetest -M ssd16xx -w $C:clear_on_disable:0
```

---

## 5 — Power Management Tests

### 5.1 Runtime PM - Autosuspend

```bash
# PM01: Check default autosuspend delay (should be 65 seconds)
cat $DEV/power/autosuspend_delay_ms   # expect: 65000

# PM02: Shorten delay for testing
echo 3000 > $DEV/power/autosuspend_delay_ms
cat $DEV/power/autosuspend_delay_ms   # confirm: 3000

# PM03: Verify autosuspend fires and MODE_1 is sent
dmesg -C
./kmstest --device="/dev/dri/card0" &
PID=$!
sleep 2
kill $PID
sleep 5  # Wait for autosuspend
cat $DEV/power/runtime_status  # expect: suspended
dmesg | grep "runtime suspend: entering deep sleep mode 0x01"

# PM04: Verify resume from MODE_1 runs hw_init
./kmstest --device="/dev/dri/card0" &
PID=$!
sleep 2
dmesg | tail -10 | grep "runtime resume.*hw_init"
cat $DEV/power/runtime_status  # expect: active
kill $PID

# PM05: Verify per-update PM gating (idle suspend while app open)
echo 10000 > $DEV/power/autosuspend_delay_ms  # 10s for this test
./kmstest --device="/dev/dri/card0" &
PID=$!
sleep 2
cat $DEV/power/runtime_status  # active (just drew)
sleep 12  # Wait past autosuspend
cat $DEV/power/runtime_status  # suspended (app still open!)
./modetest -M ssd16xx -w $C:refresh_mode:0  # Trigger update
sleep 1
cat $DEV/power/runtime_status  # active again
kill $PID

# Restore 65s default
echo 65000 > $DEV/power/autosuspend_delay_ms
```

### 5.2 System Suspend/Resume

```bash
# PM06: System suspend while app active → MODE_2
./kmstest --device="/dev/dri/card0" &
sleep 2
dmesg -C
rtcwake -m mem -s 10  # Suspend and wake in 10s
dmesg | grep "system suspend: entering deep sleep mode 0x03"
dmesg | grep "system resume"
dmesg | grep "runtime resume.*hw_init"
kill %1 2>/dev/null

# PM07: System suspend from MODE_1 (upgrading path)
echo 3000 > $DEV/power/autosuspend_delay_ms
./kmstest --device="/dev/dri/card0" &
PID=$!
sleep 2
kill $PID
sleep 5  # Let autosuspend fire → MODE_1
cat $DEV/power/runtime_status  # suspended
dmesg -C
rtcwake -m mem -s 10
dmesg | grep "upgrading MODE_1 -> MODE_2 via HWRESET"
dmesg | grep "Unbalanced pm_runtime"  # Should NOT appear (bug fixed)
kill %1 2>/dev/null

# Restore 65s
echo 65000 > $DEV/power/autosuspend_delay_ms
```

### 5.3 PM with clear_on_close (sync suspend)

```bash
# PM08: Verify sync suspend bypasses autosuspend timer
rmmod panel_ssd16xx 2>/dev/null; sleep 1
modprobe $BASE clear_on_close=1
echo 30000 > $DEV/power/autosuspend_delay_ms  # 30s delay
./kmstest --device="/dev/dri/card0" &
PID=$!
sleep 2
dmesg -C
T_START=$(date +%s)
kill $PID
sleep 3  # Wait only 3s, not 30s
T_END=$(date +%s)
cat $DEV/power/runtime_status  # Should be suspended already
dmesg | grep "runtime suspend"
ELAPSED=$((T_END - T_START))
echo "Time to suspend: ${ELAPSED}s (should be < 5s, not 30s)"

# Restore
rmmod panel_ssd16xx; sleep 1; modprobe $BASE
echo 65000 > $DEV/power/autosuspend_delay_ms
```

---

## 6 — Rotation via Device Tree Property

Requires DT overlay modification and reboot/re-apply.

### 6.1 Static DT Rotation Configuration

```bash
# DT01: Verify default rotation from DT (should be 0 if not specified)
dmesg | grep "rotation.*from DT\|orientation" | head -3
./modetest -M ssd16xx -c | grep -E "400x300|300x400"

# DT02: Test rotation=<0> in DT overlay
# Edit arch/arm64/boot/dts/ti/k3-am62l3-evm-epaper-gdey042t81.dtso:
#   epaper@0 {
#       compatible = "gooddisplay,gdey042t81";
#       ...
#       rotation = <0>;
#   };
# Rebuild: make dtbs
# Deploy and reboot
# Verify:
dmesg | grep "orientation"
./modetest -M ssd16xx -c | grep "400x300"

# DT03: Test rotation=<90> in DT overlay
# Edit overlay: rotation = <90>;
# Rebuild, reboot, verify:
dmesg | grep "orientation"
./modetest -M ssd16xx -c | grep "300x400"

# DT04: Test rotation=<180> in DT overlay
# Edit overlay: rotation = <180>;
# Rebuild, reboot, verify:
dmesg | grep "orientation"
./modetest -M ssd16xx -c | grep "400x300"

# DT05: Test rotation=<270> in DT overlay
# Edit overlay: rotation = <270>;
# Rebuild, reboot, verify:
dmesg | grep "orientation"
./modetest -M ssd16xx -c | grep "300x400"
```

### 6.2 Module Parameter vs DT Override

```bash
# DT06: Module param overrides DT value
# With rotation=<0> in DT overlay, reboot, then:
rmmod panel_ssd16xx 2>/dev/null; sleep 1
modprobe $BASE rotation=90
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "orientation=90"  # Module param wins
./modetest -M ssd16xx -c | grep "300x400"

# DT07: Runtime property overrides both DT and module param
# Still with rotation=90 module param loaded:
./modetest -M ssd16xx -w $C:rotation:180
./kmstest --device="/dev/dri/card0"
dmesg | tail -5 | grep "orientation=180"  # Runtime property wins
./modetest -M ssd16xx -c | grep "400x300"

# Restore
rmmod panel_ssd16xx; sleep 1; modprobe $BASE
```

---

## 7 — Integration Tests

### 7.1 Combined Feature Tests

```bash
# INT01: Rotation + refresh mode change
./modetest -M ssd16xx -w $C:rotation:90
./modetest -M ssd16xx -w $C:refresh_mode:1
./kmstest --device="/dev/dri/card0"
dmesg | tail -10 | grep "orientation=90.*refresh_mode=1"

# INT02: Clear on init + rotation
rmmod panel_ssd16xx 2>/dev/null; sleep 1
modprobe $BASE clear_on_init=1 rotation=90
./kmstest --device="/dev/dri/card0"
dmesg | tail -15 | grep "clear_display\|orientation=90"
rmmod panel_ssd16xx; sleep 1; modprobe $BASE

# INT03: PM suspend/resume with rotation change
./kmstest --device="/dev/dri/card0" &
sleep 2
./modetest -M ssd16xx -w $C:rotation:90
./kmstest --device="/dev/dri/card0"
kill %1
sleep 3
./kmstest --device="/dev/dri/card0" &  # Resume
sleep 2
dmesg | tail -15 | grep "orientation=90"
kill %1 2>/dev/null

# INT04: All refresh modes cycle
for mode in 0 1 2 0; do
    ./modetest -M ssd16xx -w $C:refresh_mode:$mode
    ./kmstest --device="/dev/dri/card0"
    dmesg | tail -3 | grep "refresh_mode=$mode"
    sleep 1
done
```

### 7.2 Format + Rotation Combinations

```bash
# INT05: Test multiple formats at 90° rotation
./modetest -M ssd16xx -w $C:rotation:90
for fmt in XR24 RG24 RG16 R8 YUYV NV12; do
    echo "=== Testing format $fmt at 90° ==="
    ./kmstest --device="/dev/dri/card0" -f $fmt
    dmesg | tail -3 | grep "convert.*$fmt"
    sleep 1
done
./modetest -M ssd16xx -w $C:rotation:0  # Restore

# INT06: Test multiple formats at all rotations
for angle in 0 90 180 270; do
    ./modetest -M ssd16xx -w $C:rotation:$angle
    echo "=== Testing formats at $angle° ==="
    for fmt in XR24 RG16 R8; do
        ./kmstest --device="/dev/dri/card0" -f $fmt
        sleep 1
    done
done
./modetest -M ssd16xx -w $C:rotation:0  # Restore
```

---

## 8 — Stress Tests

```bash
# ST01: Rapid property changes
for i in {1..20}; do
    ./modetest -M ssd16xx -w $C:refresh_mode:$(($i % 3))
    ./kmstest --device="/dev/dri/card0"
done

# ST02: Rapid rotation changes
for i in {1..10}; do
    angle=$((($i % 4) * 90))
    ./modetest -M ssd16xx -w $C:rotation:$angle
    ./kmstest --device="/dev/dri/card0"
done
./modetest -M ssd16xx -w $C:rotation:0  # Restore

# ST03: Multiple suspend/resume cycles
for i in {1..5}; do
    echo "=== Cycle $i ==="
    ./kmstest --device="/dev/dri/card0" &
    sleep 2
    rtcwake -m mem -s 5
    sleep 2
done
kill %1 2>/dev/null

# ST04: Long-running display (test autosuspend with multiple wakes)
./kmstest --device="/dev/dri/card0" &
for i in {1..5}; do
    sleep 15  # Let autosuspend fire
    cat $DEV/power/runtime_status
    ./modetest -M ssd16xx -w $C:refresh_mode:0  # Wake it
    sleep 2
done
kill %1 2>/dev/null
```

---

## 9 — Verification Commands

```bash
# V01: Check all properties are exposed
./modetest -M ssd16xx -c | grep -A100 "props:" | grep -E "rotation|refresh_mode|border|clear"

# V02: Check module is loaded with correct parameters
modinfo panel_ssd16xx | grep -E "parm:"

# V03: Check sysfs PM controls exist
ls -la $DEV/power/

# V04: Verify framebuffer device created
ls -la /dev/fb* | grep ssd16xx

# V05: Check DRM card registered
./modetest -M ssd16xx -c

# V06: Monitor PM state transitions in real-time
watch -n 1 "cat $DEV/power/runtime_status; echo '---'; dmesg | tail -3"
```

---

## Notes

- All `./kmstest` commands can take `-f <format>` to test specific pixel formats
- Use `./modetest` only for setting properties and querying state
- Use `./kmstest` for all display updates and drawing operations
- Use `./kmsblank -c "@$C"` to do DPMS off (triggers `crtc_atomic_disable`)
- The 4-way rotation support (0/90/180/270) works via DT, module param, and runtime property
- All 9 pixel formats (R1, XRGB8888, RGB888, RGB565, R8, NV12, NV16, YUYV, UYVY) are supported
- Fixed threshold of 127 is used for grayscale-to-monochrome conversion

### clear_on_disable test pattern notes

The `clear_on_disable` feature uses `kmsblank` to trigger DPMS off. The test
sequence always follows this pattern due to the `display_cleared_on_deinit`
guard:

```
kmstest_hold (render + exit)          # first crtc_atomic_disable sets the guard
  ↓
sleep 4                               # fbdev re-enables CRTC → guard reset to false
  ↓
dmesg -C                              # fresh log window
  ↓
./kmsblank -c "@$C"                   # second crtc_atomic_disable → clear fires
  ↓
dmesg | grep "clear_on_disable: running"   # only matches when clear actually fired
```

Property value encoding (enum with +1 offset from internal `-1..2` range):

| Property value | Internal value | Behaviour     |
|----------------|----------------|---------------|
| 0              | -1             | disabled      |
| 1              |  0             | partial clear |
| 2              |  1             | full clear    |
| 3              |  2             | fast clear    |
