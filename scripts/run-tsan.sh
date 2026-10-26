#!/usr/bin/env bash
# Run the suites that exercise threads (the engine-state saver) under
# ThreadSanitizer. TSan cannot share a process with the ASan libfprint of
# run-tests.sh, so this builds the libfprint-free driver modules alone.
#
#   scripts/run-tsan.sh [RUNS]     # default: 20 runs
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
LF=${LIBFPRINT_DIR:-$ROOT/.build/libfprint}
LF_BUILD=$LF/builddir-asan
OUT=$ROOT/.build/tsan
DRV=$ROOT/driver/goodix55b4
RUNS=${1:-20}

if [[ ! -d $LF_BUILD/libfprint ]]; then
    SANITIZE=1 "$ROOT/scripts/build-local.sh"
fi
mkdir -p "$OUT"
# Everything but the device layer, which needs libfprint internals.
mapfile -t SRCS < <(cd "$DRV" && ls ./*.c milan/*.c milan/*/*.c |
    grep -vE '^\./(goodix|goodix55b4|goodixtls)\.c$' | sed "s|^|$DRV/|")
read -ra CFLAGS <<< "$(pkg-config --cflags glib-2.0 gio-2.0 gusb)"
read -ra LIBS <<< "$(pkg-config --libs glib-2.0 gio-2.0 libdeflate)"
nice cc -std=gnu11 -O1 -g -fsanitize=thread -w -I"$DRV" -I"$LF/libfprint" \
    -I"$LF_BUILD/libfprint" "${CFLAGS[@]}" "$ROOT/tests/test-goodix55b4-state.c" \
    "${SRCS[@]}" -o "$OUT/test-goodix55b4-state" "${LIBS[@]}"

for ((i = 1; i <= RUNS; i++)); do
    TSAN_OPTIONS=halt_on_error=1 "$OUT/test-goodix55b4-state" > "$OUT/run.log" 2>&1 || {
        cat "$OUT/run.log"
        echo "TSan run $i failed" >&2
        exit 1
    }
done
echo "ThreadSanitizer: $RUNS runs clean"
