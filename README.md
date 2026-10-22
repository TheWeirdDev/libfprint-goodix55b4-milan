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

### Flashing firmware 10041

The command above only works if the sensor already runs
`GF3268_RTSEC_APP_10041`. For a sensor with other firmware, or if you are not
sure, use `run_55b4.py` from
[goodix-fp-dump](https://github.com/mpi3d/goodix-fp-dump). It flashes 10041
when needed and writes the zero PSK. On a sensor that already runs 10041 it
only writes the PSK. Adapted from
[d-k-bo's guide](https://gist.github.com/d-k-bo/15e53eab53e2845e97746f5f8661b224).

> [!WARNING]
> Flashing can break the sensor. Everything here is at your own risk. Read
> the script before running it.
>
> - Run it by hand, once. Never run it at startup or on a schedule: flashing
>   the sensor too often breaks it.
> - If you dual-boot, disable the Windows fingerprint driver completely.
>   Otherwise Windows reflashes the sensor and pairs it with a new PSK.

    sudo pacman -S --needed git python openssl
    git clone --recurse-submodules https://github.com/mpi3d/goodix-fp-dump.git
    git -C goodix-fp-dump checkout cc43bb3b3154a0bccc0412ae024013c7e1923139
    python -m venv goodix-fp-dump/.venv
    goodix-fp-dump/.venv/bin/pip install -r goodix-fp-dump/requirements.txt
    sudo systemctl stop fprintd
    cd goodix-fp-dump
    sudo .venv/bin/python run_55b4.py

The tested commit is pinned above. The venv is needed because some of the
tool's dependencies (`python-periphery`, `spidev`) are not in the Arch repos.
The script runs `openssl s_server` for the TLS session it uses to read
the image. The script prints a warning and
asks you to type a number to continue. It then prints the firmware and PSK
status, flashes if needed, and waits for a finger. It saves the image as
`fingerprint.pgm`, which should look like a fingerprint. Then restart fprintd
(`sudo systemctl start fprintd`) and enroll.

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

After each successful match the engine updates the matched template with the
new touch (adaptive learning, as on Windows), so a print follows the finger
over time, e.g. damp skin after washing. fprintd cannot store updated prints,
so the driver keeps them in `/var/lib/fprint/goodix55b4/studied/`, one file per
enrolled print, and matches against them instead of the enrolled template.
Re-enrolling a finger starts from scratch. Deleting the directory goes back to
the enrolled templates; `GOODIX55B4_NO_STUDY=1` turns learning off. Offline on
the 3-finger set this raised single-attempt identify from 95% to 97% (12-touch
enrolment) and from 78% to 88% (6 touches), with no false accepts.
