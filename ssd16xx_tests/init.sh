#!/bin/bash
# init.sh — Environment initialization for SSD16xx tests
# Sets up debug output, discovers DRM IDs, and exports test environment variables.
#
# Usage:
#   source ./init.sh
#
# Exports: C, CRTC, P, MODE, BASE, DEV

# Enable DRM debug output (all categories: 0xff = DRM_UT_ALL)
echo "0xff" > /sys/module/drm/parameters/debug 2>/dev/null
echo module panel_ssd16xx +p > /sys/kernel/debug/dynamic_debug/control 2>/dev/null

# Set kernel printk levels: console_loglevel=2, default=4, min=1, boot-time-default=8
echo "2 4 1 8" > /proc/sys/kernel/printk 2>/dev/null

# Set LD_LIBRARY_PATH for local weston/wayland libraries
export LD_LIBRARY_PATH="/home/weston/:${LD_LIBRARY_PATH:-}"

# Verify required binaries exist
if ! command -v kmstest &>/dev/null; then
    echo "ERROR: kmstest binary not found"
    exit 1
fi

if ! command -v modetest &>/dev/null; then
    echo "ERROR: modetest binary not found"
    exit 1
fi

# Discover DRM IDs
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/discover.sh"

# Discover module path
BASE=$(find /lib/modules/$(uname -r) -name "panel-ssd16xx.ko" 2>/dev/null | head -1)

# Find the SPI device driven by panel_ssd16xx (not just the first alphabetically)
DEV=""
for d in /sys/bus/spi/devices/*/; do
    drv="$(basename "$(readlink ${d}driver 2>/dev/null)" 2>/dev/null)"
    if echo "$drv" | grep -q "ssd16xx"; then
        DEV="${d%/}"
        break
    fi
done
# Fallback: first SPI device with runtime_status
[ -z "$DEV" ] && DEV=$(ls /sys/bus/spi/devices/*/power/runtime_status 2>/dev/null | head -1 | sed 's|/power.*||')

# Display configuration
echo "Connector=$C  CRTC=$CRTC  Plane=$P  Mode=$MODE  Module=$BASE  Device=$DEV"

# Export for use by test scripts
export C CRTC P MODE BASE DEV
