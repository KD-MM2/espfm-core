#pragma once
/* Internal helpers exposed for unit testing only.
 * Do NOT include this from production code. */

#include "f_http.h"
#include "cJSON.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* JSON serializers (testable pure functions) */
cJSON *fan_to_json(const f_fan_info_t *info);
cJSON *source_to_json(const f_source_info_t *info);
cJSON *curve_to_json(const f_curve_info_t *info);
cJSON *schedule_to_json(const f_schedule_info_t *info);

/* URI path parameter extraction */
int get_path_param(const char *uri, const char *base, uint8_t *id_out);

/* Content-Type by extension */
const char *get_content_type(const char *path);

#ifdef __cplusplus
}
#endif
