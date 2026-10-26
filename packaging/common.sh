# Shared by packaging/debian/build-deb.sh and packaging/fedora/build-rpm.sh.
# Versions are read from packaging/arch/PKGBUILD, the one place they are
# kept (the AUR needs that file to be self-contained).

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
LIBFPRINT_URL=https://gitlab.freedesktop.org/libfprint/libfprint.git
EXAMPLES=(img-capture enroll verify identify manage-prints)

die() {
    echo "error: $*" >&2
    exit 1
}

# Sets PKGVER (1.94.100.m1), LF_VERSION (1.94.100) and LF_COMMIT.
read_versions() {
    local vars
    vars=$(
        # shellcheck source=arch/PKGBUILD
        source "$ROOT/packaging/arch/PKGBUILD"
        for p in "${provides[@]}"; do
            if [[ $p == libfprint=* ]]; then
                printf 'LF_VERSION=%q\n' "${p#libfprint=}"
            fi
        done
        printf 'PKGVER=%q\nLF_COMMIT=%q\n' "$pkgver" "$_libfprint_commit"
    ) || die "cannot read packaging/arch/PKGBUILD"
    eval "$vars"
    [[ -n ${PKGVER:-} && -n ${LF_VERSION:-} && -n ${LF_COMMIT:-} ]] ||
        die "packaging/arch/PKGBUILD lacks pkgver, _libfprint_commit or provides=libfprint=..."
    echo "goodix55b4 $PKGVER on libfprint $LF_VERSION ($LF_COMMIT)"
}

# Writes the pristine libfprint tree at LF_COMMIT to $1. Uses the clone in
# $LIBFPRINT_DIR, else the one scripts/build-local.sh keeps (cloned when
# missing); only git objects are read, its working tree is left alone.
export_libfprint() {
    local dest=$1 repo=${LIBFPRINT_DIR:-$ROOT/.build/libfprint}
    if [[ ! -d $repo/.git ]]; then
        git clone --quiet --no-checkout "$LIBFPRINT_URL" "$repo"
    fi
    if ! git -C "$repo" cat-file -e "$LF_COMMIT^{commit}" 2>/dev/null; then
        git -C "$repo" fetch --quiet origin
    fi
    mkdir -p "$dest"
    git -C "$repo" archive "$LF_COMMIT" | tar -x -C "$dest"
}

# Copies the driver, the integration script and the license to $1. Taken
# from the working tree, so local changes are packaged too.
export_driver() {
    mkdir -p "$1"
    cp -a "$ROOT/driver" "$ROOT/scripts" "$ROOT/LICENSE" "$1/"
}

# integrate.py must have moved 55b4/5584 from libfprint's "unsupported"
# list to the goodix55b4 driver in the libfprint tree $1.
check_integration() {
    grep -q '^usb:v27C6p55B4' "$1/data/autosuspend.hwdb" ||
        die "55b4 missing from autosuspend.hwdb"
    grep -q '^usb:v27C6p5584' "$1/data/autosuspend.hwdb" ||
        die "5584 missing from autosuspend.hwdb"
    if grep -qE '0x(55b4|5584)' "$1/libfprint/fprint-list-udev-hwdb.c"; then
        die "55b4/5584 still listed as unsupported"
    fi
}
