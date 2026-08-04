#!/usr/bin/env bash
# Build and run the f_gpio claim-wiring host C unit tests under WSL (Linux/gcc).
# Compiles the REAL f_fan.c, f_source.c, f_gpio.c, f_ds18b20.c and
# f_constraints.c against stubbed ESP-IDF layers (see stubs/) with
# -DCONFIG_IDF_TARGET_ESP32 (ESP32 reserved table + F_GPIO_MAX_PINS 40 active).
# GNU ld --wrap hooks intercept calloc/free, f_ledc_add_channel,
# f_ledc_remove_channel, f_pcnt_add_input, f_pcnt_remove_input,
# esp_timer_get_time, onewire_new_bus_rmt, onewire_bus_del, esp_err_to_name and
# f_gpio_claim (pass-through with fail-on-Nth-call injection).
# -ffunction-sections/--gc-sections drop the unreferenced driver functions
# (scan/read/temp conversions), so only the retained claim paths need symbols.
set -euo pipefail
cd "$(dirname "$0")"
REPO="$(cd ../.. && pwd)"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT

SRC="$REPO/components"
STUBS="$PWD/stubs"
CC="${CC:-gcc}"

INCS="-I$STUBS"
INCS="$INCS -I$SRC/f_gpio/include"
INCS="$INCS -I$SRC/f_fan/include"
INCS="$INCS -I$SRC/f_source/include"
INCS="$INCS -I$SRC/f_ds18b20/include"
INCS="$INCS -I$SRC/f_constraints/include"
INCS="$INCS -I$SRC/f_core/include"
INCS="$INCS -I$SRC/f_curve/include"
INCS="$INCS -I$SRC/f_schedule/include"

# GNU ld --wrap hooks
WRAPS="-Wl,--wrap=calloc -Wl,--wrap=free"
WRAPS="$WRAPS -Wl,--wrap=f_ledc_add_channel -Wl,--wrap=f_ledc_remove_channel"
WRAPS="$WRAPS -Wl,--wrap=f_pcnt_add_input -Wl,--wrap=f_pcnt_remove_input"
WRAPS="$WRAPS -Wl,--wrap=esp_timer_get_time"
WRAPS="$WRAPS -Wl,--wrap=onewire_new_bus_rmt -Wl,--wrap=onewire_bus_del"
WRAPS="$WRAPS -Wl,--wrap=esp_err_to_name"
WRAPS="$WRAPS -Wl,--wrap=f_gpio_claim"

echo "==> Compiling f_gpio_claim host test harness"
"$CC" -std=gnu11 -O0 -fno-builtin -ffunction-sections -fdata-sections \
    -Wall -Wextra -DCONFIG_IDF_TARGET_ESP32 \
    -D'ESP_ERROR_CHECK(x)=((void)(x))' -include stdlib.h \
    -Wl,--gc-sections -o "$BUILD_DIR/test_f_gpio_claim" \
    test_f_gpio_claim.c \
    "$SRC/f_fan/f_fan.c" \
    "$SRC/f_source/f_source.c" \
    "$SRC/f_gpio/f_gpio.c" \
    "$SRC/f_ds18b20/f_ds18b20.c" \
    "$SRC/f_constraints/f_constraints.c" \
    $INCS $WRAPS

echo "==> Running f_gpio_claim host test harness"
"$BUILD_DIR/test_f_gpio_claim"
