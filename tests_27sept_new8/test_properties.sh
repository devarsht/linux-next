#!/bin/bash
# test_properties.sh — SSD16xx DRM plane property tests (P01-P18)
#
# Tests runtime-configurable plane properties:
#   refresh_mode (P01-P03), color_mode (P04),
#   clear_on_init (P05-P08), clear_on_close (P09-P12),
#   clear_on_disable (P13-P16), refresh_mode_init (P17-P18)
#
# Plane properties: refresh_mode (40), color_mode (41), clear_on_init (42),
#                  clear_on_close (43), clear_on_disable (44), refresh_mode_init (45)
# Property values (modetest/userspace enum): disabled=0, partial=1, full=2, fast=3
#
# IMPORTANT — internal vs. userspace enum offset:
#   clear_on_init/clear_on_close/clear_on_disable/refresh_mode_init are stored
#   internally as -1=disabled, 0=partial, 1=full, 2=fast (see
#   ssd16xx_session_prop_to_internal() in ssd16xx.c), i.e. internal = modetest_value - 1.
#   The driver's dmesg "mode=%d" / "refresh_mode_init=%d" prints use the INTERNAL
#   value, not the modetest value written via -w. All dmesg checks below account
#   for this offset. refresh_mode itself has NO offset (modetest value == internal
#   enum ssd16xx_refresh_mode value: partial=0, full=1, fast=2).
#
# Every test below runs via prop_test(), which prints a START/RUN/VERIFY/RESTORE/
# END banner for visual confirmation and restores the one property it set back
# to its default once the test finishes. A single suite-wide clean-slate reset
# (reset_all_props) also runs once, right after env_setup(), before any test
# executes, so the very first test starts from a known state too.
#
# Usage: ./test_properties.sh

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/common.sh"

echo "=== SSD16xx Plane Property Tests ==="
env_setup
reset_all_props

# ── 1. refresh_mode ──────────────────────────────────────────────────────────
hdr "refresh_mode property (P01-P03)"
# Enum: partial=0, full=1, fast=2 (no internal/userspace offset)
# Driver prints the active refresh_mode on every frame in fb_dirty():
#   drm_dbg(&device->drm, "fb_dirty: fb=%dx%d, refresh_mode=%d, orientation=%d\n", ...)
# (ssd16xx.c:1344-1346). No -1 offset applies to refresh_mode.

prop_test P01 "refresh_mode=0 (partial, ~300-500ms)"  refresh_mode 0 0 10 present "fb_dirty:.*refresh_mode=0,"
prop_test P02 "refresh_mode=1 (full, ~1.5-2s)"         refresh_mode 1 0 10 present "fb_dirty:.*refresh_mode=1,"
prop_test P03 "refresh_mode=2 (fast, ~1.0-1.5s)"       refresh_mode 2 0 10 present "fb_dirty:.*refresh_mode=2,"

# ── 2. color_mode ───────────────────────────────────────────────────────────
hdr "color_mode property (P04)"
# Enum: black-white=0 (only one value supported)
# NOTE: the driver has no dedicated drm_dbg() print for color_mode itself —
# it is only consulted internally (ssd16xx.c:1352, 1360-1368) to decide
# whether to build a red_buffer for 3-color panels. Since BW is the only
# value this hardware profile supports, there is no distinguishing string
# to grep for; "no driver errors" is the only meaningful check available.

prop_test P04 "color_mode=0 (black-white, only supported value)" color_mode 0 0 10 none

# ── 3. clear_on_init ────────────────────────────────────────────────────────
hdr "clear_on_init property (P05-P08)"
# Enum: disabled=0, partial=1, full=2, fast=3

prop_test P05 "clear_on_init=0 (disabled)" clear_on_init 0 0 10 absent  "clear_on_init: running"
prop_test P06 "clear_on_init=1 (partial)"  clear_on_init 1 0 10 present "clear_on_init: running.*mode=0"
prop_test P07 "clear_on_init=2 (full)"     clear_on_init 2 0 10 present "clear_on_init: running.*mode=1"
prop_test P08 "clear_on_init=3 (fast)"     clear_on_init 3 0 10 present "clear_on_init: running.*mode=2"

# ── 4. clear_on_close ───────────────────────────────────────────────────────
hdr "clear_on_close property (P09-P12)"
# Enum: disabled=0, partial=1, full=2, fast=3

prop_test P09 "clear_on_close=0 (disabled)" clear_on_close 0 0 10 absent  "clear_on_close: running"
prop_test P10 "clear_on_close=1 (partial)"  clear_on_close 1 0 10 present "clear_on_close: running.*mode=0"
prop_test P11 "clear_on_close=2 (full)"     clear_on_close 2 0 5 present "clear_on_close: running.*mode=1"
prop_test P12 "clear_on_close=3 (fast)"     clear_on_close 3 0 10 present "clear_on_close: running.*mode=2"

# ── 5. clear_on_disable ─────────────────────────────────────────────────────
hdr "clear_on_disable property (P13-P16)"
# Enum: disabled=0, partial=1, full=2, fast=3
# Fires when CRTC is disabled (e.g., on kmstest exit)

prop_test P13 "clear_on_disable=0 (disabled)" clear_on_disable 0 0 10 absent  "clear_on_disable: running"
prop_test P14 "clear_on_disable=1 (partial)"  clear_on_disable 1 0 10 present "clear_on_disable: running.*mode=0"
prop_test P15 "clear_on_disable=2 (full)"     clear_on_disable 2 0 5 present "clear_on_disable: running.*mode=1"
prop_test P16 "clear_on_disable=3 (fast)"     clear_on_disable 3 0 10 present "clear_on_disable: running.*mode=2"

# ── 6. refresh_mode_init ────────────────────────────────────────────────────
hdr "refresh_mode_init property (P17-P18)"
# Enum: disabled=0, partial=1, full=2, fast=3
# Sets the refresh mode for the very first frame only
# Driver (ssd16xx.c:2025-2028, crtc_atomic_enable):
#   if (device->refresh_mode_init >= 0) {
#       drm_dbg(&device->drm, "atomic_enable: refresh_mode_init=%d, using for first frame\n", ...);
#       device->init_refresh_pending = true;
#   }
# modetest value 0 = disabled -> internal -1 (ssd16xx_session_prop_to_internal),
# which is NOT >= 0, so this drm_dbg must NOT fire — assert its absence.

prop_test P17 "refresh_mode_init=0 (disabled, use default mode)" refresh_mode_init 0 0 10 absent "atomic_enable: refresh_mode_init=[0-9]+, using for first frame"
# modetest value 3 = fast (0=disabled,1=partial,2=full,3=fast); internal value
# printed in dmesg is 3-1=2 (see ssd16xx_session_prop_to_internal offset above)
prop_test P18 "refresh_mode_init=3 (use fast for first frame)" refresh_mode_init 3 0 10 present "atomic_enable: refresh_mode_init=2, using for first frame"

summary
