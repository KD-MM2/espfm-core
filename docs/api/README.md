# ESPFM CoAP API Reference

The ESPFM device exposes its management API as a CoAP/UDP server with
protobuf-serialized payloads. This reference documents every endpoint and
every message. It is the authoritative guide for implementing a CoAP client.

## Device discovery

- CoAP/UDP port `5683`, single request per datagram.
- mDNS advertises `_coap._udp`; device hostname `espfm-XXXX.local`.
- All payloads are `application/octet-stream` (nanopb-encoded protobuf).

## Endpoint map (33 surfaces)

| Resource | Methods | Doc |
| --- | --- | --- |
| `/system/info` | GET | [system.md](system.md) |
| `/system/hostname` | PUT | [system.md](system.md) |
| `/system/reboot` | POST | [system.md](system.md) |
| `/fans` | GET, POST | [fans.md](fans.md) |
| `/fans/{0..7}` | GET, PUT, DELETE | [fans.md](fans.md) |
| `/sources` | GET, POST | [sources.md](sources.md) |
| `/sources/temp` | POST | [sources.md](sources.md) |
| `/sources/{0..7}` | GET, PUT, DELETE | [sources.md](sources.md) |
| `/curves` | GET, POST | [curves.md](curves.md) |
| `/curves/{0..15}` | GET, PUT, DELETE | [curves.md](curves.md) |
| `/schedules` | GET, POST | [schedules.md](schedules.md) |
| `/schedules/{0..7}` | GET, PUT, DELETE | [schedules.md](schedules.md) |
| `/control` | GET, PUT | [control.md](control.md) |
| `/config` | GET, POST | [config.md](config.md) |
| `/ds18b20/scan` | GET | [ds18b20.md](ds18b20.md) |
| `/ds18b20/config` | POST | [ds18b20.md](ds18b20.md) |
| `/wifi/scan` | GET | [wifi.md](wifi.md) |
| `/wifi/status` | GET | [wifi.md](wifi.md) |
| `/wifi/connect` | POST | [wifi.md](wifi.md) |

## Message & enum index

All messages and enums are defined in [messages.md](messages.md):

- Core data: `FanInfo`, `SourceInfo`, `CurvePoint`, `CurveInfo`,
  `ScheduleInfo`, `WifiApRecord`, `SystemInfo`, `WifiStatus`
- Fan: `FanList`, `FanCreateRequest`, `FanUpdateRequest`, `FanId`
- Source: `SourceList`, `SourceCreateRequest`, `SourceUpdateRequest`,
  `ManualTempRequest`, `Ds18b20Device`, `Ds18b20ScanResponse`,
  `Ds18b20ConfigRequest`
- Curve: `CurveList`, `CurveCreateRequest`, `CurveUpdateRequest`
- Schedule: `ScheduleList`, `ScheduleCreateRequest`, `ScheduleUpdateRequest`
- WiFi: `WifiScanResult`, `WifiConnectRequest`, `HostnameRequest`
- Persistence: `ConfigFile`
- Generic: `Empty`, `StatusResponse`
- Control: `ControlConfig`
- Enums: `FanMode`, `SourceType`, `SourceStatus`, `FanAlarm`, `FailsafePolicy`

## Conventions

### Transport
CoAP/UDP port `5683`. Blockwise is enabled on the server; typical payloads fit
one datagram, but `GET /config` may use Block2 when the config exceeds one
message.

### Content-Format
Every request and response payload is a nanopb-encoded protobuf message with
Content-Format `application/octet-stream` (no option number). Clients encode
with the matching generated type (`espfm_pb2` for Python, the `espfm-coap`
crate for Rust).

### Method semantics
| Method | Meaning | Examples |
| --- | --- | --- |
| GET | Read a collection or single item | `GET /fans`, `GET /fans/0` |
| POST | Create (collection) or perform an action | `POST /fans`, `POST /system/reboot` |
| PUT | Update a single item or settings | `PUT /fans/0`, `PUT /control` |
| DELETE | Remove a single item | `DELETE /fans/0` |

### URI form
Collections (`/fans`) return a list message; items (`/fans/{id}`, id `0..7`)
return one entity. Special sub-paths: `/sources/temp` (set manual temperature),
`/system/reboot`, `/system/hostname`, `/ds18b20/config`, `/wifi/connect`,
`/config` (import/export).

### StatusResponse error contract
Most 4.00/5.00 error bodies are a `StatusResponse{ok, error_code, error_msg}`:
- `ok` = `false` on error.
- `error_code` = the ESP-IDF `esp_err_t` value.
- `error_msg` = a human-readable reason, empty on success.

Some error paths (e.g. a decode failure that returns a bare `4.00`) send an
empty body. Check the per-endpoint response notes: when "with `StatusResponse`"
is stated, the body decodes as one; otherwise expect an empty body.

Common `esp_err_t` values seen in responses:

| Code | Name | Meaning in this API |
| --- | --- | --- |
| 0 | `ESP_OK` | Success |
| 258 | `ESP_ERR_INVALID_ARG` | A field is out of range or a GPIO is rejected |
| 259 | `ESP_ERR_INVALID_STATE` | Operation invalid in current state (e.g. GPIO conflict) |
| 261 | `ESP_ERR_NOT_FOUND` | Requested item id does not exist |
| 257 | `ESP_ERR_NO_MEM` | Registry at capacity or allocation failed |

### `255` sentinel
`255` means "none": optional GPIO (`tach_gpio`, source `gpio`) and unbound
references (`source_id`, `curve_id`, `schedule_id`).

### Registry limits
| Registry | Max |
| --- | --- |
| Fans | 8 |
| Sources | 8 |
| Curves | 16 |
| Schedules | 8 |

Creating past capacity returns 4.00 with a `StatusResponse`.

## Error-path conventions
Error paths are documented per endpoint as the CoAP code + the cause. A `4.04
Not Found` means the item id is unallocated; a `4.00 Bad Request` carries a
`StatusResponse` with the reason.

## Related docs
- Live test run: `tools/espfm_device_test_report.md`
- Interactive shell: `tools/espfm_shell.py` (usage in `README.md`)
