#!/usr/bin/env python3
"""Deterministic synthetic Lowenstein Prisma fixture generator.

WHY THIS EXISTS. The fixtures here used to be a real night off a real machine:
884 KB of one person's breathing recorded 14.05.2026 23:04:33, next to a
device.xml carrying that machine's DeviceSerialNumber and MainboardSerialNumber.
No patient name (the WMEDF patient field holds the device's own "Patient Name"
placeholder), but a hardware serial pair plus a dated overnight recording is
health data tied to a specific machine, and this repository is public.

Everything below is generated from the format description alone. No real
recording, no real serial, no patient data. Output is byte-for-byte
deterministic, so a reviewer can regenerate and diff rather than trust it.

    python3 make_fixtures.py      # writes into the parent directory

WHAT THE TESTS NEED, and therefore what this must reproduce (tests/test_prisma.cpp):
  - device.xml with a serial, DeviceType 27, FWVersion 5.05, MainboardHWVersion 11
  - 18 signals, record_duration 1.0, and the specific names the tests look up
  - the 8-bit quirk: Prisma writes bytes-per-sample in the signal RESERVED field
    as "#1" or "#2", which is not EDF and is the whole reason this parser exists.
    Pressure must stay 2-byte and BreathFrequency 1-byte or Wmedf8BitDetection
    is testing nothing.
  - a big and a small variant of both signal and event files
  - RespEvents whose IDs map to real event types, and DeviceEvent ParameterID
    1003 so therapy_mode is present
"""

import os
import struct

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")

# A serial that cannot be mistaken for a real one, in the same shape as the
# real thing so nothing downstream has to special-case its length.
SERIAL = "TESTSN00"
MAINBOARD_SERIAL = "TESTMB000"

# Fixed start so output never changes: Tue, 02.01.2024 22:00:00.
START_DATE = "02.01.24"
START_TIME = "22.00.00"

BIG_RECORDS = 600     # ten minutes at one record per second
SMALL_RECORDS = 5


def ascii_field(value, width):
    """EDF fields are space-padded ASCII, left aligned, exactly `width` bytes."""
    s = str(value)
    if len(s) > width:
        s = s[:width]
    return s.ljust(width).encode("ascii")


# ── The 18 signals, exactly as a Prisma writes them ─────────────────────────
# (label, unit, phys_min, phys_max, dig_min, dig_max, samples_per_record, bytes)
SIGNALS = [
    ("Pressure",        "hPa",   "-327.679", "327.6700", "-32768", "32767", 5,  2),
    ("EEPAPsoll",       "hPa",   "-327.679", "327.6700", "-32768", "32767", 1,  2),
    ("IPAPsoll",        "hPa",   "-327.679", "327.6700", "-32768", "32767", 1,  2),
    ("EPAPsoll",        "hPa",   "-327.679", "327.6700", "-32768", "32767", 1,  2),
    ("RespFlow",        "l/min", "-32768.0", "32767.00", "-32768", "32767", 10, 2),
    ("rAMV",            "%",     "-128.000", "127.0000", "-128",   "127",   1,  1),
    ("BreathVolume",    "ml",    "-32768.0", "32767.00", "-32768", "32767", 1,  2),
    ("BreathFrequency", "bpm",   "-128.000", "127.0000", "-128",   "127",   1,  1),
    ("LeakFlowBreath",  "l/min", "-128.000", "127.0000", "-128",   "127",   1,  1),
    ("ObstructLevel",   "%",     "-128.000", "127.0000", "-128",   "127",   1,  1),
    ("SpO2",            "%",     "-128.000", "127.0000", "-128",   "127",   1,  1),
    ("HeartFrequency",  "bpm",   "-128.000", "127.0000", "-128",   "127",   1,  1),
    ("SPRstatus",       "-",     "-128.000", "127.0000", "-128",   "127",   5,  1),
    ("InspExpirRel",    "%",     "-128.000", "127.0000", "-128",   "127",   1,  1),
    ("MV",              "l/min", "0.000000", "25.50000", "0",      "255",   1,  1),
    ("rMVFluctuation",  "",      "0.000000", "16.00000", "0",      "160",   1,  1),
    ("TotalLeakage",    "l/min", "0.000000", "255.0000", "0",      "255",   1,  1),
    ("RSBI",            "",      "0.000000", "255.0000", "0",      "255",   1,  1),
]


def sample_value(sig_index, record, sample, width_bytes):
    """A deterministic, in-range, mildly varying value.

    Shape matters more than realism: the tests assert that samples can be READ
    and that the count is right, not that the waveform is plausible. Kept well
    inside the digital range so no value can clip into the min/max the header
    declares.
    """
    seed = (sig_index * 7919 + record * 31 + sample * 17) % 200
    return seed - 100 if width_bytes == 2 else (seed % 100) - 50


def build_wmedf(n_records):
    ns = len(SIGNALS)
    header_bytes = 256 + ns * 256

    head = b""
    head += ascii_field("1", 8)                       # version
    head += ascii_field("Patient Name", 80)           # the device's own placeholder
    head += ascii_field(
        f"Recording start at Tue, {START_DATE.replace('.', '.')} {START_TIME.replace('.', ':')}", 80)
    head += ascii_field(START_DATE, 8)
    head += ascii_field(START_TIME, 8)
    head += ascii_field(header_bytes, 8)
    head += ascii_field("#s", 44)                     # Prisma's reserved marker
    head += ascii_field(-1, 8)                        # record count unknown, as the real one
    head += ascii_field(1, 8)                         # one second per record
    head += ascii_field(ns, 4)

    def per_signal(fn, width):
        return b"".join(ascii_field(fn(s), width) for s in SIGNALS)

    head += per_signal(lambda s: s[0], 16)            # labels
    head += per_signal(lambda s: f"Transducer {s[0]}", 80)
    head += per_signal(lambda s: s[1], 8)             # units
    head += per_signal(lambda s: s[2], 8)             # physical min
    head += per_signal(lambda s: s[3], 8)             # physical max
    head += per_signal(lambda s: s[4], 8)             # digital min
    head += per_signal(lambda s: s[5], 8)             # digital max
    head += per_signal(lambda s: "None", 80)          # prefiltering
    head += per_signal(lambda s: s[6], 8)             # samples per record
    # THE 8-BIT MARKER. "#1" or "#2" in the reserved field is how a Prisma says
    # how wide each sample is. Standard EDF has no such thing and assumes 2.
    head += per_signal(lambda s: f"#{s[7]}", 32)

    assert len(head) == header_bytes, (len(head), header_bytes)

    body = bytearray()
    for rec in range(n_records):
        for i, (_, _, _, _, _, _, spr, width) in enumerate(SIGNALS):
            for smp in range(spr):
                v = sample_value(i, rec, smp, width)
                if width == 2:
                    body += struct.pack("<h", v)
                else:
                    body += struct.pack("<b", v)
    return head + bytes(body)


def build_event_xml(n_events):
    """Events the parser will actually map, plus the settings it reads.

    ParameterID 1003 is the one that matters: it carries therapy mode, and the
    tests assert settings.therapy_mode is present. 2 is APAP.
    """
    lines = ['<?xml version="1.0" encoding="utf-8"?>', "<desc>"]
    for pid, val in [
        (1003, 2),          # therapy mode -> APAP
        (1001, 20240001),   # software/config id, synthetic
        (1002, 27),         # device type
        (1084, 1),
        (1083, 3),
        (1199, 1000),
        (1128, 15),
    ]:
        lines.append(f'<DeviceEvent  DeviceEventID="0" Time="0" '
                     f'ParameterID="{pid}" NewValue="{val}"/>')

    # Cycle through IDs the parser maps, so every branch of mapRespEventId is
    # exercised by real fixture data rather than only by the unit test.
    ids = [101, 102, 103, 111, 112, 121, 131, 151, 161, 181]
    for n in range(n_events):
        rid = ids[n % len(ids)]
        end = 30 + n * 20
        lines.append(f'<RespEvent  RespEventID="{rid}" EndTime="{end}" '
                     f'Duration="10" Pressure="0" Strength="2"/>')
    lines.append("</desc>")
    return "\n".join(lines) + "\n"


DEVICE_XML = f"""<?xml version="1.0" encoding="utf-8"?>
<?weinmann version="1.1" type="dde-tpg"?>
<DeviceID>
<DeviceType value="27"/>
<DeviceSerialNumber value="{SERIAL}"/>
<MainboardSerialNumber value="{MAINBOARD_SERIAL}"/>
<BlowerIndex value="0"/>
<FWVersion value="5.05"/>
<FWRevision value="0022"/>
<FWBuild value="0000-0000-0000-Test"/>
<PMVersion value="2.17.8"/>
<StatisticVersion value="2.4.1"/>
<NotificationVersion value="00.00.01"/>
<LoggerVersion value="00.01.02"/>
<MainboardHWVersion value="11"/>
<DisplayHWVersion value="24"/>
<DeviceVariant value="0"/>
<DeviceBranding value="2"/>
</DeviceID>
"""

CONFIGURATION_XML = """<?xml version="1.0" encoding="utf-8"?>
<?weinmann version="1.1" type="dde-cfg"?>
<Configuration>
<Parameter ID="1003" value="2"/>
<Parameter ID="1084" value="1"/>
<Parameter ID="1128" value="15"/>
<Parameter ID="1199" value="1000"/>
</Configuration>
"""


def write(name, data):
    path = os.path.join(OUT, name)
    mode = "wb" if isinstance(data, bytes) else "w"
    with open(path, mode) as fh:
        fh.write(data)
    print(f"  {name}  {len(data)} bytes")


def main():
    # hms-cpap needs ONE parseable signal file: PrismaMode_DiscoversParsesAndStores
    # copies it into a synthetic SMART-max tree and writes its own event XML.
    # The identical generator lives in hms-cpapdash-parser, which needs the full
    # set; kept in step by hand rather than shared, because a test fixture that
    # depends on another repo is worse than a duplicated 200-line script.
    print("Writing synthetic Prisma fixture:")
    write("signal_synthetic.wmedf", build_wmedf(BIG_RECORDS))


if __name__ == "__main__":
    main()
