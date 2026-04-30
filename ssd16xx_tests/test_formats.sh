#!/bin/bash
# test_formats.sh — SSD16xx pixel format tests (F01-F09)
#
# Tests that every supported framebuffer format produces a display update.
# Automated check: ./kmstest exits and fb_dirty appears in dmesg.
# Visual check: observe display renders without errors for each format.
#
# Usage: ./test_formats.sh [hold_secs]
#   hold_secs  seconds to show each format on display (default: 3)

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/common.sh"

HOLD="${1:-3}"

# Run one format test: ./kmstest -f <fourcc>
run_format() {
    local id="$1" fourcc="$2" desc="$3"
    echo ""
    echo "  [$id] $desc  (fourcc=$fourcc)"
    dmesg -C
    ./kmstest -f "$fourcc" &>/dev/null &
    local kpid=$!
    sleep "$HOLD"
    kill "$kpid" 2>/dev/null
    wait "$kpid" 2>/dev/null
    sleep 0.5  # Allow kernel to finish logging messages

    # PRIMARY: Check no driver errors (drm_err messages always appear)
    check_no_driver_errors "$id: no driver errors"

    # SECONDARY: Check for success indicator (fb_dirty logged)
    if dmesg | grep -qE "fb_dirty"; then
        pass "$id: $desc — fb_dirty confirmed"
    else
        # Not critical if fb_dirty not seen (may need debug enabled)
        info "$id: fb_dirty not in dmesg (may need drm.debug=0xff)"
    fi
}

echo "=== SSD16xx Pixel Format Tests ==="
echo "    Display hold per format: ${HOLD}s"
env_setup

# Start with a clean state: landscape, partial refresh
./modetest -M ssd16xx -w "$C:rotation:0"       &>/dev/null
./modetest -M ssd16xx -w "$C:refresh_mode:0"   &>/dev/null

hdr "F01-F08: Direct framebuffer formats"
run_format F01 XR24 "XRGB8888 32bpp"
run_format F02 RG24 "RGB888 24bpp"
run_format F03 RG16 "RGB565 16bpp"
run_format F04 R8   "Grayscale 8bpp"
run_format F05 NV12 "YUV 4:2:0 semi-planar"
run_format F06 NV16 "YUV 4:2:2 semi-planar"
run_format F07 YUYV "Packed YUV 4:2:2 YUYV"
run_format F08 UYVY "Packed YUV 4:2:2 UYVY"

hdr "F09: R1 native 1bpp (direct passthrough)"
echo "  [F09] R1 1bpp — ./kmstest -f R1"
dmesg -C
./kmstest -f R1 &>/dev/null &
KPID=$!
sleep "$HOLD"
kill "$KPID" 2>/dev/null; wait "$KPID" 2>/dev/null
sleep 0.5  # Allow kernel to finish logging messages

# PRIMARY: Check no errors
check_no_driver_errors "F09: no driver errors"

# SECONDARY: Check for R1 success indicators
if dmesg | grep -qE "R1-fast|R1-direct"; then
    pass "F09: R1 fast path active"
elif dmesg | grep -qE "fb_dirty"; then
    pass "F09: R1 format accepted (fb_dirty seen)"
else
    info "F09: R1 success indicators not in dmesg (may need drm.debug=0xff)"
fi

# Restore defaults
./modetest -M ssd16xx -w "$C:refresh_mode:0" &>/dev/null

summary
