/* test_f_http.c — Unit tests for f_http REST API helpers */

#include <string.h>
#include "unity.h"
#include "f_http_testable.h"

/* ------------------------------------------------------------------ */
/*  get_path_param tests                                               */
/* ------------------------------------------------------------------ */

TEST_CASE("get_path_param: extract ID from valid URI", "[http][path]")
{
    uint8_t id;
    TEST_ASSERT_EQUAL(0, get_path_param("/api/v1/fans/3", "/api/v1/fans/", &id));
    TEST_ASSERT_EQUAL(3, id);
}

TEST_CASE("get_path_param: reject missing trailing ID", "[http][path]")
{
    uint8_t id;
    TEST_ASSERT_NOT_EQUAL(0, get_path_param("/api/v1/fans/", "/api/v1/fans/", &id));
    TEST_ASSERT_NOT_EQUAL(0, get_path_param("/api/v1/fans", "/api/v1/fans/", &id));
}

TEST_CASE("get_path_param: reject wrong base prefix", "[http][path]")
{
    uint8_t id;
    TEST_ASSERT_NOT_EQUAL(0, get_path_param("/api/v1/sources/5", "/api/v1/fans/", &id));
}

TEST_CASE("get_path_param: boundary values", "[http][path]")
{
    uint8_t id;
    TEST_ASSERT_EQUAL(0, get_path_param("/api/v1/fans/0", "/api/v1/fans/", &id));
    TEST_ASSERT_EQUAL(0, id);
    TEST_ASSERT_EQUAL(0, get_path_param("/api/v1/fans/255", "/api/v1/fans/", &id));
    TEST_ASSERT_EQUAL(255, id);
}

TEST_CASE("get_path_param: reject negative ID", "[http][path]")
{
    uint8_t id;
    TEST_ASSERT_NOT_EQUAL(0, get_path_param("/api/v1/fans/-1", "/api/v1/fans/", &id));
}

/* ------------------------------------------------------------------ */
/*  get_content_type tests                                             */
/* ------------------------------------------------------------------ */

TEST_CASE("get_content_type: HTML", "[http][mime]")
{
    TEST_ASSERT_EQUAL_STRING("text/html", get_content_type("index.html"));
    TEST_ASSERT_EQUAL_STRING("text/html", get_content_type("page.htm"));
}

TEST_CASE("get_content_type: CSS and JS", "[http][mime]")
{
    TEST_ASSERT_EQUAL_STRING("text/css", get_content_type("style.css"));
    TEST_ASSERT_EQUAL_STRING("application/javascript", get_content_type("app.js"));
}

TEST_CASE("get_content_type: JSON", "[http][mime]")
{
    TEST_ASSERT_EQUAL_STRING("application/json", get_content_type("config.json"));
}

TEST_CASE("get_content_type: images", "[http][mime]")
{
    TEST_ASSERT_EQUAL_STRING("image/png", get_content_type("icon.png"));
    TEST_ASSERT_EQUAL_STRING("image/jpeg", get_content_type("photo.jpg"));
    TEST_ASSERT_EQUAL_STRING("image/jpeg", get_content_type("photo.jpeg"));
    TEST_ASSERT_EQUAL_STRING("image/svg+xml", get_content_type("logo.svg"));
}

TEST_CASE("get_content_type: no extension", "[http][mime]")
{
    TEST_ASSERT_EQUAL_STRING("application/octet-stream", get_content_type("noext"));
}

/* ------------------------------------------------------------------ */
/*  fan_to_json tests                                                  */
/* ------------------------------------------------------------------ */

TEST_CASE("fan_to_json: all fields present", "[http][json][fan]")
{
    f_fan_info_t info = {
        .id = 1,
        .name = "CPU Fan",
        .mode = FAN_MODE_AUTO,
        .duty = 75,
        .rpm = 1200,
        .enabled = true,
        .inverted = false,
        .pwm_gpio = 5,
        .tach_gpio = 6,
        .source_id = 2,
        .curve_id = 3,
        .schedule_id = 1,
        .alarm = FAN_ALARM_NONE,
    };

    cJSON *json = fan_to_json(&info);
    TEST_ASSERT_NOT_NULL(json);

    TEST_ASSERT_EQUAL(1,   cJSON_GetObjectItem(json, "id")->valueint);
    TEST_ASSERT_EQUAL_STRING("CPU Fan", cJSON_GetObjectItem(json, "name")->valuestring);
    TEST_ASSERT_EQUAL(FAN_MODE_AUTO, cJSON_GetObjectItem(json, "mode")->valueint);
    TEST_ASSERT_EQUAL(75,  cJSON_GetObjectItem(json, "duty")->valueint);
    TEST_ASSERT_EQUAL(1200, cJSON_GetObjectItem(json, "rpm")->valueint);
    TEST_ASSERT_TRUE(cJSON_GetObjectItem(json, "enabled")->valueint);
    TEST_ASSERT_FALSE(cJSON_GetObjectItem(json, "inverted")->valueint);
    TEST_ASSERT_EQUAL(5,   cJSON_GetObjectItem(json, "pwm_gpio")->valueint);
    TEST_ASSERT_EQUAL(6,   cJSON_GetObjectItem(json, "tach_gpio")->valueint);
    TEST_ASSERT_EQUAL(2,   cJSON_GetObjectItem(json, "source_id")->valueint);
    TEST_ASSERT_EQUAL(3,   cJSON_GetObjectItem(json, "curve_id")->valueint);
    TEST_ASSERT_EQUAL(1,   cJSON_GetObjectItem(json, "schedule_id")->valueint);
    TEST_ASSERT_EQUAL_STRING("none", cJSON_GetObjectItem(json, "alarm")->valuestring);

    cJSON_Delete(json);
}

TEST_CASE("fan_to_json: alarm stall", "[http][json][fan]")
{
    f_fan_info_t info = {
        .id = 0, .name = "", .mode = 0, .duty = 0, .rpm = 0,
        .enabled = false, .inverted = false, .pwm_gpio = 0, .tach_gpio = 0xFF,
        .source_id = 0xFF, .curve_id = 0xFF, .schedule_id = 0xFF,
        .alarm = FAN_ALARM_STALL,
    };
    cJSON *json = fan_to_json(&info);
    TEST_ASSERT_EQUAL_STRING("stall", cJSON_GetObjectItem(json, "alarm")->valuestring);
    cJSON_Delete(json);
}

TEST_CASE("fan_to_json: alarm overtemp", "[http][json][fan]")
{
    f_fan_info_t info = {
        .id = 0, .name = "", .mode = 0, .duty = 0, .rpm = 0,
        .enabled = false, .inverted = true, .pwm_gpio = 0, .tach_gpio = 0xFF,
        .source_id = 0xFF, .curve_id = 0xFF, .schedule_id = 0xFF,
        .alarm = FAN_ALARM_OVERTEMP,
    };
    cJSON *json = fan_to_json(&info);
    TEST_ASSERT_EQUAL_STRING("overtemp", cJSON_GetObjectItem(json, "alarm")->valuestring);
    TEST_ASSERT_TRUE(cJSON_GetObjectItem(json, "inverted")->valueint);
    cJSON_Delete(json);
}

/* ------------------------------------------------------------------ */
/*  source_to_json tests                                               */
/* ------------------------------------------------------------------ */

TEST_CASE("source_to_json: NTC type", "[http][json][source]")
{
    f_source_info_t info = {
        .id = 0,
        .name = "Ambient",
        .type = SOURCE_TYPE_NTC,
        .status = SOURCE_STATUS_VALID,
        .temp_c = 25.5f,
        .gpio = 1,
        .last_update_us = 0,
    };
    cJSON *json = source_to_json(&info);
    TEST_ASSERT_EQUAL(0, cJSON_GetObjectItem(json, "id")->valueint);
    TEST_ASSERT_EQUAL_STRING("Ambient", cJSON_GetObjectItem(json, "name")->valuestring);
    TEST_ASSERT_EQUAL_STRING("ntc", cJSON_GetObjectItem(json, "type")->valuestring);
    TEST_ASSERT_EQUAL_STRING("valid", cJSON_GetObjectItem(json, "status")->valuestring);
    TEST_ASSERT_EQUAL_FLOAT(25.5f, cJSON_GetObjectItem(json, "temp_c")->valuedouble);
    TEST_ASSERT_EQUAL(1, cJSON_GetObjectItem(json, "gpio")->valueint);
    cJSON_Delete(json);
}

TEST_CASE("source_to_json: stale DS18B20", "[http][json][source]")
{
    f_source_info_t info = {
        .id = 3, .name = "Water",
        .type = SOURCE_TYPE_DS18B20,
        .status = SOURCE_STATUS_STALE,
        .temp_c = 18.0f, .gpio = 0xFF,
        .last_update_us = 0,
    };
    cJSON *json = source_to_json(&info);
    TEST_ASSERT_EQUAL_STRING("ds18b20", cJSON_GetObjectItem(json, "type")->valuestring);
    TEST_ASSERT_EQUAL_STRING("stale", cJSON_GetObjectItem(json, "status")->valuestring);
    cJSON_Delete(json);
}

TEST_CASE("source_to_json: invalid manual", "[http][json][source]")
{
    f_source_info_t info = {
        .id = 7, .name = "External",
        .type = SOURCE_TYPE_MANUAL,
        .status = SOURCE_STATUS_INVALID,
        .temp_c = 0.0f, .gpio = 0xFF,
        .last_update_us = 0,
    };
    cJSON *json = source_to_json(&info);
    TEST_ASSERT_EQUAL_STRING("manual", cJSON_GetObjectItem(json, "type")->valuestring);
    TEST_ASSERT_EQUAL_STRING("invalid", cJSON_GetObjectItem(json, "status")->valuestring);
    cJSON_Delete(json);
}

/* ------------------------------------------------------------------ */
/*  curve_to_json tests                                                */
/* ------------------------------------------------------------------ */

TEST_CASE("curve_to_json: multi-point curve", "[http][json][curve]")
{
    f_curve_info_t info;
    memset(&info, 0, sizeof(info));
    info.id = 2;
    strncpy(info.name, "Silent", ESPFM_NAME_MAX - 1);
    info.num_points = 3;
    info.points[0] = (f_curve_point_t){ .temp_c = 20.0f, .duty = 30 };
    info.points[1] = (f_curve_point_t){ .temp_c = 40.0f, .duty = 60 };
    info.points[2] = (f_curve_point_t){ .temp_c = 60.0f, .duty = 100 };

    cJSON *json = curve_to_json(&info);
    TEST_ASSERT_EQUAL(2, cJSON_GetObjectItem(json, "id")->valueint);
    TEST_ASSERT_EQUAL_STRING("Silent", cJSON_GetObjectItem(json, "name")->valuestring);

    cJSON *pts = cJSON_GetObjectItem(json, "points");
    TEST_ASSERT_NOT_NULL(pts);
    TEST_ASSERT_EQUAL(3, cJSON_GetArraySize(pts));

    cJSON *p0 = cJSON_GetArrayItem(pts, 0);
    TEST_ASSERT_EQUAL_FLOAT(20.0f, cJSON_GetObjectItem(p0, "temp_c")->valuedouble);
    TEST_ASSERT_EQUAL(30, cJSON_GetObjectItem(p0, "duty")->valueint);

    cJSON *p2 = cJSON_GetArrayItem(pts, 2);
    TEST_ASSERT_EQUAL_FLOAT(60.0f, cJSON_GetObjectItem(p2, "temp_c")->valuedouble);
    TEST_ASSERT_EQUAL(100, cJSON_GetObjectItem(p2, "duty")->valueint);

    cJSON_Delete(json);
}

TEST_CASE("curve_to_json: empty curve", "[http][json][curve]")
{
    f_curve_info_t info;
    memset(&info, 0, sizeof(info));
    info.id = 0;
    strncpy(info.name, "Empty", ESPFM_NAME_MAX - 1);
    info.num_points = 0;

    cJSON *json = curve_to_json(&info);
    TEST_ASSERT_NOT_NULL(json);
    cJSON *pts = cJSON_GetObjectItem(json, "points");
    TEST_ASSERT_EQUAL(0, cJSON_GetArraySize(pts));
    cJSON_Delete(json);
}

/* ------------------------------------------------------------------ */
/*  schedule_to_json tests                                             */
/* ------------------------------------------------------------------ */

TEST_CASE("schedule_to_json: normal schedule", "[http][json][sched]")
{
    f_schedule_info_t info = {
        .id = 5,
        .fan_id = 2,
        .start_min = 480,   /* 08:00 */
        .end_min = 1080,    /* 18:00 */
        .duty = 80,
        .enabled = true,
    };
    cJSON *json = schedule_to_json(&info);
    TEST_ASSERT_EQUAL(5, cJSON_GetObjectItem(json, "id")->valueint);
    TEST_ASSERT_EQUAL(2, cJSON_GetObjectItem(json, "fan_id")->valueint);
    TEST_ASSERT_EQUAL(480, cJSON_GetObjectItem(json, "start_min")->valueint);
    TEST_ASSERT_EQUAL(1080, cJSON_GetObjectItem(json, "end_min")->valueint);
    TEST_ASSERT_EQUAL(80, cJSON_GetObjectItem(json, "duty")->valueint);
    TEST_ASSERT_TRUE(cJSON_GetObjectItem(json, "enabled")->valueint);
    cJSON_Delete(json);
}

TEST_CASE("schedule_to_json: disabled overnight", "[http][json][sched]")
{
    f_schedule_info_t info = {
        .id = 0, .fan_id = 1,
        .start_min = 1320,  /* 22:00 */
        .end_min = 120,     /* 02:00 */
        .duty = 30,
        .enabled = false,
    };
    cJSON *json = schedule_to_json(&info);
    TEST_ASSERT_EQUAL(1320, cJSON_GetObjectItem(json, "start_min")->valueint);
    TEST_ASSERT_EQUAL(120, cJSON_GetObjectItem(json, "end_min")->valueint);
    TEST_ASSERT_FALSE(cJSON_GetObjectItem(json, "enabled")->valueint);
    cJSON_Delete(json);
}
