# libfprint-goodix55b4-milan

Fingerprint driver for the **Goodix 27c6:55b4** sensor (108×88)
for libfprint, with Goodix's Milan matcher ported to
native C (no Windows DLL). In testing, identification succeeds on the first
attempt 95% of the time, against 71% for the SIGFM matcher, with no false
accepts on the test set.

The USB/TLS protocol layer is based on the goodixtls55x4 driver. The image
preprocessing and matching code in `driver/goodix55b4/milan/` comes from the
Milan port in the goodix53x5 (goodix-fp-linux-dev)
libfprint work (commit in `driver/goodix55b4/milan/UPSTREAM_COMMIT`). The
55b4 uses the same die as the Dell 53x5. All code is LGPL-2.1-or-later.

## Install (Arch Linux)

The package is a drop-in replacement for `libfprint`. It builds upstream
libfprint 1.94.10 with every normal driver plus this one.

    git clone https://github.com/TheWeirdDev/libfprint-goodix55b4-milan.git
    cd libfprint-goodix55b4-milan/packaging/arch
    makepkg -si
    sudo systemctl restart fprintd
    fprintd-enroll

The package also installs the libfprint examples as
`goodix55b4-{img-capture,enroll,verify,identify,manage-prints}`. Stop fprintd
before using them; they need root for USB access.

Prints from other 55b4 drivers are not compatible. Delete them and re-enroll.

## Sensor provisioning (one time)

The driver talks to the sensor with the all-zero pre-shared key (PSK) and
refuses to start otherwise. Sensors that were last used with the Windows
driver hold a sealed random PSK. Writing the zero PSK needs firmware that
allows PSK rewrite (`GF3268_RTSEC_APP_10041`). **This overwrites the
sensor's existing PSK, so Windows will no longer pair with it.**

    sudo systemctl stop fprintd
    sudo python3 tools/capture_dataset_55b4.py /tmp/out --finger test --touches 1 --write-zero-psk

Requires `python-pyusb` and `python-cryptography`. The script is the same
tool used to record the evaluation dataset.

## Build and test without installing

    scripts/build-local.sh

Builds a libfprint with only this driver in `.build/libfprint` and prints the
paths of the example programs.

## Layout

- `driver/goodix55b4/`: the libfprint driver (`goodix55b4.c`, USB/TLS in
  `goodix*.c`, per-unit config from OTP in `goodix55b4-calib.c`, matcher in `milan/`)
- `scripts/integrate.py`: adds the driver to a libfprint source tree
- `packaging/arch/PKGBUILD`: Arch package
- `tools/`: provisioning and capture tool
- `docs/FDT-55b4.md`: notes on finger detection

## State

The matcher keeps preprocessing state between scans. It is stored in
`/var/lib/fprint/goodix55b4/engine-state.bin` (override with
`GOODIX55B4_STATE_DIR`). Deleting it hurts match rates until it is rebuilt.
