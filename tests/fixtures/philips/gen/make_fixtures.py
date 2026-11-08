#!/usr/bin/env python3
"""Synthetic Philips DreamStation 2 cards for hms-cpap's tests.

Built from the parser's format document (docs/PHILIPS_FORMAT.md in
hms-cpapdash-parser, section 9) and the same chunk writer as the parser's own
fixture generator (tests/fixtures/philips/gen/make_fixtures.py there). No
real card, no patient data; the PROP.BIN key fields are obviously fake.

    python3 make_fixtures.py
        writes ../ds2_live/ and ../ds2_encrypted/ (the test fixtures)

    python3 make_fixtures.py --night <out dir> <minutes>
        writes a longer plain card of one session, for a hand-run drill:
        P0/<session>.B05 holds every 240 s block, and the session's .B01 and
        .B02 are written beside it as <session>.B01.closed / .B02.closed, to
        be renamed in when the drill ends the session.

ds2_live/P-SERIES/PXLIVE01/ holds session 0x40: four 240 s flow blocks in its
.B05 (one session of 960 s), and its .B01 and .B02. A test takes the first k
blocks of the .B05 alone to stand for a session being recorded, then adds the
.B01 and .B02 to end it. Every block has the same length, so block k ends at
k * len(.B05) / 4.

Output is byte-for-byte deterministic.
"""

import math
import os
import struct
import sys

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")

BASE_TS = 1750000000          # fixed epoch (doc 3.1 timestamp, UTC)
SESSION = 0x40
BLOCK_S = 240                 # one .B05 chunk: 240 one-second intervals (doc 9.9)
RATE = 5                      # samples per second, interleave 5 (doc 9.9)


def checksum8(data: bytes) -> int:
    return sum(data) & 0xFF


# ── DataFormatVersion 3 container (doc 9.2) ─────────────────────────────────

def _crc32_table():
    table = []
    for i in range(256):
        c = i << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
        table.append(c)
    return table


_CRC32_TABLE = _crc32_table()


def crc32_wordfed(data: bytes) -> int:
    """MSB-first CRC-32, poly 0x04C11DB7, init 0xFFFFFFFF, no final xor, each byte
    fed as the word 00 00 00 b (doc 9.2)."""
    crc = 0xFFFFFFFF
    for b in data:
        for byte in (0, 0, 0, b):
            crc = ((crc << 8) & 0xFFFFFFFF) ^ _CRC32_TABLE[((crc >> 24) ^ byte) & 0xFF]
    return crc


def chunk_v3(ext: int, session: int, ts: int, payload: bytes, *, sizes=None,
             signal_info=b"", family=0, family_version=6) -> bytes:
    """One v3 chunk. FileType 0 when `sizes` (code -> record size) is given,
    FileType 1 when `signal_info` is given (doc 9.2)."""
    file_type = 1 if signal_info else 0
    extra = signal_info
    if sizes is not None:
        extra = bytes([len(sizes)]) + b"".join(bytes([c, s]) for c, s in sizes.items())
    fixed = struct.pack("<BBBB", file_type, family, family_version, ext) + \
        struct.pack("<II", session, ts)
    block_length = 3 + len(fixed) + len(extra) + 1 + len(payload) + 4
    header = struct.pack("<BH", 3, block_length) + fixed + extra
    return header + bytes([checksum8(header)]) + payload + struct.pack("<I", crc32_wordfed(payload))


def signal_header_v3(records: int, secs_per_interval: int, signals) -> bytes:
    out = struct.pack("<HBB", records, secs_per_interval, len(signals))
    for kind, interleave, extra in signals:
        out += struct.pack("<BHB", kind, interleave, extra)
    return out + b"\x00"


# ── .B05 flow (doc 6, 9.9): signed 8-bit L/min ──────────────────────────────

def flow_block(block: int) -> bytes:
    """240 s of breathing. The rate and depth drift slowly from block to block
    so a chart of several blocks is not one flat pattern. The phase starts
    inside inspiration: the breath detector pairs cycles from a positive first
    sample."""
    bpm = 14.0 + 2.0 * math.sin(block / 3.0)
    amp = 36.0 + 6.0 * math.cos(block / 2.0)
    period = 60.0 / bpm
    out = bytearray()
    for i in range(BLOCK_S * RATE):
        t = (block * BLOCK_S) + i / RATE
        v = int(round(amp * math.sin(2.0 * math.pi * t / period + 0.1)))
        out.append(max(-128, min(127, v)) & 0xFF)
    return bytes(out)


def waveform(first_ts: int, blocks: int) -> bytes:
    sig = signal_header_v3(BLOCK_S, 1, [(0, RATE, 8)])
    return b"".join(chunk_v3(5, SESSION, first_ts + b * BLOCK_S, flow_block(b), signal_info=sig)
                    for b in range(blocks))


# ── .B01 summary (doc 9.5 to 9.7) ───────────────────────────────────────────

V3_SUMMARY_SIZES = {0x00: 1, 0x01: 0, 0x02: 9, 0x03: 4, 0x04: 2, 0x05: 4, 0x08: 31}
V3_EVENT_SIZES = {0x01: 3, 0x06: 3, 0x11: 5}


def settings_v3() -> bytes:
    """Doc 9.7 nested (code, length, value): auto CPAP 5.0 to 12.0."""
    triples = [(0x00, [2]), (0x0d, [50, 120])]
    return b"".join(bytes([c, len(v)] + v) for c, v in triples)


def stats_v3(oa=0) -> bytes:
    """Doc 9.6: fifteen u16 words + one byte."""
    words = [0, oa, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 10000, 70, 0x0e00]
    return struct.pack("<15H", *words) + bytes([50])


def rec(code: int, body: bytes) -> bytes:
    return bytes([code]) + body


def summary(first_on: int, length: int, oa: int) -> bytes:
    """Equipment on, settings, one mask-on period, equipment off."""
    s = settings_v3()
    sizes = dict(V3_SUMMARY_SIZES)
    sizes[0x01] = len(s)
    p = rec(0x00, b"\x01") + rec(0x01, s)
    p += rec(0x03, struct.pack("<H", first_on) + bytes([0x4c, 0x6c]))
    p += rec(0x05, bytes([50, 90, 80, 70]))
    p += rec(0x04, struct.pack("<H", length))
    p += rec(0x08, stats_v3(oa=oa))
    p += rec(0x02, struct.pack("<H", 30) + bytes(7))
    return chunk_v3(1, SESSION, BASE_TS, p, sizes=sizes)


# ── .B02 events (doc 9.8) ───────────────────────────────────────────────────

def ev3(code: int, delta: int, *fields) -> bytes:
    return bytes([code]) + struct.pack("<H", delta) + bytes(fields)


def events(length: int) -> bytes:
    """Pressure 5.0 at mask-on, then a two-minute statistics record (0x11:
    total leak, snore count, pressure x10) every 120 s, the pressure climbing
    from 6.0 and settling; one obstructive apnea half way."""
    recs = [ev3(0x01, 60, 50)]
    n = length // 120
    for i in range(n):
        pressure = 60 + min(i, 8) + (i % 3)            # 6.0 .. 7.0 cmH2O
        recs.append(ev3(0x11, 120, 20 + (i % 5), 0, pressure))
        if i == n // 2:
            recs.append(ev3(0x06, 0, 12))
    return chunk_v3(2, SESSION, BASE_TS, b"".join(recs), sizes=V3_EVENT_SIZES)


def prop_bin(serial: str) -> str:
    keys = [("CF", "6"), ("SN", serial), ("MN", "520X110C"), ("PT", "0x8B"), ("DF", "0"),
            ("DFV", "3"), ("F", "0"), ("FV", "6"), ("SV", "V1.0.0.0000"), ("FD", str(BASE_TS)),
            ("LD", str(BASE_TS + 86400)), ("FN", "0"), ("SID", "FAKE-SID"),
            ("BK", "FAKE-BK"), ("SK", "FAKE-SK"), ("EK", "FAKE-EK"), ("DK", "FAKE-DK")]
    return "".join(f"{k}={v}\r\n" for k, v in keys)


def pseudo_random(n: int, seed: int) -> bytes:
    """Deterministic noise standing in for encrypted bytes (LCG)."""
    out = bytearray()
    x = seed
    for _ in range(n):
        x = (1103515245 * x + 12345) & 0x7FFFFFFF
        out.append((x >> 16) & 0xFF)
    return bytes(out)


def write(root: str, rel: str, data) -> None:
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if isinstance(data, str):
        data = data.encode("ascii")   # PROP.BIN keeps its CR LF on every platform
    with open(path, "wb") as f:
        f.write(data)
    print(f"wrote {os.path.relpath(path, root)} ({len(data)} bytes)")


def card(root: str, serial: str, blocks: int, *, closed_suffix: str) -> None:
    folder = f"P-SERIES/{serial}"
    length = blocks * BLOCK_S
    stem = f"{SESSION:08X}"
    write(root, f"{folder}/PROP.BIN", prop_bin(serial))
    write(root, f"{folder}/P0/{stem}.B05", waveform(BASE_TS + 60, blocks))
    write(root, f"{folder}/P0/{stem}.B01{closed_suffix}", summary(60, length, oa=1))
    write(root, f"{folder}/P0/{stem}.B02{closed_suffix}", events(length))


def main() -> None:
    if len(sys.argv) == 4 and sys.argv[1] == "--night":
        card(sys.argv[2], "PXLIVE02", int(sys.argv[3]) * 60 // BLOCK_S, closed_suffix=".closed")
        return

    card(os.path.join(OUT, "ds2_live"), "PXLIVE01", 4, closed_suffix="")

    # An encrypted card: a PROP.BIN that is not key=value text, and a session
    # file of noise behind the container's opening bytes.
    enc = os.path.join(OUT, "ds2_encrypted")
    head = bytes([0x0D, 0x00, 0x01, 0x00, 0x01, 0x00])
    write(enc, "P-SERIES/PXENC001/PROP.BIN", head + pseudo_random(400, 7))
    write(enc, "P-SERIES/PXENC001/P0/00000041.B05", head + pseudo_random(1400, 11))


if __name__ == "__main__":
    main()
