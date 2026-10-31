# The Mule & Miner local stream

A Mule & Miner in AirSense 11 Bluetooth mode can send the machine's night to
your own hms-cpap instead of the cloud. On the unit, two fields: the hms-cpap
address, this host's IP address or hostname and nothing else (no `ws://`, no
port, no path), and the port, 8894 unless you changed `sleep_bridge.port` in
hms-cpap. The unit builds the URL itself and connects to:

```
ws://<host>:<port>/mule/v1/device
```

Port 8894 is the Sleep Bridge's stream port (`sleep_bridge.port`); both
devices share it, each on its own route. The route answers on that port only.

## Upgrade

Plain `ws://`. One header beyond the WebSocket's own:

```
X-Device-Serial: <the unit's serial>     1-64 of [A-Za-z0-9._-]
```

No credential is sent or read. Any unit on the network is taken, and a serial
not seen before is listed on its first connect.

## Frames

Text frames are one JSON object with a string `"t"`. Binary frames start with
a 12-byte little-endian header: `req_id` u32, `seq` u32, `flags` u16 (bit 0:
last), `kind` u16.

Up, from the unit:

| frame | what hms-cpap reads |
|---|---|
| `hello` `{state, fw, miner_fw, boot_reason, mode}` | the unit's session starts. `mode: "card"` is refused with `error`. |
| `hello` `{"test": true, ...}` | a test of this address before the unit switches here: answered with `ack` and nothing else; nothing is kept. |
| `state` `{state}` | `state.as11` (`streaming`, `stream_buffered`, ...), `state.tz` (the owner's zone, POSIX), `state.resp_events` (the machine's scored events: `event`, `reportTime`, `durationSeconds`, `backdateSeconds`) |
| `result` `{req_id, ok, error?, result?}` | a command's answer |
| binary kind 1 | one CDS1 stream block (`req_id 0, seq 0, flags 1`) |
| binary kind 6 | an `as11_download`'s bytes (`req_id` the command's, `seq` from 0), then an empty frame with flags bit 0, then its `result` |

Down, from hms-cpap:

| frame | when |
|---|---|
| `ack {server_time, ping_s: 30}` | to every hello |
| `block_ack {start_ms}` | once the block is on disk |
| `error {error}` | a frame not understood, a card-mode unit |
| `cmd {req_id, cmd, ...}` | one at a time |

hms-cpap pings every 30 s and closes a connection silent for 90 s.

## The ack rule

The unit frees a block when its `block_ack` comes, and sends again any block
not acked (it holds about 13 minutes in its flash while it cannot reach
hms-cpap). So a block is written and synced to disk before it is acked, and a
block sent again (same `start_ms`) is acked again and kept once.

## CDS1

```
magic "CDS1" 4, version 1 (= 1), nchan 1, interval_ms 2 LE (40),
start_ms 8 LE (the machine's UTC clock), serial_len 1, serial,
then per channel: id 1, cadence 1 (0: one value per interval_ms,
1: one per 2 s), count 2 LE, count int16 LE values.
INT16_MIN (-32768) is "the machine sent none".
```

| id | channel | unit |
|---|---|---|
| 1 | flow | mL/s |
| 2 | mask pressure | 0.01 cmH2O |
| 3 | mask pressure (2 s) | 0.01 cmH2O |
| 4 | inspiratory pressure (2 s) | 0.01 cmH2O |
| 5 | expiratory pressure (2 s) | 0.01 cmH2O |
| 6 | leak | mL/s |
| 7 | respiratory rate | 0.1 /min |
| 8 | tidal volume | mL |
| 9 | minute ventilation | 0.01 L/min |
| 10 | snore | 0.01 |
| 11 | flow limitation | 0.001 |
| 12 | SpO2 | 0.1 % |
| 13 | pulse | bpm |

## What lands on disk

```
<data_dir>/mule/<unit>/unit.json                   the unit, its machine, its zone
<data_dir>/mule/<unit>/blocks/<start_ms>.cds1      every block, as it came
<data_dir>/as11/<machine>/DATALOG/<night>/         BRP, PLD, SAD, EVE, CSL, as a card
<data_dir>/as11/<machine>/Identification.json
<data_dir>/as11/<machine>/AS11/summary-<from>.as11, settings-<from>.as11set
<data_dir>/as11/<machine>/STR.edf                  when the model's layout is in str-layouts/
```

The night is named on the owner's wall clock from `state.tz`, the sleep day
running noon to noon, as the machine's card would name it.

When the stream has ended (`streaming` true, then false) and the unit holds
no more of it (`stream_buffered` 0), the night's files are completed and
hms-cpap asks, one at a time: `as11_download` with `spool` `Identification`
(when no Identification.json is kept), `Summary`, then `Settings`.

## Live

The night is written as it streams, not after it. Every block rewrites the
open session's BRP (and PLD, SAD) in place as a valid EDF, so the session is
current within one block; every event goes into its night's EVE (or CSL) the
moment its state frame arrives. Each block and each event also wakes
hms-cpap's own sync at once, so the night's sessions, events and counts are
in the database, the charts and the realtime sensors within a block, with
no wait for the next sync interval.

### Live on MQTT

With MQTT on, each event and therapy on and off go to the broker as they
arrive, so a Home Assistant automation can react during the night. The
topics are under the device id hms-cpap's other sensors use
(`cpap/<device_id>/...`), and Home Assistant discovers them on the same
device.

| topic | payload | retained |
|---|---|---|
| `cpap/<device_id>/live/event` | one JSON per event (below) | no |
| `cpap/<device_id>/live/last_event` | the event's type: `OA`, `CA`, `UA`, `H`, `RERA`, `CSR` | yes |
| `cpap/<device_id>/live/event_count` | apneas and hypopneas scored this night | yes |
| `cpap/<device_id>/live/ahi` | events so far per hour masked on (over five minutes at least) | yes |
| `cpap/<device_id>/live/therapy` | `ON` while the machine streams, `OFF` when it stops | yes |
| `cpap/<device_id>/live/therapy_changed` | one JSON when therapy starts or ends | no |

An event:

```json
{"type":"OA","event":"ObstructiveApneaEnd","unit":"<unit serial>","machine":"<machine serial>",
 "time":"2026-10-31T04:32:10.000Z","duration_s":12,"backdate_s":12,
 "night":{"events":5,"apneas":3,"hypopneas":2,"reras":0,"csr":0,"hours":"1.25","ahi":"4.0"}}
```

`time` is the machine's clock (UTC) when it reported the event, which is the
event's end. A therapy change:

```json
{"unit":"<unit serial>","machine":"<machine serial>","therapy":"off","at":1793422800,
 "events":5,"hours":"6.80","ahi":"0.7"}
```

The counts start over when therapy starts. Discovery announces
`sensor.<device_id>_live_event_count`, `..._live_ahi`, `..._live_last_event`
and `binary_sensor.<device_id>_live_therapy`.

## Commands

`GET /api/mule` lists the units with their status and their recent commands
and results. `POST /api/mule/command` sends one:

```
{"serial": "<unit>", "cmd": "<name>", ...arguments}
```

| cmd | arguments |
|---|---|
| `config` | `{"config": {"hms_url": "<address>", "hms_port": N}}` or `{"hms_url": ..., "hms_port": N}`: one or both, no other key. `hms_url` is an IP address or hostname only, `""` sends the unit back to the cloud; `hms_port` is 1 to 65535 (8894 when never set) and alone changes only the port of the hms-cpap the unit already points at. The unit tests the new target before it keeps it, and answers on the connection it comes back on |
| `reboot` | `target`: `mule`, `miner` or `both` (optional) |
| `logs` | `n`: 1 to 5000 lines (default 200) |
| `restate` | none: the unit sends its full state again |
| `reset` | none: the unit erases its WiFi and this address, and restarts into setup |
| `as11_pair` | none to start, `code` to confirm, `cancel: true` to drop |
| `as11_download` | `from` (YYYY-MM-DD), `spool` (`Summary`, `Identification`, `Settings`) |

Anything else is refused with 400; a unit never seen here gets 404. A command
for a unit that is offline or busy waits for it. `reboot` and `reset` are
never answered: the unit restarts.

## Updating the unit's firmware

A unit streaming here never hears from the cloud, so its firmware comes from
here too, over your own network. The unit's own control server takes an
image as a plain HTTP POST (the page at `http://<unit-ip>/api/update` does
the same by hand):

```
POST http://<unit-ip>/api/update/mule    body: the mule image
POST http://<unit-ip>/api/update/miner   body: the miner image
```

The unit's address is the peer of its stream connection, so nothing is
configured. The mule restarts after its own image and comes back here with
its new version in its hello; the miner's image goes through the mule, which
flashes the miner and keeps its own connection. A 503 "Firmware update in
progress" means the unit is already taking one.

hms-cpap does this from Settings, "Mule & Miner": each unit's card shows the
mule and miner versions it runs, "update available" when the published
manifest has a newer one, and an Update button that sends the mule first,
then the miner. The same card takes an image file you have, for the mule or
the miner, when the manifest is out of reach or you built the image yourself.
The published image is for the standard board; a unit built on another pin
map takes its own file.

**Only in the unit's downtime.** While a night is open on the unit (the
machine is streaming, or the stream ended and the unit still holds blocks of
it) the update is refused, and the job checks again right before each image
goes. Nothing is queued: press the button again once the night has landed.

The API behind the card:

```
GET  /api/mule/<serial>/firmware
     {serial, online, ip, fw, miner_fw, night_open,
      manifest: {url, error, read_at}, latest: {mule, miner},
      update_available: {mule, miner}, job}
POST /api/mule/<serial>/update
     {"target": "mule" | "miner" | "both"}            from the manifest
     multipart/form-data, a file + target=mule|miner  an image you have
     <the image as the raw body>?target=mule|miner    the same, no form
     202 {ok, job}; 409 a night open, the unit offline, an update already
     running, or nothing newer; 502 the manifest out of reach; 404, 400
GET  /api/mule/<serial>/update
     {ok, job}: phase (downloading, sending, waiting, done, failed), step
     (mule or miner), sent and total bytes, error, the versions before and
     after, started, finished, log
```

With curl, an image file to the mule of one unit:

```
curl -X POST --data-binary @mule.bin \
     "http://localhost:8893/api/mule/<serial>/update?target=mule"
curl "http://localhost:8893/api/mule/<serial>/update"
```

Sending an image takes some tens of seconds (the unit writes it to flash as
it arrives); hms-cpap allows five minutes, then two and a half for the mule
to restart and reconnect before the miner's image goes.

The manifest is `https://fw.cpapdash.com/public/latest.json`
(`{"mule": {"version", "url"}, "miner": {...}}`), read over TLS from this
host; `HMS_CPAP_MULE_MANIFEST_URL` points at another. When it cannot be read
the card says so and the file path remains.
