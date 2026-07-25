#include "f_gpio.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "f_gpio";

/* Pins reserved by ESP32-S3 hardware */
static const uint8_t reserved_pins[] = {
    0,   /* U0TXD (console) */
    1,   /* U0RXD (console) */
    2,   /* Strapping */
    3,   /* JTAG */
    19,  /* USB D- */
    20,  /* USB D+ */
    45,  /* PSRAM */
    46,  /* PSRAM */
};
#define NUM_RESERVED (sizeof(reserved_pins) / sizeof(reserved_pins[0]))

struct f_gpio {
    uint32_t pin_caps[F_GPIO_MAX_PINS];  /* 0 = free, non-zero = claimed capabilities */
};

static bool is_reserved(uint8_t pin) {
    for (size_t i = 0; i < NUM_RESERVED; i++) {
        if (reserved_pins[i] == pin) return true;
    }
    return false;
}

esp_err_t f_gpio_init(f_gpio_handle_t *handle) {
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    f_gpio_handle_t h = calloc(1, sizeof(struct f_gpio));
    if (h == NULL) return ESP_ERR_NO_MEM;
    /* Mark reserved pins as claimed (all capabilities) */
    for (size_t i = 0; i < NUM_RESERVED; i++) {
        h->pin_caps[reserved_pins[i]] = 0xFFFFFFFF;
    }
    *handle = h;
    ESP_LOGI(TAG, "GPIO registry initialized (%d pins, %zu reserved)",
             F_GPIO_MAX_PINS, NUM_RESERVED);
    return ESP_OK;
}

esp_err_t f_gpio_claim(f_gpio_handle_t handle, uint8_t pin, uint32_t cap_mask) {
    if (handle == NULL || pin >= F_GPIO_MAX_PINS || cap_mask == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (is_reserved(pin)) {
        ESP_LOGW(TAG, "Pin %d is reserved, cannot claim", pin);
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->pin_caps[pin] != 0) {
        ESP_LOGW(TAG, "Pin %d already claimed (caps=0x%lx)", pin, handle->pin_caps[pin]);
        return ESP_ERR_INVALID_STATE;
    }
    handle->pin_caps[pin] = cap_mask;
    ESP_LOGD(TAG, "Pin %d claimed (caps=0x%lx)", pin, cap_mask);
    return ESP_OK;
}

esp_err_t f_gpio_release(f_gpio_handle_t handle, uint8_t pin) {
    if (handle == NULL || pin >= F_GPIO_MAX_PINS) return ESP_ERR_INVALID_ARG;
    if (is_reserved(pin)) return ESP_ERR_INVALID_ARG;
    handle->pin_caps[pin] = 0;
    return ESP_OK;
}

bool f_gpio_is_available(f_gpio_handle_t handle, uint8_t pin) {
    if (handle == NULL || pin >= F_GPIO_MAX_PINS) return false;
    return (handle->pin_caps[pin] == 0) && !is_reserved(pin);
}

bool f_gpio_is_claimed_for(f_gpio_handle_t handle, uint8_t pin, uint32_t cap_mask) {
    if (handle == NULL || pin >= F_GPIO_MAX_PINS) return false;
    return (handle->pin_caps[pin] & cap_mask) != 0;
}

uint8_t f_gpio_get_count(f_gpio_handle_t handle) {
    if (handle == NULL) return 0;
    return F_GPIO_MAX_PINS;
}

uint32_t f_gpio_get_caps(f_gpio_handle_t handle, uint8_t pin) {
    if (handle == NULL || pin >= F_GPIO_MAX_PINS) return 0;
    return handle->pin_caps[pin];
}
