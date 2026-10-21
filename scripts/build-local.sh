#!/usr/bin/env bash
# Build libfprint (pinned release) with only the goodix55b4 driver, in
# .build/libfprint. Nothing is installed. Run as your normal user.
#
#   scripts/build-local.sh            # clone/update, integrate, build
#   LIBFPRINT_DIR=/path scripts/build-local.sh   # use an existing checkout
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
REV=0c97a47d8ef405cd577b87058c1e89cae9d242e7   # libfprint v1.94.10
URL=https://gitlab.freedesktop.org/libfprint/libfprint.git
LF=${LIBFPRINT_DIR:-$ROOT/.build/libfprint}

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

if [[ ! -d $LF/builddir ]]; then
    meson setup "$LF/builddir" "$LF" \
        -Ddrivers=goodix55b4 -Dudev_hwdb=disabled -Dudev_rules=disabled \
        -Dintrospection=false -Dinstalled-tests=false -Ddoc=false \
        -Dgtk-examples=false
fi
ninja -C "$LF/builddir"
echo "built: $LF/builddir/libfprint/libfprint-2.so"
echo "examples: $LF/builddir/examples/{img-capture,enroll,verify,identify}"
