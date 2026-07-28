#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define F_GPIO_CAP_PWM     (1 << 0)
#define F_GPIO_CAP_TACH    (1 << 1)
#define F_GPIO_CAP_ADC     (1 << 2)
#define F_GPIO_CAP_ONEWIRE (1 << 3)
#define F_GPIO_CAP_I2C     (1 << 4)
#define F_GPIO_CAP_UART    (1 << 5)

#if CONFIG_IDF_TARGET_ESP32
#define F_GPIO_MAX_PINS 40 /* GPIO 0-39 */
#elif CONFIG_IDF_TARGET_ESP32S3
#define F_GPIO_MAX_PINS 48 /* GPIO 0-47 */
#else
#error "Unsupported IDF target for f_gpio"
#endif

typedef struct f_gpio *f_gpio_handle_t;

esp_err_t f_gpio_init(f_gpio_handle_t *handle);
esp_err_t f_gpio_claim(f_gpio_handle_t handle, uint8_t pin, uint32_t cap_mask);
esp_err_t f_gpio_release(f_gpio_handle_t handle, uint8_t pin);
bool f_gpio_is_available(f_gpio_handle_t handle, uint8_t pin);
bool f_gpio_is_claimed_for(f_gpio_handle_t handle, uint8_t pin, uint32_t cap_mask);
uint8_t f_gpio_get_count(f_gpio_handle_t handle);
uint32_t f_gpio_get_caps(f_gpio_handle_t handle, uint8_t pin);

#ifdef __cplusplus
}
#endif
