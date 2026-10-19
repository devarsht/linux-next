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
# expect_path must match the "path=" value logged by convert_1bpp for this
# fourcc/orientation combo — see ssd16xx.c:1138-1144 (R1-fast | landscape | portrait)
run_format() {
    local id="$1" fourcc="$2" desc="$3" expect_path="$4"
    echo ""
    echo "  [$id] $desc  (fourcc=$fourcc)"
    dmesg -C
    # kmstest watches stdin for a keypress to quit; in the background under a
    # non-interactive script bash gives it /dev/null, whose instant EOF looks
    # like a keypress and makes it exit immediately. Attach the real tty instead.
    if [ -c /dev/tty ]; then
        ./kmstest -f "$fourcc" </dev/tty &>/dev/null &
    else
        ./kmstest -f "$fourcc" &>/dev/null &
    fi
    local kpid=$!
    sleep "$HOLD"
    kill "$kpid" 2>/dev/null
    wait "$kpid" 2>/dev/null
    sleep 0.5  # Allow kernel to finish logging messages
    dmesg > /tmp/dmesg_capture.log 2>/dev/null

    # PRIMARY: Check no driver errors (drm_err messages always appear)
    check_no_driver_errors "$id: no driver errors"

    # SECONDARY: Check the format actually took the expected conversion path
    # (not just that *some* fb_dirty happened — that alone doesn't prove the
    # right format/path was used, e.g. R1 falling back to the slow pixel loop)
    check_dmesg "fmt=$fourcc.*path=$expect_path" \
        "$id: $desc — fmt=$fourcc path=$expect_path confirmed"
}

echo "=== SSD16xx Pixel Format Tests ==="
echo "    Display hold per format: ${HOLD}s"
env_setup

# Start with a clean state: partial refresh
./modetest -M ssd16xx -w "$P:refresh_mode:0" &>/dev/null  # refresh_mode=partial

hdr "F01: XR24 (XRGB8888 32bpp)"
run_format F01 XR24 "XRGB8888 32bpp" "landscape"

hdr "F02: R1 native 1bpp (direct passthrough)"
run_format F02 R1 "R1 1bpp native zero-copy fast path" "R1-fast"

# Restore defaults
./modetest -M ssd16xx -w "$P:refresh_mode:0" &>/dev/null  # refresh_mode=partial

summary
