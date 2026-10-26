#!/usr/bin/env python3
"""Capture a raw-frame test set from a Goodix 27c6:55b4/5584 (zero PSK) for milan-cli.

Based on goodix_55b4_nofw_0psk.py, but headless, and it never changes the PSK
unless --write-zero-psk is given. Finger detection uses the dynamic FDT rules
from docs/FDT-55b4.md (down = baseline>>1, up = pressed>>1 + 20), so a session also
exercises those rules on real hardware.

Output directory:
  setup.raw            no-finger TX-on frame (u16 LE, 108x88 row-major)
  setup-<run>-NN.raw   no-finger frames captured at the start of every run
  <finger>-NN.raw      one frame per touch
  session-<time>.json  firmware, chip ID, OTP, PSK hash, config (per run)
  events.jsonl         every FDT request/reply and image capture, timestamped
  txoff.bin            reply to the optional TX-off experiment (--try-txoff)

Run as root (or with udev access), with fprintd stopped:
  sudo systemctl stop fprintd
  sudo python3 capture_dataset_55b4.py OUT --finger right-index --touches 20
  sudo python3 capture_dataset_55b4.py OUT --finger left-thumb  --touches 20
Re-running into the same OUT appends touches. Every run starts with a USB
reset (skip with --no-usb-reset) and captures fresh no-finger frames.
"""
import argparse
import json
import os
import struct
import sys
import time

import usb.core
import usb.util

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gx_tls import PskServer, TlsError  # noqa: E402

VID, PIDS = 0x27C6, (0x55B4, 0x5584)
ROWS, COLS = 88, 108
PIXELS = ROWS * COLS
IMAGE_BYTES = PIXELS // 4 * 6 + 4  # packed 12-bit + 4-byte checksum = 14260
ZERO_PSK_HASH = bytes.fromhex(
    "81b8ff490612022a121a9449ee3aad2792f32b9f3141182cd01019945ee50361")
# PSK white-box blob for the all-zero PSK (from goodix_55b4_nofw_0psk.py)
ZERO_PSK_WB = bytes.fromhex(
    "ec35ae3abb45ed3f12c4751f1e5c2cc05b3c5452e9104d9f2a3118644f37a04b"
    "6fd66b1d97cf80f1345f76c84f03ff30bb51bf308f2a9875c41e6592cd2a2f9e"
    "60809b17b5316037b69bb2fa5d4c8ac31edb3394046ec06bbdacc57da6a756c5")
# goodix_55x4_config from goodix-55b4/libfprint/drivers/goodixtls/goodix55x4.h
CONFIG = bytes.fromhex(
    "6011607124952cc114d510e500e514f9030402000008001111ba000180ca0007"
    "008400c0b38600bbc48800baba8a00b2b28c00aaaa8e00c1c19000bbbb9200b1"
    "b1940000a8960000b6980000bf9a0000ba50000105d000000070000000720078"
    "56740034122600001220001040120003042a0102002200012024003200800001"
    "005c008000560008205800010032002c028200800cba000180ca0007002a0182"
    "03200010402200012024001400800005005c0000015600082058000300820080"
    "142a0108005c0080006200090364001800220000202a0108005c000001520008"
    "0054000001000000000000000000000000000000000000000000000000009a69")
FDT_UP_DELTA = 0x14  # config section 2, tag 0x82 = 0x1480

CMD_FDT_DOWN, CMD_FDT_UP, CMD_FDT_MODE = 0x32, 0x34, 0x36
OP_FDT_DOWN, OP_FDT_UP, OP_FDT_MODE = 0x0C, 0x0E, 0x0D


def find_sensor():
    for pid in PIDS:
        dev = usb.core.find(idVendor=VID, idProduct=pid)
        if dev is not None:
            return dev
    return None


class Device:
    def __init__(self, events, usb_reset=True):
        self.events = events
        self.dev = find_sensor()
        if self.dev is None:
            sys.exit("27c6:55b4 or 27c6:5584 not found")
        if usb_reset:
            # Clears a wedged MCU/FDT state left by an interrupted session
            # (55a2 driver experience). The device re-enumerates; find it again.
            self.dev.reset()
            time.sleep(2)
            self.dev = find_sensor()
            if self.dev is None:
                sys.exit("27c6:55b4/5584 did not come back after USB reset")
        if self.dev.is_kernel_driver_active(0):
            self.dev.detach_kernel_driver(0)
        self.dev.set_configuration()
        intf = self.dev.get_active_configuration()[(0, 0)]
        self.out = usb.util.find_descriptor(intf, custom_match=lambda e:
            usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
        self.inp = usb.util.find_descriptor(intf, custom_match=lambda e:
            usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)
        self.tls = None

    def log(self, kind, **kw):
        kw.update(t=round(time.time(), 3), kind=kind)
        self.events.write(json.dumps(kw) + "\n")
        self.events.flush()

    # ---- framing (same as goodix_55b4_nofw_0psk.py) ----
    @staticmethod
    def _pad64(data):
        return data + b"\0" * (-len(data) % 64)

    @staticmethod
    def _cmd(cmd, data):
        payload = bytes((cmd,)) + struct.pack("<H", len(data) + 1) + data
        payload += bytes(((0xAA - sum(payload)) & 0xFF,))
        hdr = bytes((0xA0,)) + struct.pack("<H", len(payload))
        return hdr + bytes((sum(hdr) & 0xFF,)) + payload

    def _write(self, data):
        data = self._pad64(data)
        for i in range(0, len(data), 64):
            self.out.write(data[i:i + 64])

    def _read(self, timeout_ms):
        """Read one complete pack (a0/b0/b2: flag, u16 length, checksum, body),
        even if the device splits it over several bulk transfers."""
        buf = bytes(self.inp.read(20000, timeout=timeout_ms))
        if len(buf) >= 3:
            need = 4 + struct.unpack_from("<H", buf, 1)[0]
            while len(buf) < need:
                buf += bytes(self.inp.read(20000, timeout=timeout_ms))
        return buf

    def send(self, cmd, data, replies, timeout_ms=10000):
        self._write(self._cmd(cmd, data))
        return [self._read(timeout_ms) for _ in range(replies)]

    @staticmethod
    def payload(rsp):
        """Payload of an 0xa0 reply: strip 4-byte pack header, cmd, len, cksum."""
        if not rsp or rsp[0] != 0xA0:
            raise RuntimeError("unexpected reply %s" % rsp[:16].hex())
        ln = struct.unpack_from("<H", rsp, 5)[0]
        return rsp[7:7 + ln - 1]

    # ---- bring-up ----
    def init(self, write_zero_psk):
        info = {}
        self.send(0x00, b"\0\0\0\0", 1)
        _, fw = self.send(0xA8, b"\0\0", 2)
        info["firmware"] = self.payload(fw).rstrip(b"\0").decode(errors="replace")
        rsps = self.send(0xE4, bytes.fromhex("070002bb00000000"), 2)
        psk_hash = self.payload(rsps[-1])[9:]
        info["psk_hash"] = psk_hash.hex()
        if psk_hash != ZERO_PSK_HASH:
            if not write_zero_psk:
                sys.exit("device PSK hash %s is not the zero-PSK hash; refusing to "
                         "continue (pass --write-zero-psk to provision it)" % psk_hash.hex())
            self.send(0xE0, bytes.fromhex("020001bb" "0e000000" "4141414142424242434343434444"), 2)
            self.send(0xE0, bytes.fromhex("030001bb60000000") + ZERO_PSK_WB, 2)
            info["psk_written"] = True
        self.send(0xA2, bytes.fromhex("0514"), 2)                      # reset
        _, chip = self.send(0x82, bytes.fromhex("0000000400"), 2)       # chip id
        c = self.payload(chip)
        info["chip_id_bytes"] = c.hex()
        if len(c) >= 4:
            info["chip_id"] = "0x%08x" % (c[0] * 0x100 + c[1] + c[2] * 0x1000000 + c[3] * 0x10000)
        self.send(0x00, b"\0\0\0\0", 1)
        _, otp = self.send(0xA6, b"\0\0", 2)
        info["otp"] = self.payload(otp).hex()
        self.send(0x90, CONFIG, 2)                                     # upload config
        info["config"] = CONFIG.hex()
        self.start_tls()
        info["mcu_state_after_tls"] = self.mcu_state()
        self.log("init", **info)
        return info

    def mcu_state(self):
        """Raw MCU state (cmd 0xae, payload 0x55); logged for diagnosis."""
        rsps = self.send(0xAE, b"\x55", 2)
        try:
            return self.payload(rsps[-1]).hex()
        except RuntimeError:
            return "raw:" + rsps[-1][:32].hex()

    def start_tls(self):
        """TLS 1.2 PSK handshake, device = client, in-process (gx_tls)."""
        self.tls = PskServer(b"\0" * 32, log=self.log)
        rsps = self.send(0xD0, b"\0\0", 2)
        hello = rsps[-1]
        if hello[0] != 0xB0:
            raise RuntimeError("expected ClientHello in 0xb0 pack, got %s" % hello[:8].hex())
        for rec in self.tls.server_hello(hello[4:]):
            self._send_tls(rec)

        stream = b""
        for _ in range(8):  # CKE, CCS, Finished: however many packs they take
            pack = self._read(2000)
            if pack[0] != 0xB0:
                raise RuntimeError("unexpected pack during handshake: %s" % pack[:8].hex())
            stream += pack[4:]
            if self.tls.client_flight(stream):
                break
        else:
            raise RuntimeError("incomplete client handshake flight")
        for rec in self.tls.server_finished():
            self._send_tls(rec)
        self.log("tls-established")

    def _send_tls(self, payload):
        hdr = bytes((0xB0,)) + struct.pack("<H", len(payload))
        self._write(hdr + bytes((sum(hdr) & 0xFF,)) + payload)

    def close(self):
        usb.util.dispose_resources(self.dev)

    # ---- FDT ----
    @staticmethod
    def fdt_payload(op, thresholds):
        return bytes((op, 0x01)) + b"".join(bytes((0x80, t)) for t in thresholds)

    @staticmethod
    def parse_fdt(payload):
        irq, touch = struct.unpack_from("<HH", payload, 0)
        vals = struct.unpack_from("<12H", payload, 4)
        return irq, touch, [v >> 1 for v in vals]

    def fdt(self, cmd, op, thresholds, timeout_ms):
        _, ev = self.send(cmd, self.fdt_payload(op, thresholds), 2, timeout_ms)
        pl = self.payload(ev)
        irq, touch, vals = self.parse_fdt(pl)
        self.log("fdt", cmd="%02x" % cmd, sent=list(thresholds), irq="0x%04x" % irq,
                 touch="0x%03x" % touch, values=vals)
        return irq, touch, vals

    # ---- image ----
    def image(self, label, data=b"\x01\x00"):
        rsps = self.send(0x20, data, 2)
        rsp = rsps[-1]
        info = dict(label=label, request=data.hex(),
                    lengths=[len(r) for r in rsps], head=rsp[:16].hex())
        if not rsp or rsp[0] != 0xB2:
            # e.g. an 0xa0 message with cmd 0xd0 = "MCU wants a TLS connection"
            cmd = "%02x" % rsp[4] if len(rsp) > 4 and rsp[0] == 0xA0 else "?"
            self.log("image-error", reply_cmd=cmd, **info)
            raise RuntimeError("image request answered with %s (cmd %s), not an "
                               "encrypted image" % (rsp[:8].hex(), cmd))
        try:
            # 0xb2 pack: 4-byte header + 9-byte prefix, then TLS record(s)
            raw = self.tls.decrypt_appdata(rsp[13:])
        except TlsError as e:
            self.log("image-error", tls_error=str(e), rx_seq=self.tls.rx_seq, **info)
            raise
        info["plain_bytes"] = len(raw)
        if len(raw) != IMAGE_BYTES:
            self.log("image-error", **info)
            raise RuntimeError("got %d decrypted bytes, expected %d" % (len(raw), IMAGE_BYTES))
        self.log("image", b2_prefix=rsp[4:13].hex(), **info)
        return raw


def unpack(raw):
    out = []
    for i in range(0, PIXELS // 4 * 6, 6):
        c = raw[i:i + 6]
        out += [((c[0] & 0xF) << 8) | c[1], (c[3] << 4) | (c[0] >> 4),
                ((c[5] & 0xF) << 8) | c[2], (c[4] << 4) | (c[5] >> 4)]
    return out


def save_frame(path, raw):
    with open(path, "wb") as f:
        f.write(struct.pack("<%dH" % PIXELS, *unpack(raw)))


IRQ_TOUCH = 0x0002     # finger down / touched
IRQ_BASELINE = 0x0080  # MCU reports baseline drift on a 0x32 wait (no finger)
IRQ_UP = 0x0200        # finger lifted
MAX_REARMS = 50


def clamp(values):
    return [max(1, min(0xFE, v)) for v in values]


def wait_finger_down(dev, down):
    """Arm 0x32 until a real finger-down. Returns the (possibly refreshed) table."""
    for _ in range(MAX_REARMS):
        irq, _touch, vals = dev.fdt(CMD_FDT_DOWN, OP_FDT_DOWN, down, 60000)
        if irq & IRQ_TOUCH:
            return down
        if irq & IRQ_BASELINE:
            # Same as the OEM host (goodix-27c6-5125 manual §6.4): refresh DOWN
            # from this event's own values and re-arm.
            down = clamp(vals)
            dev.log("fdt-rearm", reason="baseline-refresh", down=down)
            continue
        dev.log("fdt-rearm", reason="unexpected-irq", irq="0x%04x" % irq)
    raise RuntimeError("no finger-down after %d re-arms" % MAX_REARMS)


def wait_finger_up(dev, up):
    """Arm 0x34 until the finger lifts. Returns the next DOWN table."""
    for _ in range(MAX_REARMS):
        irq, _touch, vals = dev.fdt(CMD_FDT_UP, OP_FDT_UP, up, 60000)
        if irq & IRQ_UP:
            return clamp(vals)
        dev.log("fdt-rearm", reason="up-unexpected-irq", irq="0x%04x" % irq)
    raise RuntimeError("no finger-up after %d re-arms" % MAX_REARMS)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--finger", required=True, help="label, e.g. right-index")
    ap.add_argument("--touches", type=int, default=20)
    ap.add_argument("--setup-frames", type=int, default=3)
    ap.add_argument("--new-setup", action="store_true",
                    help="replace setup.raw with this run's no-finger frame")
    ap.add_argument("--no-usb-reset", action="store_true",
                    help="skip the USB reset at start")
    ap.add_argument("--try-txoff", action="store_true",
                    help="also send 0x20 '81 00' once and save the reply")
    ap.add_argument("--write-zero-psk", action="store_true",
                    help="provision the zero PSK if the device has another one")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    run_id = int(time.time())
    events = open(os.path.join(args.out, "events.jsonl"), "a")
    dev = Device(events, usb_reset=not args.no_usb_reset)
    try:
        info = dev.init(args.write_zero_psk)
        info.update(finger=args.finger, fdt_up_delta=FDT_UP_DELTA, run_id=run_id)
        with open(os.path.join(args.out, "session-%d.json" % run_id), "w") as f:
            json.dump(info, f, indent=1)
        print("firmware %s  chip %s  psk ok  mcu %s" % (
            info["firmware"], info.get("chip_id"), info["mcu_state_after_tls"]))

        # Baseline: one-shot FDT reading with no finger -> first down table.
        input("Keep the sensor CLEAR (no finger), then press Enter...")
        _, _, base = dev.fdt(CMD_FDT_MODE, OP_FDT_MODE, [0x80] * 12, 2000)
        down = clamp(base)

        # Fresh no-finger frames every run (same sequence as the first, working
        # session: images before any FDT wait). setup.raw keeps the first run's
        # frame unless --new-setup.
        for i in range(args.setup_frames):
            raw = dev.image("setup-%d-%02d" % (run_id, i))
            save_frame(os.path.join(args.out, "setup-%d-%02d.raw" % (run_id, i)), raw)
        setup_path = os.path.join(args.out, "setup.raw")
        if args.new_setup or not os.path.exists(setup_path):
            save_frame(setup_path, raw)
        print("saved %d no-finger frames" % args.setup_frames)

        if args.try_txoff:
            try:
                rsps = dev.send(0x20, b"\x81\x00", 2, 3000)
                with open(os.path.join(args.out, "txoff.bin"), "wb") as f:
                    for r in rsps:
                        f.write(r)
                dev.log("txoff", lengths=[len(r) for r in rsps],
                        first=rsps[-1][:16].hex())
                print("TX-off reply lengths:", [len(r) for r in rsps])
            except Exception as e:  # experiment only
                dev.log("txoff", error=str(e))
                print("TX-off experiment failed:", e)
            # The TLS stream may now be out of sync; reconnect cleanly.
            dev.close()
            sys.exit("TX-off test done; re-run without --try-txoff to capture touches")

        existing = [n for n in os.listdir(args.out)
                    if n.startswith(args.finger + "-") and n.endswith(".raw")]
        start = len(existing)
        for t in range(start, start + args.touches):
            print("[%s %d/%d] place finger..." % (args.finger, t - start + 1, args.touches),
                  end="", flush=True)
            down = wait_finger_down(dev, down)
            raw = dev.image("%s-%02d" % (args.finger, t))
            save_frame(os.path.join(args.out, "%s-%02d.raw" % (args.finger, t)), raw)

            irq, touch, pressed = dev.fdt(CMD_FDT_MODE, OP_FDT_MODE, down, 2000)
            if not irq & IRQ_TOUCH or touch == 0:
                # Finger already gone (irq 0x0180, no touch bits): nothing to
                # wait for; this reading is a fresh baseline.
                down = clamp(pressed)
                print(" captured (finger already lifted)")
                continue
            print(" captured, lift finger...", end="", flush=True)
            down = wait_finger_up(dev, clamp(v + FDT_UP_DELTA for v in pressed))
            print(" ok")
    finally:
        dev.close()
        events.close()


if __name__ == "__main__":
    main()
