#!/usr/bin/env bash
# Build an .rpm of upstream libfprint with all of its drivers plus
# goodix55b4, for the Fedora release this runs on, from
# libfprint-goodix55b4.spec. A drop-in replacement for libfprint.
#
#   packaging/fedora/build-rpm.sh [OUTDIR]       # default: .build/rpm
#
# Install the build dependencies first:
#   sudo dnf install rpm-build git 'dnf-command(builddep)'
#   sudo dnf builddep packaging/fedora/libfprint-goodix55b4.spec
# Set LIBFPRINT_DIR to reuse an existing libfprint clone.
set -euo pipefail
# shellcheck source=../common.sh
source "$(dirname "$0")/../common.sh"

read_versions
OUT=$(realpath -m "${1:-$ROOT/.build/rpm}")
TOP=$ROOT/.build/rpm-work
rm -rf "$TOP"
mkdir -p "$TOP/SOURCES" "$TOP/tmp" "$OUT"

export_libfprint "$TOP/tmp/libfprint"
tar -C "$TOP/tmp" -czf "$TOP/SOURCES/libfprint-$LF_COMMIT.tar.gz" libfprint
export_driver "$TOP/tmp/goodix55b4-milan"
tar -C "$TOP/tmp" -czf "$TOP/SOURCES/goodix55b4-milan.tar.gz" goodix55b4-milan

rpmbuild -bb \
    --define "_topdir $TOP" \
    --define "pkgver $PKGVER" \
    --define "lf_version $LF_VERSION" \
    --define "lf_commit $LF_COMMIT" \
    "$ROOT/packaging/fedora/libfprint-goodix55b4.spec"

find "$TOP/RPMS" -name '*.rpm' -print -exec cp -t "$OUT" {} +
