"""Minimal TLS 1.2 PSK server for the Goodix 55b4 (cipher 0x00AE,
TLS_PSK_WITH_AES_128_CBC_SHA256), run in-process instead of `openssl s_server`.

The device is the TLS client. We only need: ServerHello + ServerHelloDone,
key derivation from the PSK, verifying the client Finished, sending our
Finished, and decrypting the device's application-data records (images).
No extensions are negotiated (no extended master secret, no encrypt-then-mac).
"""
import hashlib
import hmac
import os
import struct

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

CT_CCS, CT_ALERT, CT_HANDSHAKE, CT_APPDATA = 20, 21, 22, 23
HS_CLIENT_HELLO, HS_SERVER_HELLO, HS_SERVER_HELLO_DONE = 1, 2, 14
HS_CLIENT_KEY_EXCHANGE, HS_FINISHED = 16, 20
SUITE = 0x00AE
VERSION = b"\x03\x03"


class TlsError(Exception):
    pass


def prf(secret, label, seed, n):
    """TLS 1.2 PRF with SHA-256."""
    seed = label + seed
    out, a = b"", seed
    while len(out) < n:
        a = hmac.new(secret, a, hashlib.sha256).digest()
        out += hmac.new(secret, a + seed, hashlib.sha256).digest()
    return out[:n]


def split_records(buf):
    """Split a byte stream into (type, body) records; returns (records, rest)."""
    recs = []
    while len(buf) >= 5:
        ln = struct.unpack_from(">H", buf, 3)[0]
        if len(buf) < 5 + ln:
            break
        recs.append((buf[0], buf[5:5 + ln]))
        buf = buf[5 + ln:]
    return recs, buf


def record(ctype, body):
    return bytes((ctype,)) + VERSION + struct.pack(">H", len(body)) + body


def handshake(htype, body):
    return bytes((htype,)) + struct.pack(">I", len(body))[1:] + body


class PskServer:
    def __init__(self, psk, log=None):  # log(kind, **fields)
        self.psk = psk
        self.log = log or (lambda *a, **k: None)
        self.transcript = b""
        self.client_random = b""
        self.server_random = b""
        self.master = b""
        self.keys = {}
        self.rx_seq = 0   # device -> host (client write)
        self.tx_seq = 0   # host -> device (server write)

    # ---- handshake ----
    def server_hello(self, client_hello_record):
        """Returns the server flight as a list of records (send one per pack)."""
        recs, _ = split_records(client_hello_record)
        if not recs or recs[0][0] != CT_HANDSHAKE or recs[0][1][0] != HS_CLIENT_HELLO:
            raise TlsError("expected ClientHello, got %s" % client_hello_record[:16].hex())
        ch = recs[0][1]
        self.transcript += ch
        body = ch[4:]
        self.client_random = body[2:34]
        sid_len = body[34]
        p = 35 + sid_len
        n = struct.unpack_from(">H", body, p)[0]
        suites = [struct.unpack_from(">H", body, p + 2 + i)[0] for i in range(0, n, 2)]
        self.log("tls-client-hello", suites=["%04x" % s for s in suites],
                 length=len(ch))  # extensions logged below
        if SUITE not in suites:
            raise TlsError("device does not offer 0x00AE: %s" % suites)
        # Secure renegotiation (RFC 5746): the client signals support with the
        # SCSV 0x00FF (the 55b4 does) or the extension; answer with an empty
        # renegotiation_info, as openssl s_server did.
        p += 2 + n
        p += 1 + body[p]  # compression methods
        ext_types = []
        if len(body) >= p + 2:
            end = p + 2 + struct.unpack_from(">H", body, p)[0]
            q = p + 2
            while q + 4 <= end:
                etype, elen = struct.unpack_from(">HH", body, q)
                ext_types.append(etype)
                q += 4 + elen
        offered_reneg = 0x00FF in suites or 0xFF01 in ext_types

        self.server_random = os.urandom(32)
        ext = b""
        if offered_reneg:
            ext = b"\xff\x01\x00\x01\x00"
            ext = struct.pack(">H", len(ext)) + ext
        # Same shape as the Windows host (55b4 capture, frames 140/144): a
        # 32-byte session ID, and ServerHello / ServerHelloDone as separate
        # records, each sent in its own USB pack. The sensor's TLS stack
        # does not answer a combined record.
        session_id = os.urandom(32)
        sh = handshake(HS_SERVER_HELLO, VERSION + self.server_random +
                       bytes((len(session_id),)) + session_id +
                       struct.pack(">H", SUITE) + b"\x00" + ext)
        shd = handshake(HS_SERVER_HELLO_DONE, b"")
        self.transcript += sh + shd
        return [record(CT_HANDSHAKE, sh), record(CT_HANDSHAKE, shd)]

    def client_flight(self, stream):
        """Consume ClientKeyExchange, ChangeCipherSpec, Finished from the device.
        `stream` is all TLS bytes received so far; returns True when complete."""
        recs, rest = split_records(stream)
        if rest:
            return False
        types = [t for t, _ in recs]
        self.log("tls-client-flight", records=types,
                 lengths=[len(b) for _, b in recs])
        if CT_ALERT in types:
            raise TlsError("device sent alert %s" % recs[types.index(CT_ALERT)][1].hex())
        if types[-1:] != [CT_HANDSHAKE] or CT_CCS not in types:
            return False

        # ClientKeyExchange (plaintext handshake record(s) before CCS)
        ccs_at = types.index(CT_CCS)
        for t, body in recs[:ccs_at]:
            if t == CT_HANDSHAKE:
                self.transcript += body
        self._derive_keys()

        # Finished (encrypted, first record after CCS)
        fin = self.decrypt(CT_HANDSHAKE, recs[ccs_at + 1][1])
        expect = prf(self.master, b"client finished",
                     hashlib.sha256(self.transcript).digest(), 12)
        if fin[:4] != b"\x14\x00\x00\x0c" or fin[4:] != expect:
            raise TlsError("client Finished mismatch: wrong PSK or transcript")
        self.transcript += fin
        return True

    def server_finished(self):
        """Returns [ChangeCipherSpec, Finished] records (send one per pack)."""
        verify = prf(self.master, b"server finished",
                     hashlib.sha256(self.transcript).digest(), 12)
        fin = handshake(HS_FINISHED, verify)
        # One record per pack, as the Windows host does (frames 152/154).
        return [record(CT_CCS, b"\x01"),
                record(CT_HANDSHAKE, self.encrypt(CT_HANDSHAKE, fin))]

    def _derive_keys(self):
        n = len(self.psk)
        premaster = struct.pack(">H", n) + b"\x00" * n + struct.pack(">H", n) + self.psk
        self.master = prf(premaster, b"master secret",
                          self.client_random + self.server_random, 48)
        kb = prf(self.master, b"key expansion",
                 self.server_random + self.client_random, 2 * 32 + 2 * 16)
        self.keys = dict(c_mac=kb[0:32], s_mac=kb[32:64],
                         c_key=kb[64:80], s_key=kb[80:96])

    # ---- record protection (MAC-then-encrypt, explicit IV) ----
    def _mac(self, key, seq, ctype, plain):
        hdr = struct.pack(">Q", seq) + bytes((ctype,)) + VERSION + struct.pack(">H", len(plain))
        return hmac.new(key, hdr + plain, hashlib.sha256).digest()

    def decrypt(self, ctype, body):
        if len(body) < 16 + 32 + 16 or (len(body) - 16) % 16:
            raise TlsError("bad record length %d" % len(body))
        iv, ct = body[:16], body[16:]
        d = Cipher(algorithms.AES(self.keys["c_key"]), modes.CBC(iv)).decryptor()
        pt = d.update(ct) + d.finalize()
        pad = pt[-1]
        if pad + 1 > len(pt) or pt[-pad - 1:] != bytes((pad,)) * (pad + 1):
            raise TlsError("bad padding (wrong keys?)")
        pt = pt[:-pad - 1]
        plain, mac = pt[:-32], pt[-32:]
        if not hmac.compare_digest(mac, self._mac(self.keys["c_mac"], self.rx_seq, ctype, plain)):
            raise TlsError("bad record MAC at seq %d" % self.rx_seq)
        self.rx_seq += 1
        return plain

    def encrypt(self, ctype, plain):
        mac = self._mac(self.keys["s_mac"], self.tx_seq, ctype, plain)
        self.tx_seq += 1
        data = plain + mac
        pad = 15 - len(data) % 16
        data += bytes((pad,)) * (pad + 1)
        iv = os.urandom(16)
        e = Cipher(algorithms.AES(self.keys["s_key"]), modes.CBC(iv)).encryptor()
        return iv + e.update(data) + e.finalize()

    def decrypt_appdata(self, stream):
        """Decrypt every application-data record in `stream`; return plaintext."""
        recs, rest = split_records(stream)
        if rest:
            raise TlsError("truncated TLS record (%d stray bytes)" % len(rest))
        out = b""
        for t, body in recs:
            if t == CT_ALERT:
                raise TlsError("device sent alert %s" % body.hex())
            if t != CT_APPDATA:
                raise TlsError("unexpected record type %d" % t)
            out += self.decrypt(CT_APPDATA, body)
        return out
