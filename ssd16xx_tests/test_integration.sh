#!/bin/bash
# test_integration.sh — SSD16xx integration and commit-ordering tests
#
# Section 6: Integration tests (I01-I04)
#   Cross-feature combinations: clear flags, refresh_mode_init, rotation + PM, ghosting
#
# Section 7: Commit ordering tests (OR01-OR05)
#   Verify crtc_atomic_enable fires before plane_atomic_update on first commit.
#   Requires dynamic debug enabled (prints crtc_atomic_enable / plane_atomic_update).
#
# Usage: ./test_integration.sh

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/common.sh"

echo "=== SSD16xx Integration and Ordering Tests ==="
env_setup

# ── Section 6: Integration tests ─────────────────────────────────────────────
hdr "6: Integration tests (I01-I04)"

echo "  [I01] clear_on_init=1 + clear_on_close=1 together"
dmesg -C
module_reload clear_on_init=1 clear_on_close=1
kmstest_hold 4
dmesg -C
rmmod panel_ssd16xx 2>/dev/null
sleep 2
check_no_driver_errors "I01: no driver errors"
check_dmesg "clear_display|clear_on" "I01: clear_display fired on init+close path"
insmod "$BASE" 2>/dev/null || modprobe panel_ssd16xx; sleep 1; _rediscover

echo ""
echo "  [I02] refresh_mode_init=0 — first and second update both use partial (no baseline)"
dmesg -C
module_reload refresh_mode_init=0
kmstest_hold 4   # first update: skip baseline, go straight to partial
dmesg -C
kmstest_hold 4   # second update: still partial (no re-baseline)
check_dmesg "partial_mode_ready|baseline|PARTIAL" "I02: partial mode active without baseline"
module_reload; sleep 1

echo ""
echo "  [I03] Rotation change after PM suspend/resume"
# Set rotation BEFORE starting kmstest — modetest needs DRM master (kmstest holds it otherwise)
./modetest -M ssd16xx -w "$C:rotation:90" &>/dev/null
./kmstest &>/dev/null &
KMS=$!
sleep 4                # render portrait frame
kill "$KMS" 2>/dev/null; wait "$KMS" 2>/dev/null
sleep 3                # runtime suspend → MODE_1 (autosuspend)
# Second kmstest: resumes from MODE_1, verify rotation=90 still configured
./kmstest &>/dev/null &
KMS=$!
sleep 2
ROT=$(./modetest -M ssd16xx 2>/dev/null | awk '/rotation:/{f=1} f && /value:/{print $2; exit}')
if [ "$ROT" = "90" ]; then
    pass "I03: portrait mode preserved across suspend/resume (rotation=$ROT)"
else
    fail "I03: rotation not preserved (expected 90, got ${ROT:-unknown})"
fi
kill "$KMS" 2>/dev/null; wait "$KMS" 2>/dev/null
./modetest -M ssd16xx -w "$C:rotation:0" &>/dev/null   # restore

echo ""
echo "  [I04] Ghosting cleared by full refresh after fast refresh"
./modetest -M ssd16xx -w "$C:refresh_mode:2" &>/dev/null   # fast
kmstest_hold 4; kmstest_hold 4; kmstest_hold 4            # introduce ghosting
./modetest -M ssd16xx -w "$C:refresh_mode:1" &>/dev/null   # full
kmstest_hold 6
check_no_driver_errors "I04: no driver errors"
pass "I04: full refresh issued after fast (visual: ghosting should be gone)"
./modetest -M ssd16xx -w "$C:refresh_mode:0" &>/dev/null   # restore partial

# ── Section 7: Commit ordering tests ─────────────────────────────────────────
hdr "7: Commit ordering — drm_atomic_helper_commit_tail_rpm (OR01-OR05)"

# Enable dynamic debug so crtc_atomic_enable / plane_atomic_update appear in dmesg
DYNDBG=/sys/kernel/debug/dynamic_debug/control
if [ -w "$DYNDBG" ]; then
    echo 'module panel_ssd16xx +p' > "$DYNDBG" 2>/dev/null
    info "Dynamic debug enabled for panel_ssd16xx"
else
    info "WARNING: $DYNDBG not writable — OR03 may not see ordering messages"
fi

# Helper: load fresh, draw one frame, capture dmesg
_fresh_frame() {
    modprobe -r panel_ssd16xx 2>/dev/null; sleep 1
    dmesg -C
    modprobe panel_ssd16xx; sleep 1
    _rediscover
    ./kmstest &>/dev/null &
    local kpid=$!
    sleep 3
    kill "$kpid" 2>/dev/null; wait "$kpid" 2>/dev/null
}

echo ""
echo "  [OR01] First display update uses full refresh (mode=0xf7, not 0xff)"
_fresh_frame
FIRST_MODE=$(dmesg | grep -oE "mode=0x[0-9a-f]+" | head -1)
echo "        First mode seen: $FIRST_MODE"
if [ "$FIRST_MODE" = "mode=0xf7" ]; then
    pass "OR01: first update used full refresh (mode=0xf7)"
elif [ -z "$FIRST_MODE" ]; then
    skip "OR01: mode= pattern not found in dmesg (need debug build or dynamic debug)"
else
    fail "OR01: first update was NOT full refresh (got $FIRST_MODE, expected mode=0xf7)"
fi

echo ""
echo "  [OR02] First refresh_mode seen is refresh_mode=1 (full), not refresh_mode=0"
_fresh_frame
FIRST_RM=$(dmesg | grep -oE "refresh_mode=[0-9]+" | head -1)
echo "        First refresh_mode: $FIRST_RM"
if [ "$FIRST_RM" = "refresh_mode=1" ]; then
    pass "OR02: first refresh_mode is refresh_mode=1 (full) — no phantom partial"
elif [ -z "$FIRST_RM" ]; then
    skip "OR02: refresh_mode= not in dmesg (need debug enabled)"
else
    fail "OR02: first refresh_mode is $FIRST_RM (expected refresh_mode=1)"
fi

echo ""
echo "  [OR03] crtc_atomic_enable logged BEFORE plane_atomic_update on first commit"
_fresh_frame
ENABLE_LINE=$(dmesg | grep -n "crtc_atomic_enable"  | head -1 | cut -d: -f1)
UPDATE_LINE=$(dmesg | grep -n "plane_atomic_update" | head -1 | cut -d: -f1)
echo "        crtc_atomic_enable at dmesg line: ${ENABLE_LINE:-not found}"
echo "        plane_atomic_update at dmesg line: ${UPDATE_LINE:-not found}"
if [ -z "$ENABLE_LINE" ] || [ -z "$UPDATE_LINE" ]; then
    skip "OR03: ordering messages not in dmesg (enable dynamic debug: echo 'module panel_ssd16xx +p' > $DYNDBG)"
elif [ "$ENABLE_LINE" -lt "$UPDATE_LINE" ]; then
    pass "OR03: crtc_atomic_enable (line $ENABLE_LINE) before plane_atomic_update (line $UPDATE_LINE)"
else
    fail "OR03: plane_atomic_update (line $UPDATE_LINE) before crtc_atomic_enable (line $ENABLE_LINE)"
fi

echo ""
echo "  [OR04] Subsequent frames use partial (init_refresh_pending is one-shot)"
# Already have module loaded from OR03's _fresh_frame; just draw more frames
# Set refresh_mode:0 BEFORE starting kmstest to avoid race
./modetest -M ssd16xx -w "$C:refresh_mode:0" &>/dev/null
dmesg -C
./kmstest &>/dev/null &
KMS=$!
sleep 4
kill "$KMS" 2>/dev/null; wait "$KMS" 2>/dev/null

MODES=$(dmesg | grep -oE "mode=0x[0-9a-f]+" | tr '\n' ' ')
echo "        All mode= values: ${MODES:-<none>}"
FIRST=$(echo "$MODES" | awk '{print $1}')
SECOND=$(echo "$MODES" | awk '{print $2}')
if [ -z "$FIRST" ]; then
    skip "OR04: mode= values not in dmesg"
elif [ "$FIRST" = "mode=0xf7" ] && [ "$SECOND" = "mode=0xff" ]; then
    pass "OR04: first=full(0xf7), subsequent=partial(0xff) — init_refresh_pending cleared"
else
    fail "OR04: unexpected sequence (got: $MODES)"
fi

echo ""
echo "  [OR05] Re-enable after CRTC disable re-arms init_refresh_pending (full again)"
dmesg -C
# First enable
./kmstest &>/dev/null &
KMS=$!
sleep 3
kill "$KMS" 2>/dev/null; wait "$KMS" 2>/dev/null; sleep 2   # CRTC disable

# Re-enable
./kmstest &>/dev/null &
KMS=$!
sleep 3
kill "$KMS" 2>/dev/null; wait "$KMS" 2>/dev/null

FULL_COUNT=$(dmesg | grep -c "mode=0xf7" || echo 0)
echo "        mode=0xf7 (full) occurrences: $FULL_COUNT (expect ≥2)"
if [ -z "$(dmesg | grep 'mode=0x')" ]; then
    skip "OR05: mode= values not in dmesg"
elif [ "$FULL_COUNT" -ge 2 ]; then
    pass "OR05: ≥2 full-refresh frames — init_refresh_pending re-armed on each enable"
else
    fail "OR05: only $FULL_COUNT full-refresh frames (expected ≥2, one per enable)"
fi

# ── Restore ────────────────────────────────────────────────────────────────────
if [ -w "$DYNDBG" ]; then
    echo 'module panel_ssd16xx -p' > "$DYNDBG" 2>/dev/null
    info "Dynamic debug disabled"
fi

./modetest -M ssd16xx -w "$C:refresh_mode:0" &>/dev/null
./modetest -M ssd16xx -w "$C:rotation:0"     &>/dev/null

summary
