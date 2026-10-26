#!/usr/bin/env bash
# Build libfprint (pinned release) with only the goodix55b4 driver, in
# .build/libfprint. Nothing is installed. Run as your normal user.
#
#   scripts/build-local.sh            # clone/update, integrate, build
#   LIBFPRINT_DIR=/path scripts/build-local.sh   # use an existing checkout
#   SANITIZE=1 scripts/build-local.sh # ASan+UBSan build in builddir-asan
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
REV=80a4b5ec612892c5c056c48dddaa561452cf37ec   # libfprint v1.94.100
URL=https://gitlab.freedesktop.org/libfprint/libfprint.git
LF=${LIBFPRINT_DIR:-$ROOT/.build/libfprint}
BUILD=$LF/builddir
SANITIZE_ARGS=()
if [[ ${SANITIZE:-0} == 1 ]]; then
    BUILD=$LF/builddir-asan
    SANITIZE_ARGS=(-Db_sanitize=address,undefined -Db_lundef=false)
fi

if [[ ! -d $LF/.git ]]; then
    git clone --quiet "$URL" "$LF"
fi
if [[ $(git -C "$LF" rev-parse HEAD) != "$REV" ]]; then
    git -C "$LF" fetch --quiet origin
    git -C "$LF" checkout --quiet --force "$REV"
fi
# Start from a pristine tree so integration stays reproducible.
git -C "$LF" checkout --quiet -- .
python3 -I "$ROOT/scripts/integrate.py" "$LF"
# 1.94.100 fails to configure with -Dintrospection=false (one-variable
# foreach over a dict in tests/meson.build).
sed -i 's/foreach driver_test: drivers_tests/foreach driver_test, _: drivers_tests/' \
    "$LF/tests/meson.build"

OPTIONS=(-Ddrivers=goodix55b4 -Dudev_hwdb=disabled -Dudev_rules=disabled
         -Dintrospection=false -Dinstalled-tests=false -Ddoc=false
         -Dgtk-examples=false "${SANITIZE_ARGS[@]}")
if [[ ! -d $BUILD ]]; then
    meson setup "$BUILD" "$LF" "${OPTIONS[@]}"
else
    # Options changed in this script would otherwise never reach an
    # existing build directory.
    meson configure "$BUILD" "${OPTIONS[@]}"
fi
# Long build on slow machines: keep it out of the way of interactive work.
nice ninja -C "$BUILD"
echo "built: $BUILD/libfprint/libfprint-2.so"
echo "examples: $BUILD/examples/{img-capture,enroll,verify,identify}"
