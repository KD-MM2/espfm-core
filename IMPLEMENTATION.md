# ESPFanManager v2 — Implementation Status

> Based on: `.hermes/plans/2026-06-12_010500-espfm-v2-redesign.md`

## Build Phases

### Phase 0: Foundation (6/6) Done
- [x] 0.1 Project scaffold
- [x] 0.2 NVS init + f_core shared types
- [x] 0.3 f_gpio GPIO capability registry
- [x] 0.4 Task watchdog init
- [x] 0.5 f_wifi WiFi station manager
- [x] 0.6 ESP event loop + custom event base

### Phase 1: Hardware Drivers (6/6) Done
- [x] 1.1 f_ledc LEDC PWM driver
- [x] 1.2 f_pcnt PCNT tachometer driver
- [x] 1.3 f_adc ADC NTC driver
- [x] 1.4 f_ds18b20 1-Wire driver
- [x] 1.5 f_fan Fan channel model + registry
- [x] 1.6 f_source Temperature source registry

### Phase 2: Control Logic (5/5) Done
- [x] 2.1 f_curve Fan curve lookup table
- [x] 2.2 Hysteresis filter
- [x] 2.3 Slew-rate limiter
- [x] 2.4 f_control Control engine task
- [x] 2.5 Fail-safe handler

### Phase 3: Network & Storage (6/6) Done
- [x] 3.1 LittleFS partition + f_config persistent config
- [x] 3.2 f_http REST API server (fans endpoints)
- [x] 3.3 f_http sources + curves + schedules endpoints
- [x] 3.4 f_http static file serving from LittleFS
- [x] 3.5 WiFi-aware HTTP lifecycle (event-driven start/stop)
- [x] 3.6 f_http system info endpoint + CORS

### Phase 4: Scheduling & Diagnostics (4/4) Done
- [x] 4.1 SNTP time sync
- [x] 4.2 f_schedule Schedule service
- [x] 4.3 Group synchronization
- [x] 4.4 Diagnostics & alarms (stall detection, over-temp)

### Phase 5: Integration & Polish (3/3) Done
- [x] 5.1 main.c full wiring (all components, production loop)
- [x] 5.2 Dashboard SPA (index.html)
- [x] 5.3 README + IMPLEMENTATION.md final

## Progress Tracking

| Phase | Total | Done | Progress |
|-------|-------|------|----------|
| Phase 0 | 6 | 6 | 100% |
| Phase 1 | 6 | 6 | 100% |
| Phase 2 | 5 | 5 | 100% |
| Phase 3 | 6 | 6 | 100% |
| Phase 4 | 4 | 4 | 100% |
| Phase 5 | 3 | 3 | 100% |
| **Total** | **30** | **30** | **100%** |

### v3 CoAP+Protobuf Refactor (2026-06-19)

- [x] f_schema component with nanopb-generated code from espfm.proto
- [x] f_coap decomposition (lifecycle / routes / conversion)
- [x] Remove f_http + stale PB artifacts
- [x] f_config migrated from cJSON to nanopb Protobuf (config.pb)
- [x] Build passes, endpoint suite pending device test

| Phase | Total | Done | Progress |
|-------|-------|------|----------|
| v3 Refactor | 5 | 5 | 100% |
