# Oximetry CSV golden corpus

The Wellue/ViHealth apps have no stable export format: the timestamp field
follows the phone's locale (month-name vs numeric, 12h vs 24h, day-first vs
month-first), and columns shift between app versions. Every parser change
risks silently re-breaking a shape that used to work, so the contract lives
here as files rather than in anyone's memory.

## Convention

When a user report includes a real export, save the header plus the first
~20 rows in this directory (that is enough to pin timestamp format, column
layout and sentinels; full files are large and carry health data):

    <device>_<locale-hint>_<source>.csv
    e.g. o2ring_us-numeric_ticket67.csv
         checkme-o2-ultra_us-ampm_ticket67.csv
         o2ring_eu-24h_issue17.csv

Strip nothing else: keep the exact header row, quoting, CR/LF line endings
and any sentinel rows ("- -", 255/65535) as exported.

Then add a case to `tests/services/test_O2RingCsvParser.cpp` that parses the
fixture and asserts the first and last timestamps land on the correct
calendar dates. The known shapes are currently pinned as inline strings in
that file; fixtures from real exports are preferred for anything new.

## Date-order detection

`O2RingCsvParser::detectDateOrder` resolves ambiguous numeric dates in this
order (see the comment there): component above 12, midnight crossing,
filename YYYYMMDD stamp, AM/PM clock implies month-first, day-first default.
A new fixture that defeats all five tiers is exactly the kind of file that
belongs in this directory.
