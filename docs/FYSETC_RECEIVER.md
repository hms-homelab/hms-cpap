# FYSETC SD WiFi Pro -- Receiver Service

## Overview

The FysetcReceiverService is an alternative data source for hms-cpap that receives EDF data from a FYSETC SD WiFi Pro board installed inside the ResMed AirSense 10 CPAP machine. Instead of polling the ezShare WiFi SD card over HTTP (BurstCollectorService), data is pushed to hms-cpap via MQTT in real-time.

Enable with `CPAP_SOURCE=fysetc`.

## Architecture

```
ResMed AirSense 10
  |
  SD card bus (SPI)
  |
FYSETC SD WiFi Pro (ESP32, sits between CPAP and SD card)
  |  - Monitors CS line via PCNT (passive, zero CPU)
  |  - Detects therapy start (BRP file growing)
  |  - Surgical bus steals during therapy (<10ms)
  |  - Full upload after therapy ends
  |
  WiFi (station mode, joins home network)
  |
  MQTT (192.168.2.15:1883)
  |
hms-cpap FysetcReceiverService
  |  - Receives base64 EDF chunks, writes to disk
  |  - Manifest-driven: diffs remote vs local, requests missing files
  |  - Parses EDF, publishes to MQTT + PostgreSQL
  |  - LLM session summary
  |
Home Assistant / Grafana
```

## MQTT Topics

All topics under prefix `cpap/fysetc/{device_id}`:

| Topic | Direction | QoS | Retained | Description |
|-------|-----------|-----|----------|-------------|
| `status` | FYSETC -> broker | 1 | yes | Heartbeat: `{"state":"online","fw":"0.2.0","up":3600,...}` |
| `session/active` | FYSETC -> broker | 1 | yes | `ON` when BRP confirmed growing, `OFF` after upload complete |
| `manifest` | FYSETC -> hms-cpap | 1 | no | `{"date":"YYYYMMDD","files":{"file.edf":size,...}}` |
| `sync/request` | FYSETC -> hms-cpap | 1 | no | `{"date":"YYYYMMDD","files":["file.edf",...]}` |
| `sync/response` | hms-cpap -> FYSETC | 1 | no | `{"offsets":{"file.edf":bytes,...}}` |
| `chunk` | FYSETC -> hms-cpap | 1 | no | `{"f":"file.edf","d":"YYYYMMDD","o":offset,"n":len,"b64":"..."}` |
| `cmd/rescan` | hms-cpap -> FYSETC | 1 | no | Force manifest + STR publish |
| `cmd/fetch` | hms-cpap -> FYSETC | 1 | no | `{"date":"YYYYMMDD","files":[{"f":"file.edf","o":offset},...]}` |

hms-cpap also publishes to the standard CPAP topics for Home Assistant:
- `cpap/{device_id}/realtime/session_active` (ON/OFF)
- `cpap/{device_id}/realtime/{sensor}` (15 sensors)
- `cpap/{device_id}/history/{sensor}` (39 sensors)
- `cpap/{device_id}/daily/{sensor}` (13 STR sensors)

## Protocol Flow

### During Therapy (realtime)

```
1. CPAP writes BRP/PLD/SAD every 60s
2. FYSETC detects bus activity via PCNT (passive CS line monitoring)
3. FYSETC prescans: mounts SD, maps files, checks for BRP
   - No BRP found? False alarm (boot/settings), back to IDLE
   - BRP found? Publish session_active=ON
4. FYSETC sends sync/request with today's files
5. hms-cpap responds with sync/response (local byte offsets)
6. FYSETC reads BRP delta via surgical bus steals (<10ms each)
7. FYSETC sends base64 chunks via MQTT
8. hms-cpap decodes and writes to disk at correct offset
9. Repeat every 60s (matching BRP write interval)
```

### Post-Therapy (upload + manifest)

```
1. FYSETC detects 65s bus silence (BRP stopped growing, STR written ~50s after mask-off)
2. FYSETC mounts SD, uploads remaining BRP/PLD/SAD deltas
3. FYSETC reads STR.edf from SD root, sends via MQTT chunks
4. FYSETC publishes manifest (ALL files in date folder, including CSL/EVE)
5. FYSETC publishes session_active=OFF
6. hms-cpap receives manifest, diffs against local files
7. hms-cpap sends cmd/fetch for any missing/incomplete files (CSL, EVE, etc.)
8. FYSETC fulfills fetch (FSM_FETCHING state, with yield safety)
9. hms-cpap processes completed session:
   a. GroupLocalFolder -> EDFParser -> CPAPSession
   b. publishSession (MQTT + PostgreSQL)
   c. markSessionCompleted + nightly metrics
   d. processSTRFile (daily summary)
   e. generateAndPublishSummary (LLM, if enabled)
```

### On-Demand Rescan

```
1. hms-cpap publishes cmd/rescan (also done automatically on startup)
2. FYSETC mounts SD, lists all files, publishes manifest + STR
3. hms-cpap diffs and sends cmd/fetch for anything missing
```

## ResMed AirSense 10 Write Timing

Empirically verified from ezShare timestamps (March 15, 2026):

| File | When Written | Purpose |
|------|-------------|---------|
| CSL.edf | Session start, grows during therapy | Clinical summary |
| EVE.edf | During therapy, events as they happen | Apnea/hypopnea events |
| BRP.edf | Every 60s during therapy | Flow/pressure (25 Hz) |
| PLD.edf | Every 60s during therapy | Pressure/leak (0.5 Hz) |
| SAD.edf | Every 60s during therapy | SpO2/HR (1 Hz) |
| STR.edf | ~50s after mask-off | Daily therapy summary |

Key timing:
- BRP is the definitive therapy signal (no BRP = not therapy)
- STR is in the SD root (`/DATALOG/STR.edf`), not in date folders
- POST_THERAPY_IDLE_SEC = 65s (just over one BRP write interval)

## Yield Safety

The CPAP always wins bus contention. Every FYSETC bus operation:
- Calls `bus_arbiter_arm_yield()` before mounting
- Checks `bus_arbiter_yield_requested()` before each file read
- Immediately releases bus if CPAP needs it
- Interrupted uploads set `s_upload_pending` for automatic retry

## Configuration

| Variable | Default | Description |
|----------|---------|-------------|
| `CPAP_SOURCE` | `ezshare` | Set to `fysetc` to enable |
| `CPAP_DEVICE_ID` | `cpap_resmed_23243570851` | Device identifier |
| `CPAP_DEVICE_NAME` | `ResMed AirSense 10` | Display name |
| `CPAP_TEMP_DIR` | `/tmp/cpap_data` | Where chunks are written |
| `CPAP_ARCHIVE_DIR` | `/mnt/public/cpap_data/DATALOG` | Permanent storage |
| `MQTT_BROKER` | `192.168.2.15` | MQTT broker |
| `MQTT_PORT` | `1883` | MQTT port |
| `MQTT_USER` | `aamat` | MQTT username |
| `MQTT_PASSWORD` | `exploracion` | MQTT password |
| `MQTT_CLIENT_ID` | `hms_cpap_fysetc` | MQTT client ID |
| `LLM_ENABLED` | `false` | Enable AI session summary |
| `LLM_PROVIDER` | `ollama` | LLM provider |
| `LLM_ENDPOINT` | `http://192.168.2.5:11434` | LLM API endpoint |
| `LLM_MODEL` | `llama3.1:8b-instruct-q4_K_M` | LLM model |

## FYSETC FSM States

```
BOOT -> IDLE -> PRESCAN -> LISTENING -> STEALING -> PUSHING -> (loop)
                                    \-> UPLOADING -> IDLE
                                                 \-> YIELDING -> COOLDOWN -> IDLE/LISTENING
IDLE -> FETCHING -> IDLE  (cmd/fetch from hms-cpap)
IDLE -> rescan inline     (cmd/rescan from hms-cpap)
```

| State | Bus Owner | Description |
|-------|-----------|-------------|
| BOOT | CPAP | Init, connect WiFi/MQTT, clear stale state |
| IDLE | CPAP | Monitoring CS pulses, waiting for therapy |
| PRESCAN | ESP (brief) | Mount, map files, validate BRP, sync offsets |
| LISTENING | CPAP | Therapy active, waiting for 60s BRP write |
| STEALING | ESP (<10ms) | Surgical read of new BRP data |
| PUSHING | CPAP | MQTT publish of stolen chunk data |
| UPLOADING | ESP | Post-therapy: upload deltas + STR + manifest |
| FETCHING | ESP | Fulfill cmd/fetch from hms-cpap |
| YIELDING | CPAP | Bus released after CPAP interrupt |
| COOLDOWN | CPAP | Brief pause before re-entering IDLE/LISTENING |
| ERROR | CPAP | Back off 10s, return to IDLE |

## Files

### hms-cpap (receiver, C++)

| File | Description |
|------|-------------|
| `include/services/FysetcReceiverService.h` | Service header |
| `src/services/FysetcReceiverService.cpp` | Sync, chunk, manifest, session processing, LLM |
| `src/main.cpp` | `CPAP_SOURCE=fysetc` mode |
| `tests/services/test_FysetcReceiverService.cpp` | 8 unit tests |

### hms-cpap-fysetc (firmware, ESP-IDF C)

| File | Description |
|------|-------------|
| `main/config.h` | WiFi, MQTT, timing constants |
| `main/fsm.c/h` | State machine (11 states) |
| `main/mqtt_pusher.c/h` | MQTT publish/subscribe, manifest, fetch handler |
| `main/chunk_reader.c/h` | SD file mapping, surgical reads, fetch_file, fetch_str |
| `main/traffic_monitor.c/h` | PCNT-based CS line monitoring |
| `main/bus_arbiter.c/h` | Yield safety, idle detection |
| `main/sd_manager.c/h` | Bus MUX control, mount/unmount |

## Comparison: ezShare vs FYSETC

| | ezShare (BurstCollector) | FYSETC (FysetcReceiver) |
|---|---|---|
| Transport | HTTP polling | MQTT push |
| Latency | 2-4 min (2 burst cycles) | ~65s (real-time during therapy) |
| Session detection | File size comparison | BRP validation + bus monitoring |
| WiFi | Dedicated interface (wlan1 on Pi) | Station mode on home network |
| Hardware | WiFi SD card ($15) | Custom PCB inside CPAP |
| Realtime data | No (batch only) | Yes (BRP chunks every 60s) |
| Network disruption | Requires WiFi switching | Zero (shared bus, not network) |
