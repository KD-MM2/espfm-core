# microcoap → libcoap Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace vendored microcoap with ESP-IDF managed libcoap (`espressif/coap`) while preserving all 22 CoAP+Protobuf endpoints.

**Architecture:** Rewrite `f_coap.c` (lifecycle) and `f_coap_routes.c` (22 handlers) to use native libcoap-4 API. Keep `coap_context_t` alive across WiFi events (only tear down endpoints/sessions) to avoid the global state corruption that killed the previous attempt. Preserve public API (`f_coap.h`) and conversion helpers (`f_coap_conv.c`) unchanged.

**Tech Stack:** libcoap-4 (`espressif/coap ~4.3.5`), nanopb, ESP-IDF v6.0.1, FreeRTOS

---

## File Map

| File | Action | Responsibility |
|------|--------|----------------|
| `components/f_coap/CMakeLists.txt` | Modify | Swap `microcoap` → remove from REQUIRES |
| `components/f_coap/f_coap_internal.h` | Rewrite | Drop `coap_req_ctx_t`, use libcoap types |
| `components/f_coap/f_coap.c` | Rewrite | Context/session/task lifecycle |
| `components/f_coap/f_coap_routes.c` | Rewrite | 22 handlers → native libcoap signatures |
| `components/f_coap/include/f_coap.h` | No change | Public API preserved |
| `components/f_coap/f_coap_conv.c` | No change | Conversion helpers preserved |
| `main/sdkconfig.defaults` | Modify | Add DTLS/mbedtls config |
| `components/microcoap/` | Delete | Vendored component removed |

---

### Task 1: Swap dependency and delete microcoap

**Files:**
- Modify: `components/f_coap/CMakeLists.txt`
- Modify: `main/sdkconfig.defaults`
- Delete: `components/microcoap/` (entire directory)

- [ ] **Step 1: Update CMakeLists.txt dependency**

In `components/f_coap/CMakeLists.txt`, remove `microcoap` from REQUIRES:

```cmake
idf_component_register(
    SRCS f_coap.c f_coap_routes.c f_coap_conv.c
    INCLUDE_DIRS include .
    REQUIRES f_core f_fan f_source f_curve f_schedule f_config f_mdns f_constraints f_wifi f_schema nanopb
    PRIV_REQUIRES lwip esp_wifi esp_netif
)
```

`espressif/coap` is already in `main/idf_component.yml` (`^4.3.5~7`). ESP-IDF managed components are auto-linked — no need to add to REQUIRES.

- [ ] **Step 2: Add DTLS config to sdkconfig.defaults**

Append to `main/sdkconfig.defaults`:

```ini
# CoAP/libcoap — DTLS support (needed for libcoap to link even without CoAPs)
CONFIG_MBEDTLS_SSL_PROTO_DTLS=y
CONFIG_MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA=y
CONFIG_MBEDTLS_KEY_EXCHANGE_ECDHE_RSA=y
CONFIG_MBEDTLS_KEY_EXCHANGE_DHE_RSA=y
CONFIG_MBEDTLS_ECP_DP_SECP256R1_ENABLED=y
```

- [ ] **Step 3: Delete vendored microcoap**

```powershell
Remove-Item -Recurse -Force components/microcoap
```

- [ ] **Step 4: Update includes in f_coap files**

In `f_coap_internal.h`, `f_coap.c`, `f_coap_routes.c`, replace:
```c
#include "coap.h"
```
with:
```c
#include <coap3/coap.h>
```

- [ ] **Step 5: Attempt build (expect failures)**

```powershell
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue; idf.py build
```

Expected: Build fails — `coap_packet_t`, `coap_parse`, `coap_findOptions`, `coap_make_response`, `coap_build`, `coap_req_ctx_t`, `MAKE_RSPCODE`, `COAP_RSPCODE_*` don't exist in libcoap. Tasks 2-3 fix these.

- [ ] **Step 6: Commit**

```bash
git add components/f_coap/CMakeLists.txt main/sdkconfig.defaults
git rm -r components/microcoap
git commit -m "refactor(coap): swap microcoap dependency to libcoap, delete vendored component"
```

---

### Task 2: Rewrite f_coap_internal.h and f_coap.c (lifecycle)

**Files:**
- Rewrite: `components/f_coap/f_coap_internal.h`
- Rewrite: `components/f_coap/f_coap.c`

- [ ] **Step 1: Rewrite f_coap_internal.h**

Replace the entire file:

```c
/* f_coap_internal.h — shared types for f_coap (libcoap-4) */
#pragma once
#include "f_coap.h"
#include "f_fan.h"
#include "f_source.h"
#include "f_curve.h"
#include "f_schedule.h"
#include "f_config.h"
#include "f_mdns.h"
#include <coap3/coap.h>
#include "pb.h"
#include "espfm.pb.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COAP_MAX_SEG 4
#define COAP_MAX_SEG_LEN 32
#define COAP_PORT 5683
#define COAP_MTU 1280

struct f_coap {
    coap_context_t *ctx;
    coap_endpoint_t *ep;
    coap_session_t *session;
    bool running;
    TaskHandle_t task;
    SemaphoreHandle_t start_sem;
    f_fan_handle_t fan;
    f_source_handle_t source;
    f_curve_handle_t curve;
    f_schedule_handle_t schedule;
    f_config_handle_t config;
    f_mdns_handle_t mdns;
};

void f_coap_register_resources(coap_context_t *ctx, struct f_coap *h);

void f_coap_fan_to_pb(const f_fan_info_t *fi, FanInfo *pb);
void f_coap_source_to_pb(const f_source_info_t *si, SourceInfo *pb);
void f_coap_curve_to_pb(const f_curve_info_t *ci, CurveInfo *pb);
void f_coap_schedule_to_pb(const f_schedule_info_t *sci, ScheduleInfo *pb);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Rewrite f_coap.c**

Replace the entire file:

```c
/* f_coap.c — CoAP+Protobuf server lifecycle (libcoap-4) */
#include "f_coap.h"
#include "f_coap_internal.h"
#include "f_constraints.h"
#include "f_mdns.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include <coap3/coap.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "f_coap";
#define COAP_TASK_STACK 8192
#define COAP_TASK_PRIO 4

static void coap_task(void *arg)
{
    struct f_coap *h = (struct f_coap *)arg;
    while (1) {
        xSemaphoreTake(h->start_sem, portMAX_DELAY);
        ESP_LOGI(TAG, "CoAP server task running");
        while (h->running) {
            int ret = coap_io_process(h->ctx, 1000);
            if (ret < 0) {
                ESP_LOGW(TAG, "coap_io_process returned %d", ret);
                break;
            }
        }
        ESP_LOGI(TAG, "CoAP server task paused");
    }
}

static void start_endpoint(struct f_coap *h)
{
    if (h->ep) return;
    coap_address_t listen_addr;
    coap_address_init(&listen_addr);
    listen_addr.addr.sin.sin_family = AF_INET;
    listen_addr.addr.sin.sin_port = htons(COAP_PORT);
    listen_addr.addr.sin.sin_addr.s_addr = INADDR_ANY;
    h->ep = coap_new_endpoint(h->ctx, &listen_addr, COAP_PROTO_UDP);
    if (!h->ep) { ESP_LOGE(TAG, "coap_new_endpoint failed"); return; }
    h->running = true;
    xSemaphoreGive(h->start_sem);
    ESP_LOGI(TAG, "CoAP endpoint started on port %d", COAP_PORT);
}

static void stop_endpoint(struct f_coap *h)
{
    if (!h->ep) return;
    h->running = false;
    coap_free_endpoint(h->ep);
    h->ep = NULL;
    ESP_LOGI(TAG, "CoAP endpoint stopped");
}

static void on_wifi_connected(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    struct f_coap *h = (struct f_coap *)arg;
    if (h) start_endpoint(h);
}

static void on_wifi_disconnected(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    struct f_coap *h = (struct f_coap *)arg;
    if (h) stop_endpoint(h);
}

static void on_ap_stop(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    struct f_coap *h = (struct f_coap *)arg;
    if (!h) return;
    ESP_LOGI(TAG, "AP stopped, restarting CoAP endpoint");
    stop_endpoint(h);
    start_endpoint(h);
}

esp_err_t f_coap_init(f_coap_handle_t *handle, f_fan_handle_t fan,
                      f_source_handle_t source, f_curve_handle_t curve,
                      f_schedule_handle_t schedule, f_config_handle_t config,
                      f_mdns_handle_t mdns)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    struct f_coap *h = calloc(1, sizeof(*h));
    if (!h) return ESP_ERR_NO_MEM;
    h->fan = fan; h->source = source; h->curve = curve;
    h->schedule = schedule; h->config = config; h->mdns = mdns;

    coap_startup();
    h->ctx = coap_new_context(NULL);
    if (!h->ctx) { free(h); return ESP_FAIL; }

    f_coap_register_resources(h->ctx, h);

    h->start_sem = xSemaphoreCreateBinary();
    if (!h->start_sem) { coap_free_context(h->ctx); free(h); return ESP_ERR_NO_MEM; }

    esp_event_handler_register(ESPFM_EVENT, ESPFM_EVENT_WIFI_CONNECTED, on_wifi_connected, h);
    esp_event_handler_register(ESPFM_EVENT, ESPFM_EVENT_WIFI_DISCONNECTED, on_wifi_disconnected, h);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STOP, on_ap_stop, h);

    BaseType_t ret = xTaskCreate(coap_task, "coap", COAP_TASK_STACK, h, COAP_TASK_PRIO, &h->task);
    if (ret != pdPASS) {
        vSemaphoreDelete(h->start_sem);
        coap_free_context(h->ctx);
        free(h);
        return ESP_ERR_NO_MEM;
    }

    *handle = h;
    ESP_LOGI(TAG, "CoAP initialized (libcoap)");
    return ESP_OK;
}

esp_err_t f_coap_start(f_coap_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    start_endpoint(handle);
    return ESP_OK;
}

esp_err_t f_coap_stop(f_coap_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    stop_endpoint(handle);
    return ESP_OK;
}

esp_err_t f_coap_deinit(f_coap_handle_t handle)
{
    if (!handle) return ESP_OK;
    stop_endpoint(handle);
    for (int i = 0; i < 50 && handle->running; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (handle->task) { vTaskDelete(handle->task); handle->task = NULL; }
    if (handle->start_sem) vSemaphoreDelete(handle->start_sem);
    if (handle->ctx) { coap_free_context(handle->ctx); handle->ctx = NULL; }
    coap_cleanup();
    free(handle);
    return ESP_OK;
}
```

- [ ] **Step 3: Attempt build (expect route failures)**

```powershell
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue; idf.py build
```

Expected: `f_coap.c` compiles, `f_coap_routes.c` fails (still uses old API).

- [ ] **Step 4: Commit**

```bash
git add components/f_coap/f_coap.c components/f_coap/f_coap_internal.h
git commit -m "refactor(coap): rewrite lifecycle to libcoap context/endpoint model"
```

---

### Task 3: Rewrite f_coap_routes.c (22 handlers)

**Files:**
- Rewrite: `components/f_coap/f_coap_routes.c`

- [ ] **Step 1: Write the new f_coap_routes.c**

Replace the entire file. The new version:

1. Replaces `parse_uri()` with `parse_segments()` — extracts URI path from libcoap PDU
2. Replaces all 22 handler signatures to native libcoap callback format
3. Replaces dispatch table with `f_coap_register_resources()` — one resource per top-level path
4. Adds `encode_response()` / `decode_request()` helpers to replace `coap_req_ctx_t` pattern

**Migration pattern for each handler:**
- Extract payload: `coap_get_data(req, &len, &data)` replaces `ctx->payload`/`ctx->payload_len`
- Decode protobuf: `pb_decode` unchanged
- Set response: `coap_pdu_set_code(resp, CODE)` replaces `ctx->rsp_code`
- Encode response: `pb_encode` → `coap_add_data(resp, len, buf)` replaces `ctx->rsp_msg`/`ctx->rsp_desc`
- Error responses: `coap_pdu_set_code(resp, COAP_RESPONSE_CODE_400_BAD_REQUEST)` replaces `COAP_RSPCODE_BAD_REQUEST`

**Key helper functions:**

```c
static int parse_segments(const coap_pdu_t *req, char seg[][COAP_MAX_SEG_LEN], int max_seg) {
    coap_string_t *path = coap_get_uri_path(req);
    if (!path || !path->s) { if (path) coap_delete_string(path); return 0; }
    int nseg = 0;
    const uint8_t *p = path->s, *end = path->s + path->length;
    while (p < end && nseg < max_seg) {
        if (*p == '/') { p++; continue; }
        const uint8_t *start = p;
        while (p < end && *p != '/') p++;
        int len = (int)(p - start);
        if (len > COAP_MAX_SEG_LEN - 1) len = COAP_MAX_SEG_LEN - 1;
        memcpy(seg[nseg], start, len);
        seg[nseg][len] = '\0';
        nseg++;
    }
    coap_delete_string(path);
    return nseg;
}

static bool encode_response(coap_pdu_t *resp, coap_responsecode_t code,
                            const void *msg, const pb_msgdesc_t *desc) {
    coap_pdu_set_code(resp, code);
    if (!msg || !desc) return true;
    static uint8_t enc_buf[4096];
    pb_ostream_t os = pb_ostream_from_buffer(enc_buf, sizeof(enc_buf));
    if (!pb_encode(&os, desc, msg)) return false;
    coap_add_data(resp, os.bytes_written, enc_buf);
    return true;
}

static bool decode_request(const coap_pdu_t *req, void *msg, const pb_msgdesc_t *desc) {
    const uint8_t *data; size_t len;
    if (!coap_get_data(req, &len, &data) || len == 0) return false;
    pb_istream_t stream = pb_istream_from_buffer(data, len);
    return pb_decode(&stream, desc, msg);
}

static struct f_coap *get_h(coap_resource_t *resource) {
    return (struct f_coap *)coap_resource_get_userdata(resource);
}
```

**Handler signature template:**
```c
static void handle_fan_list(coap_resource_t *resource, coap_session_t *session,
                            const coap_pdu_t *req, const coap_string_t *query,
                            coap_pdu_t *resp, void *user_data) {
    struct f_coap *h = (struct f_coap *)user_data;
    // ... handler body using decode_request/encode_response ...
}
```

**Resource registration pattern:**
```c
void f_coap_register_resources(coap_context_t *ctx, struct f_coap *h) {
    coap_resource_t *r;
    r = coap_resource_init(coap_make_str_const("fans"), 0);
    coap_register_handler(r, COAP_REQUEST_METHOD_GET, handle_fan_list);
    coap_register_handler(r, COAP_REQUEST_METHOD_POST, handle_fan_create);
    coap_resource_set_userdata(r, h);
    coap_add_resource(ctx, r);
    // ... repeat for sources, curves, schedules, wifi, system ...
}
```

**Route-to-handler mapping (all 22 routes → 6 resources):**

| Resource | GET handler | POST handler | PUT handler | DELETE handler |
|----------|------------|--------------|-------------|----------------|
| `/fans` | `handle_fan_list` | `handle_fan_create` | — | — |
| `/sources` | `handle_source_list` | `handle_source_create` | — | — |
| `/curves` | `handle_curve_list` | `handle_curve_create` | — | — |
| `/schedules` | `handle_schedule_list` | `handle_schedule_create` | — | — |
| `/wifi` | `handle_wifi_scan` | `handle_wifi_connect` | — | — |
| `/system` | `handle_system_info` | — | `handle_system_hostname_put` | — |

**Sub-resource handlers (by ID)** need special handling. libcoap matches resources by exact URI path, so `/fans/3` won't match `/fans`. Options:

1. **Use `coap_resource_init` with wildcards** (libcoap-4 supports `*` wildcards)
2. **Register sub-resources dynamically** (not recommended for embedded)
3. **Use a single catch-all resource** with manual routing

For this migration, use **wildcard resources** if available in libcoap-4:
```c
r = coap_resource_init(coap_make_str_const("fans/*"), 0);
coap_register_handler(r, COAP_REQUEST_METHOD_GET, handle_fan_get);
coap_register_handler(r, COAP_REQUEST_METHOD_PUT, handle_fan_update);
coap_register_handler(r, COAP_REQUEST_METHOD_DELETE, handle_fan_delete);
```

If wildcards aren't supported, register a single `/fans` resource and dispatch by segment count inside the handler (check `parse_segments` result).

- [ ] **Step 2: Build**

```powershell
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue; idf.py build
```

Expected: Compiles clean. Fix any API differences iteratively.

Common libcoap-4 API names:
- `COAP_REQUEST_METHOD_GET` (not `COAP_METHOD_GET`)
- `coap_pdu_set_code(resp, COAP_RESPONSE_CODE_205_CONTENT)`
- `coap_add_data(resp, len, buf)` for response payload
- `coap_get_data(req, &len, &data)` for request payload

- [ ] **Step 3: Commit**

```bash
git add components/f_coap/f_coap_routes.c
git commit -m "refactor(coap): rewrite 22 route handlers to native libcoap signatures"
```

---

### Task 4: Build verification and fix iteration

**Files:**
- Any `components/f_coap/*.c` that needs fixes

- [ ] **Step 1: Clean build**

```powershell
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue; idf.py build
```

Expected: Build passes. If not, fix errors iteratively.

Common issues:
- `coap_startup()` / `coap_cleanup()` placement
- `coap_new_context(NULL)` — NULL = default allocator
- `coap_free_endpoint()` vs `coap_free_context()` — only free context in `f_coap_deinit`
- Missing `#include <coap3/coap.h>` in some files

- [ ] **Step 2: Commit build fixes**

```bash
git add components/f_coap/
git commit -m "fix(coap): resolve libcoap-4 API differences for clean build"
```

---

### Task 5: Flash test and runtime verification

**Files:**
- None (testing only)

- [ ] **Step 1: Flash to device**

```powershell
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue; idf.py flash
```

- [ ] **Step 2: Monitor boot**

```powershell
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue; idf.py monitor
```

Expected: Device boots, "CoAP initialized (libcoap)" log appears, WiFi connects, "CoAP endpoint started on port 5683" appears.

- [ ] **Step 3: Test basic endpoint**

```powershell
python tools/espfm_shell.py
# In shell: system info
```

Expected: Returns system info with hostname, version, uptime.

- [ ] **Step 4: Test all endpoint groups**

```
fans list
fans get 0
sources list
curves list
schedules list
wifi scan
wifi status
system info
```

All should return valid protobuf responses.

- [ ] **Step 5: Test WiFi reconnect cycle (critical)**

This is the test that killed the previous attempt:
```
# In shell: wifi disconnect
# Wait 5s for disconnect
# In shell: wifi connect <ssid> <password>
# Wait for reconnect
# In shell: system info
```

Expected: CoAP endpoint restarts after reconnect. No crash, no corrupted error codes.

- [ ] **Step 6: Commit (if runtime fixes needed)**

```bash
git add components/f_coap/
git commit -m "fix(coap): runtime fixes for libcoap migration"
```

---

### Task 6: Cleanup and final verification

**Files:**
- None

- [ ] **Step 1: Verify no microcoap references remain**

```powershell
grep -r "microcoap\|coap_parse\|coap_build\|coap_findOptions\|coap_make_response\|coap_packet_t\|coap_req_ctx_t" components/ main/
```

Expected: No matches.

- [ ] **Step 2: Verify test suite**

```powershell
python -m pytest tests/ -v
```

Expected: Tests pass. If `CoAPTransport` needs updates, fix them.

- [ ] **Step 3: Final commit**

```bash
git add -A
git commit -m "chore(coap): complete microcoap to libcoap migration"
```
