# ESPFanManager v2

ESP32-S3 multi-channel smart fan controller with web dashboard, REST API, and persistent configuration.

**Requires ESP-IDF v6.0.1+**

## Quick Start

```powershell
# 1. Activate ESP-IDF
& 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'

# 2. Configure WiFi credentials
idf.py menuconfig   # ESPFanManager Configuration -> WiFi SSID / Password

# 3. Build, flash, monitor
idf.py build flash monitor
```

After boot, the device starts in AP+STA mode. If STA fails, it falls back to open AP at `192.168.4.1`. Open a browser to the device IP — the dashboard loads from flash.

## Features

- **Multi-channel** — up to 8 PWM fans with independent control
- **Temperature-driven** — NTC thermistor, DS18B20 1-Wire, or manual (API-fed)
- **Fan curves** — per-fan temp-to-duty lookup with linear interpolation (up to 16 curves, 10 points each)
- **Schedules** — time-of-day duty overrides with overnight wrap (up to 8 rules)
- **WiFi AP+STA** — open AP fallback when STA fails, scan & connect from dashboard
- **REST API** — full CRUD for fans, sources, curves, schedules (22 endpoints)
- **Persistent config** — LittleFS-backed `config.pb` (Protobuf) auto-saved on every change
- **Dashboard SPA** — single-file HTML served from flash, 5 tabs, real-time polling

---

## Hardware

### Supported Peripherals

| Peripheral | Driver | Max | Notes |
|------------|--------|-----|-------|
| PWM fan output | LEDC (25 kHz, 11-bit) | 8 channels | N-channel MOSFET, GPIO 0-48 |
| Tachometer input | PCNT pulse counter | 4 units | Open-drain hall sensor, 10k pull-up to 3.3V |
| NTC thermistor | ADC1 oneshot | — | 10k NTC + 10k divider, Beta-equation conversion |
| DS18B20 1-Wire | RMT-based 1-Wire | 8 sensors per bus | 750ms conversion, blocking read |

### GPIO Constraints (ESP32-S3)

| GPIO | Status | Use |
|------|--------|-----|
| 0 | Reserved | Strapping pin (boot mode) |
| 3 | Reserved | Strapping pin (JTAG) |
| 19, 20 | USB D-/D+ | USB-serial-JTAG (avoid for fans) |
| 26-32 | PSRAM | Octal PSRAM (flash-dependent) |
| 33-37 | PSRAM | Octal PSRAM (flash-dependent) |
| 45, 46 | Reserved | Strapping / VDD_SPI |
| All others | Available | PWM, tach, ADC, 1-Wire |

Constraints enforced by `f_constraints`: GPIO 0-48, duty 0-100%, mode 0-1, temp -40 to +125 C, schedule 0-1439 min.

---

## Architecture

```
+-- HAL (Hardware Abstraction) -------+
|  f_ledc      LEDC PWM (25 kHz, 11b) |
|  f_pcnt      PCNT pulse counter     |
|  f_adc       ADC1 oneshot (NTC)     |
|  f_ds18b20   RMT 1-Wire driver      |
|  f_gpio      GPIO capability reg.   |
|  f_wifi      APSTA + SNTP + NVS     |
+-- Registry (Domain Model) ----------+
|  f_fan       Fan channel registry   |
|  f_source    Temp source registry   |
|  f_curve     Fan curve registry     |
|  f_schedule  Schedule ruleset       |
+-- Business Logic -------------------+
|  f_control   1 Hz loop: src -> curve|
|              -> hysteresis -> ramp  |
|              -> duty + diagnostics  |
|  f_config    LittleFS (Protobuf)    |
|  f_constr    Input validation       |
+-- Presentation ---------------------+
|  f_http      REST API + static file |
|  index.html  Dashboard SPA          |
+-------------------------------------+
```

---

## API Reference

All responses use the envelope format:

**Success:**
```json
{"status": "ok", "data": { ... }}
```

**Error:**
```json
{"status": "error", "error": {"code": 400, "message": "description"}}
```

### Endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/v1/fans` | List all fans |
| GET | `/api/v1/fans/:id` | Get single fan |
| PUT | `/api/v1/fans` | Create fan |
| PUT | `/api/v1/fans/:id` | Update fan |
| DELETE | `/api/v1/fans/:id` | Remove fan |
| GET | `/api/v1/sources` | List sources |
| PUT | `/api/v1/sources` | Create source |
| DELETE | `/api/v1/sources/:id` | Remove source |
| POST | `/api/v1/sources/temp` | Set manual source temp |
| GET | `/api/v1/curves` | List curves |
| GET | `/api/v1/curves/:id` | Get single curve |
| PUT | `/api/v1/curves` | Create curve |
| PUT | `/api/v1/curves/:id` | Update curve |
| DELETE | `/api/v1/curves/:id` | Remove curve |
| GET | `/api/v1/schedules` | List schedules |
| PUT | `/api/v1/schedules` | Create schedule |
| PUT | `/api/v1/schedules/:id` | Update schedule |
| DELETE | `/api/v1/schedules/:id` | Remove schedule |
| GET | `/api/v1/wifi/scan` | Scan WiFi networks |
| POST | `/api/v1/wifi/connect` | Connect to network |
| GET | `/api/v1/wifi/status` | WiFi status |
| GET | `/api/v1/system/info` | System info |

### Request / Response Examples

**Create fan:**
```json
PUT /api/v1/fans
{"pwm_gpio": 13, "tach_gpio": 12, "name": "f1"}
->
{"status": "ok", "data": {"id": 0, "name": "f1", "mode": 0, "duty": 0, "rpm": 0,
 "enabled": true, "inverted": false, "pwm_gpio": 13, "tach_gpio": 12,
 "source_id": 255, "curve_id": 255, "schedule_id": 255, "group_id": 0, "alarm": "none"}}
```

**Update fan (set AUTO mode + bind source + curve):**
```json
PUT /api/v1/fans/0
{"mode": 1, "source_id": 0, "curve_id": 0}
->
{"status": "ok", "data": { ... "mode": 1, "source_id": 0, "curve_id": 0 ... }}
```

**Create source:**
```json
PUT /api/v1/sources
{"type": "manual", "name": "room1"}
->
{"status": "ok", "data": {"id": 0}}
```

**Set manual source temperature:**
```json
POST /api/v1/sources/temp
{"id": 0, "temp_c": 35.5}
->
{"status": "ok", "data": null}
```

**Create curve:**
```json
PUT /api/v1/curves
{"name": "quiet", "points": [{"temp_c": 30, "duty": 20},
 {"temp_c": 50, "duty": 50}, {"temp_c": 70, "duty": 100}]}
->
{"status": "ok", "data": {"id": 0, "name": "quiet", "points": [ ... ]}}
```

**Create schedule:**
```json
PUT /api/v1/schedules
{"fan_id": 0, "duty": 100, "start_min": 480, "end_min": 1080}
->
{"status": "ok", "data": {"id": 0}}
```

**WiFi scan & connect:**
```json
GET /api/v1/wifi/scan
->
{"status": "ok", "data": [{"ssid": "MyWiFi", "rssi": -45, "channel": 6, "authmode": 3}]}

POST /api/v1/wifi/connect
{"ssid": "MyWiFi", "password": "secret"}
->
{"status": "ok", "data": "connecting"}
```

---

## Concepts

### Fan Modes

| Mode | Value | Description |
|------|-------|-------------|
| Manual | `0` | Fixed duty cycle set by user |
| AUTO | `1` | Duty determined by source temperature mapped through curve |

In AUTO mode, `f_control` runs every second: read source -> lookup curve -> apply hysteresis (3% deadband) -> apply ramp (10%/s up, 3%/s down) -> set duty.

### Source Types

| Type | Value | Reads from |
|------|-------|------------|
| Manual | `0` | User sets temp via API |
| NTC | `1` | ADC1 thermistor (Beta equation) |
| DS18B20 | `2` | 1-Wire digital sensor |

### Schedules (Time Format)

Schedules use **minutes since midnight** (0-1439). Example: `start_min: 480` = 08:00, `end_min: 1080` = 18:00. Overnight wraps are allowed (e.g., 1380 -> 300 = 23:00 -> 05:00).

### Alarms

| Alarm | Trigger |
|-------|---------|
| Stall | Duty > 0% but RPM = 0 for 3 consecutive seconds |
| Over-temp | Source temperature exceeds `CONFIG_ESPFM_OVERTEMP_THRESHOLD_C` (default 85 C) |
| None | Normal operation |

---

## Configuration

### Kconfig (`idf.py menuconfig`)

| Option | Default | Description |
|--------|---------|-------------|
| `ESPFM_WIFI_SSID` | myssid | STA SSID |
| `ESPFM_WIFI_PASSWORD` | mypassword | STA password |
| `ESPFM_WIFI_MAX_RETRY` | 5 | Max STA connection attempts |
| `ESPFM_OVERTEMP_THRESHOLD_C` | 85 | Over-temp alarm trigger |

### config.pb Schema (LittleFS, Protobuf)

Saved to `/littlefs/config.pb` on every API mutation (3-second debounced). Loaded automatically at boot.
Uses the `ConfigFile` Protobuf message (v3.0 format) — see `components/f_schema/proto/espfm.proto`.

---

## Troubleshooting

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| RPM always 0 | Fan in Manual mode, or tach GPIO not connected | Set mode to AUTO for automatic RPM reading, check wiring |
| Fan doesn't spin | Duty too low, inverted polarity, or MOSFET wiring | Try duty=100%, check `inverted` flag, verify MOSFET gate resistor |
| WiFi won't connect | Wrong SSID/password or weak signal | Check dashboard WiFi tab, verify via serial monitor |
| Guru Meditation crash | Stack overflow in HTTP handler | Increase `httpd_cfg.stack_size` in `f_http.c` (default 8192) |
| Config lost on reboot | LittleFS partition not flashed | Ensure `storage.bin` is flashed at offset `0x200000` |
| Temperature reads 0 | Source not configured or sensor disconnected | For manual: POST temp. For NTC/DS18: check GPIO and wiring |
| "not ready" 503 errors | HTTP server started before registries | Ensure `f_http_init` stores handles before WiFi events fire |
| Dashboard blank | LittleFS not mounted or index.html missing | Flash `storage.bin`, check partition table |

---

## Roadmap (Nice to Have)

| Feature | Effort | Impact |
|---------|--------|--------|
| MQTT telemetry (fan RPM, temps, alarms) | Medium | IoT integration |
| OTA firmware update via dashboard | Medium | Production deployment |
| mDNS hostname (`espfm.local`) | Small | Usability |
| OpenAPI 3.1 spec generation | Small | Client SDKs, docs |
| Home Assistant auto-discovery (MQTT) | Medium | Smart home |
| PID control mode (vs hysteresis) | Medium | Precision control |
| Fan runtime hour tracking / predictive maintenance | Small | Reliability |
| Push alerts (Telegram/Discord) on stall/overtemp | Medium | Monitoring |
| Basic auth for dashboard | Small | Security |
| HTTPS / TLS | Large | Security (LAN-only by default) |
| Per-entity config saves (vs full re-serialize) | Small | Flash wear reduction |
| Async DS18B20 reads (non-blocking) | Medium | Multi-sensor performance |
| GPIO conflict detection via `f_gpio` registry | Medium | Safety |

---

## Build & Flash

```powershell
& 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'

# Configure WiFi and threshold
idf.py menuconfig

# Build
idf.py build

# Flash (all partitions)
idf.py flash

# Flash + serial monitor
idf.py flash monitor
```

Partition layout (4MB flash):

| Offset | Size | Partition |
|--------|------|-----------|
| 0x00000 | 8KB | bootloader |
| 0x08000 | 16KB | partition_table |
| 0x10000 | 2MB | factory (app) |
| 0x200000 | 2MB | storage (LittleFS) |
