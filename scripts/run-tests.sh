#!/usr/bin/env bash
# Build and run the driver unit tests (tests/*.c) with ASan and UBSan,
# against the libfprint tree of scripts/build-local.sh (built first if
# missing). Nothing is installed.
#
#   scripts/run-tests.sh
#   LIBFPRINT_DIR=/path scripts/run-tests.sh
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
LF=${LIBFPRINT_DIR:-$ROOT/.build/libfprint}
BUILD=$LF/builddir-asan
OUT=$ROOT/.build/tests
DRV=$ROOT/driver/goodix55b4

if [[ ! -f $BUILD/libfprint/libfprint-2.so ]]; then
    SANITIZE=1 "$ROOT/scripts/build-local.sh"
fi
mkdir -p "$OUT"

read -ra PKG_CFLAGS <<< "$(pkg-config --cflags glib-2.0 gio-2.0 gusb openssl)"
read -ra PKG_LIBS <<< "$(pkg-config --libs glib-2.0 gio-2.0 gusb openssl)"
CFLAGS=(-std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Werror
        -fsanitize=address,undefined -fno-sanitize-recover=all
        -fno-omit-frame-pointer
        -I"$DRV" -I"$LF/libfprint" -I"$BUILD/libfprint" "${PKG_CFLAGS[@]}")
LIBS=(-L"$BUILD/libfprint" -lfprint-2 -Wl,-rpath,"$BUILD/libfprint"
      "${PKG_LIBS[@]}")

cc "${CFLAGS[@]}" -o "$OUT/test-goodixtls" \
    "$ROOT/tests/test-goodixtls.c" "$DRV/goodixtls.c" "${LIBS[@]}"

export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}
"$OUT/test-goodixtls" "$@"
