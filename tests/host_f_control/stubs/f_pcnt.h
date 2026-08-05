#pragma once
#include "esp_err.h"
#include <stdint.h>

/* Minimal f_pcnt.h stub for host-based unit tests of f_control.c. */

typedef struct f_pcnt *f_pcnt_handle_t;

esp_err_t f_pcnt_add_input(f_pcnt_handle_t handle, uint8_t gpio, uint8_t *unit_id_out);
esp_err_t f_pcnt_read_and_clear(f_pcnt_handle_t handle, uint8_t unit_id, int *count_out);
esp_err_t f_pcnt_compute_rpm(int pulse_count, uint32_t interval_ms, uint16_t pulses_per_rev,
                             uint16_t *rpm_out);
esp_err_t f_pcnt_remove_input(f_pcnt_handle_t handle, uint8_t unit_id);
