#pragma once

/* Minimal ESP-IDF esp_log.h stub for host-based unit tests.
 * Each ESP_LOG* macro forwards to __test_log, which records the last
 * formatted message so tests can assert on log output. */

void __test_log(char level, const char *tag, const char *fmt, ...);

#define ESP_LOGE(tag, ...) __test_log('E', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) __test_log('W', tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) __test_log('I', tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) __test_log('D', tag, __VA_ARGS__)
