# Changelog

Versions are `<libfprint version>.m<n>`; each `v…` tag is built and published
as a GitHub release with its section below as the release notes.

## v1.94.100.m4 - 2026-10-27

- Package builds no longer fail at random: libfprint's `egis_etu905` test (a
  different driver's replay test, flaky on CI runners) is no longer run
  while building the packages. The driver itself is unchanged.

## v1.94.100.m3 - 2026-10-26

Reliability and security fixes from a full review of the driver. Prints,
engine state and learned templates from earlier versions keep working.

- fprintd no longer hangs if the TLS handshake with the sensor stalls or
  fails: TLS now runs without a helper thread or blocking reads, a failed
  handshake is reported as an error, and closing the device during one is
  safe.
- Malformed data from the sensor is rejected instead of being read past its
  end, and corrupted USB packets and images are detected by their checksums
  (images are read again, up to 3 times).
- No more 100% CPU loop when the sensor disappears (e.g. unplugged or on
  resume): the driver stops and reports an error.
- Operations that previously could wait forever (a busy device, an
  unanswered finger-up detection) now end with an error.
- Matching is smoother: the engine state and learned templates (up to
  ~0.5 MB) are saved in the background and only when they changed, instead
  of on fprintd's main thread after every match.
- New `GOODIX55B4_STUDY_POLICY`: `native` (default, unchanged behaviour),
  `bounded` (learns only from confident matches and never replaces the
  enrolled features) or `off`. See the README for how to set it.
- New troubleshooting switches `GOODIX55B4_LAX_CHECKSUM=1` and
  `GOODIX55B4_TLS_CIPHERS`; TLS now offers only the suite the sensor uses.
- Unit tests (with sanitizers) run on every push and before every release.

## v1.94.100.m2 - 2026-10-26

- Packages for Debian 12 and 13, Ubuntu 24.04 and 26.04, and Fedora 43 and
  44, attached to each GitHub release next to the Arch package. Built by
  `packaging/debian/build-deb.sh` and `packaging/fedora/build-rpm.sh`
  (spec in `packaging/fedora/`); like the Arch package they replace the
  distribution's libfprint.
- The release workflow builds all packages in parallel. A manual run with
  an empty tag builds the branch without publishing.
- All packages, Arch included, are now built like the distributions' own
  libfprint: with `-Ddrivers=all` (adds the virtual drivers that fprintd's
  tests use) and with upstream's test suite run during the build.

## v1.94.100.m1 - 2026-10-26

- Rebased on upstream libfprint 1.94.100 (was 1.94.10). The driver itself is
  unchanged; `integrate.py` now also handles 1.94.100's new driver table in
  `meson.build`. Prints enrolled with earlier versions keep working.

## v1.94.10.m5 - 2026-10-26

- Support for the Goodix 27c6:5584 sensor: the same Milan die and protocol as
  the 55b4, so it only needed a USB ID. `integrate.py` adds both IDs to the
  autosuspend hwdb and drops them from libfprint's unsupported list, and
  `tools/capture_dataset_55b4.py` finds either sensor. The device is now named
  "Goodix 55x4 Fingerprint Sensor (Milan)". Thanks to @V-Conet (#2).
- CI: a GitHub Actions workflow builds the Arch package for every release tag
  and attaches it, with `SHA256SUMS`, to the GitHub release, using this
  changelog for the release notes.

## v1.94.10.m4 - 2026-10-26

- The USB device is no longer reset on every open. A normal open just claims
  the interface; only when activation times out (an MCU left stuck by an open
  cut short by suspend) is the device reset and activation retried once.
  Opens are faster, which avoids a race with the GNOME lock screen restarting
  verification ("Device was already claimed") after resume.
- The reset and its outcome are logged as warnings, so they show up in
  `journalctl -u fprintd`: `activation failed: …; resetting the USB device and
  retrying`, then `activation succeeded after USB reset` or `USB reset failed`.
- Fixed a cancelled read transfer being able to start a second read loop when
  the device is re-initialized.

## v1.94.10.m3 - 2026-10-24

- Reset the USB device on open, to recover an MCU that NAKs every transfer
  after an open was interrupted by system suspend ("transfer timed out").
- README: guide for flashing firmware 10041.
- PKGBUILD: `url` points at this repository, `replaces` dropped, added the
  `glib2-devel` makedepend.

## v1.94.10.m2 - 2026-10-22

- Adaptive learning: after each successful match the matched template is
  updated with the new touch (Milan study), as the Windows driver does.

## v1.94.10.m1 - 2026-10-21

- Initial release: libfprint 1.94.10 with the goodix55b4 driver for the
  Goodix 27c6:55b4 sensor, using Goodix's Milan matcher ported to native C.
