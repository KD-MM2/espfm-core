#!/usr/bin/env bash
# Build and run the f_coap host-based C unit tests under WSL (Linux/gcc).
# Compiles the real f_coap_routes.c (`static` demoted) against stubbed ESP-IDF
# headers + a fake coap3/coap.h + real nanopb/espfm.  GNU ld --wrap hooks
# intercept f_config_export_all, coap_add_data_large_response, coap_pdu_set_code,
# coap_resource_get_userdata, calloc and free.  -ffunction-sections/--gc-sections
# drop the unrelated handlers, so no wifi/mdns/ds18b20 definitions are needed.
set -euo pipefail
cd "$(dirname "$0")"
REPO="$(cd ../.. && pwd)"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT

SRC="$REPO/components"
STUBS="$PWD/stubs"
STUBS_CFG="$REPO/tests/host_f_config/stubs"
CC="${CC:-gcc}"

INCS="-I$STUBS -I$STUBS_CFG"
INCS="$INCS -I$SRC/f_coap/include"
INCS="$INCS -I$SRC/f_core/include"
INCS="$INCS -I$SRC/f_fan/include"
INCS="$INCS -I$SRC/f_source/include"
INCS="$INCS -I$SRC/f_curve/include"
INCS="$INCS -I$SRC/f_schedule/include"
INCS="$INCS -I$SRC/f_config/include"
INCS="$INCS -I$SRC/f_mdns/include"
INCS="$INCS -I$SRC/f_ds18b20/include"
INCS="$INCS -I$SRC/f_gpio/include"
INCS="$INCS -I$SRC/f_constraints/include"
INCS="$INCS -I$SRC/f_schema"
INCS="$INCS -I$SRC/f_schema/include"
INCS="$INCS -I$SRC/nanopb/include"

# GNU ld --wrap hooks: alloc tracking + the handlers' external dependencies.
WRAPS="-Wl,--wrap=calloc -Wl,--wrap=free"
WRAPS="$WRAPS -Wl,--wrap=f_config_export_all -Wl,--wrap=f_config_import_all"
WRAPS="$WRAPS -Wl,--wrap=f_config_save_all"
WRAPS="$WRAPS -Wl,--wrap=coap_add_data_large_response -Wl,--wrap=coap_add_data"
WRAPS="$WRAPS -Wl,--wrap=coap_get_data -Wl,--wrap=coap_pdu_set_code"
WRAPS="$WRAPS -Wl,--wrap=coap_resource_get_userdata"
WRAPS="$WRAPS -Wl,--wrap=coap_get_uri_path -Wl,--wrap=coap_delete_string"
WRAPS="$WRAPS -Wl,--wrap=esp_timer_create -Wl,--wrap=esp_timer_start_once"
WRAPS="$WRAPS -Wl,--wrap=esp_err_to_name"
# fan/source registry stubs (gpio claim-wiring handlers 4.00 + StatusResponse)
WRAPS="$WRAPS -Wl,--wrap=f_fan_add -Wl,--wrap=f_fan_set_gpio -Wl,--wrap=f_fan_set_name"
WRAPS="$WRAPS -Wl,--wrap=f_fan_set_mode -Wl,--wrap=f_fan_set_duty -Wl,--wrap=f_fan_set_source"
WRAPS="$WRAPS -Wl,--wrap=f_fan_set_curve -Wl,--wrap=f_fan_set_schedule -Wl,--wrap=f_fan_set_group"
WRAPS="$WRAPS -Wl,--wrap=f_fan_set_inverted -Wl,--wrap=f_fan_set_enabled -Wl,--wrap=f_fan_get_info"
WRAPS="$WRAPS -Wl,--wrap=f_fan_remove"
WRAPS="$WRAPS -Wl,--wrap=f_source_add -Wl,--wrap=f_source_add_ds18b20 -Wl,--wrap=f_source_get_info"
WRAPS="$WRAPS -Wl,--wrap=f_source_update_manual -Wl,--wrap=f_source_remove"

echo "==> Compiling f_coap host test harness"
"$CC" -std=gnu11 -O0 -fno-builtin -ffunction-sections -fdata-sections \
    -Wall -Wextra -DCONFIG_IDF_TARGET_ESP32 -Wl,--gc-sections -o "$BUILD_DIR/test_f_coap_config" \
    test_f_coap_config.c \
    "$SRC/f_schema/espfm_conv.c" \
    "$SRC/f_schema/espfm.pb.c" \
    "$SRC/nanopb/pb_encode.c" \
    "$SRC/nanopb/pb_decode.c" \
    "$SRC/nanopb/pb_common.c" \
    $INCS $WRAPS

echo "==> Running f_coap host test harness"
"$BUILD_DIR/test_f_coap_config"
