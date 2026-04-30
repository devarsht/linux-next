#!/bin/bash
# run_all_tests.sh — Master script to run the full SSD16xx driver test suite
#
# Runs all test suites in order, collects individual logs/results,
# and produces a final summary report.
#
# Usage:  cd /home/weston && bash ssd16xx_updated/run_all_tests.sh [--no-system-suspend]
# Output: ssd16xx_updated/results/run_all_<timestamp>.summary
set -x

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
SUMMARY_FILE="$SCRIPT_DIR/results/run_all_${TIMESTAMP}.summary"
PASS_TOTAL=0; FAIL_TOTAL=0; SKIP_TOTAL=0

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

mkdir -p "$SCRIPT_DIR/results" "$SCRIPT_DIR/logs"

echo -e "${BOLD}=== SSD16xx Full Test Suite ===${NC}"
echo "Timestamp : $TIMESTAMP"
echo "Summary   : $SUMMARY_FILE"
echo ""

{
    echo "=== SSD16xx Full Test Suite Run ==="
    echo "Date      : $(date)"
    echo "Board     : $(uname -n)"
    echo "Kernel    : $(uname -r)"
    echo ""
} > "$SUMMARY_FILE"

# Re-enable dynamic debug at start
echo 'module panel_ssd16xx +p' > /sys/kernel/debug/dynamic_debug/control 2>/dev/null

run_suite() {
    local name="$1"
    local script="${name}.sh"
    local extra_args="${2:-}"

    echo ""
    echo -e "${CYAN}======================================${NC}"
    echo -e "${CYAN}--- Running: $name ---${NC}"
    echo -e "${CYAN}======================================${NC}"
    echo "--- $name ---" >> "$SUMMARY_FILE"

    # Run in a clean subshell to prevent env contamination between suites
    ( cd /home/weston && bash "$SCRIPT_DIR/$script" $extra_args )
    local rc=$?

    # Re-enable dynamic debug after each suite (module_reload resets it)
    echo 'module panel_ssd16xx +p' > /sys/kernel/debug/dynamic_debug/control 2>/dev/null

    # Find the newest result file for this suite
    local result_file
    result_file=$(ls -t "$SCRIPT_DIR/results/${name}_"*.result 2>/dev/null | head -1)

    if [ -f "$result_file" ]; then
        cat "$result_file" >> "$SUMMARY_FILE"
        echo "" >> "$SUMMARY_FILE"

        local p f s
        p=$(grep -c "^PASS" "$result_file" 2>/dev/null || echo 0)
        f=$(grep -c "^FAIL" "$result_file" 2>/dev/null || echo 0)
        s=$(grep -c "^SKIP" "$result_file" 2>/dev/null || echo 0)
        PASS_TOTAL=$((PASS_TOTAL + p))
        FAIL_TOTAL=$((FAIL_TOTAL + f))
        SKIP_TOTAL=$((SKIP_TOTAL + s))

        if [ "$f" -eq 0 ]; then
            echo -e "  ${GREEN}PASS${NC} $name — ${p}p / ${s}s / 0f"
        else
            echo -e "  ${RED}FAIL${NC} $name — ${p}p / ${s}s / ${f}f"
        fi
    else
        echo -e "  ${YELLOW}WARN${NC} $name — no result file (exit code: $rc)"
        echo "  WARN  $name — no result file (rc=$rc)" >> "$SUMMARY_FILE"
    fi
}

run_suite "test_formats"
run_suite "test_properties"
run_suite "test_module_params"
run_suite "test_pm" "${1:-}"
run_suite "test_integration"

LINE="================================================"
TOTALS=$(printf "  PASS: %-4d  FAIL: %-4d  SKIP: %-4d" "$PASS_TOTAL" "$FAIL_TOTAL" "$SKIP_TOTAL")

echo ""
echo -e "${BOLD}$LINE${NC}"
echo -e "$TOTALS"
echo -e "${BOLD}$LINE${NC}"
echo ""
echo "Logs    : $SCRIPT_DIR/logs/"
echo "Results : $SCRIPT_DIR/results/"
echo "Summary : $SUMMARY_FILE"

{
    echo ""
    echo "=== GRAND TOTAL ==="
    echo "$LINE"
    echo "$TOTALS"
    echo "$LINE"
} >> "$SUMMARY_FILE"

[ "$FAIL_TOTAL" -eq 0 ]
