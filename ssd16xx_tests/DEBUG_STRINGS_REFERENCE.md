# SSD16xx Driver Debug String Reference

This document lists all debug strings printed by the panel-ssd16xx.c driver, organized by function and purpose. Use this reference when writing or updating test scripts.

## Debug Print Requirements

### Enabling DRM Debug
```bash
# Enable all DRM debug categories (required for drm_dbg() calls)
echo "0xff" > /sys/module/drm/parameters/debug

# Set kernel printk levels to show debug messages in dmesg
echo "2 4 1 8" > /proc/sys/kernel/printk
```

### Dynamic Debug (for dev_dbg)
For PM-related `dev_dbg()` calls, also enable dynamic debug:
```bash
echo "module panel_ssd16xx +p" > /sys/kernel/debug/dynamic_debug/control
```

---

## Debug Strings by Category

### 1. Hardware Initialization (hw_init)

**Location**: `ssd16xx_hw_init()` - line 740

**String**: `"hw_init: orientation=%u° data_entry=0x%02x\n"`

**Example output**:
```
[drm:ssd16xx_hw_init] hw_init: orientation=0° data_entry=0x01
[drm:ssd16xx_hw_init] hw_init: orientation=90° data_entry=0x03
```

**Test usage**: Check for `orientation=` to verify hardware initialization occurred.

---

### 2. Framebuffer Conversion (convert_1bpp)

**Location**: `convert_1bpp()` - line 1226

**String**: `"convert_1bpp: fmt=%p4cc rect=(%d,%d)-(%d,%d) orient=%u° path=%s\n"`

**Path values**:
- `"R1-fast"` - Fast path: R1 format, 0° orientation, byte-aligned X start
- `"portrait"` - Portrait orientations (90° or 270°)
- `"landscape"` - Landscape orientations (0° or 180°)

**Example output**:
```
[drm:convert_1bpp] convert_1bpp: fmt='R  1' rect=(0,0)-(400,300) orient=0° path=R1-fast
[drm:convert_1bpp] convert_1bpp: fmt='XR24' rect=(0,0)-(400,300) orient=90° path=portrait
```

**Test usage**: Check for `R1-fast` to verify optimized R1 format path.

---

### 3. Framebuffer Dirty (fb_dirty)

**Location**: `ssd16xx_fb_dirty()` - lines 1081, 1417

**Strings**:
- `"fb_dirty: fmt=%p4cc %ux%u bw=%s refresh=%d\n"`
  - bw values: `"R1-direct"` (R1 format) or `"color-pixel"` (other formats)
- `"fb_dirty: Sending border waveform: 0x%02x\n"`

**Example output**:
```
[drm:ssd16xx_fb_dirty] fb_dirty: fmt='R  1' 400x300 bw=R1-direct refresh=0
[drm:ssd16xx_fb_dirty] fb_dirty: fmt='XR24' 400x300 bw=color-pixel refresh=1
[drm:ssd16xx_fb_dirty] fb_dirty: Sending border waveform: 0x09
```

**Test usage**: Check for `fb_dirty` to verify display updates occurred.

---

### 4. Plane Update (plane_atomic_update)

**Location**: `ssd16xx_plane_atomic_update()` - lines 1582, 1592, 1607, 1610

**Strings**:
- `"plane_atomic_update: fb=%p\n"`
- `"plane_atomic_update: skipping (reinit pending)\n"`
- `"plane_atomic_update: no damage, using full screen\n"`
- `"plane_atomic_update: calling fb_dirty rect=(%d,%d)-(%d,%d)\n"`

**Example output**:
```
[drm:ssd16xx_plane_atomic_update] plane_atomic_update: fb=ffff00008e3c1800
[drm:ssd16xx_plane_atomic_update] plane_atomic_update: calling fb_dirty rect=(0,0)-(400,300)
```

**Test usage**: Check for `plane_atomic_update` to verify atomic commits.

---

### 5. CRTC Enable/Disable (crtc_atomic_enable/disable)

**Location**:
- `ssd16xx_crtc_atomic_enable()` - line 1717
- `ssd16xx_crtc_atomic_disable()` - line 1746

**Strings**:
- `"atomic_enable: %dx%d\n"`
- `"atomic_disable: clearing display%s\n"` (with optional ", mode=%d" suffix)

**Example output**:
```
[drm:ssd16xx_crtc_atomic_enable] atomic_enable: 400x300
[drm:ssd16xx_crtc_atomic_disable] atomic_disable: clearing display, mode=1
```

**Test usage**: Check for `atomic_enable` / `atomic_disable` to verify CRTC state changes.

---

### 6. CRTC Flush / Reinit (crtc_atomic_flush)

**Location**: `ssd16xx_crtc_atomic_flush()` - line 1815

**String**: `"atomic_flush: reinit, orientation=%u°\n"`

**Example output**:
```
[drm:ssd16xx_crtc_atomic_flush] atomic_flush: reinit, orientation=90°
```

**Test usage**: Check for `atomic_flush: reinit` to verify orientation changes triggered hardware reinit.

---

### 7. Connector Modes (connector_get_modes)

**Location**: `ssd16xx_connector_get_modes()` - line 1861

**String**: `"connector_get_modes: orientation=%u°\n"`

**Example output**:
```
[drm:ssd16xx_connector_get_modes] connector_get_modes: orientation=0°
[drm:ssd16xx_connector_get_modes] connector_get_modes: orientation=90°
```

**Test usage**: Check for `connector_get_modes` to verify mode queries.

---

### 8. Property Get/Set (connector_get_property / connector_set_property)

**Location**: Lines 2024, 2070

**Strings**:
- `"get_property: %s\n"`
- `"set_property: %s = %llu\n"`

**Example output**:
```
[drm:ssd16xx_connector_get_property] get_property: rotation
[drm:ssd16xx_connector_set_property] set_property: rotation = 90
[drm:ssd16xx_connector_set_property] set_property: refresh_mode = 1
```

**Test usage**: Check for `set_property` to verify property changes.

---

### 9. Clear Display Operations

**Location**: `ssd16xx_clear_display()` - lines 781, 796, 826, 842

**Strings**:
- `"clear_display: Set border init waveform: 0x%02x\n"`
- `"clear_display: Restored border update waveform: 0x%02x\n"`
- `"clear_on_init: running, mode=%d\n"`
- `"clear_on_close: running, mode=%d\n"`

**Example output**:
```
[drm:ssd16xx_clear_display] clear_display: Set border init waveform: 0x01
[drm:ssd16xx_clear_on_init] clear_on_init: running, mode=1
[drm:ssd16xx_clear_on_close] clear_on_close: running, mode=0
```

**Test usage**: Check for `clear_on_init` / `clear_on_close` to verify clear operations.

---

### 10. Power Management (Runtime PM)

**Location**: Lines 2555, 2571

**Strings** (drm_dbg):
- `"runtime_suspend: entering deep sleep mode 0x%02x\n"`
- `"runtime_resume: initialized=%d%s\n"` (suffix: ` (re-init required)` or empty)

**Example output**:
```
[drm:ssd16xx_pm_runtime_suspend] runtime_suspend: entering deep sleep mode 0x01
[drm:ssd16xx_pm_runtime_resume] runtime_resume: initialized=1
```

**Test usage**: Check for `runtime_suspend` / `runtime_resume` to verify PM transitions.

---

### 11. Power Management (System Suspend)

**Location**: Lines 2499, 2521, 2539

**Strings** (dev_dbg - requires dynamic debug):
- `"system suspend: entering deep sleep mode 0x%02x\n"`
- `"system suspend: upgrading MODE_1 -> MODE_2 via HWRESET\n"`
- `"system resume: restoring state\n"`

**Example output**:
```
panel-ssd16xx spi1.0: system suspend: entering deep sleep mode 0x03
panel-ssd16xx spi1.0: system suspend: upgrading MODE_1 -> MODE_2 via HWRESET
panel-ssd16xx spi1.0: system resume: restoring state
```

**Test usage**: Check for `deep sleep mode 0x03` to verify system suspend used MODE_2.

---

### 12. Probe / Initialization

**Location**: Lines 2361, 2372, 2375, 2464

**Strings**:
- `"Using %s orientation (%u°, %ux%u logical)\n"` (source: "DT" or "module param")
- `"Mode dimensions swapped for portrait: %ux%u\n"`
- `"Mode dimensions unchanged: %ux%u\n"`
- `"SSD16xx e-paper display initialized (%dx%d, %d° rotation)\n"`

**Example output**:
```
[drm:ssd16xx_probe] Using DT orientation (0°, 400x300 logical)
[drm:ssd16xx_probe] Mode dimensions unchanged: 400x300
[drm:ssd16xx_probe] SSD16xx e-paper display initialized (400x300, 0° rotation)
```

**Test usage**: Check for `e-paper display initialized` to verify successful probe.

---

## Common Test Patterns

### Check for orientation change:
```bash
if dmesg | grep -qE "orientation=(90|270)"; then
    pass "Portrait orientation confirmed"
fi
```

### Check for R1 fast path:
```bash
if dmesg | grep -qE "R1-fast"; then
    pass "R1 fast path active"
elif dmesg | grep -qE "fb_dirty.*R1-direct"; then
    pass "R1 format accepted (standard path)"
fi
```

### Check for PM transitions:
```bash
if dmesg | grep -qE "runtime_suspend|deep sleep mode 0x01"; then
    pass "Runtime suspend to MODE_1"
fi

if dmesg | grep -qE "system suspend.*0x03|deep sleep mode 0x03"; then
    pass "System suspend to MODE_2"
fi
```

### Check for reinit on orientation change:
```bash
if dmesg | grep -qE "atomic_flush: reinit.*orientation"; then
    pass "Hardware reinit triggered by orientation change"
fi
```

---

## Notes

1. **DRM debug categories** (0xff enables all):
   - 0x01: DRM_UT_CORE (core DRM)
   - 0x02: DRM_UT_DRIVER (driver-specific, **most ssd16xx messages**)
   - 0x04: DRM_UT_KMS (KMS/atomic)
   - 0x08: DRM_UT_PRIME (PRIME/dma-buf)
   - 0x10: DRM_UT_ATOMIC (atomic commit details)
   - 0x20: DRM_UT_VBL (vblank)
   - 0x40: DRM_UT_STATE (state debugging)
   - 0x80: DRM_UT_LEASE (leasing)

2. **drm_dbg() vs dev_dbg()**:
   - `drm_dbg()` → DRM debug system (enabled via `/sys/module/drm/parameters/debug`)
   - `dev_dbg()` → Dynamic debug or CONFIG_DEBUG (enabled via dynamic_debug control)

3. **Test best practices**:
   - Always `dmesg -C` before the operation you want to test
   - Use `grep -qE` for pattern matching (extended regex)
   - Check for absence of errors: `grep -qE "error|BUG|WARNING"`
   - For PM tests, verify both the debug message AND sysfs `runtime_status`
