# microcoap → libcoap Migration Design

> **Date:** 2026-07-27
> **Branch:** `feat/libcoap-migration`
> **Status:** Draft

## Problem

The project uses a vendored, unmaintained microcoap (`components/microcoap/`) for CoAP communication. This creates risk: no security patches, no DTLS support, no upstream improvements. The ESP-IDF ecosystem provides `espressif/coap` (libcoap-5) as a managed component with active maintenance.

A previous migration attempt (`refactor/v3-coap-protobuf`) built successfully but failed at runtime: `coap_io_process()` returned corrupted error codes after WiFi AP_STOP → restart cycles. Root cause: `coap_free_context()` corrupts libcoap's global state across restarts.

## Goal

Replace microcoap with libcoap (`espressif/coap`) while preserving all 22 CoAP+Protobuf endpoints. No behavioral changes — same request/response format, same protobuf schema, same routes.

## Architecture

### Three-layer structure (unchanged file layout)

```
f_coap/
├── CMakeLists.txt          # Dep: espressif/coap (replaces microcoap)
├── include/f_coap.h        # Public API (unchanged interface)
├── f_coap.c                # Lifecycle: context + session + task
├── f_coap_internal.h       # Shared types (drop coap_req_ctx_t, use native types)
├── f_coap_routes.c         # 22 route handlers (native libcoap signatures)
└── f_coap_conv.c           # Native → protobuf conversion helpers (unchanged)
```

### Lifecycle model (f_coap.c)

**Critical design decision:** Persist `coap_context_t` across WiFi events. Only tear down sessions.

```
f_coap_init()
  ├── coap_new_context()          ← created once, NEVER freed during runtime
  ├── coap_register_response_handler()
  ├── coap_register_nack_handler()
  ├── register WiFi/ESPFM events
  └── create coap_task (blocked on "start" signal)

WiFi CONNECTED / AP_START:
  ├── coap_resolve_address_info()  ← resolve listen address
  ├── coap_new_endpoint()          ← bind to :5683
  └── signal coap_task to start

coap_task loop:
  └── coap_io_process(ctx, 1000)   ← 1s tick, handles IO + retransmits

WiFi DISCONNECTED / AP_STOP:
  ├── coap_endpoint_set_default(ctx, NULL)
  ├── coap_free_endpoint(ep)       ← free endpoint, NOT context
  └── coap_session_release(session)

f_coap_deinit()
  ├── coap_free_endpoint()
  ├── coap_free_context()          ← only at true shutdown
  └── free(h)
```

**Why this works:** The previous attempt called `coap_free_context()` on AP_STOP, which corrupted libcoap's global CoAP option definitions (`coap_option_defs`). By keeping the context alive and only freeing endpoints/sessions, we avoid triggering the global state corruption.

### Route handlers (f_coap_routes.c)

Replace custom `coap_req_ctx_t` with native libcoap request/response objects.

**Current signature (microcoap):**
```c
typedef void (*coap_handler_t)(struct f_coap *h, coap_req_ctx_t *ctx);
// ctx->payload, ctx->payload_len, ctx->rsp_msg, ctx->rsp_desc, ctx->rsp_code
```

**New signature (libcoap):**
```c
void handler(coap_resource_t *resource, coap_session_t *session,
             const coap_pdu_t *req, const coap_string_t *query,
             coap_pdu_t *resp, void *user_data);
// user_data = struct f_coap *h (registered at resource creation)
// Payload via coap_get_data(req, &len, &data)
// Response via coap_pdu_set_code() + coap_add_data()
```

**Resource registration (one-time at init):**
```c
coap_resource_t *r = coap_resource_init(coap_make_str_const("fans"), 0);
coap_register_handler(r, COAP_METHOD_GET, handle_fans_get);
coap_register_handler(r, COAP_METHOD_POST, handle_fans_post);
coap_register_handler(r, COAP_METHOD_PUT, COAP_METHOD_DELETE, handle_fans_put_delete);
coap_resource_set_userdata(r, h);
coap_add_resource(ctx, r);
```

### Handler migration pattern

Each of the 22 handlers follows the same migration:

1. Extract payload: `coap_get_data(req, &len, &data)` replaces `ctx->payload`/`ctx->payload_len`
2. Decode protobuf: unchanged (`pb_decode` with `pb_istream_from_buffer`)
3. Set response code: `coap_pdu_set_code(resp, COAP_RESPONSE_CODE_205_CONTENT)` replaces `ctx->rsp_code`
4. Encode response: `pb_encode` → `coap_add_data(resp, len, buf)` replaces `ctx->rsp_msg`/`ctx->rsp_desc`
5. Error responses: `coap_pdu_set_code(resp, COAP_RESPONSE_CODE_400_BAD_REQUEST)` replaces error codes

### Segment parsing

Current handlers parse URI segments from `ctx->seg[]`. In libcoap, segments come from `coap_get_uri_path(req)` which returns a linked list of `coap_string_t`.

**Helper function:**
```c
static int parse_segments(const coap_pdu_t *req, char seg[][32], int max_seg) {
    coap_string_t *path = coap_get_uri_path(req);
    // iterate path->s, split on '/', fill seg[][]
    coap_delete_string(path);
    return nseg;
}
```

### Internal header changes (f_coap_internal.h)

**Remove:**
- `coap_req_ctx_t` struct
- `coap_handler_t` typedef
- `coap_packet_t` references
- `f_coap_dispatch()` declaration

**Add:**
- `#include <coap3/coap.h>` (libcoap header)
- `coap_context_t *ctx` and `coap_endpoint_t *ep` to `struct f_coap`
- Segment parsing helper declaration

### CMakeLists.txt changes

```cmake
# Remove:
REQUIRES microcoap nanopb f_core f_fan f_source f_curve f_schedule f_config f_mdns

# Add:
REQUIRES espressif__coap nanopb f_core f_fan f_source f_curve f_schedule f_config f_mdns
PRIV_REQUIRES espressif__coap  # for mbedtls linkage
```

### sdkconfig.defaults additions

```ini
# CoAP/libcoap — DTLS support (needed for libcoap to link)
CONFIG_MBEDTLS_SSL_PROTO_DTLS=y
CONFIG_MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA=y
CONFIG_MBEDTLS_KEY_EXCHANGE_ECDHE_RSA=y
CONFIG_MBEDTLS_KEY_EXCHANGE_DHE_RSA=y
CONFIG_MBEDTLS_ECP_DP_SECP256R1_ENABLED=y
```

### Dependencies

```yaml
# main/idf_component.yml — add:
espressif/coap: "~5.3.0"
```

## Files changed

| File | Change |
|------|--------|
| `components/f_coap/CMakeLists.txt` | Swap dep: `microcoap` → `espressif__coap` |
| `components/f_coap/f_coap_internal.h` | Drop `coap_req_ctx_t`, add libcoap types to struct |
| `components/f_coap/f_coap.c` | Rewrite: context/session/task lifecycle |
| `components/f_coap/f_coap_routes.c` | Rewrite: 22 handlers to native libcoap signatures |
| `components/f_coap/include/f_coap.h` | No change (public API preserved) |
| `components/f_coap/f_coap_conv.c` | No change (conversion helpers preserved) |
| `main/idf_component.yml` | Add `espressif/coap: "~5.3.0"` |
| `main/sdkconfig.defaults` | Add DTLS/mbedtls config |
| `components/microcoap/` | **Delete** (vendored component removed) |

## Out of scope

- DTLS/CoAPs — not needed for active-maintenance migration
- Observe, Block-wise transfer — not needed now
- Shell changes — espfm_shell.py uses CoAP transport directly, unaffected
- Proto changes — none needed
- Test changes — tests use CoAPTransport wrapper, may need minor updates

## Risks

| Risk | Mitigation |
|------|-----------|
| Global state corruption on restart (previous bug) | Never call `coap_free_context()` during runtime; only at true shutdown |
| mbedtls linker errors | Include DTLS config in sdkconfig.defaults even if DTLS not used |
| Handler count (22) migration effort | Systematic pattern: each handler follows identical migration steps |
| libcoap API changes between versions | Pin to `~5.3.0`, read ESP-IDF v6.0.1 bundled version docs |

## Success criteria

1. `idf.py build` passes
2. All 22 CoAP endpoints respond correctly (test via espfm_shell.py)
3. WiFi reconnect cycle (connect → disconnect → connect) works without crash
4. No memory leaks on repeated WiFi cycles
