# Upstream libfprint with all of its drivers plus goodix55b4 (Goodix
# 27c6:55b4/5584, native Milan matcher). A drop-in replacement for Fedora's
# libfprint and libfprint-devel; fprintd uses it directly.
#
# Built by build-rpm.sh, which defines the versions (read from the Arch
# PKGBUILD) and creates both source tarballs. The defaults below only keep
# the spec parseable for `dnf builddep`.
#
# Built like Fedora's own libfprint.spec (drivers=all, same files), as one
# package instead of libfprint + libfprint-devel, and with the tests run.
%{!?pkgver:     %global pkgver 0}
%{!?lf_version: %global lf_version 0}
%{!?lf_commit:  %global lf_commit 0}

# Like the Arch package: no -debuginfo split.
%global debug_package %{nil}
%global examples img-capture enroll verify identify manage-prints

Name:           libfprint-goodix55b4
Version:        %{pkgver}
Release:        1%{?dist}
Summary:        libfprint with the Goodix 27c6:55b4/5584 driver using the native Milan matcher
# libfprint/nbis is NIST-PD, as in Fedora's libfprint.
License:        LGPL-2.1-or-later AND NIST-PD
URL:            https://github.com/TheWeirdDev/libfprint-goodix55b4-milan
Source0:        libfprint-%{lf_commit}.tar.gz
Source1:        goodix55b4-milan.tar.gz

BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  meson
BuildRequires:  patchelf
BuildRequires:  python3
BuildRequires:  systemd-rpm-macros
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  pkgconfig(gobject-introspection-1.0)
BuildRequires:  pkgconfig(gudev-1.0)
BuildRequires:  pkgconfig(gusb)
BuildRequires:  pkgconfig(libdeflate)
BuildRequires:  pkgconfig(openssl)
BuildRequires:  pkgconfig(pixman-1)
# For the tests
BuildRequires:  pkgconfig(cairo)
BuildRequires:  python3-cairo
BuildRequires:  python3-gobject
BuildRequires:  umockdev

Provides:       libfprint = %{lf_version}
Provides:       libfprint%{?_isa} = %{lf_version}
Provides:       libfprint-devel = %{lf_version}
Provides:       libfprint-devel%{?_isa} = %{lf_version}
Conflicts:      libfprint
Conflicts:      libfprint-devel

%description
Drop-in replacement for libfprint: upstream libfprint %{lf_version} with all
of its drivers plus goodix55b4, which uses the native Milan matcher. Also
contains the development files and the GObject typelib, and the libfprint
examples as goodix55b4-{img-capture,enroll,verify,identify,manage-prints}.

%prep
%setup -q -n libfprint -a 1
python3 -I goodix55b4-milan/scripts/integrate.py .

%build
# drivers=all adds the virtual drivers (used by the tests here and by
# fprintd's), as in every distribution's build. Explicit udev paths: no
# udev.pc needed. The hwdb is forced on because systemd's copy does not mark
# 55b4/5584 as supported.
%meson \
    -Ddrivers=all -Dintrospection=true \
    -Dudev_rules=enabled -Dudev_rules_dir=%{_udevrulesdir} \
    -Dudev_hwdb=enabled -Dudev_hwdb_dir=%{_udevhwdbdir} \
    -Ddoc=false -Dinstalled-tests=false -Dgtk-examples=false
%meson_build

%install
%meson_install
for e in %{examples}; do
    install -Dm755 %{_vpath_builddir}/examples/$e %{buildroot}%{_bindir}/goodix55b4-$e
    # Drop the build-tree RUNPATH; the system libfprint is this package.
    patchelf --remove-rpath %{buildroot}%{_bindir}/goodix55b4-$e
done

%check
# integrate.py must have moved 55b4/5584 from the "unsupported" list to the
# goodix55b4 driver.
grep -q '^usb:v27C6p55B4' data/autosuspend.hwdb
grep -q '^usb:v27C6p5584' data/autosuspend.hwdb
if grep -qE '0x(55b4|5584)' libfprint/fprint-list-udev-hwdb.c; then
    echo "55b4/5584 still listed as unsupported" >&2
    exit 1
fi
%meson_test

%files
%license COPYING
%license goodix55b4-milan/LICENSE
%doc NEWS README.md
%{_bindir}/goodix55b4-*
%{_libdir}/libfprint-2.so
%{_libdir}/libfprint-2.so.2*
%{_libdir}/girepository-1.0/FPrint-2.0.typelib
%{_libdir}/pkgconfig/libfprint-2.pc
%{_includedir}/libfprint-2/
%{_datadir}/gir-1.0/FPrint-2.0.gir
%{_datadir}/metainfo/org.freedesktop.libfprint.metainfo.xml
%{_udevrulesdir}/70-libfprint-2.rules
%{_udevhwdbdir}/60-autosuspend-libfprint-2.hwdb
