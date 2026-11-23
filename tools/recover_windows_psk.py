#!/usr/bin/env python3
"""Recover the PSK a Goodix 27c6:55b4/5584 was paired with by Windows.

Use this instead of --write-zero-psk to keep the sensor's Windows pairing:
nothing is written to the sensor or to Windows, so the sensor keeps working
in both. The driver reads the result from /etc/goodix55b4/psk.hex.

Windows stores its PSK on the sensor, sealed with DPAPI by the Windows
Biometric Service (the SYSTEM account). This reads the sealed key and its
PMK hash from the sensor, then unseals the key offline from the Windows
installation's files: the SYSTEM and SECURITY registry hives and the SYSTEM
account's DPAPI master key. Windows does not have to run.

Run as root (or with udev access), with fprintd stopped and the Windows
partition mounted read-only:
  sudo systemctl stop fprintd
  sudo mount -o ro /dev/nvme0n1p3 /mnt/win
  sudo python3 recover_windows_psk.py --windows /mnt/win
  sudo umount /mnt/win
  sudo systemctl start fprintd

Requires python-pyusb and impacket.
"""
import argparse
import os
import struct
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_dataset_55b4 import ZERO_PSK_HASH, Device  # noqa: E402

try:
    from impacket.dpapi import DPAPI_BLOB, MasterKey, MasterKeyFile
    from impacket.examples.secretsdump import LocalOperations, LSASecrets
except ImportError:
    sys.exit("impacket is required (Arch: impacket, Debian/Fedora: python3-impacket, "
             "or pip install impacket)")

CMD_PSK_READ = 0xE4
TLV_PSK_BLOB = 0xBB010002  # the PSK, sealed with DPAPI by the Windows driver
TLV_PSK_HASH = 0xBB020007  # PMK hash of the PSK (the driver checks it)
PSK_LEN = 32
# Every DPAPI blob starts with version 1 and the DPAPI provider GUID
# {df9d8cd0-1501-11d1-8c7a-00c04fc297eb}; the master key GUID follows at 24.
DPAPI_PREFIX = bytes.fromhex("01000000d08c9ddf0115d1118c7a00c04fc297eb")
DPAPI_MASTERKEY_GUID = slice(24, 40)
DEFAULT_OUT = "/etc/goodix55b4/psk.hex"


def read_tlv(dev, flags):
    """Read one TLV with PRESET_PSK_READ (read-only). Reply payload: status,
    u32 flags, u32 length, data."""
    rsp = dev.send(CMD_PSK_READ, struct.pack("<II", flags, 0), 2)[-1]
    p = Device.payload(rsp)
    if len(p) < 9:
        sys.exit("PSK read 0x%08x: short reply %s" % (flags, p.hex()))
    rflags, length = struct.unpack_from("<II", p, 1)
    if p[0] != 0 or rflags != flags or len(p) < 9 + length:
        sys.exit("PSK read 0x%08x failed (status 0x%02x, flags 0x%08x, %d of %d bytes)"
                 % (flags, p[0], rflags, len(p) - 9, length))
    return p[9:9 + length]


def find(root, rel):
    """Case-insensitive lookup of rel ('a/b/**/c') under root (Windows paths
    are case-insensitive, Linux mounts usually are not)."""
    paths = [root]
    for part in rel.split("/"):
        nxt = []
        for base in paths:
            if part == "**":
                nxt.extend(d for d, _, _ in os.walk(base))
                continue
            try:
                nxt.extend(os.path.join(base, e) for e in os.listdir(base)
                           if e.lower() == part.lower())
            except (NotADirectoryError, FileNotFoundError):
                pass
        paths = nxt
    return paths[0] if paths else None


def dpapi_system_keys(system_hive, security_hive):
    keys = {}

    def on_secret(_type, secret):
        if secret.startswith("dpapi_machinekey:"):
            machine, user = secret.splitlines()
            keys["machine"] = bytes.fromhex(machine.split(":")[1].strip()[2:])
            keys["user"] = bytes.fromhex(user.split(":")[1].strip()[2:])

    boot_key = LocalOperations(system_hive).getBootKey()
    LSASecrets(security_hive, boot_key, None, isRemote=False, history=False,
               perSecretCallback=on_secret).dumpSecrets()
    if not keys:
        sys.exit("no DPAPI_SYSTEM secret in %s; is it from the same installation as %s?"
                 % (security_hive, system_hive))
    return keys


def unseal(blob, windows):
    guid = str(uuid.UUID(bytes_le=blob[DPAPI_MASTERKEY_GUID]))
    system = find(windows, "Windows/System32/config/SYSTEM")
    security = find(windows, "Windows/System32/config/SECURITY")
    mk_path = find(windows, "Windows/System32/Microsoft/Protect/S-1-5-18/**/" + guid)
    for name, path in (("SYSTEM hive", system), ("SECURITY hive", security),
                       ("DPAPI master key " + guid, mk_path)):
        if path is None:
            sys.exit("%s not found under %s. Pass the root of the Windows installation "
                     "that paired the sensor (where Windows/ is)." % (name, windows))

    keys = dpapi_system_keys(system, security)
    data = open(mk_path, "rb").read()
    mkf = MasterKeyFile(data)
    mk = MasterKey(data[len(mkf):][:mkf["MasterKeyLen"]])
    key = mk.decrypt(keys["user"]) or mk.decrypt(keys["machine"])
    if not key:
        sys.exit("could not decrypt DPAPI master key %s with this installation's "
                 "DPAPI_SYSTEM keys" % guid)
    psk = DPAPI_BLOB(blob).decrypt(key)
    if psk is None or len(psk) != PSK_LEN:
        sys.exit("the sealed PSK did not decrypt to %d bytes" % PSK_LEN)
    return psk


def write_key_file(path, psk, psk_hash, force):
    if os.path.exists(path) and not force:
        sys.exit("%s already exists; pass --force to replace it" % path)
    os.makedirs(os.path.dirname(path) or ".", mode=0o755, exist_ok=True)
    tmp = path + ".tmp"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write("%s\n%s\n" % (psk.hex(), psk_hash.hex()))
    os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--windows", required=True,
                    help="root of the Windows installation that paired the sensor, "
                         "mounted read-only")
    ap.add_argument("--out", default=DEFAULT_OUT,
                    help="key file to write (default %(default)s)")
    ap.add_argument("--force", action="store_true", help="replace an existing key file")
    ap.add_argument("--no-usb-reset", action="store_true",
                    help="skip the USB reset before talking to the sensor")
    args = ap.parse_args()
    if os.path.exists(args.out) and not args.force:
        sys.exit("%s already exists; pass --force to replace it" % args.out)

    dev = Device(open(os.devnull, "w"), usb_reset=not args.no_usb_reset)
    try:
        dev.send(0x00, b"\0\0\0\0", 1)  # NOP
        psk_hash = read_tlv(dev, TLV_PSK_HASH)
        blob = read_tlv(dev, TLV_PSK_BLOB)
    finally:
        dev.close()

    if psk_hash == ZERO_PSK_HASH:
        sys.exit("the sensor already uses the zero PSK; no key file is needed")
    if len(psk_hash) != PSK_LEN or not blob.startswith(DPAPI_PREFIX):
        sys.exit("the sensor holds no Windows-sealed PSK (hash %d bytes, blob %d bytes); "
                 "use --write-zero-psk with capture_dataset_55b4.py instead"
                 % (len(psk_hash), len(blob)))

    psk = unseal(blob, args.windows)
    write_key_file(args.out, psk, psk_hash, args.force)
    print("wrote %s; restart fprintd to use it" % args.out)


if __name__ == "__main__":
    main()
