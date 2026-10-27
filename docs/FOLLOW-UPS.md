# Follow-ups

Open work left after the review and fixes released in v1.94.100.m3
(PR #3). Each item says why it matters and where to start. Remove an item
when it is done.

## Small

### `goodix_decode_protocol` length 0
`goodix_proto.c`: a protocol length field of 0 underflows to 65535; the
bounds check right after rejects it (covered by
`/proto/decode-rejects-short`), but it is fragile. Reject `length == 0`
explicitly before subtracting.

## Medium

### Collapse the command wrappers in `goodix.c`
About 25 `goodix_send_*` functions repeat the same block: allocate a
`GoodixCallbackInfo` with `malloc`, fill it, call `goodix_send_protocol`
with an adapter (`goodix_receive_default`, `_none`, `_success`, ...), or
call it with `NULL, NULL` when there is no callback. One helper taking
`(cmd, payload, length, free_func, checksum, timeout, reply, adapter,
callback, user_data)` replaces ~600 lines and the `malloc`/`g_free` mix
(the adapters free with `g_autofree`). `goodix_send_protocol` must keep its
signature (contract documented above it: non-NULL callbacks are adapters
that consume a `GoodixCallbackInfo`). Behaviour must not change: the unit
tests do not cover these wrappers, so verify on the sensor (open, enroll,
verify, close) with the sanitized build. Large diff; keep it in its own PR.

### Formatting of `goodix.c` and `goodixtls.c`
Both files mix GNU style with K&R blocks inherited from goodixtls55x4.
Cosmetic; do it after the wrapper refactor, in a separate commit, so the
refactor diff stays readable.

### Measure the study policies on damp skin
`GOODIX55B4_STUDY_POLICY` (`native` default, `bounded`, `off`) was chosen
from dry-finger data only. To compare how each policy accepts and learns
damp touches, capture a damp set after washing hands:

    tools/capture_dataset_55b4.py ~/55b4-set --finger right-index-damp --touches 20

and extend `tests/test-goodix55b4-dataset.c` (manifest line for the new
set; enroll on dry, probe with damp) to report per-policy acceptance and how
many damp touches are learned. Revisit the default and the bounded score
gate (`GOODIX55B4_STUDY_BOUNDED_MIN_SCORE`, 30) with that data. Fingerprints
are personal data: keep datasets outside the repository.

A stronger variant discussed but not built: keep replacing features after
the template is full but never evict the 12 enrolled ones. Needs a change
in `milan/study/policy.c` victim selection and first a check that feature
positions are stable through study.

## Larger

### Review the Milan engine
`driver/goodix55b4/milan/` (~25k lines, vendored from goodix53x5) was only
sampled for memory safety in the template parsing paths
(`template/codec.c`, `print.c`), which looked careful. Its behaviour is
covered end to end by the real-frame suite, but not reviewed line by line.
Start with code that handles data from disk or the sensor:
`template/`, `preprocess/`, `feature/extraction.c`, then `match/`.

### Review the Python tools
`tools/capture_dataset_55b4.py` and `tools/gx_tls.py` were only read as
references for the protocol, not reviewed.

## Notes for whoever picks these up
- Build and test: `scripts/build-local.sh`, `scripts/run-tests.sh`
  (`GOODIX55B4_DATASET=~/55b4-set` adds the real-frame suite, ~3 min under
  ASan), `scripts/run-tsan.sh`.
- Hardware checks use the sanitized build with a throwaway state dir, e.g.
  `sudo env GOODIX55B4_STATE_DIR=$PWD/state ASAN_OPTIONS=detect_leaks=0
  G_MESSAGES_DEBUG=all .build/libfprint/builddir-asan/examples/img-capture`
  from a scratch directory. GLib debug output goes to stdout, warnings and
  sanitizer reports to stderr. Do not use `stdbuf` with ASan builds (or add
  `verify_asan_link_order=0`).
- The `ubuntu-latest` runner label moves to Ubuntu 26 from 2026-10-19;
  the test and Arch jobs run in an Arch container, so only the host
  changes, but watch the first runs after the switch.
