#!/usr/bin/env bash
# Build and run the f_control host-based C unit tests under WSL (Linux/gcc).
# Compiles the REAL f_control.c (`static` demoted so _ctrl_callback and the
# struct definition are reachable) against stubbed ESP-IDF / FreeRTOS layers
# (see stubs/) plus the real f_control/f_core/f_fan/f_source/f_curve/f_gpio/
# f_ds18b20 headers.  GNU ld --wrap hooks intercept calloc/free and every
# external dependency the control-loop callback touches: f_fan_update_rpm,
# f_fan_get_info, f_fan_set_alarm, f_fan_set_duty, f_source_get_reading,
# f_curve_lookup and esp_event_post.
# -ffunction-sections/--gc-sections drop _ctrl_task / f_control_start (the
# FreeRTOS task lifecycle) so no real task scheduler is needed.
set -euo pipefail
cd "$(dirname "$0")"
REPO="$(cd ../.. && pwd)"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT

SRC="$REPO/components"
STUBS="$PWD/stubs"
CC="${CC:-gcc}"

INCS="-I$STUBS"
INCS="$INCS -I$SRC/f_control/include"
INCS="$INCS -I$SRC/f_core/include"
INCS="$INCS -I$SRC/f_fan/include"
INCS="$INCS -I$SRC/f_source/include"
INCS="$INCS -I$SRC/f_curve/include"
INCS="$INCS -I$SRC/f_gpio/include"
INCS="$INCS -I$SRC/f_ds18b20/include"

# GNU ld --wrap hooks: alloc tracking + every external call in _ctrl_callback.
WRAPS="-Wl,--wrap=calloc -Wl,--wrap=free"
WRAPS="$WRAPS -Wl,--wrap=f_fan_update_rpm -Wl,--wrap=f_fan_get_info"
WRAPS="$WRAPS -Wl,--wrap=f_fan_set_alarm -Wl,--wrap=f_fan_set_duty"
WRAPS="$WRAPS -Wl,--wrap=f_source_get_reading -Wl,--wrap=f_curve_lookup"
WRAPS="$WRAPS -Wl,--wrap=esp_event_post"

echo "==> Compiling f_control host test harness"
"$CC" -std=gnu11 -O0 -fno-builtin -ffunction-sections -fdata-sections \
    -Wall -Wextra -DCONFIG_IDF_TARGET_ESP32 \
    -DCONFIG_ESPFM_CONTROL_PERIOD_MS=1000 -DCONFIG_ESPFM_OVERTEMP_THRESHOLD_C=60 \
    -Wl,--gc-sections -o "$BUILD_DIR/test_f_control" \
    test_f_control.c \
    $INCS $WRAPS

echo "==> Running f_control host test harness"
"$BUILD_DIR/test_f_control"
