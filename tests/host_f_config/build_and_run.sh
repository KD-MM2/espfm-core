#!/usr/bin/env bash
# Build and run the f_config host-based C unit tests under WSL (Linux/gcc).
# Compiles the real f_config.c against stubbed ESP-IDF headers + real nanopb.
set -euo pipefail
cd "$(dirname "$0")"
REPO="$(cd ../.. && pwd)"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT

SRC="$REPO/components"
STUBS="$PWD/stubs"
CC="${CC:-gcc}"

INCS="-I$STUBS"
INCS="$INCS -I$SRC/f_config/include"
INCS="$INCS -I$SRC/f_core/include"
INCS="$INCS -I$SRC/f_fan/include"
INCS="$INCS -I$SRC/f_source/include"
INCS="$INCS -I$SRC/f_curve/include"
INCS="$INCS -I$SRC/f_schedule/include"
INCS="$INCS -I$SRC/f_constraints/include"
INCS="$INCS -I$SRC/f_gpio/include"
INCS="$INCS -I$SRC/f_schema"
INCS="$INCS -I$SRC/f_schema/include"
INCS="$INCS -I$SRC/nanopb/include"

# GNU ld --wrap hooks: calloc/free/fopen/fwrite/fclose/pb_encode
WRAPS="-Wl,--wrap=calloc -Wl,--wrap=free -Wl,--wrap=fopen"
WRAPS="$WRAPS -Wl,--wrap=fwrite -Wl,--wrap=fclose -Wl,--wrap=pb_encode"

echo "==> Compiling f_config host test harness"
"$CC" -std=gnu11 -O0 -fno-builtin -Wall -Wextra -DCONFIG_IDF_TARGET_ESP32 -o "$BUILD_DIR/test_f_config" \
    test_f_config.c \
    "$SRC/f_config/f_config.c" \
    "$SRC/f_schema/espfm_conv.c" \
    "$SRC/f_schema/espfm.pb.c" \
    "$SRC/nanopb/pb_encode.c" \
    "$SRC/nanopb/pb_decode.c" \
    "$SRC/nanopb/pb_common.c" \
    $INCS $WRAPS

echo "==> Running f_config host test harness"
"$BUILD_DIR/test_f_config"
