#include "f_http.h"
#include "f_constraints.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>

static const char *TAG = "f_http";

/* Validation helper — evaluate constraint, return 400 with message on failure */
#define CHECK(expr, req) do {               \
    const char *_v = NULL;                  \
    if ((expr) != ESP_OK)                   \
        return send_error((req), _v, 400);  \
} while(0)

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

#define HTTPD_SCRATCH_SIZE 8192

static void add_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods",
                       "GET, PUT, POST, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json_obj, const char *http_status)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    char *str = cJSON_PrintUnformatted(json_obj);
    if (str == NULL) return ESP_ERR_NO_MEM;
    httpd_resp_set_status(req, http_status);
    esp_err_t err = httpd_resp_send(req, str, strlen(str));
    cJSON_free(str);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *message, int http_code)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "error");
    cJSON *err_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(err_obj, "code", http_code);
    cJSON_AddStringToObject(err_obj, "message", message);
    cJSON_AddItemToObject(root, "error", err_obj);
    const char *status_str = http_code == 400 ? "400 Bad Request" :
                             http_code == 404 ? "404 Not Found" :
                             http_code == 503 ? "503 Service Unavailable" : "500 Internal Server Error";
    return send_json(req, root, status_str);
}

int get_path_param(const char *uri, const char *base, uint8_t *id_out)
{
    int base_len = strlen(base);
    if (strncmp(uri, base, base_len) != 0) return -1;
    const char *rest = uri + base_len;
    if (*rest == '\0' || *rest == '?') return -1;

    char *endptr = NULL;
    long val = strtol(rest, &endptr, 10);
    if (endptr == rest || val < 0 || val > 255) return -1;
    *id_out = (uint8_t)val;
    return 0;
}

static bool read_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    int total = 0;
    int remaining = req->content_len;
    while (remaining > 0 && total < (int)(buf_size - 1)) {
        int r = httpd_req_recv(req, buf + total, remaining);
        if (r <= 0) break;
        total += r;
        remaining -= r;
    }
    buf[total] = '\0';
    return (total > 0);
}

const char *get_content_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (ext == NULL) return "application/octet-stream";
    ext++;
    if (!strcasecmp(ext, "html") || !strcasecmp(ext, "htm")) return "text/html";
    if (!strcasecmp(ext, "css"))  return "text/css";
    if (!strcasecmp(ext, "js"))   return "application/javascript";
    if (!strcasecmp(ext, "json")) return "application/json";
    if (!strcasecmp(ext, "png"))  return "image/png";
    if (!strcasecmp(ext, "jpg") || !strcasecmp(ext, "jpeg")) return "image/jpeg";
    if (!strcasecmp(ext, "svg"))  return "image/svg+xml";
    if (!strcasecmp(ext, "ico"))  return "image/x-icon";
    return "application/octet-stream";
}

/* ------------------------------------------------------------------ */
/*  f_http struct                                                      */
/* ------------------------------------------------------------------ */

struct f_http {
    httpd_handle_t      server;
    bool                running;
    f_fan_handle_t      fan;
    f_source_handle_t   source;
    f_curve_handle_t    curve;
    f_schedule_handle_t schedule;
    f_config_handle_t   config;
};

/* ------------------------------------------------------------------ */
/*  WiFi event handlers                                                */
/* ------------------------------------------------------------------ */

static void _on_wifi_connected(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    f_http_handle_t h = (f_http_handle_t)arg;
    if (h == NULL) return;
    f_http_start(h);
}

static void _on_wifi_disconnected(void *arg, esp_event_base_t base,
                                  int32_t id, void *data)
{
    f_http_handle_t h = (f_http_handle_t)arg;
    if (h == NULL) return;
    f_http_stop(h);
}

/* ================================================================== */
/*  OPTIONS handler (CORS preflight)                                   */
/* ================================================================== */

static esp_err_t options_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ================================================================== */
/*  Fan Helpers                                                        */
/* ================================================================== */

cJSON *fan_to_json(const f_fan_info_t *info)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", info->id);
    cJSON_AddStringToObject(o, "name", info->name);
    cJSON_AddNumberToObject(o, "mode", info->mode);
    cJSON_AddNumberToObject(o, "duty", info->duty);
    cJSON_AddNumberToObject(o, "rpm", info->rpm);
    cJSON_AddBoolToObject(o, "enabled", info->enabled);
    cJSON_AddBoolToObject(o, "inverted", info->inverted);
    cJSON_AddNumberToObject(o, "pwm_gpio", info->pwm_gpio);
    cJSON_AddNumberToObject(o, "tach_gpio", info->tach_gpio);
    cJSON_AddNumberToObject(o, "source_id", info->source_id);
    cJSON_AddNumberToObject(o, "curve_id", info->curve_id);
    cJSON_AddNumberToObject(o, "schedule_id", info->schedule_id);
    cJSON_AddNumberToObject(o, "group_id", info->group_id);
    const char *alarm_str = "none";
    if (info->alarm == FAN_ALARM_STALL)   alarm_str = "stall";
    if (info->alarm == FAN_ALARM_OVERTEMP) alarm_str = "overtemp";
    cJSON_AddStringToObject(o, "alarm", alarm_str);
    return o;
}

/* ================================================================== */
/*  Fan Endpoints                                                      */
/* ================================================================== */

static esp_err_t fans_list_handler(httpd_req_t *req)
{
    f_http_handle_t h = (f_http_handle_t)req->user_ctx;
    if (h == NULL || h->fan == NULL) return send_error(req, "not ready", 503);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON *arr = cJSON_AddArrayToObject(root, "data");
    for (uint8_t i = 0; i < F_FAN_MAX_COUNT; i++) {
        f_fan_info_t info;
        if (f_fan_get_info(h->fan, i, &info) == ESP_OK)
            cJSON_AddItemToArray(arr, fan_to_json(&info));
    }
    return send_json(req, root, "200 OK");
}

static esp_err_t fan_get_handler(httpd_req_t *req)
{
    f_http_handle_t h = (f_http_handle_t)req->user_ctx;
    if (h == NULL || h->fan == NULL) return send_error(req, "not ready", 503);

    uint8_t id;
    if (get_path_param(req->uri, "/api/v1/fans/", &id) != 0)
        return send_error(req, "invalid id", 400);

    f_fan_info_t info;
    if (f_fan_get_info(h->fan, id, &info) != ESP_OK)
        return send_error(req, "fan not found", 404);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddItemToObject(root, "data", fan_to_json(&info));
    return send_json(req, root, "200 OK");
}

static esp_err_t fan_create_handler(httpd_req_t *req)
{
    f_http_handle_t h = (f_http_handle_t)req->user_ctx;
    if (h == NULL || h->fan == NULL) return send_error(req, "not ready", 503);

    char body[1024];
    if (!read_body(req, body, sizeof(body)))
        return send_error(req, "empty body", 400);

    cJSON *json = cJSON_Parse(body);
    if (json == NULL) return send_error(req, "invalid JSON", 400);

    cJSON *pwm_node = cJSON_GetObjectItem(json, "pwm_gpio");
    cJSON *name_node = cJSON_GetObjectItem(json, "name");
    if (!cJSON_IsNumber(pwm_node) || !cJSON_IsString(name_node)) {
        cJSON_Delete(json);
        return send_error(req, "pwm_gpio and name required", 400);
    }

    int gpio_val = pwm_node->valueint;
    CHECK(f_constraints_gpio(gpio_val, &_v), req);
    CHECK(f_constraints_name(name_node->valuestring, &_v), req);
    CHECK(f_constraints_fan_count(f_fan_get_count(h->fan), &_v), req);

    uint8_t gpio = (uint8_t)gpio_val;
    uint8_t tach = F_FAN_TACH_NONE;
    cJSON *tach_node = cJSON_GetObjectItem(json, "tach_gpio");
    if (cJSON_IsNumber(tach_node)) {
        CHECK(f_constraints_gpio(tach_node->valueint, &_v), req);
        tach = (uint8_t)tach_node->valueint;
    }

    uint8_t new_id;
    esp_err_t err = f_fan_add(h->fan, gpio, tach, name_node->valuestring, &new_id);
    cJSON_Delete(json);
    if (err != ESP_OK) return send_error(req, "failed to create fan", 500);

    if (h->config) f_config_save_all(h->config, h->fan, h->source, h->curve, h->schedule);

    f_fan_info_t info;
    f_fan_get_info(h->fan, new_id, &info);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddItemToObject(root, "data", fan_to_json(&info));
    return send_json(req, root, "201 Created");
}

static esp_err_t fan_update_handler(httpd_req_t *req)
{
    f_http_handle_t h = (f_http_handle_t)req->user_ctx;
    if (h == NULL || h->fan == NULL) return send_error(req, "not ready", 503);

    uint8_t id;
    if (get_path_param(req->uri, "/api/v1/fans/", &id) != 0)
        return send_error(req, "invalid id", 400);

    f_fan_info_t info;
    if (f_fan_get_info(h->fan, id, &info) != ESP_OK)
        return send_error(req, "fan not found", 404);

    char body[1024];
    if (!read_body(req, body, sizeof(body)))
        return send_error(req, "empty body", 400);

    cJSON *json = cJSON_Parse(body);
    if (json == NULL) return send_error(req, "invalid JSON", 400);

    cJSON *mode_node = cJSON_GetObjectItem(json, "mode");
    if (cJSON_IsNumber(mode_node)) {
        CHECK(f_constraints_mode(mode_node->valueint, &_v), req);
        f_fan_set_mode(h->fan, id, (fan_mode_t)mode_node->valueint);
    }

    cJSON *duty_node = cJSON_GetObjectItem(json, "duty");
    if (cJSON_IsNumber(duty_node)) {
        CHECK(f_constraints_duty(duty_node->valueint, &_v), req);
        f_fan_set_duty(h->fan, id, (uint8_t)duty_node->valueint);
    }

    cJSON *source_node = cJSON_GetObjectItem(json, "source_id");
    if (cJSON_IsNumber(source_node)) {
        uint8_t sid = (uint8_t)source_node->valueint;
        if (sid != 0xFF) {
            f_source_info_t si;
            if (f_source_get_info(h->source, sid, &si) != ESP_OK) {
                cJSON_Delete(json);
                return send_error(req, "source_id not found", 400);
            }
        }
        f_fan_set_source(h->fan, id, sid);
    }

    cJSON *curve_node = cJSON_GetObjectItem(json, "curve_id");
    if (cJSON_IsNumber(curve_node)) {
        uint8_t cid = (uint8_t)curve_node->valueint;
        if (cid != 0xFF) {
            f_curve_info_t ci;
            if (f_curve_get_info(h->curve, cid, &ci) != ESP_OK) {
                cJSON_Delete(json);
                return send_error(req, "curve_id not found", 400);
            }
        }
        f_fan_set_curve(h->fan, id, cid);
    }

    cJSON *sched_node = cJSON_GetObjectItem(json, "schedule_id");
    if (cJSON_IsNumber(sched_node)) {
        uint8_t scid = (uint8_t)sched_node->valueint;
        if (scid != 0xFF) {
            f_schedule_info_t si;
            if (f_schedule_get_info(h->schedule, scid, &si) != ESP_OK) {
                cJSON_Delete(json);
                return send_error(req, "schedule_id not found", 400);
            }
        }
        f_fan_set_schedule(h->fan, id, scid);
    }

    cJSON *inv_node = cJSON_GetObjectItem(json, "inverted");
    if (cJSON_IsBool(inv_node))
        f_fan_set_inverted(h->fan, id, cJSON_IsTrue(inv_node));

    cJSON *group_node = cJSON_GetObjectItem(json, "group_id");
    if (cJSON_IsNumber(group_node))
        f_fan_set_group(h->fan, id, (uint8_t)group_node->valueint);

    cJSON_Delete(json);

    if (h->config) f_config_save_all(h->config, h->fan, h->source, h->curve, h->schedule);

    f_fan_get_info(h->fan, id, &info);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddItemToObject(root, "data", fan_to_json(&info));
    return send_json(req, root, "200 OK");
}

static esp_err_t fan_delete_handler(httpd_req_t *req)
{
    f_http_handle_t h = (f_http_handle_t)req->user_ctx;
    if (h == NULL || h->fan == NULL) return send_error(req, "not ready", 503);

    uint8_t id;
    if (get_path_param(req->uri, "/api/v1/fans/", &id) != 0)
        return send_error(req, "invalid id", 400);

    if (f_fan_remove(h->fan, id) != ESP_OK)
        return send_error(req, "fan not found", 404);

    if (h->config) f_config_save_all(h->config, h->fan, h->source, h->curve, h->schedule);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddNullToObject(root, "data");
    return send_json(req, root, "200 OK");
}

/* ================================================================== */
/*  Source Endpoints                                                   */
/* ================================================================== */

cJSON *source_to_json(const f_source_info_t *info)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", info->id);
    cJSON_AddStringToObject(o, "name", info->name);
    const char *t = (info->type == SOURCE_TYPE_NTC) ? "ntc" :
                    (info->type == SOURCE_TYPE_DS18B20) ? "ds18b20" : "manual";
    cJSON_AddStringToObject(o, "type", t);
    const char *s = (info->status == SOURCE_STATUS_VALID) ? "valid" :
                    (info->status == SOURCE_STATUS_STALE) ? "stale" : "invalid";
    cJSON_AddStringToObject(o, "status", s);
    cJSON_AddNumberToObject(o, "temp_c", info->temp_c);
    cJSON_AddNumberToObject(o, "gpio", info->gpio);
    return o;
}

static esp_err_t sources_list_handler(httpd_req_t *req)
{
    f_http_handle_t h = (f_http_handle_t)req->user_ctx;
    if (h == NULL || h->source == NULL) return send_error(req, "not ready", 503);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON *arr = cJSON_AddArrayToObject(root, "data");

    for (uint8_t i = 0; i < F_SOURCE_MAX_COUNT; i++) {
        f_source_info_t info;
        if (f_source_get_info(h->source, i, &info) == ESP_OK)
            cJSON_AddItemToArray(arr, source_to_json(&info));
    }
    return send_json(req, root, "200 OK");
}

static esp_err_t source_create_handler(httpd_req_t *req)
{
    f_http_handle_t h = (f_http_handle_t)req->user_ctx;
    if (h == NULL || h->source == NULL) return send_error(req, "not ready", 503);

    char body[1024];
    if (!read_body(req, body, sizeof(body)))
        return send_error(req, "empty body", 400);

    cJSON *json = cJSON_Parse(body);
    if (json == NULL) return send_error(req, "invalid JSON", 400);

    cJSON *type_node = cJSON_GetObjectItem(json, "type");
    cJSON *name_node = cJSON_GetObjectItem(json, "name");
    if (!cJSON_IsString(type_node) || !cJSON_IsString(name_node)) {
        cJSON_Delete(json);
        return send_error(req, "type and name required", 400);
    }

    CHECK(f_constraints_name(name_node->valuestring, &_v), req);
    CHECK(f_constraints_source_count(f_source_get_count(h->source), &_v), req);

    source_type_t type;
    const char *tstr = type_node->valuestring;
    if (!strcmp(tstr, "ntc")) type = SOURCE_TYPE_NTC;
    else if (!strcmp(tstr, "ds18b20")) type = SOURCE_TYPE_DS18B20;
    else if (!strcmp(tstr, "manual")) type = SOURCE_TYPE_MANUAL;
    else { cJSON_Delete(json); return send_error(req, "unknown type", 400); }

    uint8_t gpio = F_SOURCE_GPIO_NONE;
    cJSON *gpio_node = cJSON_GetObjectItem(json, "gpio");
    if (cJSON_IsNumber(gpio_node)) {
        CHECK(f_constraints_gpio(gpio_node->valueint, &_v), req);
        gpio = (uint8_t)gpio_node->valueint;
    }

    uint8_t new_id;
    esp_err_t err = f_source_add(h->source, type, gpio,
                                  name_node->valuestring, &new_id);
    cJSON_Delete(json);
    if (err != ESP_OK) return send_error(req, "failed to create source", 500);

    if (h->config) f_config_save_all(h->config, h->fan, h->source, h->curve, h->schedule);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "id", new_id);
    cJSON_AddItemToObject(root, "data", data);
    return send_json(req, root, "201 Created");
}

static esp_err_t source_temp_handler(httpd_req_t *req)
{
    f_http_handle_t h = (f_http_handle_t)req->user_ctx;
    if (h == NULL || h->source == NULL) return send_error(req, "not ready", 503);

    char body[512];
    if (!read_body(req, body, sizeof(body)))
        return send_error(req, "empty body", 400);

    cJSON *json = cJSON_Parse(body);
    if (json == NULL) return send_error(req, "invalid JSON", 400);

    cJSON *id_node = cJSON_GetObjectItem(json, "id");
    cJSON *temp_node = cJSON_GetObjectItem(json, "temp_c");
    if (!cJSON_IsNumber(id_node) || !cJSON_IsNumber(temp_node)) {
        cJSON_Delete(json);
        return send_error(req, "id and temp_c required", 400);
    }

    CHECK(f_constraints_temp_c((float)temp_node->valuedouble, &_v), req);

    esp_err_t err = f_source_update_manual(h->source,
                       (uint8_t)id_node->valueint,
                       (float)temp_node->valuedouble);
    cJSON_Delete(json);
    if (err != ESP_OK) return send_error(req, "source not found or not manual", 400);

    if (h->config) f_config_save_all(h->config, h->fan, h->source, h->curve, h->schedule);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddNullToObject(root, "data");
    return send_json(req, root, "200 OK");
}

static esp_err_t source_delete_handler(httpd_req_t *req)
{
    f_http_handle_t h = (f_http_handle_t)req->user_ctx;
    if (h == NULL || h->source == NULL) return send_error(req, "not ready", 503);

    uint8_t id;
    if (get_path_param(req->uri, "/api/v1/sources/", &id) != 0)
        return send_error(req, "invalid id", 400);

    if (f_source_remove(h->source, id) != ESP_OK)
        return send_error(req, "source not found", 404);

    if (h->config) f_config_save_all(h->config, h->fan, h->source, h->curve, h->schedule);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddNullToObject(root, "data");
    return send_json(req, root, "200 OK");
}

/* ================================================================== */
/*  Curve Endpoints                                                    */
/* ================================================================== */

cJSON *curve_to_json(const f_curve_info_t *info)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", info->id);
    cJSON_AddStringToObject(o, "name", info->name);
    cJSON *pts = cJSON_AddArrayToObject(o, "points");
    for (int i = 0; i < info->num_points; i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "temp_c", info->points[i].temp_c);
        cJSON_AddNumberToObject(p, "duty", info->points[i].duty);
        cJSON_AddItemToArray(pts, p);
    }
    return o;
}

static esp_err_t curves_list_handler(httpd_req_t *req)
{
    f_http_handle_t h = (f_http_handle_t)req->user_ctx;
    if (h == NULL || h->curve == NULL) return send_error(req, "not ready", 503);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON *arr = cJSON_AddArrayToObject(root, "data");
    for (uint8_t i = 0; i < F_CURVE_MAX_COUNT; i++) {
        f_curve_info_t info;
        if (f_curve_get_info(h->curve, i, &info) == ESP_OK)
            cJSON_AddItemToArray(arr, curve_to_json(&info));
    }
    return send_json(req, root, "200 OK");
}

static esp_err_t curve_get_handler(httpd_req_t *req)
{
    f_http_handle_t h = (f_http_handle_t)req->user_ctx;
    if (h == NULL || h->curve == NULL) return send_error(req, "not ready", 503);

    uint8_t id;
    if (get_path_param(req->uri, "/api/v1/curves/", &id) != 0)
        return send_error(req, "invalid id", 400);

    f_curve_info_t info;
    if (f_curve_get_info(h->curve, id, &info) != ESP_OK)
        return send_error(req, "curve not found", 404);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddItemToObject(root, "data", curve_to_json(&info));
    return send_json(req, root, "200 OK");
}

static esp_err_t curve_create_handler(httpd_req_t *req)
{
    f_http_handle_t h = (f_http_handle_t)req->user_ctx;
    if (h == NULL || h->curve == NULL) return send_error(req, "not ready", 503);

    char body[2048];
    if (!read_body(req, body, sizeof(body)))
        return send_error(req, "empty body", 400);

    cJSON *json = cJSON_Parse(body);
    if (json == NULL) return send_error(req, "invalid JSON", 400);

    cJSON *name_node = cJSON_GetObjectItem(json, "name");
    cJSON *pts_node = cJSON_GetObjectItem(json, "points");
    if (!cJSON_IsString(name_node) || !cJSON_IsArray(pts_node)) {
        cJSON_Delete(json);
        return send_error(req, "name and points required", 400);
    }

    CHECK(f_constraints_name(name_node->valuestring, &_v), req);
    CHECK(f_constraints_curve_count(f_curve_get_count(h->curve), &_v), req);

    f_curve_info_t info;
    memset(&info, 0, sizeof(info));
    strncpy(info.name, name_node->valuestring, ESPFM_NAME_MAX - 1);
    info.num_points = (uint8_t)cJSON_GetArraySize(pts_node);
    if (info.num_points > F_CURVE_MAX_POINTS) info.num_points = F_CURVE_MAX_POINTS;

    for (int i = 0; i < info.num_points; i++) {
        cJSON *pt = cJSON_GetArrayItem(pts_node, i);
        cJSON *tc = cJSON_GetObjectItem(pt, "temp_c");
        cJSON *d  = cJSON_GetObjectItem(pt, "duty");
        info.points[i].temp_c = (float)(tc ? tc->valuedouble : 0.0);
        info.points[i].duty   = (uint8_t)(d ? d->valueint : 0);
    }

    CHECK(f_constraints_curve_points(info.points, info.num_points, &_v), req);

    uint8_t new_id;
    esp_err_t err = f_curve_upsert(h->curve, &info, &new_id);
    cJSON_Delete(json);
    if (err != ESP_OK) return send_error(req, "failed to create curve", 500);

    if (h->config) f_config_save_all(h->config, h->fan, h->source, h->curve, h->schedule);

    f_curve_get_info(h->curve, new_id, &info);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddItemToObject(root, "data", curve_to_json(&info));
    return send_json(req, root, "201 Created");
}

static esp_err_t curve_update_handler(httpd_req_t *req)
{
    f_http_handle_t h = (f_http_handle_t)req->user_ctx;
    if (h == NULL || h->curve == NULL) return send_error(req, "not ready", 503);

    uint8_t id;
    if (get_path_param(req->uri, "/api/v1/curves/", &id) != 0)
        return send_error(req, "invalid id", 400);

    char body[2048];
    if (!read_body(req, body, sizeof(body)))
        return send_error(req, "empty body", 400);

    cJSON *json = cJSON_Parse(body);
    if (json == NULL) return send_error(req, "invalid JSON", 400);

    f_curve_info_t info;
    memset(&info, 0, sizeof(info));
    info.id = id;

    cJSON *name_node = cJSON_GetObjectItem(json, "name");
    if (cJSON_IsString(name_node)) {
        CHECK(f_constraints_name(name_node->valuestring, &_v), req);
        strncpy(info.name, name_node->valuestring, ESPFM_NAME_MAX - 1);
    }

    cJSON *pts_node = cJSON_GetObjectItem(json, "points");
    if (cJSON_IsArray(pts_node)) {
        info.num_points = (uint8_t)cJSON_GetArraySize(pts_node);
        if (info.num_points > F_CURVE_MAX_POINTS) info.num_points = F_CURVE_MAX_POINTS;
        for (int i = 0; i < info.num_points; i++) {
            cJSON *pt = cJSON_GetArrayItem(pts_node, i);
            cJSON *tc = cJSON_GetObjectItem(pt, "temp_c");
            cJSON *d  = cJSON_GetObjectItem(pt, "duty");
            info.points[i].temp_c = (float)(tc ? tc->valuedouble : 0.0);
            info.points[i].duty   = (uint8_t)(d ? d->valueint : 0);
        }
        CHECK(f_constraints_curve_points(info.points, info.num_points, &_v), req);
    }

    uint8_t out_id;
    esp_err_t err = f_curve_upsert(h->curve, &info, &out_id);
    cJSON_Delete(json);
    if (err != ESP_OK) return send_error(req, "failed to update curve", 500);

    if (h->config) f_config_save_all(h->config, h->fan, h->source, h->curve, h->schedule);

    f_curve_get_info(h->curve, out_id, &info);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddItemToObject(root, "data", curve_to_json(&info));
    return send_json(req, root, "200 OK");
}

static esp_err_t curve_delete_handler(httpd_req_t *req)
{
    f_http_handle_t h = (f_http_handle_t)req->user_ctx;
    if (h == NULL || h->curve == NULL) return send_error(req, "not ready", 503);

    uint8_t id;
    if (get_path_param(req->uri, "/api/v1/curves/", &id) != 0)
        return send_error(req, "invalid id", 400);

    if (f_curve_remove(h->curve, id) != ESP_OK)
        return send_error(req, "curve not found", 404);

    if (h->config) f_config_save_all(h->config, h->fan, h->source, h->curve, h->schedule);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddNullToObject(root, "data");
    return send_json(req, root, "200 OK");
}

/* ================================================================== */
/*  Schedule Endpoints                                                 */
/* ================================================================== */

cJSON *schedule_to_json(const f_schedule_info_t *info)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", info->id);
    cJSON_AddNumberToObject(o, "fan_id", info->fan_id);
    cJSON_AddNumberToObject(o, "start_min", info->start_min);
    cJSON_AddNumberToObject(o, "end_min", info->end_min);
    cJSON_AddNumberToObject(o, "duty", info->duty);
    cJSON_AddBoolToObject(o, "enabled", info->enabled);
    return o;
}

static esp_err_t schedules_list_handler(httpd_req_t *req)
{
    f_http_handle_t h = (f_http_handle_t)req->user_ctx;
    if (h == NULL || h->schedule == NULL) return send_error(req, "not ready", 503);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON *arr = cJSON_AddArrayToObject(root, "data");

    for (uint8_t i = 0; i < F_SCHEDULE_MAX_COUNT; i++) {
        f_schedule_info_t info;
        if (f_schedule_get_info(h->schedule, i, &info) == ESP_OK)
            cJSON_AddItemToArray(arr, schedule_to_json(&info));
    }
    return send_json(req, root, "200 OK");
}

static esp_err_t schedule_create_handler(httpd_req_t *req)
{
    f_http_handle_t h = (f_http_handle_t)req->user_ctx;
    if (h == NULL || h->schedule == NULL) return send_error(req, "not ready", 503);

    char body[1024];
    if (!read_body(req, body, sizeof(body)))
        return send_error(req, "empty body", 400);

    cJSON *json = cJSON_Parse(body);
    if (json == NULL) return send_error(req, "invalid JSON", 400);

    cJSON *fan_node = cJSON_GetObjectItem(json, "fan_id");
    cJSON *duty_node = cJSON_GetObjectItem(json, "duty");
    cJSON *start_node = cJSON_GetObjectItem(json, "start_min");
    cJSON *end_node = cJSON_GetObjectItem(json, "end_min");
    if (!cJSON_IsNumber(fan_node) || !cJSON_IsNumber(duty_node) ||
        !cJSON_IsNumber(start_node) || !cJSON_IsNumber(end_node)) {
        cJSON_Delete(json);
        return send_error(req, "fan_id, duty, start_min, and end_min required", 400);
    }

    CHECK(f_constraints_duty(duty_node->valueint, &_v), req);
    CHECK(f_constraints_schedule_time(start_node->valueint, end_node->valueint, &_v), req);
    CHECK(f_constraints_schedule_count(f_schedule_get_count(h->schedule), &_v), req);

    f_schedule_info_t info = {
        .fan_id = (uint8_t)fan_node->valueint,
        .duty = (uint8_t)duty_node->valueint,
        .start_min = (uint16_t)start_node->valueint,
        .end_min = (uint16_t)end_node->valueint,
        .enabled = true,
    };

    cJSON *en_node = cJSON_GetObjectItem(json, "enabled");
    if (cJSON_IsBool(en_node)) info.enabled = cJSON_IsTrue(en_node);

    uint8_t new_id;
    esp_err_t err = f_schedule_add(h->schedule, &info, &new_id);
    cJSON_Delete(json);
    if (err != ESP_OK) return send_error(req, "failed to create schedule", 500);

    if (h->config) f_config_save_all(h->config, h->fan, h->source, h->curve, h->schedule);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "id", new_id);
    cJSON_AddItemToObject(root, "data", data);
    return send_json(req, root, "201 Created");
}

static esp_err_t schedule_update_handler(httpd_req_t *req)
{
    f_http_handle_t h = (f_http_handle_t)req->user_ctx;
    if (h == NULL || h->schedule == NULL) return send_error(req, "not ready", 503);

    uint8_t id;
    if (get_path_param(req->uri, "/api/v1/schedules/", &id) != 0)
        return send_error(req, "invalid id", 400);

    char body[1024];
    if (!read_body(req, body, sizeof(body)))
        return send_error(req, "empty body", 400);

    cJSON *json = cJSON_Parse(body);
    if (json == NULL) return send_error(req, "invalid JSON", 400);

    cJSON *fan_node = cJSON_GetObjectItem(json, "fan_id");
    cJSON *duty_node = cJSON_GetObjectItem(json, "duty");
    cJSON *start_node = cJSON_GetObjectItem(json, "start_min");
    cJSON *end_node = cJSON_GetObjectItem(json, "end_min");

    if (!cJSON_IsNumber(fan_node) || !cJSON_IsNumber(duty_node) ||
        !cJSON_IsNumber(start_node) || !cJSON_IsNumber(end_node)) {
        cJSON_Delete(json);
        return send_error(req, "fan_id, duty, start_min, and end_min required", 400);
    }

    CHECK(f_constraints_duty(duty_node->valueint, &_v), req);
    CHECK(f_constraints_schedule_time(start_node->valueint, end_node->valueint, &_v), req);

    f_schedule_info_t info = {
        .fan_id    = (uint8_t)fan_node->valueint,
        .duty      = (uint8_t)duty_node->valueint,
        .start_min = (uint16_t)start_node->valueint,
        .end_min   = (uint16_t)end_node->valueint,
        .enabled   = true,
    };

    cJSON *en_node = cJSON_GetObjectItem(json, "enabled");
    if (cJSON_IsBool(en_node)) info.enabled = cJSON_IsTrue(en_node);

    esp_err_t err = f_schedule_update(h->schedule, id, &info);
    cJSON_Delete(json);
    if (err != ESP_OK) return send_error(req, "schedule not found", 404);

    if (h->config) f_config_save_all(h->config, h->fan, h->source, h->curve, h->schedule);

    f_schedule_get_info(h->schedule, id, &info);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddItemToObject(root, "data", schedule_to_json(&info));
    return send_json(req, root, "200 OK");
}

static esp_err_t schedule_delete_handler(httpd_req_t *req)
{
    f_http_handle_t h = (f_http_handle_t)req->user_ctx;
    if (h == NULL || h->schedule == NULL) return send_error(req, "not ready", 503);

    uint8_t id;
    if (get_path_param(req->uri, "/api/v1/schedules/", &id) != 0)
        return send_error(req, "invalid id", 400);

    if (f_schedule_remove(h->schedule, id) != ESP_OK)
        return send_error(req, "schedule not found", 404);

    if (h->config) f_config_save_all(h->config, h->fan, h->source, h->curve, h->schedule);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddNullToObject(root, "data");
    return send_json(req, root, "200 OK");
}

/* ================================================================== */
/*  WiFi Scan & Connect                                                */
/* ================================================================== */

static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        return send_error(req, "scan failed", 500);
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "status", "ok");
        cJSON_AddItemToObject(root, "data", cJSON_CreateArray());
        return send_json(req, root, "200 OK");
    }

    wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (ap_records == NULL) return send_error(req, "oom", 500);
    esp_wifi_scan_get_ap_records(&ap_count, ap_records);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON *arr = cJSON_AddArrayToObject(root, "data");

    for (int i = 0; i < ap_count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", (const char *)ap_records[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", ap_records[i].rssi);
        cJSON_AddNumberToObject(o, "channel", ap_records[i].primary);
        cJSON_AddNumberToObject(o, "authmode", ap_records[i].authmode);
        cJSON_AddItemToArray(arr, o);
    }

    free(ap_records);
    return send_json(req, root, "200 OK");
}

static esp_err_t wifi_connect_handler(httpd_req_t *req)
{
    char body[256];
    if (!read_body(req, body, sizeof(body)))
        return send_error(req, "empty body", 400);

    cJSON *json = cJSON_Parse(body);
    if (json == NULL) return send_error(req, "invalid JSON", 400);

    cJSON *ssid_node = cJSON_GetObjectItem(json, "ssid");
    cJSON *pass_node = cJSON_GetObjectItem(json, "password");
    if (!cJSON_IsString(ssid_node) || !cJSON_IsString(pass_node)) {
        cJSON_Delete(json);
        return send_error(req, "ssid and password required", 400);
    }

    wifi_config_t sta_config = { 0 };
    strncpy((char *)sta_config.sta.ssid, ssid_node->valuestring,
            sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, pass_node->valuestring,
            sizeof(sta_config.sta.password) - 1);
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    cJSON_Delete(json);
    if (err != ESP_OK) {
        return send_error(req, "config failed", 500);
    }

    /* Disconnect and reconnect with new credentials */
    esp_wifi_disconnect();
    esp_wifi_connect();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddStringToObject(root, "data", "connecting");
    return send_json(req, root, "200 OK");
}

static esp_err_t wifi_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON *data = cJSON_CreateObject();

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    bool sta_ok = false;
    char sta_ip[16] = "";
    if (sta) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) {
            sta_ok = true;
            snprintf(sta_ip, sizeof(sta_ip), IPSTR, IP2STR(&ip.ip));
        }
    }
    cJSON_AddBoolToObject(data, "sta_connected", sta_ok);
    cJSON_AddStringToObject(data, "sta_ip", sta_ip);
    cJSON_AddStringToObject(data, "ap_ip", "192.168.4.1");

    cJSON_AddItemToObject(root, "data", data);
    return send_json(req, root, "200 OK");
}

/* ================================================================== */
/*  System Info                                                        */
/* ================================================================== */

static esp_err_t system_info_handler(httpd_req_t *req)
{
    f_http_handle_t h = (f_http_handle_t)req->user_ctx;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON *data = cJSON_CreateObject();

    char ver[16];
    snprintf(ver, sizeof(ver), "%d.%d.%d",
             ESPFM_VERSION_MAJOR, ESPFM_VERSION_MINOR, ESPFM_VERSION_PATCH);
    cJSON_AddStringToObject(data, "version", ver);
    cJSON_AddNumberToObject(data, "uptime_s",
        (int)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(data, "heap_free",
        (int)esp_get_free_heap_size());

    if (h) {
        cJSON_AddNumberToObject(data, "fan_count",
            h->fan ? f_fan_get_count(h->fan) : 0);
        cJSON_AddNumberToObject(data, "source_count",
            h->source ? f_source_get_count(h->source) : 0);
        cJSON_AddNumberToObject(data, "curve_count",
            h->curve ? f_curve_get_count(h->curve) : 0);
        cJSON_AddNumberToObject(data, "schedule_count",
            h->schedule ? f_schedule_get_count(h->schedule) : 0);
    }

    cJSON_AddItemToObject(root, "data", data);
    return send_json(req, root, "200 OK");
}

/* ================================================================== */
/*  Static File Serving                                                */
/* ================================================================== */

static esp_err_t static_file_handler(httpd_req_t *req)
{
    add_cors_headers(req);

    const char *uri = req->uri;
    if (uri == NULL || strcmp(uri, "/") == 0) uri = "/index.html";

    char fpath[256];
    strlcpy(fpath, "/littlefs", sizeof(fpath));
    strlcat(fpath, uri, sizeof(fpath));

    struct stat st;
    if (stat(fpath, &st) != 0) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, get_content_type(uri));

    FILE *fd = fopen(fpath, "r");
    if (fd == NULL) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    uint8_t *chunk = malloc(HTTPD_SCRATCH_SIZE);
    if (chunk == NULL) { fclose(fd); return ESP_ERR_NO_MEM; }

    size_t bytes_read;
    do {
        bytes_read = fread(chunk, 1, HTTPD_SCRATCH_SIZE, fd);
        if (bytes_read > 0) {
            if (httpd_resp_send_chunk(req, (char *)chunk, bytes_read) != ESP_OK) {
                fclose(fd); free(chunk);
                return ESP_FAIL;
            }
        }
    } while (bytes_read > 0);

    fclose(fd);
    free(chunk);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */

esp_err_t f_http_init(f_http_handle_t *handle, f_fan_handle_t fan,
                      f_source_handle_t source, f_curve_handle_t curve,
                      f_schedule_handle_t schedule, f_config_handle_t config)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;

    f_http_handle_t h = calloc(1, sizeof(struct f_http));
    if (h == NULL) return ESP_ERR_NO_MEM;

    h->fan = fan;
    h->source = source;
    h->curve = curve;
    h->schedule = schedule;
    h->config = config;

    esp_event_handler_register(ESPFM_EVENT, ESPFM_EVENT_WIFI_CONNECTED,
                               _on_wifi_connected, h);
    esp_event_handler_register(ESPFM_EVENT, ESPFM_EVENT_WIFI_DISCONNECTED,
                               _on_wifi_disconnected, h);

    *handle = h;
    ESP_LOGI(TAG, "HTTP server initialized (WiFi-aware)");
    return ESP_OK;
}

esp_err_t f_http_start(f_http_handle_t handle)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;

    if (handle->running) {
        ESP_LOGD(TAG, "HTTP server already running, skipping start");
        return ESP_OK;
    }

    httpd_config_t httpd_cfg = HTTPD_DEFAULT_CONFIG();
    httpd_cfg.stack_size      = 8192;  /* need headroom: curve handlers have body[2048] on stack */
    httpd_cfg.uri_match_fn    = httpd_uri_match_wildcard;
    httpd_cfg.max_uri_handlers = 24;
    httpd_cfg.server_port     = 80;
    httpd_cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&handle->server, &httpd_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %d", err);
        return err;
    }
    handle->running = true;

    /* OPTIONS preflight */
    const httpd_uri_t opt_uri = {
        .uri = "/*", .method = HTTP_OPTIONS,
        .handler = options_handler, .user_ctx = handle,
    };
    httpd_register_uri_handler(handle->server, &opt_uri);

    /* Fans */
    const httpd_uri_t fan_uris[] = {
        { .uri = "/api/v1/fans",   .method = HTTP_GET,    .handler = fans_list_handler,  .user_ctx = handle },
        { .uri = "/api/v1/fans/*", .method = HTTP_GET,    .handler = fan_get_handler,    .user_ctx = handle },
        { .uri = "/api/v1/fans",   .method = HTTP_PUT,    .handler = fan_create_handler, .user_ctx = handle },
        { .uri = "/api/v1/fans/*", .method = HTTP_PUT,    .handler = fan_update_handler, .user_ctx = handle },
        { .uri = "/api/v1/fans/*", .method = HTTP_DELETE, .handler = fan_delete_handler, .user_ctx = handle },
    };
    for (int i = 0; i < sizeof(fan_uris)/sizeof(fan_uris[0]); i++)
        httpd_register_uri_handler(handle->server, &fan_uris[i]);

    /* Sources */
    const httpd_uri_t src_uris[] = {
        { .uri = "/api/v1/sources",      .method = HTTP_GET,  .handler = sources_list_handler,  .user_ctx = handle },
        { .uri = "/api/v1/sources",      .method = HTTP_PUT,  .handler = source_create_handler, .user_ctx = handle },
        { .uri = "/api/v1/sources/temp", .method = HTTP_POST,   .handler = source_temp_handler,   .user_ctx = handle },
        { .uri = "/api/v1/sources/*",    .method = HTTP_DELETE, .handler = source_delete_handler, .user_ctx = handle },
    };
    for (int i = 0; i < sizeof(src_uris)/sizeof(src_uris[0]); i++)
        httpd_register_uri_handler(handle->server, &src_uris[i]);

    /* Curves */
    const httpd_uri_t cur_uris[] = {
        { .uri = "/api/v1/curves",   .method = HTTP_GET,    .handler = curves_list_handler,  .user_ctx = handle },
        { .uri = "/api/v1/curves/*", .method = HTTP_GET,    .handler = curve_get_handler,    .user_ctx = handle },
        { .uri = "/api/v1/curves",   .method = HTTP_PUT,    .handler = curve_create_handler, .user_ctx = handle },
        { .uri = "/api/v1/curves/*", .method = HTTP_PUT,    .handler = curve_update_handler, .user_ctx = handle },
        { .uri = "/api/v1/curves/*", .method = HTTP_DELETE, .handler = curve_delete_handler, .user_ctx = handle },
    };
    for (int i = 0; i < sizeof(cur_uris)/sizeof(cur_uris[0]); i++)
        httpd_register_uri_handler(handle->server, &cur_uris[i]);

    /* Schedules */
    const httpd_uri_t sched_uris[] = {
        { .uri = "/api/v1/schedules",   .method = HTTP_GET,    .handler = schedules_list_handler,  .user_ctx = handle },
        { .uri = "/api/v1/schedules",   .method = HTTP_PUT,    .handler = schedule_create_handler, .user_ctx = handle },
        { .uri = "/api/v1/schedules/*", .method = HTTP_PUT,    .handler = schedule_update_handler, .user_ctx = handle },
        { .uri = "/api/v1/schedules/*", .method = HTTP_DELETE, .handler = schedule_delete_handler, .user_ctx = handle },
    };
    for (int i = 0; i < sizeof(sched_uris)/sizeof(sched_uris[0]); i++)
        httpd_register_uri_handler(handle->server, &sched_uris[i]);

    /* WiFi */
    const httpd_uri_t wifi_uris[] = {
        { .uri = "/api/v1/wifi/scan",    .method = HTTP_GET,  .handler = wifi_scan_handler,    .user_ctx = handle },
        { .uri = "/api/v1/wifi/connect", .method = HTTP_POST, .handler = wifi_connect_handler, .user_ctx = handle },
        { .uri = "/api/v1/wifi/status",  .method = HTTP_GET,  .handler = wifi_status_handler,  .user_ctx = handle },
    };
    for (int i = 0; i < sizeof(wifi_uris)/sizeof(wifi_uris[0]); i++)
        httpd_register_uri_handler(handle->server, &wifi_uris[i]);

    /* System info */
    const httpd_uri_t sys_uri = {
        .uri = "/api/v1/system/info", .method = HTTP_GET,
        .handler = system_info_handler, .user_ctx = handle,
    };
    httpd_register_uri_handler(handle->server, &sys_uri);

    /* Static files (wildcard — last so API routes match first) */
    const httpd_uri_t static_uri = {
        .uri = "/*", .method = HTTP_GET,
        .handler = static_file_handler, .user_ctx = handle,
    };
    httpd_register_uri_handler(handle->server, &static_uri);

    ESP_LOGI(TAG, "HTTP server started on port %d (WiFi-aware)", httpd_cfg.server_port);
    return ESP_OK;
}

esp_err_t f_http_stop(f_http_handle_t handle)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;

    if (!handle->running) {
        ESP_LOGD(TAG, "HTTP server not running, skipping stop");
        return ESP_OK;
    }

    esp_err_t err = httpd_stop(handle->server);
    if (err == ESP_OK) {
        handle->running = false;
        handle->server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    } else {
        ESP_LOGE(TAG, "Failed to stop HTTP server: %d", err);
    }
    return err;
}
