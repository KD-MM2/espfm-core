# ESPFM Protobuf Messages & Enums

Source of truth: `components/f_schema/proto/espfm.proto`. Field numbers are
authoritative. `optional` (proto2-style) fields may be omitted; omitting them
leaves the value unchanged on updates. `255` = "none" for GPIO/reference fields.

## Enums

### FanMode
| Value | Meaning |
| --- | --- |
| `FAN_MODE_MANUAL = 0` | Manual duty control |
| `FAN_MODE_AUTO = 1` | Curve-driven duty |

### SourceType
| Value | Meaning |
| --- | --- |
| `SOURCE_TYPE_NTC = 0` | NTC thermistor on ADC |
| `SOURCE_TYPE_DS18B20 = 1` | DS18B20 1-Wire sensor |
| `SOURCE_TYPE_MANUAL = 2` | Manual temperature (API-fed) |

### SourceStatus
| Value | Meaning |
| --- | --- |
| `SOURCE_STATUS_VALID = 0` | Reading fresh |
| `SOURCE_STATUS_STALE = 1` | Reading not updated recently |
| `SOURCE_STATUS_INVALID = 2` | Sensor error |

### FanAlarm
| Value | Meaning |
| --- | --- |
| `FAN_ALARM_NONE = 0` | No alarm |
| `FAN_ALARM_STALL = 1` | RPM=0 while duty>0 |
| `FAN_ALARM_OVERTEMP = 2` | Source over temperature |

### FailsafePolicy
| Value | Meaning |
| --- | --- |
| `FAILSAFE_HOLD = 0` | Hold last duty |
| `FAILSAFE_FULL_SPEED = 1` | 100% duty |
| `FAILSAFE_SAFE_DUTY = 2` | Fixed safe duty |
| `FAILSAFE_ALT_SOURCE = 3` | Use alternate source |

## Core data types

### FanInfo
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `id` | uint32 | 0-7 | Registry slot id |
| 2 | `name` | string | ≤15 chars | Display name |
| 3 | `mode` | FanMode | 0-1 | Manual or auto |
| 4 | `duty` | uint32 | 0-100 | Current duty % |
| 5 | `rpm` | uint32 | — | Measured RPM (live) |
| 6 | `enabled` | bool | — | Fan enabled |
| 7 | `inverted` | bool | — | Inverted PWM output |
| 8 | `pwm_gpio` | uint32 | 0-48 | PWM output pin |
| 9 | `tach_gpio` | uint32 | 255=none | Tachometer input pin |
| 10 | `source_id` | uint32 | 255=none | Bound temperature source |
| 11 | `curve_id` | uint32 | 255=none | Bound curve |
| 12 | `schedule_id` | uint32 | 255=none | Bound schedule |
| 13 | `group_id` | uint32 | 0-255 | Group sync id |
| 14 | `alarm` | FanAlarm | 0-2 | Current alarm (live) |

### SourceInfo
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `id` | uint32 | 0-7 | Registry slot id |
| 2 | `name` | string | — | Display name |
| 3 | `type` | SourceType | 0-2 | NTC / DS18B20 / manual |
| 4 | `status` | SourceStatus | 0-2 | Validity (live) |
| 5 | `temp_c` | float | — | Temperature °C (live) |
| 6 | `gpio` | uint32 | 255=none | GPIO (NTC only) |
| 7 | `ds18b20_rom_code` | uint64 | — | DS18B20 ROM code |

### CurvePoint
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `temp_c` | float | — | Temperature breakpoint °C |
| 2 | `duty` | uint32 | 0-100 | Duty at this temp |

### CurveInfo
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `id` | uint32 | 0-15 | Registry slot id |
| 2 | `name` | string | — | Display name |
| 3 | `points` | repeated CurvePoint | 2-10 | Sorted by temp_c ascending |

### ScheduleInfo
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `id` | uint32 | 0-7 | Registry slot id |
| 2 | `fan_id` | uint32 | 0-7 | Bound fan |
| 3 | `duty` | uint32 | 0-100 | Duty while active |
| 4 | `start_min` | uint32 | 0-1439 | Start minute since midnight |
| 5 | `end_min` | uint32 | 0-1439 | End minute since midnight |
| 6 | `enabled` | bool | — | Rule enabled |
| 7 | `name` | string | — | Optional display name |

### WifiApRecord
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `ssid` | string | — | Network name |
| 2 | `rssi` | int32 | — | Signal strength dBm |
| 3 | `channel` | uint32 | 1-13 | WiFi channel |
| 4 | `authmode` | uint32 | — | Auth mode code |

### SystemInfo
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `version` | string | — | Firmware version |
| 2 | `uptime_s` | uint32 | — | Seconds since boot |
| 3 | `heap_free` | uint32 | — | Free heap bytes |
| 4 | `fan_count` | uint32 | 0-8 | Allocated fans |
| 5 | `source_count` | uint32 | 0-8 | Allocated sources |
| 6 | `curve_count` | uint32 | 0-16 | Allocated curves |
| 7 | `schedule_count` | uint32 | 0-8 | Allocated schedules |
| 8 | `hostname` | string | — | mDNS hostname (no `.local`) |

### WifiStatus
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `sta_connected` | bool | — | STA link up |
| 2 | `sta_ip` | string | — | STA IP address |
| 3 | `ap_ip` | string | — | SoftAP IP address |

## Lists & requests

### FanList
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `fans` | repeated FanInfo | — | All fans |

### FanCreateRequest
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `pwm_gpio` | uint32 | 0-48 (required) | PWM output pin |
| 2 | `tach_gpio` | uint32 | 255=none | Tach input pin |
| 3 | `mode` | optional FanMode | 0-1 | Manual or auto |
| 4 | `duty` | optional uint32 | 0-100 | Initial duty |
| 5 | `source_id` | optional uint32 | 255=none | Bind source |
| 6 | `curve_id` | optional uint32 | 255=none | Bind curve |
| 7 | `schedule_id` | optional uint32 | 255=none | Bind schedule |
| 8 | `group_id` | optional uint32 | — | Group sync id |
| 9 | `inverted` | optional bool | — | Inverted PWM |
| 10 | `enabled` | optional bool | — | Enabled |
| 11 | `name` | optional string | ≤15 chars | Display name |

### FanUpdateRequest
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `id` | uint32 | 0-7 (required) | Fan to update |
| 2 | `mode` | optional FanMode | — | Change mode |
| 3 | `duty` | optional uint32 | 0-100 | Change duty |
| 4 | `source_id` | optional uint32 | 255=none | Rebind source |
| 5 | `curve_id` | optional uint32 | 255=none | Rebind curve |
| 6 | `schedule_id` | optional uint32 | 255=none | Rebind schedule |
| 7 | `group_id` | optional uint32 | — | Change group |
| 8 | `inverted` | optional bool | — | Change inverted |
| 9 | `enabled` | optional bool | — | Enable/disable |
| 10 | `pwm_gpio` | optional uint32 | 0-48 | Move PWM pin (swap) |
| 11 | `tach_gpio` | optional uint32 | 255=none | Move tach pin |
| 12 | `name` | optional string | ≤15 chars | Rename |

### FanId
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `id` | uint32 | 0-7 | Fan id |

### SourceList
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `sources` | repeated SourceInfo | — | All sources |

### SourceCreateRequest
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `type` | SourceType | 0-2 | Source kind |
| 2 | `name` | string | — | Display name |
| 3 | `gpio` | uint32 | 255=none | GPIO (NTC only) |
| 4 | `ds18b20_rom_code` | uint64 | — | DS18B20 ROM code |

### SourceUpdateRequest
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `id` | uint32 | 0-7 | Source to update |
| 2 | `name` | string | — | New name |

### ManualTempRequest
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `id` | uint32 | 0-7 | Manual source id |
| 2 | `temp_c` | float | -40..125 | Temperature to set |

### Ds18b20Device
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `index` | uint32 | — | Sensor index |
| 2 | `rom_code` | uint64 | — | 1-Wire ROM code |
| 3 | `temp_c` | float | — | Measured °C |

### Ds18b20ScanResponse
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `devices` | repeated Ds18b20Device | — | Found sensors |
| 2 | `device_count` | uint32 | 0-4 | Sensor count |

### Ds18b20ConfigRequest
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `gpio` | uint32 | 0-48 | 1-Wire bus GPIO |

### CurveList
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `curves` | repeated CurveInfo | — | All curves |

### CurveCreateRequest
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `name` | string | — | Display name |
| 2 | `points` | repeated CurvePoint | 2-10 | Curve points (sorted) |

### CurveUpdateRequest
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `id` | uint32 | 0-15 | Curve to update |
| 2 | `name` | string | — | New name |
| 3 | `points` | repeated CurvePoint | 2-10 | Full replace points |

### ScheduleList
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `schedules` | repeated ScheduleInfo | — | All schedules |

### ScheduleCreateRequest
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `fan_id` | uint32 | 0-7 | Bound fan |
| 2 | `duty` | uint32 | 0-100 | Duty while active |
| 3 | `start_min` | uint32 | 0-1439 | Start minute |
| 4 | `end_min` | uint32 | 0-1439 | End minute |
| 5 | `enabled` | bool | — | Rule enabled |
| 6 | `name` | string | — | Optional name |

### ScheduleUpdateRequest
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `id` | uint32 | 0-7 | Schedule to update |
| 2 | `fan_id` | optional uint32 | 0-7 | Rebind fan |
| 3 | `duty` | optional uint32 | 0-100 | Change duty |
| 4 | `start_min` | optional uint32 | 0-1439 | Change start |
| 5 | `end_min` | optional uint32 | 0-1439 | Change end |
| 6 | `enabled` | optional bool | — | Enable/disable |
| 7 | `name` | optional string | — | Rename |

### WifiScanResult
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `aps` | repeated WifiApRecord | max 16 | Found APs |

### WifiConnectRequest
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `ssid` | string | — | Network SSID |
| 2 | `password` | string | — | Network password |

### HostnameRequest
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `hostname` | string | — | New mDNS hostname (no `.local`) |

### ConfigFile
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `version` | string | "3.0" | Config schema version |
| 2 | `fans` | FanList | — | All fans |
| 3 | `sources` | SourceList | — | All sources |
| 4 | `curves` | CurveList | — | All curves |
| 5 | `schedules` | ScheduleList | — | All schedules |

### Empty
No fields. Defined in the schema but not used by any current handler or
response; a bare error response (e.g. a decode-failure `4.00`) has an empty
body rather than an encoded `Empty` message.

### StatusResponse
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `ok` | bool | — | Success flag |
| 2 | `error_code` | uint32 | 0 on success | `esp_err_t` value |
| 3 | `error_msg` | string | "" on success | Human-readable reason |

### ControlConfig
| # | Field | Type | Range/Default | Semantics |
| --- | --- | --- | --- | --- |
| 1 | `hysteresis` | optional uint32 | 0-100 | Temp hysteresis % |
| 2 | `ramp_up` | optional uint32 | 0-100 | Ramp-up rate % |
| 3 | `ramp_down` | optional uint32 | 0-100 | Ramp-down rate % |
| 4 | `failsafe_policy` | optional FailsafePolicy | 0-3 | Failsafe behavior |
| 5 | `safe_duty` | optional uint32 | 0-100 | Safe-duty % |
