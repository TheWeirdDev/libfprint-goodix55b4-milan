# Goodix 55b4 — FDT (Finger Detection Threshold) explained

Based on the 53x5 driver code (`goodix53x5-calibration.c`, `goodix53x5-scan.c`)
and the decoded Windows capture `55b4_enroll_success.pcapng` (Windows capture, not included)
(decoded with a Wireshark Goodix dissector). Statements marked *inference*
are not directly observed.

## The idea

FDT lets the device's microcontroller (MCU) watch for a finger cheaply, so the
host doesn't have to keep requesting full 108×88 images.

The sensor is capacitive: a finger changes the capacitance under it. Besides
full images, the chip can take a quick, coarse reading of **12 zones**
(channels) of the sensor surface, each giving one number. The MCU can do this
repeatedly at low power.

A finger lowers the zone values:

| State | Typical channel values (after `>>1`) |
|---|---|
| No finger (baseline) | `b3 c6 bc a8 b9 ca c2 ab b7 c6 bc a7` |
| Finger pressed | `83 89 87 79 8d 99 91 78 8e 9b 8d 6e` |

The host gives the MCU 12 thresholds and says "tell me when the values cross
these". The MCU replies only when that happens. Until then the USB request just
waits, which is why the capture shows 0.3–1.2 s gaps (the user moving their finger).

## The three commands

| Command | Payload op | Meaning | When the reply comes |
|---|---|---|---|
| `0x32` FDT down | `0x0c` | "Tell me when a finger lands" (values drop) | When a finger is placed |
| `0x36` FDT mode | `0x0d` | "Take one reading right now" | Immediately (~16 ms) |
| `0x34` FDT up | `0x0e` | "Tell me when the finger leaves" (values rise) | When the finger is lifted |

### Request payload (26 bytes)

```
0c 01 | 80 b3 | 80 c6 | 80 bc | ... (12 channels)
op ?? | ch0   | ch1   | ch2   |
```

- Each channel entry is a little-endian `u16` = `(threshold << 8) | 0x80`. The
  useful part is the high byte; the `0x80` is constant in every capture.
- The second byte is always `01` (the 53x5 code also hardcodes it).

### Reply payload (28 bytes)

```
02 00 | ee 0e | 61 01 | 4e 01 | ...  (12 × u16)
irq   | touch | ch0   | ch1   |
```

- `irq` says which event fired. Observed values: `0x0002` = down,
  `0x0102` = mode/one-shot, `0x0200` = up.
- `touch` is a 12-bit mask of which channels count as touched. `0xfff` means
  all 12; `0xeee` means a few zones weren't covered yet when it triggered.
- The readings compare against thresholds after `>> 1`. For example
  `0x0161 >> 1 = 0xb0`. Why there's an extra low bit is unknown; the 53x5
  driver does the same shift.

## One full touch, with real numbers from the capture

```
 host                                    MCU / sensor
  │ 0x32 DOWN  thr = baseline b3 c6 bc..   │  waits...
  │                                        │  (finger placed, 1.18 s)
  │ ◄── irq=0x02 touch=0xeee  b0 a7 91 ..  │  values fell → finger!
  │ 0x20 GET IMAGE  ───────────────────►   │
  │ ◄── encrypted 108×88 frame             │
  │ 0x36 MODE (one-shot reading)           │
  │ ◄── irq=0x102  83 89 87 79 ..          │  "pressed" levels
  │ 0x34 UP   thr = 83+20, 89+20 ..        │  = 97 9d 9b 8d ..
  │ 0x34 UP   (same, sent again)           │  waits...
  │                                        │  (finger lifted, 0.5 s)
  │ ◄── irq=0x200  b3 c6 bc a8 ..          │  values rose → finger gone
  │ 0x32 DOWN  thr = b3 c6 bc a8 ..        │  new baseline = what it just read
```

## The two rules Windows follows

These hold on every touch in the capture:

1. **Up threshold = finger-down reading + 20 on touched channels, `0x12` on
   untouched ones.** *(Corrected 2026-10-18.)* Windows sends `0x34` twice. The
   first one (not waited on) uses the `0x36` reply value >> 1 + 20 on every
   channel. The second one replaces it and is the one that counts: it's built
   from the `0x32` finger-down event, value >> 1 + 20 where the event's touch
   bit `i` is set, and a fixed `0x12` where it isn't. Example
   (`55b4_init_lock_unlock`, frame 844): touch `0x0373` → up table
   `97 bf 12 12 97 a4 a7 12 95 b0 12 12`. Giving untouched channels
   baseline + 20 instead can leave the lift unreported forever after an
   off-centre touch.

   *Per unit (2026-10-18):* 20 is your unit's calibration, not a constant.
   Windows computes it from OTP byte 22 (`goodix55b4-calib.c`; RE of
   `usbinterface.dll`, sensor type 11) and writes it to config tag `0x82`; the
   untouched-channel value is that delta − 2. The finger counts as lifted once values rise 20
   above where they sat while pressed. The 20 comes from the config (tag `0x82`
   in section 2 = `0x1480`, so delta = `0x14`). The margin is **hysteresis**:
   small wobbles while pressing don't count as a lift.
2. **Next down threshold = the reading taken as the finger lifted** (the `0x34`
   reply value >> 1, +0). Every lift re-measures the empty sensor, so the
   baseline follows slow drift from temperature, humidity and skin residue. The
   `0x36` sent after the image reuses that same table.

Unexplained: the down threshold equals the baseline with no margin, yet it
doesn't fire constantly. *Inference:* the MCU applies its own margin
internally, probably from config.

### Pseudocode

```c
/* reply: irq u16, touch u16, val[12] u16 */
for (i = 0; i < 12; i++)
  up_table[i]   = (((mode_reply.val[i] >> 1) + 20) << 8) | 0x80;   /* send with 0x34, op 0x0e */
for (i = 0; i < 12; i++)
  down_table[i] = (((up_reply.val[i] >> 1)) << 8) | 0x80;         /* send with 0x32, op 0x0c */
/* payload = { op, 0x01, table[0..11] as u16 LE } */
```

## Why this matters for the driver

`goodix55x4.c` sends **fixed** tables, captured once from a Windows session:

- **Down** `0c01 80b1 80c6 80bc 80a6 …` is very close to the real baseline
  (`b3 c6 bc a8 …`), so it works until the sensor drifts. Then you get missed
  touches or phantom ones.
- **Up** `0e01 8092 809d 8093 …` is one specific past press plus 20. If the
  finger presses harder or lighter than that recorded one, the threshold can be
  wrong. It may fire "up" while the finger is still pressing, or never fire at all.

Replacing them with the two rules above makes detection track the actual finger
and sensor, like Windows.

## Observed on real hardware (Linux, firmware 10041, 2026-10-18)

From `capture_dataset_55b4.py` runs (`~/55b4-set/events.jsonl`):

| Command | IRQ in reply | Meaning |
|---|---|---|
| `0x36` mode, no finger | `0x0180`, touch `0x000` | One-shot reading, nothing touched |
| `0x36` mode, finger | `0x0102`, touch `0xfff` | One-shot reading, finger present |
| `0x32` down | `0x0002`, touch `0xfff`/`0x7f7` | Real finger-down |
| `0x32` down | **`0x0080`**, touch `0x000` | **Baseline drift, no finger**: refresh DOWN from these values and re-arm |
| `0x34` up | `0x0200`, touch `0x000` | Finger lifted |

So bit `0x0002` = touched, `0x0080` = baseline/untouched, `0x0100` = one-shot
(`0x36`) reading, `0x0200` = lifted.

**The `0x0080` case really happens.** In the first run, the up-event values
were taken mid-lift (ch3 = 141 vs a baseline of ~167), so the next down table
was too low on some channels, and the MCU answered the next `0x32` with
`0x0080`. Treating that as a finger produced a no-finger image, then an
impossible up threshold, and the capture hung. The current driver (fixed
tables, no `0x0080` handling) is exposed to the same trap.

## Confirmations from other drivers

- **libfprint-55a2** (same goodixtls55x4 driver family) independently derives
  the down table from the `0x36` reply as `>> 1`.
- **goodix-27c6-5125** (TECHNICAL_MANUAL.md §6.4, reverse engineered from the
  OEM host logic, 6-channel chip) uses the same mechanism and fills two gaps:
  - **Untouched channels:** on finger-down, channels whose touch bit is 0 get a
    *fixed* up-threshold (`0x1B` on 5125) instead of reading + delta. On 5125
    the up delta for touched channels is `0x1D`; on 55b4 it's 20 (`0x14`).
  - **IRQ `0x0080`** arriving while waiting on `0x32` = the MCU reporting
    baseline drift, not a finger. The host re-derives the down table from that
    event's values (`>> 1`) and re-arms `0x32`. Not handling it is a plausible
    cause of the "FDT stops firing after many cycles" problem reported on 55a2
    (*inference*).
  - It rejects threshold bytes `0x00`/`0xFF` and never reuses a stale table.
- **goodix53x5 upstream** uses the same op codes, with the high bit meaning TX
  off: manual `0x0D`/`0x8D`, down `0x0C`, up `0x0E`.

## Open details

- Why Windows sends `0x34` twice (first without waiting for a reply; the driver
  copies this as `FDT_UP_NO_REPLY`).
- How the very first down table is produced at init. *Inference:* a one-shot
  reading with no finger. Check the init captures (`55b4_init_enroll`,
  `55b4_init_login`) to confirm.
- The 53x5 driver also uses the `touch` mask: zones that weren't touched get a
  different, fixed up threshold. Your captures always show `0xfff` after the
  one-shot reading, so it's unknown whether 55b4 needs that.
