# SSD16xx Driver Error Messages Reference

This document lists all error messages that can be printed by the panel-ssd16xx.c driver. Tests should verify these messages do NOT appear during normal operation.

## Error Messages by Function

### 1. ssd16xx_wait_for_panel() - BUSY timeout
**Line**: 498

**Error**: `"Busy wait timed out after %lums\n"`

**Cause**: Panel BUSY pin stayed HIGH beyond timeout (default 5000ms)

**Test pattern**: `"Busy wait timed out"`

---

### 2. ssd16xx_hw_init() - Hardware initialization failure
**Line**: 746

**Error**: `"Hardware initialization failed: %d\n"`

**Cause**: Error during SPI command sequence in hw_init

**Test pattern**: `"Hardware initialization failed"`

---

### 3. ssd16xx_clear_display() - Display clear failure
**Line**: 804

**Error**: `"Clear display failed: %d\n"`

**Cause**: Error during display clear operation (baseline establishment or clear sequence)

**Test pattern**: `"Clear display failed"`

---

### 4. ssd16xx_fb_dirty() - Display update failure
**Line**: 1522

**Error**: `"Display update failed: %d\n"`

**Cause**: Error during framebuffer update sequence (RAM write or display refresh command)

**Test pattern**: `"Display update failed"`

---

### 5. ssd16xx_plane_atomic_update() - PM resume failure
**Line**: 1598

**Error**: `"plane_atomic_update: failed to resume: %d\n"`

**Cause**: pm_runtime_resume_and_get() returned error

**Test pattern**: `"plane_atomic_update: failed to resume"`

---

### 6. ssd16xx_crtc_atomic_enable() - PM resume or HW init failure
**Lines**: 1712, 1731

**Errors**:
- `"crtc_atomic_enable: failed to resume: %d\n"`
- `"crtc_atomic_enable: HW init failed: %d\n"`

**Cause**: PM resume failed or hw_init returned error

**Test pattern**: `"crtc_atomic_enable: failed"`

---

### 7. ssd16xx_crtc_atomic_flush() - PM resume or reinit failure
**Lines**: 1808, 1820

**Errors**:
- `"atomic_flush: failed to resume: %d\n"`
- `"Orientation re-init failed: %d\n"`

**Cause**: PM resume failed or orientation reinit (hw_init) returned error

**Test pattern**: `"atomic_flush: failed|Orientation re-init failed"`

---

### 8. Probe Errors (ssd16xx_probe)
**Lines**: 2283, 2292, 2385, 2389, 2393

**Errors**:
- `"SPI setup failed: %d\n"` (line 2283)
- `"Unknown panel model: %d\n"` (line 2292)
- `"Failed to get RESET GPIO\n"` (line 2385)
- `"Failed to get BUSY GPIO\n"` (line 2389)
- `"Failed to get DC GPIO\n"` (line 2393)

**Test pattern**: `"SPI setup failed|Unknown panel model|Failed to get.*GPIO"`

---

## Comprehensive Error Pattern for Tests

Use this pattern to check for ANY driver error:

```bash
ERROR_PATTERN="Busy wait timed out|Hardware initialization failed|Clear display failed|Display update failed|failed to resume|HW init failed|Orientation re-init failed|SPI setup failed|Unknown panel model|Failed to get.*GPIO"

if dmesg | grep -qE "$ERROR_PATTERN"; then
    fail "Driver errors detected in dmesg"
else
    pass "No driver errors detected"
fi
```

## Recommended Test Pattern Updates

### For Format Tests (test_formats.sh)
```bash
run_format() {
    # ... run kmstest ...

    # Check for errors (primary validation)
    check_dmesg_absent "Display update failed|Busy wait timed out|Hardware initialization failed" \
        "$id: no driver errors"

    # Check for success (secondary validation)
    if dmesg | grep -qE "fb_dirty.*fb="; then
        pass "$id: $desc — fb_dirty completed successfully"
    fi
}
```

### For Property Tests (test_properties.sh)
```bash
# Instead of just checking for "refresh_mode=0", also verify no errors
check_dmesg_absent "Display update failed|Busy wait timed out" "P01: no errors"
check_dmesg "refresh_mode=0" "P01: partial mode set"
```

### For PM Tests (test_pm.sh, test_pm_enhanced.sh)
```bash
# Check runtime PM operations don't produce errors
check_dmesg_absent "failed to resume|Hardware initialization failed" "PM: no resume errors"
```

### For Integration Tests (test_integration.sh)
```bash
# Check full workflows complete without errors
check_dmesg_absent "Display update failed|Clear display failed|failed to resume" \
    "I01: clear_on_init+close workflow error-free"
```

---

## Success Indicators (Optional Secondary Checks)

### fb_dirty Success
**Pattern**: `"fb_dirty: fb=.*refresh_mode=.*orientation="`
**Meaning**: Display update parameters logged successfully

### hw_init Success
**Pattern**: `"hw_init: orientation=.*data_entry="`
**Meaning**: Hardware initialization completed successfully

### Runtime Suspend Success
**Pattern**: `"runtime_suspend: entering deep sleep mode"`
**Meaning**: Runtime PM suspend executed successfully

### System Suspend Success
**Pattern**: `"system suspend: entering deep sleep mode"`
**Meaning**: System PM suspend executed successfully

---

## Test Philosophy

**Primary Validation**: Absence of errors (MUST check)
```bash
check_dmesg_absent "error pattern" "description"
```

**Secondary Validation**: Presence of success indicators (OPTIONAL)
```bash
check_dmesg "success pattern" "description"
```

**Why**: Error absence is definitive proof of success. Success indicators may not always appear due to:
- Debug levels not enabled
- Ring buffer overflow
- Timing issues

But errors will ALWAYS appear (they use `drm_err()` not `drm_dbg()`).

---

## Implementation Priority

1. **HIGH**: Update all tests to check for error absence first
2. **MEDIUM**: Keep existing success pattern checks as secondary validation
3. **LOW**: Add comprehensive error pattern check to summary

### Example Implementation

```bash
# common.sh - Add comprehensive error check
check_no_driver_errors() {
    local desc="${1:-No driver errors}"
    local ERROR_PATTERN="Busy wait timed out|Hardware initialization failed|Clear display failed|Display update failed|failed to resume|HW init failed|Orientation re-init failed"
    check_dmesg_absent "$ERROR_PATTERN" "$desc"
}
```

Then in tests:
```bash
run_format F01 XR24 "XRGB8888 32bpp"
check_no_driver_errors "F01: no errors"  # Primary check
check_dmesg "fb_dirty" "F01: fb_dirty seen"  # Secondary check
```
