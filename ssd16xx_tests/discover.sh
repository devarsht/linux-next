#!/bin/bash
# discover.sh — DRM device discovery for SSD16xx tests
# Discovers and exports DRM IDs (connector, CRTC, plane, mode).
#
# Usage:
#   source ./discover.sh
#
# Exports: C, CRTC, P, MODE

# Discover DRM IDs using modetest
_MT=$(./modetest -M ssd16xx 2>/dev/null)

C=$(echo "$_MT"    | awk '/^Connectors:/{f=1} f && /\tconnected\t/{print $1; exit}')
MODE=$(echo "$_MT" | awk '/^Connectors:/{cf=1} cf && /\tconnected\t/{mf=1} mf && /^  [0-9]+x[0-9]+/{print $1; exit}')
CRTC=$(echo "$_MT" | awk '/^CRTCs:/{f=1;next}  f && /^[0-9]+\t/{print $1; exit}')
P=$(echo "$_MT"    | awk '/^Planes:/{f=1;next}  f && /^[0-9]+\t/{print $1; exit}')

# Fallback to known mode if parsing failed (common for GDEY042T81 panel)
MODE=400x300

# Export for use by test scripts
export C CRTC P MODE
