# ESPFanManager v3

ESP32-S3 multi-channel smart fan controller with CoAP+Protobuf API, interactive shell, and persistent configuration.

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

After boot, the device starts in AP+STA mode. If STA fails, it falls back to open AP at `192.168.4.1`.

## Features

- **Multi-channel** — up to 8 PWM fans with independent control (duty, mode, RPM feedback)
- **Temperature-driven** — NTC thermistor (ADC), DS18B20 (1-Wire), or manual (API-fed)
- **Fan curves** — per-fan temp-to-duty lookup with linear interpolation (up to 16 curves, 10 points each)
- **Schedules** — time-of-day duty overrides with overnight wrap (up to 8 rules)
- **Group sync** — fans in the same group mirror the master fan's duty (lowest ID)
- **Diagnostics** — stall detection (RPM=0 when duty>0), over-temp alarms, fail-safe handler
- **WiFi AP+STA** — open AP fallback when STA fails, scan & connect from shell
- **CoAP+Protobuf** — lightweight UDP protocol with binary serialization (nanopb)
- **Persistent config** — LittleFS-backed `config.pb` (Protobuf) auto-saved on every change
- **Interactive shell** — Python CoAP client for device management (`tools/espfm_shell.py`)
- **mDNS** — device advertised as `espfm-XXXX.local` with CoAP service

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

### 4-Layer Design

```
+-- Presentation ----------------------+
|  f_coap    CoAP server (UDP 5683)    |  libcoap-4 + nanopb Protobuf
|  f_mdns    mDNS service advertisement|
+-- Business Logic --------------------+
|  f_control   1 Hz control loop       |  source -> curve -> hysteresis -> ramp -> duty
|  f_schedule  Time-based overrides    |  60s timer, overnight wrap
|  f_config    Persistent storage      |  LittleFS, Protobuf, 3s debounce
|  f_constr    Input validation        |  duty, mode, GPIO, temp, schedule bounds
+-- Registry (Domain Model) -----------+
|  f_fan       Fan channel registry    |  8 slots, LEDC+PCNT binding
|  f_source    Temp source registry    |  8 slots, NTC/DS18B20/manual
|  f_curve     Fan curve registry      |  16 slots, 10-point lookup
|  f_schedule  Schedule ruleset        |  8 slots
+-- HAL (Hardware Abstraction) --------+
|  f_ledc      LEDC PWM (25kHz, 11b)   |
|  f_pcnt      PCNT pulse counter      |
|  f_adc       ADC1 oneshot (NTC)      |
|  f_ds18b20   RMT 1-Wire driver       |
|  f_gpio      GPIO capability reg.    |
|  f_wifi      APSTA + SNTP + NVS      |
|  f_provision WiFi captive portal     |
+--------------------------------------+
```

### Control Loop (1 Hz)

```
Source temperature -> Fan Curve (linear interpolation)
  -> Hysteresis (3% deadband) -> Ramp Limiter (10%/s up, 3%/s down)
  -> LEDC PWM duty -> Fan
                      |
                 PCNT Tach -> RPM feedback -> Stall/Over-temp diagnostics
```

### Stack

| Layer | Technology | Notes |
|-------|-----------|-------|
| Transport | libcoap-4 (`espressif/coap ^4.3.5`) | UDP CoAP server, single `coap_task` thread |
| Serialization | nanopb-0.4.9.1 | Proto at `components/f_schema/proto/espfm.proto` |
| Storage | NVS + LittleFS | `f_config` persists fan/source/curve/schedule configs |
| WiFi | `f_wifi` + captive portal provisioning | `f_provision` for initial WiFi setup |
| mDNS | `f_mdns` | Advertises `_coap._udp` service |

---

## API Reference (CoAP)

All endpoints use CoAP over UDP port 5683. Request/response bodies are Protobuf-encoded (see `components/f_schema/proto/espfm.proto`).

### Fan Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `fans` | List all fans |
| POST | `fans` | Create fan |
| GET | `fans/{0..7}` | Get fan by ID |
| PUT | `fans/{0..7}` | Update fan (mode, duty, source, curve, schedule, group, inverted, enabled) |
| DELETE | `fans/{0..7}` | Delete fan |

### Source Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `sources` | List all sources |
| POST | `sources` | Create source |
| GET | `sources/{0..7}` | Get source by ID |
| POST | `sources/{0..7}` | Update source |
| DELETE | `sources/{0..7}` | Delete source |
| POST | `sources/temp` | Set manual source temperature |

### Curve Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `curves` | List all curves |
| POST | `curves` | Create curve |
| GET | `curves/{0..7}` | Get curve by ID |
| PUT | `curves/{0..7}` | Update curve |
| DELETE | `curves/{0..7}` | Delete curve |

### Schedule Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `schedules` | List all schedules |
| POST | `schedules` | Create schedule |
| GET | `schedules/{0..7}` | Get schedule by ID |
| PUT | `schedules/{0..7}` | Update schedule |
| DELETE | `schedules/{0..7}` | Delete schedule |

### System Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `system/info` | Version, uptime, heap, entity counts |
| PUT | `system/hostname` | Set device hostname |
| POST | `system/reboot` | Reboot device (2s delayed restart) |

### WiFi Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `wifi/scan` | Scan for available APs |
| GET | `wifi/status` | Current WiFi connection status |
| POST | `wifi/connect` | Connect to AP (ssid, password) |

---

## Interactive Shell

Python-based CoAP client for device management.

```bash
# Install dependencies
pip install protobuf rich prompt_toolkit zeroconf

# Connect (auto-discovers via mDNS)
python tools/espfm_shell.py

# Connect to specific IP
python tools/espfm_shell.py --host 192.168.0.22
```

### Shell Commands

| Command | Description |
|---------|-------------|
| `connect <host>` | Connect to device |
| `disconnect` | Close connection |
| `fans list / get / create / update / delete / enable / disable` | Fan management |
| `sources list / get / create / temp / delete` | Temperature source management |
| `curves list / get / create / update / delete` | Fan curve management |
| `schedules list / create / update / delete` | Schedule management |
| `wifi scan / status / connect` | WiFi management |
| `system info / reboot` | System info and reboot |
| `dashboard` | Multi-table summary |
| `export <file.json>` | Dump config to JSON |
| `import <file.json>` | Apply config from JSON |

---

## Concepts

### Fan Modes

| Mode | Value | Description |
|------|-------|-------------|
| Manual | `0` | Fixed duty cycle set by user |
| AUTO | `1` | Duty determined by source temperature mapped through curve |

### Source Types

| Type | Value | Reads from |
|------|-------|------------|
| Manual | `0` | User sets temp via API |
| NTC | `1` | ADC1 thermistor (Beta equation) |
| DS18B20 | `2` | 1-Wire digital sensor |

### Schedules

Schedules use **minutes since midnight** (0-1439). Example: `start_min: 480` = 08:00, `end_min: 1080` = 18:00. Overnight wraps allowed (e.g., 1380 -> 300 = 23:00 -> 05:00).

### Alarms

| Alarm | Trigger |
|-------|---------|
| Stall | Duty > 0% but RPM = 0 for 3 consecutive seconds |
| Over-temp | Source temperature exceeds threshold (default 85 C) |

---

## Configuration

### Kconfig (`idf.py menuconfig`)

| Option | Default | Description |
|--------|---------|-------------|
| `ESPFM_WIFI_SSID` | myssid | STA SSID |
| `ESPFM_WIFI_PASSWORD` | mypassword | STA password |
| `ESPFM_WIFI_MAX_RETRY` | 5 | Max STA connection attempts |
| `ESPFM_OVERTEMP_THRESHOLD_C` | 85 | Over-temp alarm trigger |

### Persistent Config

Saved to `/littlefs/config.pb` (Protobuf) on every API mutation (3-second debounce). Loaded automatically at boot. Uses the `ConfigFile` Protobuf message — see `components/f_schema/proto/espfm.proto`.

---

## Build & Flash

```powershell
& 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'

# Configure
idf.py menuconfig

# Build
idf.py build

# Flash + monitor
idf.py flash monitor
```

### Partition Layout (4MB flash)

| Offset | Size | Partition |
|--------|------|-----------|
| 0x00000 | 8KB | bootloader |
| 0x08000 | 16KB | partition_table |
| 0x10000 | 2MB | factory (app) |
| 0x200000 | 2MB | storage (LittleFS) |

### Protobuf Code Generation

```powershell
# Regenerate nanopb after editing espfm.proto
tools/gen_proto.ps1
```

Or manually:
```powershell
& 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'
python C:/Espressif/tools/python/v6.0.1/venv/Lib/site-packages/nanopb/generator/nanopb_generator.py `
  -I components/f_schema/proto -I components/f_schema -D components/f_schema/ `
  components/f_schema/proto/espfm.proto
```

Generated files: `components/f_schema/espfm.pb.h`, `components/f_schema/espfm.pb.c`

---

## Troubleshooting

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| RPM always 0 | Fan in Manual mode, or tach GPIO not connected | Set mode to AUTO, check wiring |
| Fan doesn't spin | Duty too low, inverted polarity, or MOSFET wiring | Try duty=100%, check `inverted` flag |
| WiFi won't connect | Wrong SSID/password or weak signal | Check via serial monitor, use provisioning |
| Config lost on reboot | LittleFS partition not flashed | Flash `storage.bin` at offset `0x200000` |
| Temperature reads 0 | Source not configured or sensor disconnected | For manual: POST temp. For NTC/DS18: check GPIO |
| CoAP 4.04 errors | Resource path mismatch | Check endpoint catalog above, use full paths |

---

## Project Status

| Phase | Status |
|-------|--------|
| v2 Redesign (30 tasks) | **100%** Complete |
| v3 CoAP+Protobuf Refactor | **100%** Complete |
| CoAP endpoint suite | **Working** on device |

### v3 Changes from v2

- HTTP REST API replaced with CoAP+Protobuf (UDP, lighter for IoT)
- `f_http` component removed, replaced by `f_coap` (libcoap-4)
- Config storage migrated from JSON (cJSON) to Protobuf (nanopb)
- Interactive shell (`tools/espfm_shell.py`) replaces browser dashboard for management
- mDNS service discovery added

---

## Tech Stack

- **C11** — firmware language
- **ESP-IDF v6.0.1** — framework
- **FreeRTOS** — task scheduling
- **libcoap-4** — CoAP server (UDP)
- **nanopb** — Protobuf codec for embedded
- **LittleFS** — persistent config storage
- **Python** — interactive shell client
