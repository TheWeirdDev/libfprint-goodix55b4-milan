# Changelog

Versions are `<libfprint version>.m<n>`; each `v…` tag is built and published
as a GitHub release with its section below as the release notes.

## Unreleased

- CI: a GitHub Actions workflow builds the Arch package for every release tag
  and attaches it, with `SHA256SUMS`, to the GitHub release.

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
