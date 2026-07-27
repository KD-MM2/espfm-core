# ESPFanManager (espfm-core) — Mô Tả Chi Tiết Dự Án

## Tổng Quan

**ESPFanManager** là một bộ điều khiển quạt (fan controller) đa kênh thông minh chạy trên vi điều khiển **ESP32-S3**, sử dụng **ESP-IDF v6.0.1+**. Phần mềm cho phép điều khiển tối đa **8 quạt PWM** một cách độc lập, dựa trên nhiệt độ từ cảm biến (NTC thermistor, DS18B20 1-Wire) hoặc giá trị thủ công. Dự án đang ở giai đoạn chuyển đổi từ **v2 (HTTP REST API)** sang **v3 (CoAP + Protobuf)** để tối ưu cho giao tiếp IoT nhẹ hơn, phù hợp với các mạng constrained.

## Tính Năng Chính

| Tính năng | Chi tiết |
| --- | --- |
| **Điều khiển đa kênh** | Tối đa 8 quạt PWM với điều khiển độc lập (duty cycle, chế độ, RPM feedback) |
| **Điều khiển theo nhiệt độ** | NTC thermistor (ADC1), DS18B20 (1-Wire RMT), hoặc Manual (API) |
| **Fan Curves** | Bảng tra duty-theo-nhiệt với nội suy tuyến tính, tối đa 16 curves × 10 điểm |
| **Lập lịch (Schedules)** | Ghi đè duty theo thời gian trong ngày, hỗ trợ overnight wrap, tối đa 8 luật |
| **Đồng bộ nhóm (Group Sync)** | Các quạt cùng group tự động mirror duty của quạt master (ID thấp nhất) |
| **Chẩn đoán & Cảnh báo** | Phát hiện stall (RPM=0 khi duty>0), over-temp, với cơ chế fail-safe |
| **WiFi AP+STA** | Station mode + SoftAP fallback khi mất kết nối, quét & kết nối từ dashboard |
| **Cấu hình bền vững** | LittleFS-backed `config.json`, auto-save với debounce 3 giây trên mọi thay đổi |
| **Dashboard SPA** | Giao diện single-page HTML 5 tab phục vụ từ flash, polling real-time |

## Kiến Trúc 4 Lớp

```text
┌──────────────────────────────────┐
│  Presentation                    │
│  f_coap (CoAP + Protobuf)        │  ← v3: giao thức IoT nhẹ qua UDP
│  index.html (Dashboard SPA)      │
├──────────────────────────────────┤
│  Business Logic                  │
│  f_control  — Vòng lặp 1 Hz     │  source → curve → hysteresis → ramp → duty
│  f_schedule — Dịch vụ lập lịch   │
│  f_config   — LittleFS persistence│
│  f_constraints — Input validation │
├──────────────────────────────────┤
│  Registries (Domain Model)       │
│  f_fan, f_source, f_curve        │  Pure data + CRUD, không truy cập HW
├──────────────────────────────────┤
│  HAL (Hardware Abstraction)      │
│  f_ledc (PWM), f_pcnt (Tach),    │  Thin wrappers quanh ESP-IDF APIs
│  f_adc (NTC), f_ds18b20 (1-Wire), │
│  f_gpio, f_wifi (APSTA + SNTP)   │
└──────────────────────────────────┘
```

### Chi Tiết Từng Lớp

**Lớp HAL (6 components):**

- `f_ledc` — LEDC PWM driver: 25kHz, 11-bit resolution, 1 timer dùng chung, tối đa 8 kênh
- `f_pcnt` — Pulse Counter driver: đếm xung từ cảm biến hall-effect để đo RPM, 4 units
- `f_adc` — ADC1 oneshot driver: đọc NTC thermistor qua mạch chia áp 10k, dùng Beta equation
- `f_ds18b20` — RMT-based 1-Wire driver: đọc tối đa 8 sensor DS18B20, blocking 750ms
- `f_gpio` — GPIO capability registry: quản lý pin, phát hiện xung đột, kiểm tra reserved pins
- `f_wifi` — APSTA manager + NVS persistence + SNTP time sync (non-blocking, 10s timeout)

**Lớp Registry (3 components):**

- `f_fan` — Fan channel registry: quản lý tối đa 8 kênh quạt, mỗi kênh bind với LEDC + PCNT
- `f_source` — Temperature source registry: quản lý nguồn nhiệt (NTC/DS18B20/manual)
- `f_curve` — Fan curve lookup table: nội suy tuyến tính, tối đa 16 curves × 10 điểm

**Lớp Business Logic (4 components):**

- `f_control` — Control engine task 1 Hz: đọc source → tra curve → áp hysteresis (3% deadband) → ramp limiter (10%/s up, 3%/s down) → set duty → chẩn đoán
- `f_schedule` — Schedule service: timer 60s kiểm tra lịch và ghi đè duty, hỗ trợ overnight wrap
- `f_config` — Config persistence: LittleFS, debounce 3s để giảm mòn flash
- `f_constraints` — Input validation: duty 0-100%, mode 0-1, GPIO 0-48, temp -40..+125°C, schedule 0-1439 min

**Lớp Presentation (2 components):**

- `f_coap` — CoAP server (UDP port 5683, WiFi-aware lifecycle), encode/decode Protobuf với nanopb, phân tách 3 file: `f_coap.c` (lifecycle), `f_coap_routes.c` (route handlers), `f_coap_conv.c` (proto conversion)
- `index.html` — Dashboard SPA 5 tab: Fans, Sources, Curves, Schedules, WiFi

## Thông Số Kỹ Thuật

| Thông số           | Giá trị                                                 |
| ------------------ | ------------------------------------------------------- |
| **Chip**           | ESP32-S3 (có thể chạy trên ESP32-WROOM-32)              |
| **Framework**      | ESP-IDF v6.0.1+                                         |
| **PWM**            | LEDC 25 kHz, 11-bit (0-2047), 8 kênh                    |
| **Tachometer**     | PCNT pulse counter, 4 units                             |
| **ADC**            | ADC1 oneshot, 12-bit                                    |
| **Nhiệt độ**       | NTC (Beta equation), DS18B20 (1-Wire RMT), Manual       |
| **WiFi**           | AP+STA mode, SNTP time sync                             |
| **Giao thức v3**   | CoAP (UDP 5683) + Protobuf (nanopb)                     |
| **Lưu trữ**        | 4MB Flash: 2MB app + 2MB LittleFS                       |
| **Stack**          | 8192 bytes cho CoAP task                                |

## Sơ Đồ Điều Khiển

```text
Nhiệt độ (Source) → Fan Curve (nội suy) → Hysteresis (deadband 3%)
→ Ramp Limiter (10%/s lên, 3%/s xuống) → LEDC PWM → Quạt
                                              ↓
                                         PCNT Tach → RPM feedback
                                              ↓
                                         Chẩn đoán (stall, over-temp)
```

## Chế Độ Quạt

| Chế Độ           | Mô Tả                                                                  |
| ---------------- | ---------------------------------------------------------------------- |
| **Manual (0)**   | Duty cycle cố định do người dùng đặt qua API/dashboard                 |
| **AUTO (1)**     | Duty tự động tính từ nhiệt độ nguồn → fan curve → hysteresis → ramp    |

## Giao Thức v3: CoAP + Protobuf

Phiên bản v3 thay thế HTTP REST API bằng **CoAP (Constrained Application Protocol)** qua UDP port 5683, sử dụng **Protobuf (nanopb)** để encode/decode message. Kiến trúc `f_coap` được phân tách thành 3 file:

| File                | Trách nhiệm                                                   |
| ------------------- | ------------------------------------------------------------- |
| `f_coap.c`          | Khởi tạo/dừng server, quản lý vòng đời WiFi-aware             |
| `f_coap_routes.c`   | Định nghĩa các route handler cho từng tài nguyên CoAP         |
| `f_coap_conv.c`     | Chuyển đổi Protobuf ↔ C structs của registry                  |

**Schema Protobuf** (`espfm.proto`): định nghĩa 19 messages bao gồm `FanInfo`, `SourceInfo`, `CurveInfo`, `ScheduleInfo`, `WifiStatus`, `SystemInfo`, cùng các request/response CRUD tương ứng.

**f_schema component**: chứa code nanopb được generate từ `espfm.proto`, cung cấp các struct đã optimize cho embedded (fixed-size arrays, callbacks cho strings).

## API Endpoints (v3 CoAP)

Hỗ trợ đầy đủ CRUD cho tất cả tài nguyên:

| Tài nguyên    | Endpoints                                      |
| ------------- | ---------------------------------------------- |
| **Fans**      | List, Get, Create, Update, Delete              |
| **Sources**   | List, Get, Create, Delete, Set Manual Temp     |
| **Curves**    | List, Get, Create, Update, Delete              |
| **Schedules** | List, Create, Delete                           |
| **WiFi**      | Scan, Connect, Status                          |
| **System**    | Info                                           |

## Luồng Khởi Động (main.c)

```text
1. NVS Flash init → 2. Task Watchdog → 3. Event Loop
→ 4. GPIO Registry → 5. ADC + DS18B20 + LEDC + PCNT drivers
→ 6. Fan + Source + Curve + Schedule registries
→ 7. Config load từ LittleFS → 8. CoAP server init
→ 9. WiFi APSTA (AP start ngay lập tức) → 10. Đợi WiFi connect (30s timeout)
→ 11. Control Engine start (1 Hz task) → 12. Schedule Service start
→ 13. Boot hoàn tất
```

## Trạng Thái Dự Án

| Giai đoạn | Tiến độ |
| --- | --- |
| **v2 Redesign** | **30/30 (100%)** — Hoàn thành |
| **v3 CoAP+Protobuf** | **3/4 (75%)** — f_schema, f_coap decomposition, xóa f_http đã xong; endpoint suite đang chờ test trên thiết bị |

## Các Bug Đã Biết (từ Audit)

| Mức độ         | Vấn đề                                                              |
| -------------- | ------------------------------------------------------------------- |
| 🔴 Critical    | source_id/curve_id/schedule_id không được restore khi load config   |
| 🔴 Critical    | group_id không được lưu vào config                                  |
| 🔴 Critical    | Thiếu endpoint DELETE cho sources                                   |
| 🟠 Major       | Thiếu schedule UPDATE function + endpoint                           |
| 🟠 Major       | Schedule timer có thể kích hoạt trước khi SNTP sync                 |

## Công Nghệ Sử Dụng

- **C11** — toàn bộ firmware
- **ESP-IDF v6.0.1** — framework chính
- **FreeRTOS** — task scheduling (control loop, WiFi events, CoAP server)
- **nanopb** — Protobuf codec cho embedded systems
- **microcoap** — CoAP server tối giản
- **LittleFS** — filesystem cho persistent config
- **cJSON** — JSON parsing/config storage
- **Python** — test suite (coap_client.py, test_all.py, 28 test cases)
