#!/usr/bin/env python3
"""Add the goodix55b4 driver to a libfprint source tree (tested: 1.94.10).

  integrate.py LIBFPRINT_DIR

Copies driver/goodix55b4 to LIBFPRINT_DIR/libfprint/drivers/goodix55b4 (replacing
an earlier copy) and edits the build files. Safe to run repeatedly.
"""
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRIVER = "goodix55b4"


def edit(path, fn):
    with open(path) as f:
        old = f.read()
    new = fn(old)
    if new != old:
        with open(path, "w") as f:
            f.write(new)
        print("patched", os.path.relpath(path))


def insert_after(text, anchor, line, path):
    if line.strip() in text:
        return text
    if anchor not in text:
        sys.exit("%s: anchor not found: %r" % (path, anchor))
    return text.replace(anchor, anchor + line, 1)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    lf = os.path.abspath(sys.argv[1])
    src = os.path.join(ROOT, "driver", DRIVER)
    dst = os.path.join(lf, "libfprint", "drivers", DRIVER)
    if not os.path.isfile(os.path.join(lf, "libfprint", "meson.build")):
        sys.exit("%s is not a libfprint source tree" % lf)

    if os.path.isdir(dst):
        shutil.rmtree(dst)
    shutil.copytree(src, dst)
    print("copied driver to", os.path.relpath(dst))

    # libfprint/meson.build: subdir, driver sources, cflags
    p = os.path.join(lf, "libfprint", "meson.build")

    def lib(t):
        sub = "subdir('drivers/%s')\n\n" % DRIVER
        if sub not in t:
            t = t.replace("driver_sources = {", sub + "driver_sources = {", 1)
        t = insert_after(t, "driver_sources = {\n",
                         "    '%s' :\n        %s_sources,\n" % (DRIVER, DRIVER), p)
        t = insert_after(t, "drivers_cflags = []\n",
                         "drivers_cflags += %s_cflags\n" % DRIVER, p)
        return t
    edit(p, lib)

    # meson.build: default driver list + OpenSSL helper (TLS layer)
    p = os.path.join(lf, "meson.build")

    def top(t):
        t = insert_after(t, "default_drivers = [\n", "    '%s',\n" % DRIVER, p)
        t = insert_after(t, "driver_helper_mapping = {\n",
                         "    '%s' : [ 'openssl' ],\n" % DRIVER, p)
        return t
    edit(p, top)

    # The device is no longer "unsupported"
    p = os.path.join(lf, "libfprint", "fprint-list-udev-hwdb.c")
    edit(p, lambda t: re.sub(r"\n\s*\{ \.vid = 0x27c6, \.pid = 0x55b4 \},", "", t))

    p = os.path.join(lf, "data", "autosuspend.hwdb")

    def hwdb(t):
        block = ("# Supported by libfprint driver %s\nusb:v27C6p55B4*\n"
                 " ID_AUTOSUSPEND=1\n ID_PERSIST=0\n\n" % DRIVER)
        # Drop 55B4 from the unsupported list only (not from our own block).
        head, ours, tail = t.partition(block)
        t = (re.sub(r"\nusb:v27C6p55B4\*", "", head) + ours +
             re.sub(r"\nusb:v27C6p55B4\*", "", tail))
        if block not in t:
            anchor = "# Supported by libfprint driver goodixmoc\n"
            if anchor not in t:
                sys.exit("%s: anchor not found" % p)
            t = t.replace(anchor, block + anchor, 1)
        return t
    edit(p, hwdb)


if __name__ == "__main__":
    main()
