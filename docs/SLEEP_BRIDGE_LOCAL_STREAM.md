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
   **My hms-cpap** and type this computer's IP address or hostname, e.g.
   `192.168.1.20`, and nothing else: no port, no `ws://`. The port is its own
   field, prefilled 8894: leave it unless you changed `sleep_bridge.port` in
   hms-cpap. The bridge tests the address before it keeps it.
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

Leave the port at 8894 unless you have to: a bridge reaches another port only
with that port typed in its own Port field (hms-cpap says so at start when the
port is not 8894). In Docker, publish it as `8894:8894`. Only the stream answers on it: the
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
| `{"t":"result","req_id":N,"result":{...}}` | The answer to a command (below). |
| binary, kind 7 | One batch (below). |
| binary, kind 4 | A command's log text, ahead of its result (below). |

`state` is the bridge's own report (firmware version, counters); hms-cpap
shows it and acts on none of it.

A hello whose state carries `"probe": true` or `"portal_test": true` is a
test of this address: the setup page's test, or a bridge testing hms-cpap
before it switches here. It is answered with `{"t":"ack"}` and nothing
else; its state is not kept, it is not the bridge's session, no command is
sent on it, and a bridge never seen before does not appear because of it.
The bridge closes it within seconds.

### Down (hms-cpap to bridge)

| Frame | Meaning |
|---|---|
| `{"t":"ack"}` | The answer to `hello`. |
| `{"t":"ack_sectors","seq":N,"status":S,...}` | The answer to one batch (below). |
| `{"t":"cmd","cmd":"<name>","req_id":N,...}` | A command (below). |
| `{"t":"error","error":"..."}` | A frame that could not be read. |
| binary, kind 8 | One chunk of a firmware image, after an `ota` command ([Updating the bridge](#updating-the-bridge)). |

A file (binary kind 5) is ignored.

## Commands

A bridge takes these commands from a local hms-cpap:

| Command | Arguments | What the bridge does |
|---|---|---|
| `stream` | `url`: an IP address or hostname only (the bridge adds `ws://` and the path), or `""` for the cloud; `port`: 1 to 65535, optional, 8894 when left out | To another hms-cpap: tests the new target with a probe hello, answers, and only then keeps the address and port and moves; on a failed test nothing changes. To the cloud (`""`): nothing is tested; the bridge answers on this connection, keeps the cloud, and restarts. |
| `reboot` | | Restarts. Never answered. |
| `logs` | `n`: lines, 1 to 5000 (200 when left out) | Sends its log as kind 4 frames, then the result. |
| `restate` | | Sends its card statement again. |
| `config_reset` | | Forgets its network and stream address and restarts into setup. Keeps its name and pairing. Answered first. |
| `ota` | `target`: `"tapper"`, `size`: the image's bytes, `sha256`: its digest, 64 hex digits; no `url` | Takes the image that follows as kind 8 frames, checks it, answers, and restarts into it only on `ok`. Sent by the update route below, never by `POST /api/sleepbridge/command`. |

The answer is `{"t":"result","req_id":N,"result":{"ok":bool,...}}`, with
`error` when `ok` is false; `stream` adds `url`, `changed` (and `switched`,
the same value) and, on a switch to the cloud, `restarting: true`. It can
come on a later connection than the command went on: a `stream` to another
hms-cpap closes the connection to test the new target, then answers on the
one it comes back on. A `stream` to the cloud answers on this connection and
then leaves: expect the disconnect, and no return. A log pull's text comes as
binary kind 4 frames whose header `req_id` is the command's, `seq` counting
the pieces and flags bit 0 on the last.

No command is refused while the machine records: the card keeps the night,
and when to switch is the owner's call. The one exception is the firmware
update, which hms-cpap itself refuses while a night is open on the bridge
([Updating the bridge](#updating-the-bridge)). A `stream` or `ota` that
arrives while the bridge is in the middle of a switch or an update is
answered `{"ok":false,"error":"busy"}`.

From the web port:

```
POST /api/sleepbridge/command
{"sender":"TAP-S3-00000123","cmd":"stream","url":""}

{"ok":true,"req_id":815000123,"command":{"state":"sent",...}}
```

It is sent at once when the bridge is online, or on its next hello when it is
not (`"state":"queued"`). 400 for a command or argument a bridge does not
take, 404 for a bridge never seen. `GET /api/sleepbridge` lists each bridge's
last 20 commands, newest first, with `state` (`queued`, `sent`, `answered`),
`result` and, for `logs`, the text.

### Binary frame header

| Offset | Size | Field |
|---|---|---|
| 0 | u32 | `req_id`: 0 for a batch; the command's for a log text or a firmware chunk |
| 4 | u32 | `seq`: counts batches on this connection from 0, echoed in the ack; counts the pieces of a log text or the chunks of an image from 0 |
| 8 | u16 | `flags`: bit 0 set on the last frame of an item (always 1 for a batch) |
| 10 | u16 | `kind`: 7 a batch (up), 4 a log text (up), 8 a firmware chunk (down) |
| 12 | | the body |

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
    geometry.json                         the card's layout, across restarts
    bridge.json                           the bridge's name, label and chip id
```

The batches are the record; the card files are derived from them. After a
restart the rebuild reads the whole bank again from the start. A file is
written only when every sector of it was seen; a file still missing sectors is
listed in `held.json` and never written with holes in it.

`geometry.json` is the card's layout as the rebuild trusts it (sectors per
cluster, where the data and the FATs begin, the root cluster), the card it
describes, where it came from (`read`: the card's boot sector; `stated`: the
bridge's card statement; `solved`: from the writes alone) and what confirmed
it in which bridge session. It is written durably when a layout is first
trusted and whenever it changes, and read when hms-cpap starts, before any
batch is fed, so a restart neither forgets the layout nor holds the session
until the bank proves it again. It is a cache of what the batches said last:
when a later boot sector says otherwise, the boot sector wins and the file is
rewritten. Delete it and the bank establishes it again. (A build before it
kept the same thing as `rebuild.state`; that file is read once and replaced.)
The status page shows it under each bridge's `rebuild.geometry`.

## Updating the bridge

A bridge pointed at hms-cpap never hears from the cloud, so its firmware
comes from here, over the same connection it streams on. hms-cpap sends an
image you have (a `.bin` file); a published release feed can come later.

```
POST /api/sleepbridge/TAP-S3-00000123/update
Content-Type: application/octet-stream    (the image as the body)
    or multipart/form-data with a file field

200 {"ok":true,"req_id":815000124,"update":{"phase":"sending",...}}
```

Refused with 400 when the body is not a Sleep Bridge image (the ESP32 image
magic and an app descriptor whose project is `sd_capture`), 404 for a bridge
never seen, and 409 while a night is open on the bridge, while it is not
connected, or while an update is already running. **A night is open while
the machine writes to its card**: a batch with records landed in the last
15 minutes. The machine writes through therapy and for a minute or two after
the mask comes off, so fifteen quiet minutes is its downtime. The bridge
itself refuses nothing here; this end keeps that rule, because an update
parks the bridge's capture while it writes its flash.

On the stream connection, hms-cpap then sends:

1. `{"t":"cmd","cmd":"ota","req_id":N,"target":"tapper","size":S,"sha256":"<64 hex>"}`,
   the image's byte count and SHA-256 (lowercase hex);
2. the image as binary frames with the header above: `req_id` N, `seq` from
   0, `kind` 8, each carrying the next 4096 bytes of the image and nothing
   else, the last one (possibly shorter, never empty) with flags bit 0 set.

The bridge writes each chunk into its spare firmware slot as it lands, so the
connection's own flow control paces the flash. When the last chunk is in it
checks the byte count and the digest against the command, verifies the
image, and answers `{"t":"result","req_id":N,"result":{"ok":true,"slot":
"ota_1","version":"1.0.0","project":"sd_capture","built":"..."}}`; then it
restarts into the new image, so expect the disconnect. On any failure (a
chunk out of order, a byte count or digest that disagrees, an image its
loader rejects, 60 s without a chunk) it answers `{"ok":false,"error":...}`,
drops the slot, and keeps running what it has. It never restarts on an
error. Its next hello carries `fw_version`, which `GET /api/sleepbridge`
shows per bridge.

`GET /api/sleepbridge/{bridge}/update` (and `update` on each bridge in
`GET /api/sleepbridge`) follows the job:

| `phase` | meaning |
|---|---|
| `sending` | the command went; `sent` of `total` bytes handed to the connection |
| `waiting` | every chunk went; the bridge is checking and answering |
| `restarting` | answered `ok`; the bridge is rebooting into the image |
| `done` | the bridge came back; `fw_after` is the version it runs now |
| `failed` | `error` says why: the bridge's answer, a connection that went, or no answer in 5 minutes |

`image_version`, `image_project` and `image_built` are read from the file
before anything is sent; `fw_before` is what the bridge reported in its
last hello.

## Licence

hms-cpap's source is MIT. The Sleep Bridge component that rebuilds files from
the stream is a prebuilt binary under its own licence,
[LICENSE-sleepbridge](../LICENSE-sleepbridge), published as versioned assets on
this repository's releases and pinned by version and SHA-256 in
`cmake/sleepbridge-pins.cmake`. Its one public header is
`<sleepbridge/rebuild.h>`. A build with `-DBUILD_WITH_SLEEPBRIDGE=OFF` (the
default from source) has none of it and refuses bridges as described above.
