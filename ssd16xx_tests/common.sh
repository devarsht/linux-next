#!/bin/bash
# common.sh — Shared setup and helpers for SSD16xx driver tests
# Source this file; do not run directly.

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
RESULT_FILE=""  # Set by env_setup; plain-text summary of PASS/FAIL/SKIP only

# Write a plain (no ANSI colour) line to the result file
_result() { [ -n "$RESULT_FILE" ] && echo "$1" >> "$RESULT_FILE"; }

pass() { echo -e "  ${GREEN}PASS${NC}  $1"; ((PASS_COUNT++)); _result "PASS  $1"; }
fail() { echo -e "  ${RED}FAIL${NC}  $1"; ((FAIL_COUNT++)); _result "FAIL  $1"; }
skip() { echo -e "  ${YELLOW}SKIP${NC}  $1"; ((SKIP_COUNT++)); _result "SKIP  $1"; }
info() { echo -e "  ${CYAN}INFO${NC}  $1"; }
hdr()  { echo ""; echo "--- $* ---"; _result ""; _result "--- $* ---"; }

summary() {
    local totals
    totals=$(printf "  PASS: %-4d  FAIL: %-4d  SKIP: %-4d" "$PASS_COUNT" "$FAIL_COUNT" "$SKIP_COUNT")
    echo ""
    echo "================================================"
    echo "$totals"
    echo "================================================"
    _result ""; _result "================================================"
    _result "$totals"; _result "================================================"
    [ "$FAIL_COUNT" -eq 0 ]
}

# Auto-discover DRM IDs and driver paths — call once at script start
env_setup() {
    # Auto-logging: tee all output to a timestamped log file
    local _script_base="${0##*/}"; _script_base="${_script_base%.sh}"
    local _timestamp; _timestamp=$(date +%Y%m%d_%H%M%S)
    local _log_dir="$SCRIPT_DIR/logs"
    mkdir -p "$_log_dir"
    LOG_FILE="$_log_dir/${_script_base}_${_timestamp}.log"
    exec > >(tee -a "$LOG_FILE") 2>&1
    echo "=== Log: $LOG_FILE ==="

    # Result file: clean PASS/FAIL/SKIP summary (no verbose output, no colours)
    local _result_dir="$SCRIPT_DIR/results"
    mkdir -p "$_result_dir"
    RESULT_FILE="$_result_dir/${_script_base}_${_timestamp}.result"
    printf "=== %s  %s ===\n" "$_script_base" "$_timestamp" > "$RESULT_FILE"
    echo "=== Result: $RESULT_FILE ==="

    # Command tracing: 'set -x' trace goes to log only via BASH_XTRACEFD,
    # not to the terminal, so the console stays clean while the log captures
    # every command that the test script launches.
    exec 3>>"$LOG_FILE"
    export BASH_XTRACEFD=3
    PS4='+ [L${LINENO}] '
    set -x

    if ! lsmod | grep -q panel_ssd16xx 2>/dev/null; then
        echo "ERROR: panel_ssd16xx module not loaded. Load it first."
        exit 1
    fi

    # Ensure SCRIPT_DIR is set (it should be set by the calling test script)
    if [ -z "$SCRIPT_DIR" ]; then
        SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    fi

    # Source the init script to set up environment and discover DRM IDs
    source "$SCRIPT_DIR/init.sh"

    # Validate that discovery succeeded
    if [ -z "$C" ] || [ -z "$CRTC" ] || [ -z "$P" ]; then
        echo "ERROR: Failed to discover DRM IDs (C='$C' CRTC='$CRTC' P='$P')"
        exit 1
    fi
    if [ -z "$DEV" ]; then
        echo "WARNING: SPI device path not found — PM sysfs checks will be skipped."
    fi

    echo ""
}

# Re-run modetest discovery after a module reload
_rediscover() {
    # Ensure SCRIPT_DIR is set
    if [ -z "$SCRIPT_DIR" ]; then
        SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    fi
    source "$SCRIPT_DIR/discover.sh"
}

# Reload module with optional parameters, then re-discover IDs
module_reload() {
    rmmod panel_ssd16xx 2>/dev/null
    sleep 1
    if [ -n "$*" ]; then
        insmod "$BASE" "$@" || { echo "ERROR: insmod failed"; exit 1; }
    else
        insmod "$BASE" 2>/dev/null || modprobe panel_ssd16xx || { echo "ERROR: module load failed"; exit 1; }
    fi
    # Re-enable dynamic debug after reload (dyndbg entries reset on rmmod)
    echo 'module panel_ssd16xx +p' > /sys/kernel/debug/dynamic_debug/control 2>/dev/null
    sleep 1
    _rediscover
}

# Run kmstest in background for N seconds, then kill it
# kmstest auto-selects the only DRM device (no -M needed when tidss is not loaded)
# Saves dmesg output to /tmp/dmesg_capture.log for immediate checking (avoids ring buffer overflow)
kmstest_hold() {
    local secs="${1:-3}"
    ./kmstest &>/dev/null &
    local kpid=$!
    sleep "$secs"
    kill "$kpid" 2>/dev/null
    wait "$kpid" 2>/dev/null
    sleep 0.5  # Allow kernel to finish logging
    # Save dmesg immediately to avoid ring buffer overflow
    dmesg > /tmp/dmesg_capture.log 2>/dev/null
}

# PASS if dmesg contains pattern, else FAIL
# Uses /tmp/dmesg_capture.log if exists (from kmstest_hold), otherwise calls dmesg
check_dmesg() {
    local pattern="$1" desc="$2"
    if [ -f /tmp/dmesg_capture.log ]; then
        if grep -E "$pattern" /tmp/dmesg_capture.log 2>/dev/null; then
            pass "$desc"
            return 0
        else
            fail "$desc  [pattern: '$pattern' not in dmesg]"
            return 1
        fi
    else
        if dmesg | grep -qE "$pattern"; then
            pass "$desc"
            return 0
        else
            fail "$desc  [pattern: '$pattern' not in dmesg]"
            return 1
        fi
    fi
}

# PASS if dmesg does NOT contain pattern
# Uses /tmp/dmesg_capture.log if exists (from kmstest_hold), otherwise calls dmesg
check_dmesg_absent() {
    local pattern="$1" desc="$2"
    if [ -f /tmp/dmesg_capture.log ]; then
        if grep -qE "$pattern" /tmp/dmesg_capture.log 2>/dev/null; then
            fail "$desc  [unexpected pattern found: '$pattern']"
            return 1
        else
            pass "$desc"
            return 0
        fi
    else
        if dmesg | grep -qE "$pattern"; then
            fail "$desc  [unexpected pattern found: '$pattern']"
            return 1
        else
            pass "$desc"
            return 0
        fi
    fi
}

# Check that NO driver error messages appear
# This is the PRIMARY validation method - errors always appear via drm_err()
check_no_driver_errors() {
    local desc="${1:-No driver errors detected}"
    local ERROR_PATTERN="Busy wait timed out|Hardware initialization failed|Clear display failed|Display update failed|failed to resume|HW init failed|Orientation re-init failed|SPI setup failed|Unknown panel model|Failed to get.*GPIO"
    check_dmesg_absent "$ERROR_PATTERN" "$desc"
}

# PASS if runtime_status equals expected value
check_pm_status() {
    local expected="$1" desc="$2"
    if [ -z "$DEV" ]; then skip "$desc (DEV not set)"; return; fi
    local actual
    actual=$(cat "$DEV/power/runtime_status" 2>/dev/null || echo "unavailable")
    if [ "$actual" = "$expected" ]; then
        pass "$desc  (status: $actual)"
    else
        fail "$desc  (expected=$expected, got=$actual)"
    fi
}

# Shorten autosuspend delay for PM tests (default 30s is too slow)
set_autosuspend() {
    local ms="${1:-3000}"
    if [ -n "$DEV" ]; then
        echo "$ms" > "$DEV/power/autosuspend_delay_ms"
    fi
}

# Kill all background jobs on exit
cleanup() { exec 1>/dev/null 2>/dev/null; kill "$(jobs -p)" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT
