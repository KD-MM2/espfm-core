#pragma once
#include "esp_err.h"
#include "f_core.h"
#include "f_adc.h"
#include "f_ds18b20.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define F_SOURCE_MAX_COUNT 8
#define F_SOURCE_GPIO_NONE 0xFF

typedef struct {
    uint8_t   id;
    char      name[ESPFM_NAME_MAX];
    source_type_t type;
    source_status_t status;
    float     temp_c;
    uint8_t   gpio;
    uint8_t   ds18b20_index;  /* device index from scan (for SOURCE_TYPE_DS18B20) */
    int64_t   last_update_us;
} f_source_info_t;

typedef struct f_source *f_source_handle_t;

esp_err_t f_source_init(f_source_handle_t *handle, f_adc_handle_t adc, f_ds18b20_handle_t ds18b20);
esp_err_t f_source_add(f_source_handle_t handle, source_type_t type, uint8_t gpio,
                       const char *name, uint8_t *id_out);
esp_err_t f_source_remove(f_source_handle_t handle, uint8_t id);
esp_err_t f_source_get_reading(f_source_handle_t handle, uint8_t id,
                                float *temp_c_out, source_status_t *status_out);
esp_err_t f_source_update_manual(f_source_handle_t handle, uint8_t id, float temp_c);
uint8_t f_source_get_count(f_source_handle_t handle);
esp_err_t f_source_get_info(f_source_handle_t handle, uint8_t id, f_source_info_t *info_out);
esp_err_t f_source_for_each(f_source_handle_t handle,
                             void (*cb)(const f_source_info_t *, void *), void *ctx);

#ifdef __cplusplus
}
#endif
