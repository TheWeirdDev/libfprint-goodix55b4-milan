#!/usr/bin/env bash
# Build a .deb of upstream libfprint with all of its drivers plus goodix55b4,
# for the Debian/Ubuntu release this runs on (a .deb is tied to the library
# versions of one release). Like the Arch package, it is a drop-in
# replacement for libfprint-2-2 (plus -dev and the typelib); fprintd uses it
# directly.
#
#   packaging/debian/build-deb.sh [OUTDIR]       # default: .build/deb
#
# Install packaging/debian/build-deps first. Set LIBFPRINT_DIR to reuse an
# existing libfprint clone, DEB_BUILD_OPTIONS=nocheck to skip the tests.
#
# Built like Debian's own libfprint (debian/rules: drivers=all, udev rules
# and hwdb, tests run), as one package instead of Debian's split ones.
set -euo pipefail
# shellcheck source=../common.sh
source "$(dirname "$0")/../common.sh"

read_versions
# shellcheck source=/dev/null
. /etc/os-release
case $ID in
debian) DISTRO=debian$VERSION_ID ;;
ubuntu) DISTRO=ubuntu$VERSION_ID ;;
*) die "unsupported distribution: $ID (Debian or Ubuntu only)" ;;
esac
# "~" sorts before the bare version, as for backports.
DEB_VERSION=$PKGVER-1~$DISTRO
ARCH=$(dpkg --print-architecture)
MULTIARCH=$(dpkg-architecture -qDEB_HOST_MULTIARCH)
OUT=$(realpath -m "${1:-$ROOT/.build/deb}")
WORK=$ROOT/.build/deb-work/$DISTRO
SRC=$WORK/libfprint
# Where dpkg-shlibdeps expects a package's files.
STAGE=$WORK/debian/libfprint-goodix55b4
rm -rf "$WORK"
mkdir -p "$WORK" "$OUT"

export_libfprint "$SRC"
python3 -I "$ROOT/scripts/integrate.py" "$SRC"
check_integration "$SRC"

# Debian's compiler and hardening flags, as dh would use.
eval "$(dpkg-buildflags --export=sh)"
# drivers=all adds the virtual drivers (used by the tests here and by
# fprintd's), as in every distribution's build. Explicit udev paths: no
# udev.pc needed. The hwdb is forced on because systemd's copy does not mark
# 55b4/5584 as supported.
meson setup "$WORK/build" "$SRC" \
    --prefix=/usr --libdir="lib/$MULTIARCH" --sysconfdir=/etc \
    --localstatedir=/var --buildtype=plain --wrap-mode=nodownload \
    -Ddrivers=all -Dintrospection=true \
    -Dudev_rules=enabled -Dudev_rules_dir=/usr/lib/udev/rules.d \
    -Dudev_hwdb=enabled -Dudev_hwdb_dir=/usr/lib/udev/hwdb.d \
    -Ddoc=false -Dinstalled-tests=false -Dgtk-examples=false
meson compile -C "$WORK/build"
if [[ " ${DEB_BUILD_OPTIONS:-} " != *" nocheck "* ]]; then
    # Same timeout as Debian's build, for slow builders.
    meson test -C "$WORK/build" --print-errorlogs --timeout-multiplier 5
fi
meson install -C "$WORK/build" --destdir "$STAGE" --strip

install -d "$STAGE/usr/bin"
for e in "${EXAMPLES[@]}"; do
    install -m755 "$WORK/build/examples/$e" "$STAGE/usr/bin/goodix55b4-$e"
    # Drop the build-tree RUNPATH; the system libfprint is this package.
    patchelf --remove-rpath "$STAGE/usr/bin/goodix55b4-$e"
    strip --strip-unneeded "$STAGE/usr/bin/goodix55b4-$e"
done
DOC=$STAGE/usr/share/doc/libfprint-goodix55b4
install -d "$DOC"
cat > "$DOC/copyright" <<EOF
libfprint-goodix55b4: upstream libfprint $LF_VERSION with the goodix55b4 driver.
Sources: $LIBFPRINT_URL (commit $LF_COMMIT)
         https://github.com/TheWeirdDev/libfprint-goodix55b4-milan

libfprint and the goodix55b4 driver: LGPL-2.1-or-later, see
/usr/share/common-licenses/LGPL-2.1.
libfprint/nbis (NIST Biometric Image Software): public domain (NIST-PD).
EOF
install -m644 "$SRC/NEWS" "$SRC/README.md" "$DOC/"
gzip -9n "$DOC/NEWS"

# Runtime dependencies from the library's NEEDED entries (the examples only
# add libfprint itself). dpkg-shlibdeps wants a debian/control, and finds
# the package root by its DEBIAN directory.
install -d "$STAGE/DEBIAN"
printf 'Source: libfprint-goodix55b4\n\nPackage: libfprint-goodix55b4\nArchitecture: any\n' \
    > "$WORK/debian/control"
DEPENDS=$(cd "$WORK" &&
    dpkg-shlibdeps -O "-edebian/libfprint-goodix55b4/usr/lib/$MULTIARCH/libfprint-2.so.2.0.0" |
    sed -n 's/^shlibs:Depends=//p')
[[ -n $DEPENDS ]] || die "dpkg-shlibdeps found no dependencies"

# Same names as Debian's own split packages, with an epoch-1 version so
# fprintd's "libfprint-2-2 (>= 1:1.94.x)" is satisfied.
REPLACED="libfprint-2-2, libfprint-2-dev, gir1.2-fprint-2.0"
PROVIDED="libfprint-2-2 (= 1:$LF_VERSION), libfprint-2-dev (= 1:$LF_VERSION),
 gir1.2-fprint-2.0 (= 1:$LF_VERSION), gir1.2-fprint-2.0-dev (= 1:$LF_VERSION)"

cat > "$STAGE/DEBIAN/control" <<EOF
Package: libfprint-goodix55b4
Version: $DEB_VERSION
Architecture: $ARCH
Maintainer: Alireza S.N. <alireza6677@gmail.com>
Installed-Size: $(du -sk "$STAGE" | cut -f1)
Depends: $DEPENDS
Provides: $PROVIDED
Conflicts: $REPLACED
Replaces: $REPLACED
Section: libs
Priority: optional
Homepage: https://github.com/TheWeirdDev/libfprint-goodix55b4-milan
Description: libfprint with the Goodix 27c6:55b4/5584 driver (Milan matcher)
 Drop-in replacement for libfprint-2-2: upstream libfprint $LF_VERSION with
 all of its drivers plus goodix55b4, which uses the native Milan matcher.
 Also contains the development files and the GObject typelib, and the
 libfprint examples as goodix55b4-{img-capture,enroll,verify,identify,
 manage-prints}.
EOF
echo "activate-noawait ldconfig" > "$STAGE/DEBIAN/triggers"
# Packages built against this library depend on libfprint-2-2, which this
# package provides (Debian's libfprint-2-2.symbols serves the same purpose).
echo "libfprint-2 2 libfprint-2-2 (>= 1:$LF_VERSION)" > "$STAGE/DEBIAN/shlibs"
# Apply the hwdb (autosuspend) to an attached sensor now, as Debian's
# libfprint-2-2.postinst does for the devices it supports.
cat > "$STAGE/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = configure ] && command -v udevadm > /dev/null 2>&1; then
	udevadm trigger --action=add --attr-match=idVendor=27c6 --attr-match=idProduct=55b4 || true
	udevadm trigger --action=add --attr-match=idVendor=27c6 --attr-match=idProduct=5584 || true
fi
exit 0
EOF
chmod 755 "$STAGE/DEBIAN/postinst"
(cd "$STAGE" && find . -path ./DEBIAN -prune -o -type f -printf '%P\0' |
    LC_ALL=C sort -z | xargs -0 md5sum > DEBIAN/md5sums)

# The release is in the file name (without the "~"), so the builds for
# several releases can be attached side by side to one GitHub release.
DEB=$OUT/libfprint-goodix55b4_$PKGVER-1_${DISTRO}_$ARCH.deb
dpkg-deb --root-owner-group --build "$STAGE" "$DEB"
dpkg-deb --info "$DEB" | sed -n '/Package:/,$p'
echo "built: $DEB"
