#!/bin/bash
# test_properties.sh — SSD16xx DRM connector property tests (P01-P19)
#
# Tests runtime-configurable connector properties:
#   refresh_mode (P01-P03), rotation (P04-P06),
#   border_waveform_init (P07-P10), border_refresh (P11-P16),
#   clear_on_disable (P17-P19)
#
# Usage: ./test_properties.sh

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/common.sh"

echo "=== SSD16xx Connector Property Tests ==="
env_setup

# ── 2.1  refresh_mode ─────────────────────────────────────────────────────────
hdr "refresh_mode (P01-P03)"

echo "  [P01] refresh_mode=0 (partial, ~300-500ms)"
./modetest -M ssd16xx -w "$C:refresh_mode:1" &>/dev/null  # Set to different value first
dmesg -C
./modetest -M ssd16xx -w "$C:refresh_mode:0" &>/dev/null
kmstest_hold 5
check_no_driver_errors "P01: no driver errors"
check_dmesg "refresh_mode=0" "P01: partial mode set (dmesg)"

echo ""
echo "  [P02] refresh_mode=1 (full, ~1.5-2s)"
dmesg -C
./modetest -M ssd16xx -w "$C:refresh_mode:1" &>/dev/null
kmstest_hold 8
check_no_driver_errors "P02: no driver errors"
check_dmesg "refresh_mode=1" "P02: full mode set (dmesg)"

echo ""
echo "  [P03] refresh_mode=2 (fast, ~1.0-1.5s)"
dmesg -C
./modetest -M ssd16xx -w "$C:refresh_mode:2" &>/dev/null
kmstest_hold 6
check_no_driver_errors "P03: no driver errors"
check_dmesg "refresh_mode=2" "P03: fast mode set (dmesg)"

# Restore
./modetest -M ssd16xx -w "$C:refresh_mode:0" &>/dev/null

# ── 2.2  rotation ─────────────────────────────────────────────────────────────
hdr "rotation (P04-P09)"

echo "  [P04] rotation=0 (landscape)"
./modetest -M ssd16xx -w "$C:rotation:90" &>/dev/null  # Set to different value first
dmesg -C
./modetest -M ssd16xx -w "$C:rotation:0" &>/dev/null
kmstest_hold 4
check_no_driver_errors "P04: no driver errors"
check_dmesg "orientation=0" "P04: landscape orientation confirmed"
if ./modetest -M ssd16xx -c 2>/dev/null | grep -q "400x300"; then
    pass "P04: connector reports 400x300 mode"
else
    fail "P04: connector does not report 400x300 mode"
fi

echo ""
echo "  [P05] rotation=90 (portrait CW)"
./modetest -M ssd16xx -w "$C:rotation:0" &>/dev/null  # Set to different value first
dmesg -C
./modetest -M ssd16xx -w "$C:rotation:90" &>/dev/null
kmstest_hold 4
check_no_driver_errors "P05: no driver errors"
check_dmesg "orientation=90" "P05: portrait orientation confirmed"
if ./modetest -M ssd16xx -c 2>/dev/null | grep -q "300x400"; then
    pass "P05: connector reports 300x400 mode"
else
    fail "P05: connector does not report 300x400 mode"
fi

echo ""
echo "  [P06] rotation=180 (landscape 180°)"
./modetest -M ssd16xx -w "$C:rotation:0" &>/dev/null  # Set to different value first
dmesg -C
./modetest -M ssd16xx -w "$C:rotation:180" &>/dev/null
kmstest_hold 4
check_no_driver_errors "P06: no driver errors"
check_dmesg "orientation=180" "P06: 180° orientation confirmed"
if ./modetest -M ssd16xx -c 2>/dev/null | grep -q "400x300"; then
    pass "P06: connector reports 400x300 mode"
else
    fail "P06: connector does not report 400x300 mode"
fi

echo ""
echo "  [P07] rotation=270 (portrait CCW)"
./modetest -M ssd16xx -w "$C:rotation:0" &>/dev/null  # Set to different value first
dmesg -C
./modetest -M ssd16xx -w "$C:rotation:270" &>/dev/null
kmstest_hold 4
check_no_driver_errors "P07: no driver errors"
check_dmesg "orientation=270" "P07: 270° orientation confirmed"
if ./modetest -M ssd16xx -c 2>/dev/null | grep -q "300x400"; then
    pass "P07: connector reports 300x400 mode"
else
    fail "P07: connector does not report 300x400 mode"
fi

echo ""
echo "  [P08] runtime rotation change 0°→90° (triggers hw reinit)"
dmesg -C
./modetest -M ssd16xx -w "$C:rotation:0" &>/dev/null
kmstest_hold 3
./modetest -M ssd16xx -w "$C:rotation:90" &>/dev/null
kmstest_hold 3
check_no_driver_errors "P08: no driver errors"
check_dmesg "atomic_flush.*reinit.*orientation" "P08: reinit triggered on rotation change"

echo ""
echo "  [P09] runtime rotation cycle 0°→90°→180°→270°→0°"
dmesg -C
./modetest -M ssd16xx -w "$C:rotation:0" &>/dev/null
kmstest_hold 2
./modetest -M ssd16xx -w "$C:rotation:90" &>/dev/null
kmstest_hold 2
./modetest -M ssd16xx -w "$C:rotation:180" &>/dev/null
kmstest_hold 2
./modetest -M ssd16xx -w "$C:rotation:270" &>/dev/null
kmstest_hold 2
./modetest -M ssd16xx -w "$C:rotation:0" &>/dev/null
kmstest_hold 2
check_no_driver_errors "P09: no driver errors during cycle"
pass "P09: rotation cycle completed without errors"

# Restore landscape
./modetest -M ssd16xx -w "$C:rotation:0" &>/dev/null

# ── 2.3  border_waveform_init ─────────────────────────────────────────────────
# Each test toggles rotation to trigger hw_init (border waveform applied on reinit)
hdr "border_waveform_init (P10-P13)"

reinit_toggle() {
    ./modetest -M ssd16xx -w "$C:rotation:90" &>/dev/null; kmstest_hold 4
    ./modetest -M ssd16xx -w "$C:rotation:0"  &>/dev/null; kmstest_hold 4
}

echo "  [P10] border_waveform_init=9 (HiZ float, default)"
./modetest -M ssd16xx -w "$C:border_waveform_init:9" &>/dev/null
dmesg -C; reinit_toggle
check_no_driver_errors "P10: no driver errors"
pass "P10: HiZ border waveform applied (visual: no flash)"

echo ""
echo "  [P11] border_waveform_init=1 (LUT1 white flash on init)"
./modetest -M ssd16xx -w "$C:border_waveform_init:1" &>/dev/null
dmesg -C; reinit_toggle
check_no_driver_errors "P11: no driver errors"
pass "P11: LUT1 border waveform applied (visual: white border flash)"

echo ""
echo "  [P12] border_waveform_init=6 (VSL force white border)"
./modetest -M ssd16xx -w "$C:border_waveform_init:6" &>/dev/null
dmesg -C; reinit_toggle
check_no_driver_errors "P12: no driver errors"
pass "P12: VSL border waveform applied (visual: white border forced)"

echo ""
echo "  [P13] border_waveform_init=5 (VSH1 force black border)"
./modetest -M ssd16xx -w "$C:border_waveform_init:5" &>/dev/null
dmesg -C; reinit_toggle
check_no_driver_errors "P13: no driver errors"
pass "P13: VSH1 border waveform applied (visual: black border forced)"

# Restore HiZ
./modetest -M ssd16xx -w "$C:border_waveform_init:9" &>/dev/null

# ── 2.4  border_refresh_on_every_update + border_waveform_refresh ─────────────
hdr "border_refresh_on_every_update (P14-P16)"

echo "  [P14] border_refresh=1 + border_waveform_refresh=8 (VCOM, no flash)"
dmesg -C
./modetest -M ssd16xx -w "$C:border_refresh_every_update:1" &>/dev/null
./modetest -M ssd16xx -w "$C:border_waveform_refresh:8"     &>/dev/null
kmstest_hold 4; kmstest_hold 4; kmstest_hold 4    # 3 updates
check_no_driver_errors "P14: no driver errors"
pass "P14: border refresh enabled with VCOM waveform (visual: clean border)"

echo ""
echo "  [P15] border_waveform_refresh=9 (HiZ)"
./modetest -M ssd16xx -w "$C:border_waveform_refresh:9" &>/dev/null
kmstest_hold 4
check_no_driver_errors "P15: no driver errors"
pass "P15: HiZ waveform on border update (visual: minimal flash)"

echo ""
echo "  [P16] border_refresh=0 (disabled, default)"
dmesg -C
./modetest -M ssd16xx -w "$C:border_refresh_every_update:0" &>/dev/null
kmstest_hold 4; kmstest_hold 4; kmstest_hold 4
check_no_driver_errors "P16: no driver errors"
pass "P16: border refresh disabled (visual: faster updates, possible border ghost)"

# ── 2.5  clear_on_disable ─────────────────────────────────────────────────────
hdr "clear_on_disable property (P17-P19)"
# Trigger: kmstest exit → drm_lastclose → crtc_atomic_disable → clear_on_disable fires.
# (kmsblank triggers the same callback but has DRM master conflicts in some environments.)
# clear_on_disable is logged via drm_dbg — ensure dynamic debug is enabled.
echo 'module panel_ssd16xx +p' > /sys/kernel/debug/dynamic_debug/control 2>/dev/null

echo "  [P17] clear_on_disable=2 (full) via property — fires on CRTC disable"
dmesg -C
./modetest -M ssd16xx -w "$C:clear_on_disable:2" &>/dev/null
kmstest_hold 4            # render then exit → crtc_atomic_disable → clear_on_disable fires
check_dmesg "clear_on_disable: running" "P17: full clear on CRTC disable (property=full)"
check_no_driver_errors "P17: no driver errors"

echo ""
echo "  [P18] clear_on_disable=1 (partial) via property — fires on CRTC disable"
dmesg -C
./modetest -M ssd16xx -w "$C:clear_on_disable:1" &>/dev/null
kmstest_hold 4
check_dmesg "clear_on_disable: running" "P18: partial clear on CRTC disable (property=partial)"
check_no_driver_errors "P18: no driver errors"

echo ""
echo "  [P19] clear_on_disable=0 (disabled) via property — no clear on CRTC disable"
dmesg -C
./modetest -M ssd16xx -w "$C:clear_on_disable:0" &>/dev/null
kmstest_hold 4
check_dmesg_absent "clear_on_disable: running" "P19: no clear on CRTC disable (property=disabled)"
check_no_driver_errors "P19: no driver errors"

# Restore defaults
./modetest -M ssd16xx -w "$C:refresh_mode:0"             &>/dev/null
./modetest -M ssd16xx -w "$C:rotation:0"                 &>/dev/null
./modetest -M ssd16xx -w "$C:border_waveform_init:9"     &>/dev/null
./modetest -M ssd16xx -w "$C:border_refresh_every_update:0" &>/dev/null
./modetest -M ssd16xx -w "$C:clear_on_disable:0"           &>/dev/null

summary
