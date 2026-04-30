#!/bin/bash
# test_pm.sh — SSD16xx power management tests
#
# Sections:
#   5.0  Basic smoke (PM01-PM06)
#   5.1  Runtime suspend — autosuspend + MODE_1 (RS01-RS05)
#   5.2  Runtime suspend — clear_on_close path (RS06-RS07)
#   5.3  System suspend — active panel → MODE_2 (SS01-SS03)
#   5.4  System suspend — from MODE_1, Bug 3 fix (SS04-SS06)
#   5.5  System suspend — edge cases (SS07-SS10)
#   5.6  System suspend + Resume — active app (TEST4)
#   5.7  Runtime PM idle + System suspend + Resume (TEST5)
#
# Usage: ./test_pm.sh [--no-system-suspend]
#   --no-system-suspend  skip all rtcwake tests (sections 5.3-5.7)
#
# Requires: rtcwake (for system suspend tests), DEV sysfs path

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/common.sh"

SKIP_SS=0
[[ "${1:-}" == "--no-system-suspend" ]] && SKIP_SS=1

echo "=== SSD16xx Power Management Tests ==="
[ "$SKIP_SS" -eq 1 ] && info "System suspend tests DISABLED (--no-system-suspend)"
env_setup

if [ -z "$DEV" ]; then
    echo "ERROR: SPI device not found — PM tests require DEV sysfs path"
    exit 1
fi

# Check rtcwake availability
if ! command -v rtcwake &>/dev/null; then
    info "rtcwake not found — system suspend tests will be skipped"
    SKIP_SS=1
fi

# Verify /dev/rtc0 exists
if [ "$SKIP_SS" -eq 0 ] && [ ! -e /dev/rtc0 ]; then
    info "/dev/rtc0 not found — system suspend tests will be skipped"
    SKIP_SS=1
fi

# Count patterns in current dmesg (avoids grep -c integer expression bug)
dmesg_count() { dmesg | grep -E "$1" | wc -l; }

# Do a system suspend via rtcwake and wait for resume
do_rtcwake() {
    local secs="${1:-10}"
    dmesg -C
    rm -f /tmp/dmesg_capture.log  # clear stale log before suspend
    rtcwake -m mem -s "$secs"
    sleep 1  # settle after resume
    # Capture dmesg immediately after resume for check_dmesg use
    dmesg > /tmp/dmesg_capture.log 2>/dev/null
}

AUTOSUSPEND_DEFAULT=$(cat "$DEV/power/autosuspend_delay_ms" 2>/dev/null || echo 35000)
info "Board autosuspend default: ${AUTOSUSPEND_DEFAULT}ms"

# ── 5.0  Basic smoke checks ───────────────────────────────────────────────────
hdr "5.0 Basic smoke (PM01-PM06)"

# Shorten autosuspend for all PM tests
echo 3000 > "$DEV/power/autosuspend_delay_ms"
info "Autosuspend shortened to 3s for all PM tests"

echo "  [PM01] Runtime suspend after app close"
rm -f /tmp/dmesg_capture.log
dmesg -C
./kmstest &>/dev/null &
KMS=$!
sleep 5
kill "$KMS" 2>/dev/null; wait "$KMS" 2>/dev/null
sleep 4
dmesg > /tmp/dmesg_capture.log
check_no_driver_errors "PM01: no driver errors"
check_pm_status "suspended" "PM01: device suspended after app close"
check_dmesg "runtime suspend|atomic_disable" "PM01: suspend event in dmesg"

echo ""
echo "  [PM02] Runtime resume on app open"
rm -f /tmp/dmesg_capture.log
dmesg -C
./kmstest &>/dev/null &
KMS=$!
sleep 2
dmesg > /tmp/dmesg_capture.log
check_no_driver_errors "PM02: no driver errors"
check_pm_status "active" "PM02: device active while app running"
check_dmesg "runtime resume|atomic_enable" "PM02: resume event in dmesg"
kill "$KMS" 2>/dev/null; wait "$KMS" 2>/dev/null

echo ""
echo "  [PM03] Resume from MODE_1 (RAM retained)"
rm -f /tmp/dmesg_capture.log
dmesg -C
./kmstest &>/dev/null &
KMS=$!
sleep 2
kill "$KMS" 2>/dev/null; wait "$KMS" 2>/dev/null
sleep 4                        # → MODE_1
./kmstest &>/dev/null &
KMS=$!
sleep 2
dmesg > /tmp/dmesg_capture.log
check_no_driver_errors "PM03: no driver errors"
check_dmesg "deep sleep mode 0x01|runtime resume" "PM03: MODE_1 deep sleep + resume"
kill "$KMS" 2>/dev/null; wait "$KMS" 2>/dev/null

if [ "$SKIP_SS" -eq 0 ]; then
    echo ""
    echo "  [PM04+PM05] System suspend/resume (rtcwake 10s)"
    ./kmstest &>/dev/null &
    KMS=$!
    sleep 2
    do_rtcwake 10
    check_dmesg "system suspend|system resume|deep sleep mode" "PM04: system suspend+resume events"
    check_dmesg "system resume" "PM05: system resume confirmed"
    kill "$KMS" 2>/dev/null; wait "$KMS" 2>/dev/null

    echo ""
    echo "  [PM06] MODE_2 path on system suspend"
    check_dmesg "deep sleep mode 0x03" "PM06: MODE_2 sent before system suspend"
    check_dmesg "HW_INIT|runtime resume" "PM06: full reinit on resume from MODE_2"
else
    skip "PM04: system suspend test (--no-system-suspend)"
    skip "PM05: system resume test (--no-system-suspend)"
    skip "PM06: MODE_2 path test (--no-system-suspend)"
fi

# ── 5.1  Runtime suspend — autosuspend + MODE_1 ───────────────────────────────
hdr "5.1 Runtime suspend — autosuspend + MODE_1 (RS01-RS05)"

echo "  [RS01] Verify driver default autosuspend delay"
echo "$AUTOSUSPEND_DEFAULT" > "$DEV/power/autosuspend_delay_ms"  # restore default
DELAY=$(cat "$DEV/power/autosuspend_delay_ms" 2>/dev/null || echo "0")
if [ "$DELAY" -eq "$AUTOSUSPEND_DEFAULT" ]; then
    pass "RS01: driver default autosuspend = ${DELAY}ms"
else
    fail "RS01: expected ${AUTOSUSPEND_DEFAULT}ms, got ${DELAY}ms"
fi

echo ""
echo "  [RS02] Re-apply shortened autosuspend (3s)"
echo 3000 > "$DEV/power/autosuspend_delay_ms"
DELAY=$(cat "$DEV/power/autosuspend_delay_ms")
[ "$DELAY" -eq 3000 ] && pass "RS02: autosuspend re-applied to ${DELAY}ms" || fail "RS02: delay not set"

echo ""
echo "  [RS03] MODE_1 deep sleep on autosuspend"
rm -f /tmp/dmesg_capture.log
dmesg -C
./kmstest &>/dev/null &
KMS=$!
sleep 2
kill "$KMS" 2>/dev/null; wait "$KMS" 2>/dev/null
sleep 5                        # 3s autosuspend + 2s margin
dmesg > /tmp/dmesg_capture.log
check_no_driver_errors "RS03: no driver errors"
check_pm_status "suspended" "RS03: device suspended after autosuspend"
check_dmesg "runtime suspend: entering deep sleep mode 0x01" "RS03: MODE_1 deep sleep command sent"

echo ""
echo "  [RS04] Resume from MODE_1 runs hw_init"
rm -f /tmp/dmesg_capture.log
dmesg -C
./kmstest &>/dev/null &
KMS=$!
sleep 2
dmesg > /tmp/dmesg_capture.log
check_no_driver_errors "RS04: no driver errors"
check_dmesg "runtime resume" "RS04: hw_init on resume from MODE_1"
check_pm_status "active" "RS04: device active after resume"
kill "$KMS" 2>/dev/null; wait "$KMS" 2>/dev/null

echo ""
echo "  [RS05] Full active→MODE_1→active→MODE_1 lifecycle (2 cycles)"
rm -f /tmp/dmesg_capture.log
# Ensure device is suspended before cycle1 so its launch always triggers a resume event
sleep 5
dmesg -C
./kmstest &>/dev/null &
KMS=$!
sleep 1; check_pm_status "active"    "RS05 cycle1: active"
kill "$KMS" 2>/dev/null; wait "$KMS" 2>/dev/null
sleep 5; check_pm_status "suspended" "RS05 cycle1: suspended (MODE_1)"
./kmstest &>/dev/null &
KMS=$!
sleep 1; check_pm_status "active"    "RS05 cycle2: active again"
kill "$KMS" 2>/dev/null; wait "$KMS" 2>/dev/null
sleep 5; check_pm_status "suspended" "RS05 cycle2: suspended again"
check_no_driver_errors "RS05: no driver errors during lifecycle"
dmesg > /tmp/dmesg_capture.log
RESUME_COUNT=$(dmesg_count "runtime resume")
SUSPEND_COUNT=$(dmesg_count "runtime suspend")
[ "$RESUME_COUNT" -ge 2 ]  && pass "RS05: ≥2 runtime resume events (got $RESUME_COUNT)" \
                            || fail "RS05: <2 runtime resume events (got $RESUME_COUNT)"
[ "$SUSPEND_COUNT" -ge 2 ] && pass "RS05: ≥2 runtime suspend events (got $SUSPEND_COUNT)" \
                            || fail "RS05: <2 runtime suspend events (got $SUSPEND_COUNT)"

# ── 5.2  clear_on_close immediate deep sleep ──────────────────────────────────
hdr "5.2 clear_on_close immediate deep sleep (RS06-RS07)"

echo "  [RS06] Deep sleep fires immediately after clear (not after 30s timer)"
module_reload clear_on_close=1
echo 30000 > "$DEV/power/autosuspend_delay_ms"  # 30s timer — sync suspend must fire first
./kmstest &>/dev/null &
KMS=$!
sleep 2
rm -f /tmp/dmesg_capture.log
dmesg -C
T_BEFORE=$(date +%s%3N)
kill "$KMS" 2>/dev/null; wait "$KMS" 2>/dev/null
sleep 3
T_AFTER=$(date +%s%3N)
ELAPSED=$(( T_AFTER - T_BEFORE ))
dmesg > /tmp/dmesg_capture.log
# Note: fbdev resumes device immediately after master_drop clears + suspends,
# so checking status="suspended" here would be a false fail — check dmesg instead
check_dmesg "runtime suspend" "RS06: runtime suspend fired (clear_on_close sync suspend)"
if [ "$ELAPSED" -lt 20000 ]; then
    pass "RS06: suspend within ${ELAPSED}ms (expected <20s, not ~30s timer)"
else
    fail "RS06: took ${ELAPSED}ms — likely triggered by 30s timer, not clear_on_close"
fi

echo ""
echo "  [RS07] Order: clear_display → runtime suspend"
module_reload clear_on_close=1
./kmstest &>/dev/null &
KMS=$!
sleep 2
rm -f /tmp/dmesg_capture.log
dmesg -C
kill "$KMS" 2>/dev/null; wait "$KMS" 2>/dev/null
sleep 3
dmesg > /tmp/dmesg_capture.log
echo "  dmesg ordering (clear_display should appear before runtime suspend):"
dmesg | grep -E "clear_display|runtime suspend" | head -6 | sed 's/^/    /'
CLEAR_LINE=$(dmesg | grep -n "clear_display" | head -1 | cut -d: -f1)
SUSP_LINE=$(dmesg  | grep -n "runtime suspend" | head -1 | cut -d: -f1)
if [ -n "$CLEAR_LINE" ] && [ -n "$SUSP_LINE" ] && [ "$CLEAR_LINE" -lt "$SUSP_LINE" ]; then
    pass "RS07: clear_display appears before runtime suspend"
else
    fail "RS07: ordering wrong or events missing (clear_line=$CLEAR_LINE susp_line=$SUSP_LINE)"
fi

module_reload
echo 3000 > "$DEV/power/autosuspend_delay_ms"

# ── 5.3  System suspend — active panel ────────────────────────────────────────
hdr "5.3 System suspend — active panel → MODE_2 (SS01-SS03)"
if [ "$SKIP_SS" -eq 1 ]; then
    skip "SS01-SS03: system suspend (--no-system-suspend)"
else

echo "  [SS01-SS02] System suspend while app running → MODE_2"
./kmstest &>/dev/null &
KMS=$!
sleep 2
do_rtcwake 10
check_dmesg "deep sleep mode 0x03" "SS01: MODE_2 sent on system suspend"
check_dmesg "system resume" "SS01: system resume confirmed"
check_dmesg "deep sleep mode 0x03" "SS02: MODE_2 flag in dmesg"
check_dmesg "runtime resume" "SS02: hw_init after MODE_2 resume"
kill "$KMS" 2>/dev/null; wait "$KMS" 2>/dev/null

echo ""
echo "  [SS03] Runtime PM continues after system resume"
rm -f /tmp/dmesg_capture.log
dmesg -C
./kmstest &>/dev/null &
KMS=$!
sleep 2; check_pm_status "active" "SS03: device active after system resume"
kill "$KMS" 2>/dev/null; wait "$KMS" 2>/dev/null
sleep 5; check_pm_status "suspended" "SS03: runtime autosuspend works after system resume"
dmesg > /tmp/dmesg_capture.log
check_dmesg "runtime suspend.*deep sleep mode 0x01" "SS03: MODE_1 on autosuspend post-resume"
fi

# ── 5.4  System suspend from MODE_1 — Bug 3 fix ───────────────────────────────
hdr "5.4 System suspend from MODE_1 — upgrading path (SS04-SS06)"
if [ "$SKIP_SS" -eq 1 ]; then
    skip "SS04-SS06: system suspend from MODE_1 (--no-system-suspend)"
else

echo "  [SS04-SS05] Autosuspend → MODE_1, then system suspend → upgrading path"
rm -f /tmp/dmesg_capture.log
dmesg -C
./kmstest &>/dev/null &
KMS=$!
sleep 2
kill "$KMS" 2>/dev/null; wait "$KMS" 2>/dev/null
sleep 5                             # → MODE_1
dmesg > /tmp/dmesg_capture.log
check_pm_status "suspended" "SS04: device in MODE_1 before system suspend"
check_dmesg "runtime suspend: entering deep sleep mode 0x01" "SS04: MODE_1 confirmed in dmesg"

do_rtcwake 10
check_dmesg "upgrading MODE_1.*MODE_2.*HWRESET" "SS04: Bug 3 fix — MODE_1→MODE_2 upgrade path fired"
check_dmesg "system resume|runtime resume" "SS04: system resume + hw_init"

DEEP_SLEEP_COUNT=$(dmesg_count "entering deep sleep")
if [ "$DEEP_SLEEP_COUNT" -le 1 ]; then
    pass "SS05: force_suspend skipped when already in MODE_1 (count=$DEEP_SLEEP_COUNT)"
else
    fail "SS05: too many 'entering deep sleep' events (count=$DEEP_SLEEP_COUNT)"
fi

echo ""
echo "  [SS06] Panel fully functional after MODE_1→MODE_2 resume"
sleep 6  # wait for post-resume autosuspend to fire (MODE_1), so kmstest triggers fresh runtime resume
rm -f /tmp/dmesg_capture.log
dmesg -C
./kmstest &>/dev/null &
KMS=$!
sleep 2; check_pm_status "active" "SS06: device active after resume"
dmesg > /tmp/dmesg_capture.log
check_dmesg "runtime resume" "SS06: display re-initialized on resume"
kill "$KMS" 2>/dev/null; wait "$KMS" 2>/dev/null
sleep 5; check_pm_status "suspended" "SS06: runtime suspend works after system resume"
fi

# ── 5.5  System suspend — edge cases ─────────────────────────────────────────
hdr "5.5 System suspend — edge cases (SS07-SS10)"
if [ "$SKIP_SS" -eq 1 ]; then
    skip "SS07-SS10: system suspend edge cases (--no-system-suspend)"
else

echo "  [SS07] System suspend with no display client (CRTC disabled)"
module_reload
do_rtcwake 5
check_dmesg "system suspend|system resume" "SS07: suspend/resume with no client"
# Check only panel_ssd16xx errors — unrelated hardware errors (e.g. xhci) are ignored
if grep "panel_ssd16xx" /tmp/dmesg_capture.log 2>/dev/null | grep -E "error|BUG|oops|null pointer"; then
    fail "SS07: no kernel errors on suspend with no client"
else
    pass "SS07: no kernel errors on suspend with no client"
fi

echo ""
echo "  [SS08] Repeated suspend/resume cycles (3×) — hw_init exactly once per wake"
./kmstest &>/dev/null &
SS08_KMS=$!
sleep 1
for i in 1 2 3; do
    echo "    cycle $i"
    do_rtcwake 5
    HW_INITS=$(dmesg_count "runtime resume")
    if [ "$HW_INITS" -eq 1 ]; then
        pass "SS08 cycle$i: runtime resume fired exactly once on wake"
    else
        fail "SS08 cycle$i: runtime resume count=$HW_INITS (expected 1)"
    fi
done
kill "$SS08_KMS" 2>/dev/null; wait "$SS08_KMS" 2>/dev/null

echo ""
echo "  [SS09] System suspend immediately after display update"
./kmstest &>/dev/null &
KMS=$!
sleep 2
kill "$KMS" 2>/dev/null; wait "$KMS" 2>/dev/null
do_rtcwake 5
check_dmesg "system suspend|system resume" "SS09: clean suspend after update"

echo ""
echo "  [SS10] Back-to-back: MODE_1→system suspend, ×2 — upgrade path fires each time"
# Reset driver state: after SS07-SS09 cycles, PM state may be stuck active.
module_reload
echo 3000 > "$DEV/power/autosuspend_delay_ms"
for i in 1 2; do
    echo "    pass $i"
    ./kmstest &>/dev/null &
    KMS=$!
    sleep 2; kill "$KMS" 2>/dev/null; wait "$KMS" 2>/dev/null
    # Poll until MODE_1 fires (fbdev post-close refresh resets the timer, so fixed sleep is unreliable)
    for _w in $(seq 1 15); do
        sleep 1
        [ "$(cat $DEV/power/runtime_status 2>/dev/null)" = "suspended" ] && break
    done
    check_pm_status "suspended" "SS10 pass$i: device in MODE_1 before suspend"
    do_rtcwake 5
    check_dmesg "upgrading MODE_1.*MODE_2" "SS10 pass$i: upgrade path fired"
    check_dmesg "system resume" "SS10 pass$i: system resume"
    sleep 1
done
fi

# ── 5.6  TEST4: System suspend + Resume — active app ─────────────────────────
hdr "5.6 TEST4: System suspend + Resume — active app (T401-T404)"
if [ "$SKIP_SS" -eq 1 ]; then
    skip "TEST4: system suspend (--no-system-suspend)"
else

info "TEST4: kmstest running (app active) → system suspend → resume"

echo "  [T401] Launch kmstest and verify display active"
rm -f /tmp/dmesg_capture.log
dmesg -C
./kmstest --flip &>/dev/null &
KMS=$!
sleep 3
check_pm_status "active" "T401: device active while kmstest running"
check_no_driver_errors "T401: no driver errors at launch"

echo "  [T402] System suspend while app active → MODE_2 sent"
do_rtcwake 10
check_dmesg "system suspend.*deep sleep mode|deep sleep mode 0x03" \
    "T402: MODE_2 sent during system suspend"

echo "  [T403] Resume from system suspend → full reinit"
check_dmesg "system resume" "T403: system resume event in dmesg"
check_dmesg "runtime resume" "T403: hw_init on resume (RAM lost after MODE_2)"
check_no_driver_errors "T403: no driver errors after resume"

echo "  [T404] App continues to draw after resume"
sleep 2
check_pm_status "active" "T404: device active after resume (app still running)"
kill "$KMS" 2>/dev/null; wait "$KMS" 2>/dev/null
fi

# ── 5.7  TEST5: Runtime PM idle + System suspend + Resume ────────────────────
hdr "5.7 TEST5: Runtime PM idle + System suspend + Resume (T501-T506)"
if [ "$SKIP_SS" -eq 1 ]; then
    skip "TEST5: system suspend (--no-system-suspend)"
else

info "TEST5: kmstest runs → idle → MODE_1 autosuspend → system suspend → MODE_2 upgrade → resume"

echo "  [T501] Launch kmstest, verify active"
echo 3000 > "$DEV/power/autosuspend_delay_ms"  # 3s for faster test
rm -f /tmp/dmesg_capture.log
dmesg -C
./kmstest --flip &>/dev/null &
KMS=$!
sleep 3
check_pm_status "active" "T501: device active while kmstest running"

echo "  [T502] Pause app → wait for MODE_1 autosuspend"
kill -STOP "$KMS" 2>/dev/null  # SIGSTOP: pause without closing fd
sleep 6                        # 3s autosuspend + 3s margin
dmesg > /tmp/dmesg_capture.log
check_pm_status "suspended" "T502: device in MODE_1 (autosuspend fired)"
check_dmesg "runtime suspend: entering deep sleep mode 0x01" \
    "T502: MODE_1 deep sleep command confirmed in dmesg"
check_no_driver_errors "T502: no driver errors during idle suspend"

echo "  [T503] System suspend while already in MODE_1 → MODE_2 upgrade path"
do_rtcwake 10
check_dmesg "upgrading MODE_1.*MODE_2.*HWRESET" \
    "T503: Bug 3 fix — MODE_1→MODE_2 upgrade path fired"
check_dmesg "system resume" "T503: system resume confirmed"
check_no_driver_errors "T503: no driver errors during suspend/resume"

echo "  [T504] Resume from MODE_2 — full hw_init required (RAM was lost)"
check_dmesg "runtime resume" "T504: hw_init on resume from MODE_2"

echo "  [T505] Resume app → device becomes active again"
kill -CONT "$KMS" 2>/dev/null  # SIGCONT: resume app
sleep 2
check_pm_status "active" "T505: device active after app resumed"
check_no_driver_errors "T505: no driver errors after resume"

echo "  [T506] Runtime PM works normally after full MODE_1→MODE_2 cycle"
kill "$KMS" 2>/dev/null; wait "$KMS" 2>/dev/null
sleep 6
check_pm_status "suspended" "T506: device returns to MODE_1 on next idle"
dmesg > /tmp/dmesg_capture.log
check_dmesg "runtime suspend" "T506: runtime suspend fires after resume"
fi

# ── Restore ────────────────────────────────────────────────────────────────────
echo ""
echo "$AUTOSUSPEND_DEFAULT" > "$DEV/power/autosuspend_delay_ms" 2>/dev/null || true
info "Autosuspend delay restored to ${AUTOSUSPEND_DEFAULT}ms"
module_reload 2>/dev/null || true
rm -f /tmp/dmesg_capture.log

summary
