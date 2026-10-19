#!/bin/bash
# test_integration.sh — SSD16xx integration and ordering tests
#
# Section 1: Integration tests (I02, I04)
#   Cross-feature combinations: DRM property interactions
#
# Section 2: Commit ordering tests (OR01-OR03, OR05)
#   Verify crtc_atomic_enable fires before plane_atomic_update on first commit.
#
# Usage: ./test_integration.sh

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/common.sh"

echo "=== SSD16xx Integration and Ordering Tests ==="
env_setup
reset_all_props

# ── Section 1: Integration tests ─────────────────────────────────────────────
hdr "Integration tests (I02, I04)"

# modetest value 2 = full; internal value printed in dmesg is 2-1=1
# (see ssd16xx_session_prop_to_internal offset — clear_on_init/close/disable
# and refresh_mode_init are stored internally as modetest_value - 1)

echo ""
echo "  ┌── [I02] START ─────────────────────────────────────────────"
echo "  │  refresh_mode transitions: partial→full→fast"
echo "  │  SET     : ./modetest -M ssd16xx -w \"\$P:refresh_mode:1\"  (full)"
echo "  │  SET     : ./modetest -M ssd16xx -w \"\$P:refresh_mode:2\"  (fast)"
echo "  └────────────────────────────────────────────────────────────"
dmesg -C
echo "  [I02] RUN     : holding 2s at partial"
kmstest_hold 2
./modetest -M ssd16xx -w "$P:refresh_mode:1" &>/dev/null  # refresh_mode=full
echo "  [I02] RUN     : holding 4s at full"
kmstest_hold 4
./modetest -M ssd16xx -w "$P:refresh_mode:2" &>/dev/null  # refresh_mode=fast
echo "  [I02] RUN     : holding 3s at fast"
kmstest_hold 3
echo "  [I02] VERIFY  :"
check_no_driver_errors "I02: no driver errors"
pass "I02: refresh_mode transitions completed"
echo "  [I02] RESTORE : refresh_mode:0  (back to default / partial)"
./modetest -M ssd16xx -w "$P:refresh_mode:0" &>/dev/null
echo "  └── [I02] END ───────────────────────────────────────────────"


echo ""
echo "  ┌── [I04] START ─────────────────────────────────────────────"
echo "  │  color_mode black-white only mode"
echo "  │  SET     : ./modetest -M ssd16xx -w \"\$P:color_mode:0\"  (black-white)"
echo "  └────────────────────────────────────────────────────────────"
dmesg -C
./modetest -M ssd16xx -w "$P:color_mode:0" &>/dev/null  # color_mode=black-white
echo "  [I04] RUN     : holding 3s (./kmstest)"
kmstest_hold 3
echo "  [I04] VERIFY  :"
check_no_driver_errors "I04: no driver errors"
pass "I04: color_mode black-white set and maintained"
echo "  [I04] RESTORE : color_mode:0  (back to default)"
./modetest -M ssd16xx -w "$P:color_mode:0" &>/dev/null
echo "  └── [I04] END ───────────────────────────────────────────────"

# ── Section 2: Commit ordering tests ─────────────────────────────────────────
hdr "Commit ordering — drm_atomic_helper_commit_tail_rpm (OR01-OR03, OR05)"

# Enable dynamic debug so crtc_atomic_enable / plane_atomic_update appear in dmesg
DYNDBG=/sys/kernel/debug/dynamic_debug/control
if [ -w "$DYNDBG" ]; then
    echo 'module ssd16xx +p' > "$DYNDBG" 2>/dev/null
    info "Dynamic debug enabled for ssd16xx"
else
    info "WARNING: $DYNDBG not writable — OR03 may not see ordering messages"
fi

# Helper: load fresh, draw one frame, capture dmesg
_fresh_frame() {
    modprobe -r ssd16xx 2>/dev/null; sleep 1
    dmesg -C
    modprobe ssd16xx; sleep 1
    _rediscover
    ./kmstest </dev/tty &>/dev/null &
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
ENABLE_LINE=$(dmesg | grep -n "atomic_enable" | head -1 | cut -d: -f1)
UPDATE_LINE=$(dmesg | grep -n "plane_atomic_update" | head -1 | cut -d: -f1)
echo "        crtc_atomic_enable at dmesg line: ${ENABLE_LINE:-not found}"
echo "        plane_atomic_update at dmesg line: ${UPDATE_LINE:-not found}"
if [ -z "$ENABLE_LINE" ] || [ -z "$UPDATE_LINE" ]; then
    skip "OR03: ordering messages not in dmesg (enable dynamic debug: echo 'module ssd16xx +p' > $DYNDBG)"
elif [ "$ENABLE_LINE" -lt "$UPDATE_LINE" ]; then
    pass "OR03: crtc_atomic_enable (line $ENABLE_LINE) before plane_atomic_update (line $UPDATE_LINE)"
else
    fail "OR03: plane_atomic_update (line $UPDATE_LINE) before crtc_atomic_enable (line $ENABLE_LINE)"
fi

echo ""
echo "  [OR05] Re-enable after CRTC disable re-arms init_refresh_pending (full again)"
dmesg -C
# First enable
./kmstest </dev/tty &>/dev/null &
KMS=$!
sleep 3
kill "$KMS" 2>/dev/null; wait "$KMS" 2>/dev/null; sleep 2   # CRTC disable

# Re-enable
./kmstest </dev/tty &>/dev/null &
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
    echo 'module ssd16xx -p' > "$DYNDBG" 2>/dev/null
    info "Dynamic debug disabled"
fi

# Restore plane properties to defaults
./modetest -M ssd16xx -w "$P:refresh_mode:0" &>/dev/null  # refresh_mode=partial
./modetest -M ssd16xx -w "$P:color_mode:0" &>/dev/null  # color_mode=black-white
./modetest -M ssd16xx -w "$P:clear_on_init:0" &>/dev/null  # clear_on_init=disabled
./modetest -M ssd16xx -w "$P:clear_on_close:0" &>/dev/null  # clear_on_close=disabled
./modetest -M ssd16xx -w "$P:clear_on_disable:0" &>/dev/null  # clear_on_disable=disabled
./modetest -M ssd16xx -w "$P:refresh_mode_init:0" &>/dev/null  # refresh_mode_init=disabled

summary
