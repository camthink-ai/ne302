# Sensor Test Guide

This document describes how to test the SensorExt sensors on the NE302 platform via the debug CLI (AICAM shell). It is based on actual test logs and covers the I2C bus scan, sensor example (sexp) modes, and expected outputs.

---

## 1. Prerequisites

- I2C bus 1 connected to the sensor expansion board
- **TFT vs. Audio (mutually exclusive):** TFT and NAU881x audio share the same peripheral (SPI6/I2S6). Only one can be connected at a time via hardware resistor soldering options. Use TFT for `sexp` modes; use NAU881x for audio tests.
- TFT ST7789V display connected via SPI6 (for sexp modes; requires TFT option populated)
- Camera pipe2 already configured and running (for `sexp start` non-IR mode only)

---

## 2. I2C Bus Scan

Use `i2c_tool detect` to scan for I2C devices on the bus. This verifies sensor presence and I2C addresses before running the sensor example.

### Command

```
i2c_tool detect [bus] [start_addr] [end_addr]
```

| Parameter    | Default | Description                              |
|-------------|---------|------------------------------------------|
| bus         | 1       | I2C bus number (only bus 1 supported)    |
| start_addr  | 0x03    | Start 7-bit address (hex or decimal)     |
| end_addr    | 0x77    | End 7-bit address (hex or decimal)       |

### Example Output

```
AICAM> i2c_tool detect
Scanning I2C bus 1, address range 0x03-0x77
      00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00:          -- -- -- -- -- -- -- -- -- -- -- -- -- 
10: -- -- -- -- -- -- -- -- -- -- 1a -- -- -- -- -- 
20: -- -- 22 -- -- -- -- -- -- 29 -- -- -- -- -- -- 
30: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 
40: -- -- -- -- 44 -- -- -- -- -- -- -- -- -- -- -- 
50: -- 51 -- -- -- -- -- -- -- -- -- -- -- -- -- -- 
60: -- -- -- -- -- -- 66 -- -- -- 6a -- -- -- -- -- 
70: -- -- -- -- -- -- -- --                        
```

### I2C Address Map (from scan)

| Addr (7-bit) | Device      | Description                          |
|--------------|-------------|--------------------------------------|
| 0x1a         | NAU881x     | Audio codec                          |
| 0x22         | LTR-31x     | Ambient light sensor (ALS)           |
| 0x29         | VL53L1X     | ToF distance sensor                  |
| 0x44         | SHT3x       | Temperature & humidity sensor        |
| 0x51         | DTS6012M    | TOF LIDAR distance sensor            |
| 0x66         | MLX90642    | Thermal IR array (32×24 pixels)      |
| 0x6a         | LSM6DSR     | IMU (accelerometer + gyroscope)      |

If any sensor is missing, verify hardware connections and power. `--` means no device responded at that address.

---

## 3. Sensor Example (sexp) Commands

The `sexp` command provides an integrated sensor demo that initializes the TFT display and all supported sensors, then displays camera preview or thermal image.

### 3.1 Start Camera Preview Mode

```
sexp start
```

**Behavior:**

- Initializes TFT ST7789VW on SPI6 (240×240)
- Initializes all sensors on I2C port 1
- Runs preview thread: camera pipe2 (256×256, RGB888) → center crop → TFT
- Runs sensor read thread (reads SHT3x, ALS, LSM6DSR, VL53L1X, DTS6012M periodically)
- Runs MLX90642 thermal read thread

**Expected Log Output (success):**

```
[DRIVER] ST7789VW: init start (SPI6)
[DRIVER] ST7789VW: init done, 240x240
[DRIVER] sexp: Initializing sensors...
[DRIVER] sexp: SHT3x initialized (periodic 2Hz)
[DRIVER] sexp: ALS initialized
[DRIVER] lsm6dsr: init OK (addr=0x6A, id=0x6B, default: acc=12.5Hz/±2g, gyro=12.5Hz/±2000dps)
[DRIVER] sexp: LSM6DSR initialized
[DRIVER] vl53l1x: Sensor ID = 0xEACC (Model ID 0xEA verified, continuing)
[DRIVER] vl53l1x: init OK (addr=0x29, sensor_id=0xEACC)
[DRIVER] sexp: VL53L1X initialized
[DRIVER] dts6012m: init OK (addr=0x51)
[DRIVER] sexp: DTS6012M initialized
[DRIVER] mlx90642: init OK (addr=0x66)  rate=2Hz  emissivity=0x3D71  Treflected=-136 (-1.36 C)
[DRIVER] sexp: MLX90642 initialized
[DRIVER] sexp: started (pipe2 256x256 bpp=3 -> TFT 240x240)
```

**Note:** Camera pipe2 must already be started by the application before running `sexp start`.

---

### 3.2 Start IR Thermal Mode

```
sexp start ir
```

**Behavior:**

- Same TFT and sensor init as `sexp start`
- Displays **thermal image only** (no camera); each 4×4 thermal cell maps to one TFT pixel
- Thermal grid: 32×24 cells → 240×240 display
- No dependency on camera pipe2

**Expected Log Output (success):**

```
[DRIVER] ST7789VW: init start (SPI6)
[DRIVER] ST7789VW: init done, 240x240
[DRIVER] sexp: Initializing sensors...
[DRIVER] sexp: SHT3x initialized (periodic 2Hz)
[DRIVER] sexp: ALS initialized
[DRIVER] lsm6dsr: init OK (addr=0x6A, id=0x6B, default: acc=12.5Hz/±2g, gyro=12.5Hz/±2000dps)
[DRIVER] sexp: LSM6DSR initialized
[DRIVER] vl53l1x: init OK (addr=0x29, sensor_id=0xEACC)
[DRIVER] sexp: VL53L1X initialized
[DRIVER] dts6012m: init OK (addr=0x51)
[DRIVER] sexp: DTS6012M initialized
[DRIVER] mlx90642: init OK (addr=0x66)  rate=2Hz  emissivity=0x3D71  Treflected=-134 (-1.34 C)
[DRIVER] sexp: MLX90642 initialized
[DRIVER] sexp: started in IR mode (TFT 240x240, thermal image 32x24 cells 4x4)
```

---

### 3.3 Stop Sensor Example

```
sexp stop
```

**Behavior:**

- Stops preview/thermal thread and sensor threads
- Releases TFT and other resources
- Does **not** stop camera or pipe2

**Expected Log Output:**

```
[DRIVER] sexp: stopped, resources released (camera/pipe2 unchanged)
```

---

## 4. Sensor Init Verification Summary

| Sensor   | Addr | Init Success Log                                       |
|----------|------|--------------------------------------------------------|
| SHT3x    | 0x44 | `sexp: SHT3x initialized (periodic 2Hz)`               |
| LTR-31x  | 0x22 | `sexp: ALS initialized`                                |
| LSM6DSR  | 0x6a | `lsm6dsr: init OK (addr=0x6A, id=0x6B, ...)`           |
| VL53L1X  | 0x29 | `vl53l1x: init OK (addr=0x29, sensor_id=0xEACC)`       |
| DTS6012M | 0x51 | `dts6012m: init OK (addr=0x51)`                        |
| MLX90642 | 0x66 | `mlx90642: init OK (addr=0x66) rate=2Hz emissivity=...`|
| ST7789VW | SPI6 | `ST7789VW: init done, 240x240`                         |

---

## 5. Individual Sensor Commands (Optional)

For deeper testing, use the per-sensor commands. See `sensor_exemple.h` for full reference.

| Command                  | Description                                      |
|--------------------------|--------------------------------------------------|
| `als init` / `als read`  | LTR-31x ALS init and read ALS/IR counts          |
| `sht3x init [addr]`      | SHT3x init (0x44 or 0x45)                        |
| `mlx90642 init [addr]`   | MLX90642 init, measure, dump, rate, emissivity   |
| `dts6012m init [addr]`   | DTS6012M init, read, laser on/off                |

---

## 6. Troubleshooting

| Symptom                      | Possible Cause                             | Action                                   |
|-----------------------------|--------------------------------------------|------------------------------------------|
| `--` at expected addr       | Sensor not connected or wrong address      | Check wiring, power, I2C pull-ups         |
| `sexp: SHT3x init failed`   | SHT3x missing or bus fault                 | Run `i2c_tool detect`, verify 0x44        |
| `sexp: ALS init failed`     | LTR-31x missing                            | Verify 0x22 in I2C scan                   |
| `sexp: LSM6DSR init failed` | LSM6DSR at 0x6a or 0x6b not detected       | Check SA0 pin, verify I2C address         |
| `sexp: VL53L1X init failed` | VL53L1X not found                          | Verify 0x29, check XSHUT and power        |
| `sexp: DTS6012M init failed`| DTS6012M not found                         | Verify 0x51, ensure laser power OK        |
| `sexp: MLX90642 init failed`| MLX90642 not found                         | Verify 0x66, check power and SDA/SCL      |
| `TFT init failed`           | SPI6 or display connection issue           | Check SPI6 wiring and display power       |

---

## 7. Test Sequence (Recommended)

1. `i2c_tool detect` – confirm all expected sensors appear.
2. `sexp start ir` – verify TFT and thermal image without camera.
3. `sexp stop` – confirm clean stop.
4. (Optional) `sexp start` – verify camera preview mode if pipe2 is running.
5. (Optional) `sexp stop` – release resources.

---

## 8. Audio Test (NAU881x)

### 8.1 Hardware Conflict: TFT vs. Audio

> **Important:** The TFT display (ST7789VW) and the audio codec (NAU881x) are **mutually exclusive**. Only one can be connected at a time.
>
> Both use the same MCU peripheral (SPI6/I2S6) and share the same pins. The hardware uses **resistor soldering options** to select which device is wired: either the TFT or the NAU881x, but not both.

---

### 8.2 Test Sequence

**Prerequisites:** NAU881x populated (TFT not connected), I2C port 1, file system for record/play.

1. **Init audio (I2S6 + codec):**
   ```
   nau881x audio_init
   ```
   Expected: `nau881x: audio_init OK (I2S6 + codec ready)`

2. **Test playback (C major scale):**
   ```
   nau881x play_scale
   nau881x play_scale -15
   ```
   Optional `-15` sets volume in dB. Expected output:
   ```
   nau881x: play_scale: Do(C4)
   nau881x: play_scale: Re(D4)
   ... Mi, Fa, Sol, La, Si ...
   nau881x: play_scale: Do(C5)
   nau881x: play_scale: done
   ```

3. **Test play from file:**
   ```
   nau881x play dx.wav -15
   nau881x play_stop
   ```
   Use `play_stop` to stop playback early. WAV must be 16 kHz, 16-bit, mono or stereo.

4. **Test record:**
   ```
   nau881x record test.wav 10 32
   ```
   Records 10 s to `test.wav` with PGA gain 32. If `record timeout` occurs on first run, retry once.
   ```
   nau881x: recording 10 s to test.wav (pga_gain=32)
   nau881x: record done, 160000 samples
   ```

5. **Verify recording by playback:**
   ```
   nau881x play test.wav
   ```

---

### 8.3 Troubleshooting

| Symptom | Action |
|---------|--------|
| `0x1a` not in I2C scan | Check resistor soldering for audio option |
| `audio_init failed` | Ensure hardware is in audio mode, not TFT |
| `record timeout` on first record | Retry; may occur if previous play/record left I2S busy |
| No sound | Run `nau881x` with no args to see register commands (pga_input, spk_en, etc.) |
