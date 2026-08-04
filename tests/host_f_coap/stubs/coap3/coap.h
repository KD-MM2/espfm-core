#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Minimal libcoap-4 fake header for host-based unit tests of f_coap_routes.c.
 * Only the types/constants/functions the route-handler TU references are
 * declared.  Definitions are provided by the test TU or GNU ld --wrap hooks;
 * with --gc-sections only the symbols reachable from the tests need real
 * definitions at link time. */

typedef struct coap_context_t_ {
    int _dummy;
} coap_context_t;

typedef struct coap_endpoint_t_ {
    int _dummy;
} coap_endpoint_t;

typedef struct coap_pdu_t_ {
    int _dummy;
} coap_pdu_t;

typedef struct coap_session_t_ {
    int _dummy;
} coap_session_t;

typedef struct coap_resource_t_ {
    int _dummy;
} coap_resource_t;

typedef struct coap_string_t_ {
    size_t length;
    uint8_t *s;
} coap_string_t;

typedef struct coap_str_const_t_ {
    size_t length;
    const uint8_t *s;
} coap_str_const_t;

/* struct f_coap in f_coap_internal.h carries a FreeRTOS task handle; on host
 * an opaque pointer is enough. */
typedef void *TaskHandle_t;

typedef enum coap_pdu_code_t {
    COAP_REQUEST_CODE_GET                  = 1,
    COAP_REQUEST_CODE_POST                 = 2,
    COAP_REQUEST_CODE_PUT                  = 3,
    COAP_REQUEST_CODE_DELETE               = 4,
    COAP_RESPONSE_CODE_CREATED             = 0x41,
    COAP_RESPONSE_CODE_DELETED             = 0x42,
    COAP_RESPONSE_CODE_CHANGED             = 0x44,
    COAP_RESPONSE_CODE_CONTENT             = 0x45,
    COAP_RESPONSE_CODE_BAD_REQUEST         = 0x80,
    COAP_RESPONSE_CODE_NOT_FOUND           = 0x84,
    COAP_RESPONSE_CODE_INTERNAL_ERROR      = 0xA0,
    COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE = 0xA3,
} coap_pdu_code_t;

typedef void (*coap_method_handler_t)(coap_resource_t *resource, coap_session_t *session,
                                      const coap_pdu_t *request, const coap_string_t *query,
                                      coap_pdu_t *response);
typedef void (*coap_release_large_data_t)(coap_session_t *session, void *app_ptr);

#define COAP_MEDIATYPE_APPLICATION_OCTET_STREAM 42

/* Route-handler / registration API (compile-time declarations). */
void *coap_resource_get_userdata(coap_resource_t *resource);
void coap_resource_set_userdata(coap_resource_t *resource, void *data);
coap_resource_t *coap_resource_init(coap_str_const_t *uri_path, int flags);
coap_str_const_t *coap_make_str_const(const char *string);
void coap_register_handler(coap_resource_t *resource, coap_pdu_code_t method,
                           coap_method_handler_t handler);
void coap_add_resource(coap_context_t *context, coap_resource_t *resource);
void coap_pdu_set_code(coap_pdu_t *pdu, coap_pdu_code_t code);
coap_string_t *coap_get_uri_path(const coap_pdu_t *request);
void coap_delete_string(coap_string_t *string);
int coap_get_data(const coap_pdu_t *pdu, size_t *len, const uint8_t **data);
int coap_add_data(coap_pdu_t *pdu, size_t len, const uint8_t *data);
int coap_add_data_large_response(coap_resource_t *resource, coap_session_t *session,
                                 const coap_pdu_t *request, coap_pdu_t *response,
                                 const coap_string_t *query, uint16_t media_type, int maxage,
                                 uint64_t etag, size_t length, const uint8_t *data,
                                 coap_release_large_data_t release_func, void *app_ptr);
