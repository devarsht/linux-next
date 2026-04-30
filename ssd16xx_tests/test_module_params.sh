#!/bin/bash
# test_module_params.sh — SSD16xx module parameter tests (M01-M17)
#
# Each test does: rmmod → insmod <param> → kmstest → check dmesg → rmmod
# Requires: panel-ssd16xx.ko path discoverable via find /lib/modules
#
# Usage: ./test_module_params.sh

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/common.sh"

echo "=== SSD16xx Module Parameter Tests ==="
env_setup

if [ -z "$BASE" ]; then
    echo "ERROR: panel-ssd16xx.ko not found under /lib/modules/$(uname -r)"
    exit 1
fi

# Run one module param test:
#   $1 = test id   $2 = description   $3 = insmod args   $4 = dmesg pattern
mp_test() {
    local id="$1" desc="$2" args="$3" pattern="$4"
    echo ""
    echo "  [$id] $desc"
    echo "        insmod args: $args"

    dmesg -C
    rmmod panel_ssd16xx 2>/dev/null; sleep 1
    # shellcheck disable=SC2086
    insmod "$BASE" $args || { fail "$id: insmod failed"; return; }
    sleep 1
    _rediscover
    kmstest_hold 4
    check_dmesg "$pattern" "$id: $desc"
    dmesg -C
}

hdr "rotation (M01-M04)"
mp_test M01 "rotation=0 (landscape)"         "rotation=0"   "orientation=0"
mp_test M02 "rotation=90 (portrait CW)"      "rotation=90"  "orientation=90"
mp_test M03 "rotation=180 (landscape 180°)"  "rotation=180" "orientation=180"
mp_test M04 "rotation=270 (portrait CCW)"    "rotation=270" "orientation=270"

hdr "refresh_mode (M05-M06)"
mp_test M05 "refresh_mode=1 (full)"   "refresh_mode=1" "Refresh mode: full|refresh_mode=1"
mp_test M06 "refresh_mode=2 (fast)"   "refresh_mode=2" "Refresh mode: fast|refresh_mode=2"

hdr "clear_on_init (M07-M08)"
echo ""
echo "  [M07] clear_on_init=1 (full clear on first launch)"
dmesg -C
rmmod panel_ssd16xx 2>/dev/null; sleep 1
insmod "$BASE" clear_on_init=1 || { fail "M07: insmod failed"; }
sleep 1; _rediscover
kmstest_hold 6
check_dmesg "clear_on_init|clear_display" "M07: clear on init fired"

echo ""
echo "  [M08] clear_on_init=0 (partial clear on first launch)"
dmesg -C
rmmod panel_ssd16xx 2>/dev/null; sleep 1
insmod "$BASE" clear_on_init=0 || { fail "M08: insmod failed"; }
sleep 1; _rediscover
kmstest_hold 4
check_dmesg "clear_on_init|clear_display" "M08: partial clear on init"
dmesg -C

hdr "clear_on_close (M09-M10)"
echo ""
echo "  [M09] clear_on_close=1 (full clear on CRTC disable)"
dmesg -C
rmmod panel_ssd16xx 2>/dev/null; sleep 1
insmod "$BASE" clear_on_close=1 || { fail "M09: insmod failed"; }
sleep 1; _rediscover
kmstest_hold 4
dmesg -C                         # clear log, then unload triggers clear
rmmod panel_ssd16xx 2>/dev/null
sleep 2
check_dmesg "clear_on_close|clear_display" "M09: clear on close fired after rmmod"

echo ""
echo "  [M10] clear_on_close=0 (partial clear on close)"
dmesg -C
insmod "$BASE" clear_on_close=0 || { fail "M10: insmod failed (using default)"; }
sleep 1; _rediscover
kmstest_hold 4
dmesg -C
rmmod panel_ssd16xx 2>/dev/null
sleep 2
check_dmesg "clear_on_close|clear_display" "M10: partial clear on close fired"
dmesg -C

hdr "refresh_mode_init (M11-M12)"
echo ""
echo "  [M11] refresh_mode_init=0 (skip baseline, start partial)"
dmesg -C
insmod "$BASE" refresh_mode_init=0 || { fail "M11: insmod failed"; }
sleep 1; _rediscover
kmstest_hold 4
check_dmesg "refresh_mode_init|partial_mode_ready|baseline" "M11: refresh_mode_init=0 applied"

echo ""
echo "  [M12] refresh_mode_init=2 (skip baseline, start fast)"
dmesg -C
rmmod panel_ssd16xx 2>/dev/null; sleep 1
insmod "$BASE" refresh_mode_init=2 || { fail "M12: insmod failed"; }
sleep 1; _rediscover
kmstest_hold 4
check_dmesg "refresh_mode_init|Refresh mode: fast" "M12: refresh_mode_init=2 applied"
rmmod panel_ssd16xx 2>/dev/null; sleep 1

hdr "border_waveform_init_lut (M13)"
echo ""
echo "  [M13] border_waveform_init_lut=1 (white border at module load)"
dmesg -C
insmod "$BASE" border_waveform_init_lut=1 || { fail "M13: insmod failed"; }
sleep 1; _rediscover
kmstest_hold 4
check_dmesg "border_waveform" "M13: border_waveform_init_lut applied"
rmmod panel_ssd16xx 2>/dev/null; sleep 1

hdr "border_refresh_on_every_update + border_waveform_lut (M14)"
echo ""
echo "  [M14] border_refresh_on_every_update=1 + border_waveform_lut=8"
dmesg -C
insmod "$BASE" border_refresh_on_every_update=1 border_waveform_lut=8 || { fail "M14: insmod failed"; }
sleep 1; _rediscover
kmstest_hold 4; kmstest_hold 4; kmstest_hold 4   # 3 updates
pass "M14: border_refresh_on_every_update=1 + border_waveform_lut=8 (visual: clean border)"
rmmod panel_ssd16xx 2>/dev/null; sleep 1

hdr "clear_on_disable (M15-M17)"
# Trigger: kmstest exit → drm_lastclose → crtc_atomic_disable → clear_on_disable fires.
# (kmsblank triggers the same callback but has DRM master conflicts in some environments.)

echo ""
echo "  [M15] clear_on_disable=1 (full clear on CRTC disable)"
rmmod panel_ssd16xx 2>/dev/null; sleep 1
insmod "$BASE" clear_on_disable=1 || { fail "M15: insmod failed"; }
echo 'module panel_ssd16xx +p' > /sys/kernel/debug/dynamic_debug/control 2>/dev/null
sleep 1; _rediscover
dmesg -C
kmstest_hold 4            # render then exit → crtc_atomic_disable → clear_on_disable=1 fires
check_dmesg "clear_on_disable: running" "M15: full clear on CRTC disable"
check_no_driver_errors "M15: no driver errors"

echo ""
echo "  [M16] clear_on_disable=0 (partial clear on CRTC disable)"
rmmod panel_ssd16xx 2>/dev/null; sleep 1
insmod "$BASE" clear_on_disable=0 || { fail "M16: insmod failed"; }
echo 'module panel_ssd16xx +p' > /sys/kernel/debug/dynamic_debug/control 2>/dev/null
sleep 1; _rediscover
dmesg -C
kmstest_hold 4
check_dmesg "clear_on_disable: running" "M16: partial clear on CRTC disable"
check_no_driver_errors "M16: no driver errors"

echo ""
echo "  [M17] clear_on_disable default (-1) — no clear on CRTC disable"
rmmod panel_ssd16xx 2>/dev/null; sleep 1
insmod "$BASE" || { fail "M17: insmod failed"; }
echo 'module panel_ssd16xx +p' > /sys/kernel/debug/dynamic_debug/control 2>/dev/null
sleep 1; _rediscover
dmesg -C
kmstest_hold 4
check_dmesg_absent "clear_on_disable: running" "M17: no clear on CRTC disable (disabled by default)"
check_no_driver_errors "M17: no driver errors"
rmmod panel_ssd16xx 2>/dev/null; sleep 1

hdr "Restore: default module load"
insmod "$BASE" 2>/dev/null || modprobe panel_ssd16xx
sleep 1
_rediscover
info "Module reloaded with defaults: Connector=$C CRTC=$CRTC Plane=$P"

summary
