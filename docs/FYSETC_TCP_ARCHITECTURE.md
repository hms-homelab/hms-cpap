# Fysetc TCP Architecture — Raw-Sector Protocol + IDataSource Adapter

## Overview

The Fysetc WiFi SD Pro sits in the CPAP's SD card slot and reads raw sectors from the
onboard NAND flash over SPI. It serves those sectors to hms-cpap (or any client) over a
persistent TCP connection. hms-cpap parses FAT32 from the raw sectors and presents the
result through the `IDataSource` interface — the same interface `EzShareClient` implements.
The entire downstream pipeline (session discovery, checkpoint comparison, file download,
EDF parsing, DB, MQTT) runs unchanged.

This document covers the full architecture for anyone implementing a compatible client
(e.g., the CpapDash mobile app connecting directly to the Fysetc).

## System Diagram

```
┌─────────────────────────────┐
│   CPAP Machine (AirSense)   │
│   SD slot ← Fysetc card     │
│   3.3V rail, SDIO bus       │
└─────────┬───────────────────┘
          │ WiFi (2.4 GHz)
          │
┌─────────▼───────────────────┐
│   Fysetc ESP32-PICO-D4      │
│                              │
│   session_manager.c          │
│   ├─ TCP client → dials out  │
│   ├─ GPIO26 MUX control      │
│   ├─ sdmmc_read_sectors()    │
│   └─ log_forwarder → TCP     │
│                              │
│   No FAT mount               │
│   No HTTP server              │
│   No yield ISR                │
└─────────┬───────────────────┘
          │ TCP port 9000
          │ Binary protocol (length-prefixed)
          │
┌─────────▼───────────────────┐
│   hms-cpap (NUC)            │
│                              │
│   FysetcTcpServer            │
│   ├─ HELLO handshake         │
│   ├─ SECTOR_READ req/resp    │
│   ├─ LOG sink                │
│   └─ TCP keepalive           │
│                              │
│   Fat32Parser                │
│   ├─ BPB + FAT1 parsing      │
│   ├─ Cluster chain traversal  │
│   ├─ LFN directory entries    │
│   └─ 64-sector bulk cache     │
│                              │
│   FysetcDataSource           │
│   ├─ implements IDataSource   │
│   ├─ listDateFolders()        │
│   ├─ listFiles()              │
│   ├─ downloadFile()           │
│   ├─ downloadFileRange()      │
│   └─ downloadRootFile()       │
│                              │
│   ┌──────────────────────┐   │
│   │ Existing Pipeline    │   │
│   │ (unchanged)          │   │
│   │                      │   │
│   │ SessionDiscoveryService  │
│   │ BurstCollectorService    │
│   │ EDFParser                │
│   │ DatabaseService          │
│   │ DataPublisherService     │
│   └──────────────────────┘   │
└──────────────────────────────┘
```

## TCP Protocol

### Connection

- Fysetc dials out to a configured IPv4:port (default 9000)
- Reconnect with exponential backoff: 5s → 10s → 20s → 40s → 60s cap
- TCP_NODELAY enabled, TCP keepalive (idle=30s, interval=10s, count=3)

### Wire Format

Every message: `[uint32_le length][uint8 type][uint8 flags][uint16 req_id][payload...]`

`length` = byte count of type + flags + req_id + payload. Max 64 KiB.

### Message Types

| Type | Code | Direction | Purpose |
|------|------|-----------|---------|
| HELLO | 0x01 | F→H | Device serial, fw version, boot count, uptime, heap |
| HELLO_ACK | 0x02 | H→F | Server time (sets ESP32 clock), session ID |
| SECTOR_READ_REQ | 0x10 | H→F | Up to 16 sector ranges (LBA + count) |
| SECTOR_READ_RESP | 0x11 | F→H | Sector data + status (OK/PARTIAL/ERROR) |
| SECTOR_READ_ERR | 0x12 | F→H | Error code (NOT_READY/READ_FAIL/BUS_BUSY) |
| LOG | 0x20 | F→H | Forwarded ESP_LOG line (level, tag, message) |
| STATUS | 0x30 | F→H | Telemetry (uptime, heap, yields, reads, bus_hold_max) |
| PING | 0x40 | H→F | Keepalive nonce |
| PONG | 0x41 | F→H | Nonce echo + STATUS payload |
| BYE | 0x50 | either | Graceful disconnect with reason code |

Full protocol spec: `projects/hms-cpap-fysetc/docs/tcp-protocol.md`

## IDataSource Interface

```cpp
class IDataSource {
public:
    virtual ~IDataSource() = default;
    virtual std::vector<std::string> listDateFolders() = 0;
    virtual std::vector<EzShareFileEntry> listFiles(const std::string& date_folder) = 0;
    virtual bool downloadFile(const std::string& date_folder,
                              const std::string& filename,
                              const std::string& local_path) = 0;
    virtual bool downloadFileRange(const std::string& date_folder,
                                   const std::string& filename,
                                   const std::string& local_path,
                                   size_t start_byte,
                                   size_t& bytes_downloaded) = 0;
    virtual bool downloadRootFile(const std::string& filename,
                                  const std::string& local_path) = 0;
};
```

Two implementations:
- `EzShareClient` — HTTP polling of ezShare WiFi SD card
- `FysetcDataSource` — FAT32 parsing over raw TCP sectors

Both pluggable into `SessionDiscoveryService` and `BurstCollectorService`.

## FysetcDataSource Implementation

### listDateFolders()

1. Ensure FAT32 initialized (read boot sector, find DATALOG cluster)
2. Read DATALOG directory cluster chain
3. Return all 8-char directory names (YYYYMMDD format)

### listFiles(date_folder)

1. Find the date folder's cluster in the DATALOG directory
2. Read folder's directory entries
3. Convert each `Fat32DirEntry` to `EzShareFileEntry`:
   - `name` = LFN or 8.3 short name
   - `size_kb` = ceil(file_size / 1024)
   - `year/month/day/hour/minute/second` = FAT modify timestamp

### downloadFile(date_folder, filename, local_path)

1. Find file in the date folder's directory
2. Trace cluster chain from `first_cluster`
3. Convert clusters to sector LBA ranges
4. Read sectors in batches of 16 ranges (max 64 sectors per range)
5. Write to `local_path`

### downloadFileRange(date_folder, filename, local_path, start_byte, bytes_downloaded)

Same as downloadFile but:
- Uses `Fat32Parser::fileSectorRanges(first_cluster, file_size, start_byte)` to skip sectors before `start_byte`
- Appends to existing file at `start_byte` offset
- Reports bytes actually downloaded via `bytes_downloaded` output param

### downloadRootFile(filename, local_path)

Same as downloadFile but searches the root directory cluster instead of a date folder.
Used for `STR.edf`.

## Fat32Parser

Minimal read-only FAT32 parser. Operates on a `SectorReader` callback — never touches
the storage directly. The callback sends `SECTOR_READ_REQ` over TCP.

### Key features:
- **Bulk FAT sector cache**: on first FAT entry lookup, reads 64 consecutive FAT sectors
  (32 KB = 8,192 entries). Cache holds up to 128 sectors. Reduces TCP round-trips from
  thousands (one per cluster) to ~2 per file.
- **LFN support**: parses UCS-2 long filename entries alongside 8.3 short names
- **Cluster chain to sector ranges**: merges contiguous clusters into minimal range list
- **Byte-offset sector ranges**: `fileSectorRanges(cluster, size, offset)` computes exactly
  which sectors to read for a byte range — no wasted reads
- **Alignment-safe**: all struct reads use `memcpy` instead of `reinterpret_cast` (ARM-safe)

### API:
```cpp
class Fat32Parser {
public:
    using SectorReader = std::function<bool(uint32_t lba, uint32_t count, vector<uint8_t>& out)>;
    
    Fat32Parser(SectorReader reader);
    bool init();                                           // Read boot sector, parse BPB
    vector<Fat32DirEntry> listDir(uint32_t cluster);       // List directory entries
    vector<Fat32DirEntry> listPath(const string& path);    // Walk path components
    vector<uint32_t> clusterChain(uint32_t first);         // Follow FAT chain
    vector<SectorRange> clusterChainToSectorRanges(uint32_t first);
    vector<SectorRange> fileSectorRanges(uint32_t first, uint32_t size, uint32_t offset);
    void clearFatCache();                                  // Clear between cycles
};
```

## Firmware Architecture

### Boot flow

```
app_main()
  → sd_manager_init()          // GPIO26 HIGH (CPAP owns bus)
  → nvs_flash_init()
  → led_status_init()
  → nvs_store_has_wifi()?
      NO  → captive_portal_start()  // AP mode, WiFi + destination config
      YES → session_manager_run()   // connect WiFi, dial TCP, service requests
```

### Session manager states

```
BOOT → PROVISIONING (no WiFi) → CONNECTING → ONLINE
```

In ONLINE state: services SECTOR_READ_REQ, responds to PING, drains log buffer.
On disconnect: back to CONNECTING with exponential backoff.

### Sector read flow (per range)

```
1. Take bus: GPIO26 → LOW
2. Wait 10ms (MUX settle)
3. Read sectors in 8-sector chunks via sdmmc_read_sectors()
4. Release bus: GPIO26 → HIGH
5. Send SECTOR_READ_RESP with data
```

No yield ISR. No idle check. The CPAP buffers samples in RAM and flushes every 60-120s.
Multi-second bus holds are tolerated (proven by the old HTTP file server which held the bus
for entire file downloads without CPAP errors).

### Captive portal

Opens SoftAP "CpapDash-XXXX" with DNS hijack. Configures:
- WiFi SSID + password
- Destination: Local (IPv4 of hms-cpap) or Cloud (hardcoded api.cpapdash.com)
- Default port: 9000

## Implementing a Compatible Client (e.g., Mobile App)

To connect the CpapDash mobile app directly to the Fysetc:

### 1. TCP connection

- Connect to Fysetc IP on port 9000 (or discover via mDNS)
- Send HELLO, receive HELLO_ACK
- Keep connection alive with PING/PONG every 30s

### 2. Implement Fat32Parser

Port the `Fat32Parser` class to your platform (Dart, Swift, Kotlin). The parser is
~400 lines of C++ with no dependencies beyond `memcpy` and `vector`. Key operations:
- Parse BPB from sector 0 (or MBR → partition → VBR)
- Read FAT entries (4 bytes per cluster, little-endian)
- Parse directory entries (32 bytes each, handle LFN)
- Compute sector ranges from cluster chains

### 3. Implement IDataSource equivalent

Map the 5 methods to your app's data flow:
- `listDateFolders()` → populate date picker / session list
- `listFiles(folder)` → populate session file list with sizes
- `downloadFile/Range()` → fetch EDF data for parsing
- `downloadRootFile("STR.edf")` → daily summary

### 4. Session discovery

Use the same gap-based grouping as `SessionDiscoveryService`:
- Sort checkpoint files (BRP/PLD/SAD) by creation timestamp (from filename)
- Gap between file N's modify time and file N+1's creation time > 1 hour = new session
- Match CSL/EVE to sessions by timestamp proximity (within 12 hours)

### 5. Change detection

Compare BRP/PLD/SAD file sizes (in KB) against last known values:
- All unchanged → session completed, skip
- Any changed or new → session active, download all files including CSL/EVE
- CSL/EVE are NOT in the checkpoint comparison — they follow the BRP/PLD/SAD decision

### 6. EDF parsing

Parse the downloaded EDF files to extract therapy metrics. The EDF format is
standardized (European Data Format). ResMed-specific signal labels:
- BRP: Flow.40ms, Press.40ms (25 Hz breathing waveforms)
- PLD: MaskPress, EPR, Snore, FlowLim, Leak (2-second summaries)
- SAD: additional device data
- EVE: EDF+ annotations (apnea, hypopnea, RERA events with timestamps)
- CSL: session clinical summary
- STR: daily therapy records (AHI, usage hours, settings)

## Configuration

### hms-cpap config.json

```json
{
  "source": "fysetc",
  "fysetc": {
    "enabled": true,
    "listen_port": 9000,
    "listen_bind": "0.0.0.0"
  }
}
```

### Fysetc NVS (captive portal)

| Key | Value |
|-----|-------|
| wifi/ssid | WiFi network name |
| wifi/pass | WiFi password |
| dest/ip | hms-cpap IPv4 (e.g. "192.168.2.15") |
| dest/port | TCP port (default 9000) |

## Hardware Notes

- **Power**: ESP32 draws up to 300mA during WiFi TX. The CPAP's 3.3V SD rail is marginal.
  Rapid GPIO26 toggling (from yield ISR) caused current spikes → brownouts. Steady holds
  are fine — the old HTTP server held the bus for seconds without issues.
- **GPIO26**: MUX control. HIGH = CPAP owns bus, LOW = ESP32 owns bus. External pullup
  on the board defaults to CPAP during ESP32 reset.
- **GPIO33**: CS_SENSE tap on the CPAP side of the MUX. Detects CPAP bus activity.
  Currently not used for yield (too noisy). PCNT hardware counter still runs for telemetry.
- **SD bus timing**: CPAP polls every 40-100ms. Tolerates up to 200ms bus absence.
  Buffers 25 Hz flow/pressure data in RAM, flushes to SD every 60-120 seconds.

## Files Reference

### hms-cpap (public)

| File | Purpose |
|------|---------|
| `include/clients/IDataSource.h` | Interface (5 methods) |
| `include/clients/FysetcDataSource.h` | Adapter header |
| `src/clients/FysetcDataSource.cpp` | Adapter: FAT32 → IDataSource |
| `include/clients/FysetcTcpServer.h` | TCP server header |
| `src/clients/FysetcTcpServer.cpp` | TCP listener + protocol handling |
| `include/clients/FysetcProtocol.h` | Wire format codec |
| `include/parsers/Fat32Parser.h` | FAT32 parser header |
| `src/parsers/Fat32Parser.cpp` | FAT32 implementation |

### hms-cpap-fysetc (private)

| File | Purpose |
|------|---------|
| `main/main.c` | Boot: SD init → NVS → portal or session manager |
| `main/session_manager.c` | FSM + sector read handler |
| `main/tcp_client.c` | TCP connection management |
| `main/tcp_protocol.h` | Wire format (mirrors FysetcProtocol.h) |
| `main/log_forwarder.c` | ESP_LOG → TCP ring buffer |
| `main/sd_manager.c` | GPIO26 MUX + sdmmc driver |
| `main/bus_arbiter.c` | CS_SENSE ISR (currently disabled) |
| `main/captive_portal.c` | WiFi + destination config UI |
| `docs/tcp-protocol.md` | Protocol specification |
