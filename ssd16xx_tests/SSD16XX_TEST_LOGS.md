# SSD16xx Driver Test Logs

Driver: `panel-ssd16xx`
Panel: GDEY042T81 (400×300, SSD1683 controller)

---

## Session Info

```
Board        :
Kernel       :
Driver commit:
Date         :
Tester       :
```

---

## Step 0 — Environment

### modetest output
```
(paste full modetest -M ssd16xx output here)
```

### Resolved IDs
```
Connector id :
CRTC id      :
Plane id     :
Module path  :
Device path  :
```

---

## 1 — DRM Pixel Formats

### F01: XRGB8888
```bash
$ modetest -M ssd16xx -s $C@$CRTC:$MODE -P $P@$CRTC:${MODE}+0+0@XR24
```
**dmesg:**
```
(paste here)
```
**Display observation:**

---

### F02: RGB888
```bash
$ modetest -M ssd16xx -s $C@$CRTC:$MODE -P $P@$CRTC:${MODE}+0+0@RG24
```
**dmesg:**
```
(paste here)
```
**Display observation:**

---

### F03: RGB565
```bash
$ modetest -M ssd16xx -s $C@$CRTC:$MODE -P $P@$CRTC:${MODE}+0+0@RG16
```
**dmesg:**
```
(paste here)
```
**Display observation:**

---

### F04: R8
```bash
$ modetest -M ssd16xx -s $C@$CRTC:$MODE -P $P@$CRTC:${MODE}+0+0@R8
```
**dmesg:**
```
(paste here)
```
**Display observation:**

---

### F05: NV12
```bash
$ modetest -M ssd16xx -s $C@$CRTC:$MODE -P $P@$CRTC:${MODE}+0+0@NV12
```
**dmesg:**
```
(paste here)
```
**Display observation:**

---

### F06: NV16
```bash
$ modetest -M ssd16xx -s $C@$CRTC:$MODE -P $P@$CRTC:${MODE}+0+0@NV16
```
**dmesg:**
```
(paste here)
```
**Display observation:**

---

### F07: YUYV
```bash
$ modetest -M ssd16xx -s $C@$CRTC:$MODE -P $P@$CRTC:${MODE}+0+0@YUYV
```
**dmesg:**
```
(paste here)
```
**Display observation:**

---

### F08: UYVY
```bash
$ modetest -M ssd16xx -s $C@$CRTC:$MODE -P $P@$CRTC:${MODE}+0+0@UYVY
```
**dmesg:**
```
(paste here)
```
**Display observation:**

---

### F09: R1
```bash
$ dmesg | grep "R1-fast\|R1-direct"
```
**dmesg:**
```
(paste here)
```
**Display observation:**

---

## 2 — DRM Connector Properties

### P01: refresh_mode=0 (partial)
```bash
$ modetest -M ssd16xx -w $C:refresh_mode:0
$ kmstest -M ssd16xx
$ dmesg | grep "refresh_mode=0\|PARTIAL"
```
**dmesg:**
```
(paste here)
```
**Update time observed:**
**Display observation:**

---

### P02: refresh_mode=1 (full)
```bash
$ modetest -M ssd16xx -w $C:refresh_mode:1
$ kmstest -M ssd16xx
$ dmesg | grep "refresh_mode=1\|FULL"
```
**dmesg:**
```
(paste here)
```
**Update time observed:**
**Display observation:**

---

### P03: refresh_mode=2 (fast)
```bash
$ modetest -M ssd16xx -w $C:refresh_mode:2
$ kmstest -M ssd16xx
$ dmesg | grep "refresh_mode=2\|FAST"
```
**dmesg:**
```
(paste here)
```
**Update time observed:**
**Display observation:**

---

### P04: rotation=0 (landscape)
```bash
$ modetest -M ssd16xx -w $C:rotation:0
$ kmstest -M ssd16xx
$ dmesg | grep "orientation=0\|landscape"
$ modetest -M ssd16xx -c | grep "400x300"
```
**dmesg:**
```
(paste here)
```
**Display observation:**

---

### P05: rotation=90 (portrait)
```bash
$ modetest -M ssd16xx -w $C:rotation:90
$ kmstest -M ssd16xx
$ dmesg | grep "orientation=90\|portrait"
$ modetest -M ssd16xx -c | grep "300x400"
```
**dmesg:**
```
(paste here)
```
**Display observation:**

---

### P06: rotation change at runtime
```bash
$ modetest -M ssd16xx -w $C:rotation:0  && kmstest -M ssd16xx
$ modetest -M ssd16xx -w $C:rotation:90 && kmstest -M ssd16xx
$ dmesg | grep "reinit\|atomic_flush"
```
**dmesg:**
```
(paste here)
```
**Display observation:**

---

### P07: border_waveform_init=9 (HiZ)
```bash
$ modetest -M ssd16xx -w $C:border_waveform_init:9
$ modetest -M ssd16xx -w $C:rotation:90 && kmstest -M ssd16xx
```
**Display observation (border):**

---

### P08: border_waveform_init=1 (LUT1 white flash)
```bash
$ modetest -M ssd16xx -w $C:border_waveform_init:1
$ modetest -M ssd16xx -w $C:rotation:90 && kmstest -M ssd16xx
```
**Display observation (border):**

---

### P09: border_waveform_init=6 (VSL force white)
```bash
$ modetest -M ssd16xx -w $C:border_waveform_init:6
$ modetest -M ssd16xx -w $C:rotation:90 && kmstest -M ssd16xx
```
**Display observation (border):**

---

### P10: border_waveform_init=5 (VSH1 force black)
```bash
$ modetest -M ssd16xx -w $C:border_waveform_init:5
$ modetest -M ssd16xx -w $C:rotation:90 && kmstest -M ssd16xx
```
**Display observation (border):**

---

### P11: border_refresh_every_update=1 + border_waveform_refresh=8
```bash
$ modetest -M ssd16xx -w $C:border_refresh_every_update:1
$ modetest -M ssd16xx -w $C:border_waveform_refresh:8
$ kmstest -M ssd16xx && kmstest -M ssd16xx && kmstest -M ssd16xx
```
**dmesg:**
```
(paste here)
```
**Display observation (border after 3 updates):**

---

### P12: border_refresh_every_update=0 (disabled)
```bash
$ modetest -M ssd16xx -w $C:border_refresh_every_update:0
$ kmstest -M ssd16xx && kmstest -M ssd16xx && kmstest -M ssd16xx
```
**Display observation:**

---

## 3 — Module Parameters

### M01: rotation=0
```bash
$ insmod $BASE rotation=0
$ kmstest -M ssd16xx
$ dmesg | grep "orientation=0"
```
**dmesg:**
```
(paste here)
```

---

### M02: rotation=90
```bash
$ insmod $BASE rotation=90
$ kmstest -M ssd16xx
$ dmesg | grep "orientation=90"
$ modetest -M ssd16xx -c | grep "300x400"
```
**dmesg:**
```
(paste here)
```

---

### M03: refresh_mode=1
```bash
$ insmod $BASE refresh_mode=1
$ kmstest -M ssd16xx
$ dmesg | grep "Refresh mode: full"
```
**dmesg:**
```
(paste here)
```

---

### M04: refresh_mode=2
```bash
$ insmod $BASE refresh_mode=2
$ kmstest -M ssd16xx
$ dmesg | grep "Refresh mode: fast"
```
**dmesg:**
```
(paste here)
```

---

### M05: clear_on_init=1
```bash
$ insmod $BASE clear_on_init=1
$ kmstest -M ssd16xx
$ dmesg | grep "clear_on_init\|clear_display"
```
**dmesg:**
```
(paste here)
```

---

### M06: clear_on_init=0
```bash
$ insmod $BASE clear_on_init=0
$ kmstest -M ssd16xx
$ dmesg | grep "clear_on_init\|clear_display"
```
**dmesg:**
```
(paste here)
```

---

### M07: clear_on_close=1
```bash
$ insmod $BASE clear_on_close=1
$ kmstest -M ssd16xx
$ rmmod panel_ssd16xx
$ dmesg | grep "clear_on_close\|clear_display"
```
**dmesg:**
```
(paste here)
```

---

### M08: clear_on_close=0
```bash
$ insmod $BASE clear_on_close=0
$ kmstest -M ssd16xx
$ rmmod panel_ssd16xx
$ dmesg | grep "clear_on_close\|clear_display"
```
**dmesg:**
```
(paste here)
```

---

### M09: refresh_mode_init=0
```bash
$ insmod $BASE refresh_mode_init=0
$ kmstest -M ssd16xx
$ dmesg | grep "partial_mode_ready\|baseline"
```
**dmesg:**
```
(paste here)
```

---

### M10: refresh_mode_init=2
```bash
$ insmod $BASE refresh_mode_init=2
$ kmstest -M ssd16xx
$ dmesg | grep "Refresh mode: fast\|partial_mode_ready"
```
**dmesg:**
```
(paste here)
```

---

### M11: border_waveform_init_lut=1
```bash
$ insmod $BASE border_waveform_init_lut=1
$ kmstest -M ssd16xx
$ dmesg | grep "border_waveform"
```
**dmesg:**
```
(paste here)
```
**Display observation (border):**

---

### M12: border_refresh_on_every_update=1 + border_waveform_lut=8
```bash
$ insmod $BASE border_refresh_on_every_update=1 border_waveform_lut=8
$ kmstest -M ssd16xx && kmstest -M ssd16xx && kmstest -M ssd16xx
$ dmesg | grep "border"
```
**dmesg:**
```
(paste here)
```
**Display observation:**

---

## 4 — DT Properties

### D01: rotation=0 in overlay
```
Overlay used:
```
```bash
$ dmesg | grep "orientation\|rotation"
$ modetest -M ssd16xx -c | grep "400x300"
```
**dmesg:**
```
(paste here)
```

---

### D02: rotation=90 in overlay
```
Overlay used:
```
```bash
$ dmesg | grep "orientation\|rotation"
$ modetest -M ssd16xx -c | grep "300x400"
```
**dmesg:**
```
(paste here)
```

---

### D03: module param overrides DT rotation
```bash
$ insmod $BASE rotation=90
$ dmesg | grep "orientation=90"
```
**dmesg:**
```
(paste here)
```

---

## 5 — Power Management

### PM01: runtime suspend
```bash
$ kmstest -M ssd16xx &; sleep 5; kill %1; sleep 3
$ cat $DEV/power/runtime_status
$ dmesg | grep "runtime suspend\|atomic_disable"
```
**runtime_status:**
**dmesg:**
```
(paste here)
```

---

### PM02: runtime resume
```bash
$ kmstest -M ssd16xx &; sleep 2
$ cat $DEV/power/runtime_status
$ dmesg | grep "runtime resume\|atomic_enable"
```
**runtime_status:**
**dmesg:**
```
(paste here)
```

---

### PM03: runtime resume — Mode 1 (RAM retained)
```bash
$ dmesg | grep "deep_sleep_mode=0x01\|partial_mode_ready"
```
**dmesg:**
```
(paste here)
```

---

### PM04+PM05: system suspend/resume
```bash
$ echo mem > /sys/power/state
(wake board)
$ dmesg | grep "system suspend\|system resume\|deep_sleep_mode"
```
**dmesg:**
```
(paste here)
```
**Display observation after resume:**

---

### PM06: system suspend — Mode 2 path
```bash
$ dmesg | grep "deep_sleep_mode=0x03"
$ dmesg | grep "HW init\|atomic_enable"
```
**dmesg:**
```
(paste here)
```

---

## 6 — Integration Tests

### I01: clear_on_init=1 + clear_on_close=1
```bash
$ insmod $BASE clear_on_init=1 clear_on_close=1
$ kmstest -M ssd16xx
$ rmmod panel_ssd16xx
$ dmesg | grep "clear_display\|clear_on"
```
**dmesg:**
```
(paste here)
```
**Display observation:**

---

### I02: refresh_mode_init + subsequent partials
```bash
$ insmod $BASE refresh_mode_init=0
$ kmstest -M ssd16xx && kmstest -M ssd16xx
$ dmesg | grep "partial_mode_ready\|baseline\|PARTIAL"
```
**dmesg:**
```
(paste here)
```

---

### I03: rotation change after PM suspend/resume
```bash
$ kmstest -M ssd16xx &
$ modetest -M ssd16xx -w $C:rotation:90 && kmstest -M ssd16xx
$ kill %1; sleep 3
$ kmstest -M ssd16xx &; sleep 2
$ dmesg | grep "orientation=90\|partial_mode_ready"
```
**dmesg:**
```
(paste here)
```
**Display observation:**

---

### I04: ghosting cleared by full after fast
```bash
$ modetest -M ssd16xx -w $C:refresh_mode:2
$ kmstest -M ssd16xx && kmstest -M ssd16xx && kmstest -M ssd16xx
(observe ghosting)
$ modetest -M ssd16xx -w $C:refresh_mode:1
$ kmstest -M ssd16xx
```
**Display observation before full refresh:**
**Display observation after full refresh:**
