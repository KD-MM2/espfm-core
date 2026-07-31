# ESPFanManager — Development Guide

Build, flash, and extend the ESPFanManager firmware.

## Build & Flash

```powershell
# Activate ESP-IDF environment
& 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'

# Configure (WiFi credentials, etc.)
idf.py menuconfig

# Build
idf.py build

# Flash + monitor
idf.py flash monitor
```

## Partition Layout (4MB flash)

| Offset | Size | Partition |
|--------|------|-----------|
| 0x00000 | 8KB | bootloader |
| 0x08000 | 16KB | partition_table |
| 0x10000 | 2MB | factory (app) |
| 0x200000 | 2MB | storage (LittleFS) |

## Protobuf Code Generation

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

## Building the Shell Executable

```powershell
# Build espfm_shell.py into a standalone .exe
.\tools\build_shell.ps1

# Output: tools/dist/espfm_shell.exe
.\tools\dist\espfm_shell.exe --host 192.168.0.22
```

Requires: `pip install pyinstaller protobuf rich prompt_toolkit zeroconf`

## Kconfig (`idf.py menuconfig`)

| Option | Default | Description |
|--------|---------|-------------|
| `ESPFM_WIFI_SSID` | myssid | STA SSID |
| `ESPFM_WIFI_PASSWORD` | mypassword | STA password |
| `ESPFM_WIFI_MAX_RETRY` | 5 | Max STA connection attempts |
| `ESPFM_OVERTEMP_THRESHOLD_C` | 85 | Over-temp alarm trigger |

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

## Tech Stack

- **C11** — firmware language
- **ESP-IDF v6.0.1** — framework
- **FreeRTOS** — task scheduling
- **libcoap-4** — CoAP server (UDP)
- **nanopb** — Protobuf codec for embedded
- **LittleFS** — persistent config storage
- **Python** — interactive shell client
