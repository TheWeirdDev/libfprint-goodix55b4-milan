#!/usr/bin/env bash
# Build and run the driver unit tests (tests/*.c) with ASan and UBSan,
# against the libfprint tree of scripts/build-local.sh (a sanitized build is
# made first if missing). Nothing is installed.
#
#   scripts/run-tests.sh                 # all tests
#   scripts/run-tests.sh goodix-proto    # one suite (meson test name)
#   LIBFPRINT_DIR=/path scripts/run-tests.sh
#   GOODIX55B4_DATASET=~/55b4-set scripts/run-tests.sh   # + real-frame tests
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
LF=${LIBFPRINT_DIR:-$ROOT/.build/libfprint}
LF_BUILD=$LF/builddir-asan
BUILD=$ROOT/.build/tests

if [[ ! -f $LF_BUILD/libfprint/libfprint-2.so ]]; then
    SANITIZE=1 "$ROOT/scripts/build-local.sh"
fi
if [[ ! -d $BUILD ]]; then
    meson setup "$BUILD" "$ROOT" \
        -Dlibfprint_src="$LF" -Dlibfprint_build="$LF_BUILD" \
        -Db_sanitize=address,undefined -Db_lundef=false \
        -Dbuildtype=debug
fi
meson configure "$BUILD" -Ddataset="${GOODIX55B4_DATASET:-}"
nice ninja -C "$BUILD"
meson test -C "$BUILD" --print-errorlogs "$@"
