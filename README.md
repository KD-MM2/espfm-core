# ESPFanManager v1

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

## Interactive Shell

Python-based CoAP client for device management. Supports tab-completion, mDNS device discovery, and full CRUD for all entities.

### Install

```bash
pip install protobuf rich prompt_toolkit zeroconf
```

### Run

```bash
# Auto-connect via mDNS or connect manually
python tools/espfm_shell.py

# Connect to specific IP
python tools/espfm_shell.py --host 192.168.0.22

# Custom port and timeout
python tools/espfm_shell.py --host 192.168.0.22 --port 5683 --timeout 5
```

### Build Standalone Executable

```powershell
# Build into a single .exe (no Python needed to run)
.\tools\build_shell.ps1
.\tools\dist\espfm_shell.exe --host 192.168.0.22
```

### Connection

| Command | Description |
|---------|-------------|
| `connect <host> [--port N] [--timeout N]` | Connect to device |
| `disconnect` | Close connection |
| `devices scan [--timeout N]` | Scan LAN for ESPFM devices (mDNS) |
| `devices connect XXYY` | Connect to device by MAC suffix |
| `devices update XXYY --hostname NAME` | Change device hostname |

### Fan Management

| Command | Description |
|---------|-------------|
| `fans list` | List all fans |
| `fans get <id>` | Show fan detail |
| `fans create --pwm <gpio> --name <name> [--tach <gpio>]` | Create fan |
| `fans update <id> [--duty N] [--mode auto\|manual] [--source N] [--curve N]` | Update fan |
| `fans enable <id>` | Enable a fan |
| `fans disable <id>` | Disable a fan |
| `fans delete <id>` | Delete fan |

### Temperature Sources

| Command | Description |
|---------|-------------|
| `sources list` | List all sources |
| `sources get <id>` | Show source detail |
| `sources create --type <ntc\|ds18b20\|manual> --name <name> [--gpio N]` | Create source |
| `sources update <id> --name <name>` | Rename source |
| `sources temp <id> <temp_c>` | Set manual temperature |
| `sources delete <id>` | Delete source |

### Fan Curves

| Command | Description |
|---------|-------------|
| `curves list` | List all curves |
| `curves get <id>` | Show curve with points |
| `curves create --name <name> --points "30:30,50:60,70:100"` | Create curve |
| `curves update <id> [--name ...] [--points ...]` | Update curve |
| `curves delete <id>` | Delete curve |

### Schedules

| Command | Description |
|---------|-------------|
| `schedules list` | List all schedules |
| `schedules create --fan N --duty N --start N --end N [--enabled true\|false]` | Create schedule |
| `schedules update <id> [--fan N] [--duty N] [--start N] [--end N]` | Update schedule |
| `schedules delete <id>` | Delete schedule |

Start/end values are minutes since midnight (e.g., `--start 480 --end 1080` = 08:00-18:00).

### WiFi

| Command | Description |
|---------|-------------|
| `wifi scan` | Scan for nearby APs |
| `wifi status` | Show current connection status |
| `wifi connect --ssid <name> --pass <password>` | Connect to AP |

### System

| Command | Description |
|---------|-------------|
| `system info` | Show version, uptime, heap, entity counts |
| `system reboot` | Reboot device (2s delay) |

### DS18B20

| Command | Description |
|---------|-------------|
| `ds18b20 scan` | Scan 1-Wire bus for sensors |
| `ds18b20 config --gpio <pin>` | Configure DS18B20 bus GPIO |

### Data Operations

| Command | Description |
|---------|-------------|
| `dashboard` | Multi-table summary of all entities |
| `export <file.json>` | Dump full device config to JSON |
| `import <file.json> [--no-delete]` | Apply config from JSON |

### Tips

- Tab-completion works for commands, flags, and enum values (`auto`, `manual`, `ntc`, etc.)
- After `ds18b20 scan`, discovered ROM codes are added to tab-completion
- `export`/`import` preserves fans, sources, curves, schedules, and WiFi status
- Use `--no-delete` with `import` to avoid removing entities not in the JSON

### Usage example

```
.\espfm_shell.exe
ESPFM Interactive Shell v1.0
Type 'help' for commands, 'exit' to quit.

espfm> devices scan
Scanning for ESPFM devices (3.0s)...
                      ESPFM Devices
┏━━━━━━━━━━━━┳━━━━━━━━━━━━━━┳━━━━━━┳━━━━━━━━━┳━━━━━━━━━━┓
┃ Hostname   ┃ IP Address   ┃ Port ┃ Version ┃ Firmware ┃
┡━━━━━━━━━━━━╇━━━━━━━━━━━━━━╇━━━━━━╇━━━━━━━━━╇━━━━━━━━━━┩
│ espfm-b629 │ 192.168.0.22 │ 5683 │ 1       │ dev      │
└────────────┴──────────────┴──────┴─────────┴──────────┘
espfm> connect espfm-b629
Connected to espfm-b629:5683
espfm> fans list
No fans configured.
espfm> sources create --type manual --name gpu-manual
Created source 0: gpu-manual (manual)
espfm> curves create --name gpu-temp --points 40:20,50:30,60:45,70:60,80:100
Created curve 0: gpu-temp (5 points)
espfm> fans create --pwm 22 --tach 23 --name gpu-fan-1 --source 0 --curve 0 --mode auto --inverted true
Created fan 0: gpu-fan-1 (PWM GPIO 22)
espfm> sources temp 0 20
Set source 0 temperature to 20.0 C.
espfm> fans list
                                                          Fans
┏━━━━┳━━━━━━━━━┳━━━━━━┳━━━━━━━━┳━━━━━━┳━━━━━━━━━┳━━━━━━━━━┳━━━━━━━━┳━━━━━━━━━┳━━━━━━━━┳━━━━━━━┳━━━━━━━━┳━━━━━━━┳━━━━━━━┓
┃    ┃         ┃      ┃        ┃      ┃         ┃         ┃    PWM ┃    Tach ┃        ┃       ┃        ┃       ┃       ┃
┃ ID ┃ Name    ┃ Mode ┃ Duty % ┃  RPM ┃ Enabled ┃ Invert… ┃   GPIO ┃    GPIO ┃ Source ┃ Curve ┃ Sched… ┃ Group ┃ Alarm ┃
┡━━━━╇━━━━━━━━━╇━━━━━━╇━━━━━━━━╇━━━━━━╇━━━━━━━━━╇━━━━━━━━━╇━━━━━━━━╇━━━━━━━━━╇━━━━━━━━╇━━━━━━━╇━━━━━━━━╇━━━━━━━╇━━━━━━━┩
│  0 │ gpu-fa… │ auto │     20 │ 3090 │ yes     │ yes     │     22 │      23 │      0 │     0 │      - │     0 │ none  │
└────┴─────────┴──────┴────────┴──────┴─────────┴─────────┴────────┴─────────┴────────┴───────┴────────┴───────┴───────┘
espfm> fans get 0
╭─────────────────────────────────────────────────────── Fan 0 ────────────────────────────────────────────────────────╮
│ Fan 0                                                                                                                │
│   Name:      gpu-fan-1                                                                                               │
│   Mode:      auto                                                                                                    │
│   Duty:      20%                                                                                                     │
│   RPM:       3120                                                                                                    │
│   Enabled:   yes                                                                                                     │
│   Inverted:  yes                                                                                                     │
│   PWM GPIO:  22                                                                                                      │
│   Tach GPIO: 23                                                                                                      │
│   Source:    0                                                                                                       │
│   Curve:     0                                                                                                       │
│   Schedule:  none                                                                                                    │
│   Group:     0                                                                                                       │
│   Alarm:     none                                                                                                    │
╰──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────╯
espfm>
```

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

### Persistent Config

Saved to `/littlefs/config.pb` (Protobuf) on every API mutation (3-second debounce). Loaded automatically at boot. Uses the `ConfigFile` Protobuf message — see `components/f_schema/proto/espfm.proto`.

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

## Development

See [DEVELOPMENT.md](DEVELOPMENT.md) for build instructions, architecture details, protobuf codegen, and tech stack.
