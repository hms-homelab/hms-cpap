# The Sleep Bridge local stream

A Sleep Bridge Cloud sits between the CPAP machine and its SD card and reads
the SD bus. It sees every sector the machine writes, and the sectors the
machine reads around a mount. Pointed at hms-cpap instead of the cloud, it
streams those sectors over one WebSocket on your local network, and hms-cpap
rebuilds the card's files from them into a folder it reads like any card.

This page is the wire protocol, so the receiver can be checked, tested and
reimplemented. Rebuilding files from the sectors is done by the Sleep Bridge
component, a prebuilt library that hms-cpap links when it is built with
`BUILD_WITH_SLEEPBRIDGE=ON` (see [Licence](#licence)); that part is not
described here.

The machine can be a ResMed AirSense or a BMC / React Health Luna. The stream
is the same for both. The rebuilt folder is in that machine's own card layout,
and hms-cpap reads it with the matching card format (`resmed` or `bmc`).

## Setting it up

1. On the bridge's setup page (hold its button 10 s to reopen it), choose
   **My hms-cpap** and type this computer's address, e.g. `192.168.1.20` or
   `192.168.1.20:8894`. The bridge tests the address before it keeps it.
2. Power the machine off and on once, outside therapy. The machine reads the
   card when it starts, and that read is what lets the rebuild trust the
   card's layout. Until then hms-cpap holds what it receives and writes no
   file.
3. Each bridge's files land in `<data_dir>/sleepbridge/<bridge>/`. Point a
   Local folder source at it, with the card format of the machine. `GET
   /api/sleepbridge` lists every bridge with its folder (`card_root`), the
   machine it recognised (`card_machine`) and the format to read it with
   (`card_format`).

There is no login on the stream. Keep the port on your local network.

## Configuration

`config.json`:

```json
"sleep_bridge": { "enabled": true, "port": 8894 }
```

The port must differ from the web port. Only the stream answers on it: the
web UI and the API are not served there, and the stream is not served on the
web port.

A build without the Sleep Bridge component still listens, and refuses every
bridge with HTTP 501 `{"ok":false,"error":"The Sleep Bridge is not in this
build"}`. The bridge's setup test then fails, so it is never left pointing at
a receiver that cannot use what it sends.

## Connection

```
GET ws://<host>:8894/sleepbridge/v1/device
X-Coyote-Sender:  <the bridge's name, e.g. TAP-S3-00000123>
X-Coyote-Machine: <optional label, e.g. airsense-bench>
X-Chip-Id:        <the bridge's WiFi MAC, 12 hex digits>
```

- `X-Coyote-Sender` is required: 1 to 64 of `[A-Za-z0-9._-]`, not starting
  with `.`. It names the bridge and its folder. Anything else is refused with
  HTTP 400.
- `X-Coyote-Machine` is optional, the same shape. `X-Chip-Id` is shown only.
- There is no authentication. An `Authorization` header is never read, stored
  or logged; a Sleep Bridge pointed at a local address sends none.
- Plain `ws://`. The bridge reconnects 5 s after a drop.
- Pings every 30 s from both ends. A connection silent for 90 s is closed, and
  the bridge shows offline until it reconnects.

## Frames

Text frames are one JSON object with a string `t`. Binary frames start with
a 12-byte header. All integers are little-endian.

### Up (bridge to hms-cpap)

| Frame | Meaning |
|---|---|
| `{"t":"hello","state":{...}}` | First frame on every connection. Nothing else flows until it is answered. |
| `{"t":"state","state":{...}}` | Every 30 s. Kept for the status page, never answered. |
| binary, kind 7 | One batch (below). |

`state` is the bridge's own report (firmware version, counters); hms-cpap
shows it and acts on none of it. The setup page's test sends one hello with
`"portal_test": true` in its state, waits for the ack, and closes.

### Down (hms-cpap to bridge)

| Frame | Meaning |
|---|---|
| `{"t":"ack"}` | The answer to `hello`. |
| `{"t":"ack_sectors","seq":N,"status":S,...}` | The answer to one batch (below). |
| `{"t":"error","error":"..."}` | A frame that could not be read. |

hms-cpap sends no commands on the local stream: no firmware update, no log
pull. A `result` frame, log text (binary kind 4) or a file (kind 5) is
ignored.

### Binary frame header

| Offset | Size | Field |
|---|---|---|
| 0 | u32 | `req_id`, 0 for a batch |
| 4 | u32 | `seq`: counts batches on this connection from 0; echoed in the ack |
| 8 | u16 | `flags`: bit 0 set on the last frame of an item (always 1 for a batch) |
| 10 | u16 | `kind`: 7 for a batch |
| 12 | | the batch body |

## Batches

Every record carries a 64-bit sequence number. The bridge numbers its
records in one increasing space; the high 32 bits are the bridge's boot
count, so a reboot moves the sequence on, never back.

### Batch header (18 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic: `CYS1`, `CYS2` or `CYR1` |
| 4 | u16 | `count`, the number of records, at most 512 |
| 6 | u64 | `first_sequence`, the first record's sequence; the rest follow by one |
| 14 | u32 | `geometry_epoch` |

### CYS1: sector writes

`count` records of `lba` (u32) and the 512 bytes the machine wrote there.
The body is exactly `18 + count * 516` bytes, or it is refused.

### CYS2: sector writes, fills folded

The same header with magic `CYS2`. Each record is `lba` (u32), a kind byte,
then:

- kind 0: the 512 bytes;
- kind 1: one byte, meaning that byte 512 times (a fresh FAT, a zeroed block).

hms-cpap expands a CYS2 to the CYS1 it stands for before anything else reads
it, and keeps only the CYS1. A record of another kind, or a body that is not
exactly its records, refuses the whole batch.

### CYR1: sector reads

The same layout as CYS1, magic `CYR1`: sectors the machine READ, around a
mount (the boot sector, the FAT, the root directory), in the same sequence
space as the writes.

### Heartbeat

A CYS1 header with `count` 0 and nothing after it. Answered with the mark;
stores nothing.

### Card statement

A CYS1 header with `count` 0 followed by a JSON object, sent when the
machine mounts a card:

```json
{"cid":"<32 hex>","csd":"<32 hex>","capacity_blocks":15523840,"mount":3,"seq":1288490188800}
```

`cid` is required, and either `csd` or a layout (`sectors_per_cluster`, a
power of two up to 128, and `data_begin_lba`). `mount` is the boot the mount
was seen in, and `seq` the sequence of the first record after it. A
statement saying neither is refused. It is stored at its place in the stream
and moves no mark.

## The ack

For each bridge hms-cpap holds a high-water mark: the last sequence stored.
The ack carries it, and the bridge drops every record at or below it and
sends the rest again. So:

- A batch wholly at or below the mark is a re-send: acked with
  `duplicate: true`, not stored again.
- A batch overlapping the mark is stored; `accepted` counts its new part.
- A batch starting past `mark + 1` is a gap: stored, acked with `gap: true`,
  and the missing range recorded. A gap is records the bridge lost (its queue
  overflowed, or it rebooted while hms-cpap was away).
- A bridge's first batch is never a gap, however high it starts.
- The mark never moves backwards.

```json
{"t":"ack_sectors","seq":17,"status":200,"ok":true,"ack":1288490188863,"accepted":64,"gap":false,"duplicate":false}
{"t":"ack_sectors","seq":18,"status":200,"ok":true,"ack":1288490188863,"heartbeat":true}
{"t":"ack_sectors","seq":19,"status":200,"ok":true,"ack":1288490188863,"card":true,"cid":"..."}
{"t":"ack_sectors","seq":20,"status":400,"ok":false,"error":"record count disagrees with body length","ack":1288490188863}
```

`status` is 200 when the batch was taken. A 400 is a batch that can never be
taken as sent and still carries the mark, so the bridge can move past it. A
500 means it was not stored; the bridge sends it again.

**Stored before acked.** A batch is written to a temporary file, flushed to
disk and renamed into place; the mark is saved the same way after it; only
then is the ack sent. If hms-cpap stops anywhere in between, the mark is
where it was, the bridge sends the batch again, and it lands on the same file:
stored once.

## When hms-cpap is away

The bridge keeps unacknowledged records in its own memory (more than a
night's writes) and sends them when hms-cpap answers again, from the last ack. If it
is away longer than the bridge can hold, or the bridge restarts meanwhile,
the records in between are lost and show as a gap. The bridge does not fall
back to the cloud.

## On disk

```
<data_dir>/sleepbridge/<bridge>/          the rebuilt card, read as a local folder
<data_dir>/sleepbridge/<bridge>/batches/  what the stream sent:
    <first>-<last>.cys                    a batch as received (CYS2 expanded)
    cards/<next>-<ms>-<n>.json            a card statement, before sequence <next>
    cursor.json                           the high-water mark
    gaps.jsonl                            one line per gap
    held.json                             files seen in part, never written
    rebuild.state                         the rebuild's own state, across restarts
    bridge.json                           the bridge's name, label and chip id
```

The batches are the record; the card files are derived from them. After a
restart the rebuild reads the whole bank again from the start. A file is
written only when every sector of it was seen; a file still missing sectors is
listed in `held.json` and never written with holes in it.

## Licence

hms-cpap's source is MIT. The Sleep Bridge component that rebuilds files from
the stream is a prebuilt binary under its own licence,
[LICENSE-sleepbridge](../LICENSE-sleepbridge), published as versioned assets on
this repository's releases and pinned by version and SHA-256 in
`cmake/sleepbridge-pins.cmake`. Its one public header is
`<sleepbridge/rebuild.h>`. A build with `-DBUILD_WITH_SLEEPBRIDGE=OFF` (the
default from source) has none of it and refuses bridges as described above.
