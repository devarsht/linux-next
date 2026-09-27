#!/bin/bash
# test_module_params.sh — SSD16xx module parameter tests (NOT APPLICABLE)
#
# NOTE: This test is NOT APPLICABLE in the current driver.
#
# The SSD16xx driver in the current branch (new4) does NOT expose any insmod
# module parameters. All configuration that was previously done via module
# parameters (rotation, refresh_mode, clear_on_init, etc.) is now handled
# exclusively via DRM plane properties, which are set at runtime using modetest.
#
# For property testing, see: ./test_properties.sh
#
# Usage: ./test_module_params.sh

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/common.sh"

echo "=== SSD16xx Module Parameter Tests ==="
echo ""
echo "STATUS: SKIPPED — Not applicable in current branch"
echo ""
echo "Reason:"
echo "  The SSD16xx driver no longer exposes insmod module parameters."
echo "  All configuration is now done via DRM plane properties at runtime."
echo ""
echo "For property tests, run:"
echo "  ./test_properties.sh"
echo ""
echo "DRM Plane Properties (via modetest -w):"
echo "  - refresh_mode (ID 40): partial/full/fast"
echo "  - color_mode (ID 41): black-white"
echo "  - clear_on_init (ID 42): disabled/partial/full/fast"
echo "  - clear_on_close (ID 43): disabled/partial/full/fast"
echo "  - clear_on_disable (ID 44): disabled/partial/full/fast"
echo "  - refresh_mode_init (ID 45): disabled/partial/full/fast"
echo ""

# Setup logging infrastructure (required by common.sh)
env_setup

# Log the skip
_result "=== SSD16xx Module Parameter Tests ==="
_result ""
_result "SKIP  All tests — Module parameters not applicable in current driver"
_result "      Configuration via DRM plane properties only (see test_properties.sh)"

summary
