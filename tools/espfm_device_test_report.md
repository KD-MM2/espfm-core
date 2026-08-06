# ESPFM Live-Device CoAP Integration Test Report

- Target IP: `192.168.0.28`
- Device hostname: `espfm-c425`
- Firmware version: `1.0.0`
- Run timestamp: `2026-08-06 12:44:54`

## Endpoint Table of Contents

- **GET /system/info** — Read system identity, uptime, heap, and entity counts
  - Request: none
  - Expected: 2.05 SystemInfo{version, uptime_s, heap_free, fan_count, source_count, curve_count, schedule_count, hostname}
- **PUT /system/hostname** — Set the device hostname (persists to NVS)
  - Request: HostnameRequest{hostname: "espfm-test"}
  - Expected: 2.04 StatusResponse{ok=true}; 4.04 path mismatch; 4.00 undecodable/mdns fail
- **POST /system/reboot** — Reboot the device after ~2 s
  - Request: none
  - Expected: 2.04 StatusResponse{ok=true}; 5.03 "reboot pending"
- **GET /fans** — List all fans
  - Request: none
  - Expected: 2.05 FanList{repeated FanInfo}
- **POST /fans** — Create a fan
  - Request: FanCreateRequest{pwm_gpio: <free>, tach_gpio: <free or 255>, name: "test-fan"}
  - Expected: 2.01 FanInfo{id, pwm_gpio, tach_gpio, name}; 4.00 StatusResponse{ok=false} on claim/constraint failure
- **GET /fans/{id}** — Read one fan
  - Request: none
  - Expected: 2.05 FanInfo{id}; 4.04 if unallocated
- **PUT /fans/{id}** — Update fan fields
  - Request: FanUpdateRequest{id, duty: <test value>}
  - Expected: 2.04 FanInfo{id, duty}; 4.04 not found; 4.00 StatusResponse on GPIO-swap failure
- **DELETE /fans/{id}** — Delete a fan
  - Request: none
  - Expected: 2.02 StatusResponse{ok=true}; 4.04 not found
- **GET /sources** — List all sources
  - Request: none
  - Expected: 2.05 SourceList{repeated SourceInfo}
- **POST /sources** — Create a source
  - Request: SourceCreateRequest{type: SOURCE_TYPE_MANUAL, name: "test-source", gpio: 255}
  - Expected: 2.01 SourceInfo{id, name, type}; 4.00 StatusResponse on add failure
- **POST /sources/temp** — Set a manual source temperature
  - Request: ManualTempRequest{id: <manual source id>, temp_c: 20.0}
  - Expected: 2.04 StatusResponse{ok=true}; 4.04 source not found; 4.00 on fail
- **GET /sources/{id}** — Read one source
  - Request: none
  - Expected: 2.05 SourceInfo{id}; 4.04
- **PUT /sources/{id}** — Rename a source
  - Request: SourceUpdateRequest{id, name: <test name>}
  - Expected: 2.04 SourceInfo{id, name}; 4.04/4.00
- **DELETE /sources/{id}** — Delete a source
  - Request: none
  - Expected: 2.02 StatusResponse{ok=true}; 4.04
- **GET /curves** — List all curves
  - Request: none
  - Expected: 2.05 CurveList{repeated CurveInfo}
- **POST /curves** — Create a curve
  - Request: CurveCreateRequest{name: "test-curve", points: [2-10 CurvePoint{temp_c, duty}]}
  - Expected: 2.01 CurveInfo{id, name, points}; 4.00 undecodable/upsert fail
- **GET /curves/{id}** — Read one curve
  - Request: none
  - Expected: 2.05 CurveInfo{id}; 4.04
- **PUT /curves/{id}** — Update curve name and/or points
  - Request: CurveUpdateRequest{id, name, points}
  - Expected: 2.04 CurveInfo{id, name, points}; 4.04/4.00
- **DELETE /curves/{id}** — Delete a curve
  - Request: none
  - Expected: 2.02 StatusResponse{ok=true}; 4.04
- **GET /schedules** — List all schedules
  - Request: none
  - Expected: 2.05 ScheduleList{repeated ScheduleInfo}
- **POST /schedules** — Create a schedule
  - Request: ScheduleCreateRequest{fan_id: <live fan id>, duty: 50, start_min: 600, end_min: 1080, enabled: true, name: "test-schedule"}
  - Expected: 2.01 ScheduleInfo{id}; 4.00 on add failure
- **GET /schedules/{id}** — Read one schedule
  - Request: none
  - Expected: 2.05 ScheduleInfo{id}; 4.04
- **PUT /schedules/{id}** — Update schedule fields
  - Request: ScheduleUpdateRequest{id, duty: <test value>}
  - Expected: 2.04 ScheduleInfo{id, duty}; 4.04/4.00
- **DELETE /schedules/{id}** — Delete a schedule
  - Request: none
  - Expected: 2.02 StatusResponse{ok=true}; 4.04
- **GET /config** — Export the full config
  - Request: none
  - Expected: 2.05 ConfigFile{version, fans, sources, curves, schedules}
- **POST /config** — Import a full config (reboots after ~2 s)
  - Request: ConfigFile{<pre-run snapshot>}
  - Expected: 2.04 StatusResponse{ok=true}; 4.00 validation fail; 5.00 persist fail
- **GET /control** — Read control tunables
  - Request: none
  - Expected: 2.05 ControlConfig{hysteresis, ramp_up, ramp_down, failsafe_policy, safe_duty}; 5.03 control unset
- **PUT /control** — Set control tunables
  - Request: ControlConfig{hysteresis, ramp_up, ramp_down, failsafe_policy, safe_duty}
  - Expected: 2.04 StatusResponse{ok=true}; 4.00 range/decode fail; 5.03 control unset
- **GET /ds18b20/scan** — Scan the DS18B20 bus
  - Request: none
  - Expected: 2.05 Ds18b20ScanResponse{devices, device_count}; 5.03 no bus; 5.00 scan fail
- **POST /ds18b20/config** — Configure the DS18B20 bus GPIO
  - Request: Ds18b20ConfigRequest{gpio: <free pin>}
  - Expected: 2.04 StatusResponse{ok=true}; 4.00 init/claim fail
- **GET /wifi/scan** — Scan nearby WiFi APs (~3.5 s)
  - Request: none
  - Expected: 2.05 WifiScanResult{repeated WifiApRecord}; 5.03 scan fail
- **GET /wifi/status** — Read STA/AP status
  - Request: none
  - Expected: 2.05 WifiStatus{sta_connected, sta_ip, ap_ip}
- **POST /wifi/connect** — Connect to a WiFi network
  - Request: WifiConnectRequest{ssid: "<WIFI_SSID>", password: "<WIFI_PASSWORD>"}
  - Expected: 2.04 StatusResponse{ok=true}; 5.03 set-config fail; or timeout

## Per-Endpoint Results

### GET /system/info

- Request: none
  - Expected: 2.05
  - Actual: 2.05 Content
  - Response: version=1.0.0, uptime_s=676, heap_free=152080, fan_count=1, source_count=1, curve_count=1, schedule_count=0, hostname=espfm-c425
  - Verdict: PASS
- **Endpoint result: PASS**

### GET /fans

- Request: none
  - Expected: 2.05
  - Actual: 2.05 Content
  - Response: [id=0, name='gpu-fan-1', mode=1, duty=20, rpm=13290, pwm_gpio=22, tach_gpio=23, source_id=0, curve_id=0, schedule_id=255, group_id=0, enabled=True, alarm=0]
  - Verdict: PASS
- Request: none
  - Expected: 2.05
  - Actual: 2.05
  - Response: id=0, name='gpu-fan-1', mode=1, duty=20, rpm=13290, pwm_gpio=22, tach_gpio=23, source_id=0, curve_id=0, schedule_id=255, group_id=0, enabled=True, alarm=0
  - Verdict: PASS
- **Endpoint result: PASS**

### GET /sources

- Request: none
  - Expected: 2.05
  - Actual: 2.05 Content
  - Response: [id=0, name='gpu-manual', type=2, status=1, temp_c=20.0, gpio=255]
  - Verdict: PASS
- **Endpoint result: PASS**

### GET /curves

- Request: none
  - Expected: 2.05
  - Actual: 2.05 Content
  - Response: [id=0, name='gpu-temp', points=5]
  - Verdict: PASS
- **Endpoint result: PASS**

### GET /schedules

- Request: none
  - Expected: 2.05
  - Actual: 2.05 Content
  - Response: none
  - Verdict: PASS
- **Endpoint result: PASS**

### GET /config

- Request: none
  - Expected: 2.05
  - Actual: 2.05 Content
  - Response: version='3.0', fans=1, sources=1, curves=1, schedules=0
  - Verdict: PASS
- **Endpoint result: PASS**

### GET /wifi/status

- Request: none
  - Expected: 2.05
  - Actual: 2.05 Content
  - Response: sta_connected=True, sta_ip='192.168.0.28', ap_ip='192.168.4.1'
  - Verdict: PASS
- **Endpoint result: PASS**

### GET /wifi/scan

- Request: none
  - Expected: 2.05
  - Actual: 2.05 Content
  - Response: [ssid='TP-Link_E84F_2G', rssi=-50, channel=3, authmode=3]; [ssid='eufy RoboVac G30 Hybrid-247D', rssi=-55, channel=6, authmode=0]; [ssid='60DDC802-5G', rssi=-89, channel=7, authmode=4]; [ssid='F660A-cz9b-G', rssi=-91, channel=6, authmode=4]; [ssid='30E7BC73C517-2G', rssi=-92, channel=1, authmode=4]; [ssid='HUMAX-88040', rssi=-92, channel=11, authmode=4]; [ssid='elecom2g-EDE55C', rssi=-93, channel=4, authmode=3]; [ssid='Buffalo-G-0DFE', rssi=-94, channel=1, authmode=3]
  - Verdict: PASS
- **Endpoint result: PASS**

### GET /ds18b20/scan

- Request: none
  - Expected: 2.05 or 5.03
  - Actual: 2.05 Content
  - Response: device_count=0
  - Verdict: PASS
- **Endpoint result: PASS**

### GET /control

- Request: none
  - Expected: 2.05
  - Actual: 2.05 Content
  - Response: hysteresis=3, ramp_up=10, ramp_down=3, failsafe_policy=2, safe_duty=50
  - Verdict: PASS
- **Endpoint result: PASS**

### GET /fans + GET /sources

- Request: none
  - Expected: n/a
  - Actual: n/a
  - Response: used_gpio=[22, 23], free_fan_pwm=2, free_fan_tach=4, free_ds_pin=5
  - Verdict: PASS
- **Endpoint result: PASS**

### POST /fans

- Request: FanCreateRequest{pwm_gpio=2, tach_gpio=4, name='test-fan'}
  - Expected: 2.01
  - Actual: 2.01 Created
  - Response: id=1, name='test-fan', mode=0, duty=0, rpm=0, pwm_gpio=2, tach_gpio=4, source_id=255, curve_id=255, schedule_id=255, group_id=0, enabled=True, alarm=0
  - Verdict: PASS
- **Endpoint result: PASS**

### POST /sources

- Request: SourceCreateRequest{type=2 (SOURCE_TYPE_MANUAL), name='test-source', gpio=255}
  - Expected: 2.01
  - Actual: 2.01 Created
  - Response: id=1, name='test-source', type=2, status=0, temp_c=0.0, gpio=255
  - Verdict: PASS
- **Endpoint result: PASS**

### POST /curves

- Request: CurveCreateRequest{name='test-curve', points=5}
  - Expected: 2.01
  - Actual: 2.01 Created
  - Response: id=0, name='test-curve', points=5
  - Verdict: PASS
- **Endpoint result: PASS**

### POST /schedules

- Request: ScheduleCreateRequest{fan_id=0, duty=50, start_min=600, end_min=1080, enabled=True, name='test-schedule'}
  - Expected: 2.01
  - Actual: 2.01 Created
  - Response: id=0, fan_id=0, duty=50, start_min=600, end_min=1080, enabled=True, name='test-schedule'
  - Verdict: PASS
- **Endpoint result: PASS**

### POST /sources/temp

- Request: ManualTempRequest{id=0, temp_c=20.0}
  - Expected: 2.04
  - Actual: 2.04 Changed
  - Response: ok=True
  - Verdict: PASS
- **Endpoint result: PASS**

### POST /ds18b20/config

- Request: Ds18b20ConfigRequest{gpio=5}
  - Expected: 2.04
  - Actual: 2.04 Changed
  - Response: ok=True
  - Verdict: PASS
- **Endpoint result: PASS**

### /fans/0

- Request: none
  - Expected: 2.05
  - Actual: 2.05 Content
  - Response: id=0, name='gpu-fan-1', mode=1, duty=20, rpm=13260, pwm_gpio=22, tach_gpio=23, source_id=0, curve_id=0, schedule_id=255, group_id=0, enabled=True, alarm=0
  - Verdict: PASS
- **Endpoint result: PASS**

### /sources/0

- Request: none
  - Expected: 2.05
  - Actual: 2.05 Content
  - Response: id=0, name='gpu-manual', type=2, status=0, temp_c=20.0, gpio=255
  - Verdict: PASS
- **Endpoint result: PASS**

### /curves/0

- Request: none
  - Expected: 2.05
  - Actual: 2.05 Content
  - Response: id=0, name='test-curve', points=5
  - Verdict: PASS
- **Endpoint result: PASS**

### /schedules/0

- Request: none
  - Expected: 2.05
  - Actual: 2.05 Content
  - Response: id=0, fan_id=0, duty=50, start_min=600, end_min=1080, enabled=True, name='test-schedule'
  - Verdict: PASS
- **Endpoint result: PASS**

### GET /fans/7

- Request: none
  - Expected: 4.04
  - Actual: 4.04
  - Response: 4.04 Not Found (unallocated slot)
  - Verdict: PASS
- **Endpoint result: PASS**

### PUT /fans/0

- Request: FanUpdateRequest{id=0, duty=40}
  - Expected: 2.04
  - Actual: 2.04 Changed
  - Response: id=0, name='gpu-fan-1', mode=1, duty=40, rpm=13260, pwm_gpio=22, tach_gpio=23, source_id=0, curve_id=0, schedule_id=255, group_id=0, enabled=True, alarm=0
  - Verdict: PASS
- Request: FanUpdateRequest{id=0, duty=20}
  - Expected: 2.04
  - Actual: 2.04 Changed
  - Response: id=0, name='gpu-fan-1', mode=1, duty=20, rpm=13260, pwm_gpio=22, tach_gpio=23, source_id=0, curve_id=0, schedule_id=255, group_id=0, enabled=True, alarm=0
  - Verdict: PASS
- **Endpoint result: PASS**

### PUT /sources/0

- Request: SourceUpdateRequest{id=0, name='gpu-manual-test'}
  - Expected: 2.04
  - Actual: 2.04 Changed
  - Response: id=0, name='gpu-manual-test', type=2, status=0, temp_c=20.0, gpio=255
  - Verdict: PASS
- Request: SourceUpdateRequest{id=0, name='gpu-manual'}
  - Expected: 2.04
  - Actual: 2.04 Changed
  - Response: id=0, name='gpu-manual', type=2, status=0, temp_c=20.0, gpio=255
  - Verdict: PASS
- **Endpoint result: PASS**

### PUT /curves/0

- Request: CurveUpdateRequest{id=0, name='gpu-temp-test', points=0}
  - Expected: 2.04
  - Actual: 2.04 Changed
  - Response: id=0, name='gpu-temp-test', points=5
  - Verdict: PASS
- Request: CurveUpdateRequest{id=0, name='test-curve', points=5}
  - Expected: 2.04
  - Actual: 2.04 Changed
  - Response: id=0, name='test-curve', points=5
  - Verdict: PASS
- **Endpoint result: PASS**

### PUT /schedules/0

- Request: ScheduleUpdateRequest{id=0, duty=80}
  - Expected: 2.04
  - Actual: 2.04 Changed
  - Response: id=0, fan_id=0, duty=80, start_min=600, end_min=1080, enabled=True, name='test-schedule'
  - Verdict: PASS
- Request: ScheduleUpdateRequest{id=0, duty=50}
  - Expected: 2.04
  - Actual: 2.04 Changed
  - Response: id=0, fan_id=0, duty=50, start_min=600, end_min=1080, enabled=True, name='test-schedule'
  - Verdict: PASS
- **Endpoint result: PASS**

### PUT /control

- Request: ControlConfig{hysteresis=3, ramp_up=10, ramp_down=3, failsafe_policy=2, safe_duty=50}
  - Expected: 2.04
  - Actual: 2.04 Changed
  - Response: ok=True, error_code=0, error_msg=''
  - Verdict: PASS
- **Endpoint result: PASS**

### PUT /control (out-of-range)

- Request: ControlConfig{hysteresis=150, ramp_up=10, ramp_down=3, failsafe_policy=2, safe_duty=50}
  - Expected: 4.00
  - Actual: 4.00
  - Response: ok=False, error_code=258, error_msg='hysteresis out of range'
  - Verdict: PASS
- **Endpoint result: PASS**

### PUT /system/hostname

- Request: HostnameRequest{hostname='espfm-test'}
  - Expected: 2.04
  - Actual: 2.04 Changed
  - Response: ok=True, error_code=0, error_msg=''
  - Verdict: PASS
- **Endpoint result: PASS**

### PUT /system/hostname (restore)

- Request: HostnameRequest{hostname='espfm-c425'}
  - Expected: 2.04
  - Actual: 2.04 Changed
  - Response: ok=True, error_code=0, error_msg=''
  - Verdict: PASS
- **Endpoint result: PASS**

### DELETE /fans/1

- Request: none
  - Expected: 2.02
  - Actual: 2.02 Deleted
  - Response: ok=True, error_code=0, error_msg=''
  - Verdict: PASS
- **Endpoint result: PASS**

### DELETE /sources/1

- Request: none
  - Expected: 2.02
  - Actual: 2.02 Deleted
  - Response: ok=True, error_code=0, error_msg=''
  - Verdict: PASS
- **Endpoint result: PASS**

### DELETE /curves/{created_id}

- Request: none
  - Expected: 2.02
  - Actual: NOT TESTED
  - Response: none
  - Verdict: NOT TESTED (test curve reused the pre-existing curve slot; skipped (config restore handles it))
- **Endpoint result: NOT TESTED**

### DELETE /schedules/0

- Request: none
  - Expected: 2.02
  - Actual: 2.02 Deleted
  - Response: ok=True, error_code=0, error_msg=''
  - Verdict: PASS
- **Endpoint result: PASS**

### POST /config

- Request: ConfigFile{version='3.0', fans=1, sources=1, curves=1, schedules=0}
  - Expected: 2.04
  - Actual: 2.04 Changed
  - Response: ok=True, error_code=0, error_msg=''; device returned after reboot
  - Verdict: PASS
- **Endpoint result: PASS**

### POST /system/reboot

- Request: none
  - Expected: 2.04 or 5.03
  - Actual: 5.03
  - Response: ok=False, error_code=0, error_msg='reboot pending'; device returned after pending reboot
  - Verdict: PASS
- **Endpoint result: PASS**

### GET /config (verify)

- Request: none
  - Expected: 2.05
  - Actual: 2.05 Content
  - Response: version='3.0', fans=1, sources=1, curves=1, schedules=0
  - Verdict: PASS
- **Endpoint result: PASS**

### POST /wifi/connect

- Request: WifiConnectRequest{ssid='TP-Link_E84F_2G', password='LE5!3#LS&J9Yji'}
  - Expected: 2.04 or 5.03 or TIMEOUT
  - Actual: TIMEOUT
  - Response: TIMEOUT
  - Verdict: PASS
- **Endpoint result: PASS**

