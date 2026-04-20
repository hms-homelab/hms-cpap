# QA Pass 1 — hms-cpap Fysetc TCP Server (v1.0.0)

**Date:** 2026-04-20
**Scope:** Code review of the server-side Fysetc TCP raw-sector protocol implementation
**Files reviewed:** `FysetcProtocol.h`, `FysetcTcpServer.cpp/.h`, `FysetcSectorCollectorService.cpp/.h`, `Fat32Parser.cpp/.h`, `BurstCollectorService.cpp` (fysetc integration)

---

## CRITICAL — Will crash, hang, or corrupt data

### C1. `recvMessage` recursive LOG/STATUS drain — unbounded stack growth

**File:** `FysetcTcpServer.cpp:280-290`

```cpp
// Process async messages (LOG, STATUS) transparently — recurse for next real message
if (hdr.type == fysetc::MsgType::LOG) {
    processLog(payload);
    return recvMessage(hdr, payload, timeout_ms);  // RECURSE
}
if (hdr.type == fysetc::MsgType::STATUS) {
    processStatus(payload);
    return recvMessage(hdr, payload, timeout_ms);  // RECURSE
}
```

Every LOG or STATUS message received triggers a recursive call to `recvMessage`. The Fysetc drains its 32-slot log buffer over TCP before servicing sector reads, so during reconnection bursts or heavy logging, 32+ consecutive LOG messages arrive before the first SECTOR_READ_RESP. Each recursion adds a full stack frame (including the `std::vector<uint8_t> payload` allocation, `MsgHeader`, poll structs, etc.) — roughly 200-400 bytes per frame.

On a NUC with typical 8MB default thread stack this is not immediately fatal, but:
1. The `timeout_ms` is **reset** on each recursion instead of tracking against the original deadline. A flood of LOG messages can keep the recv loop alive far past the intended 30-second timeout.
2. If the Fysetc sends LOG messages continuously (e.g., infinite log loop from a bug), this recurses until stack overflow.

**Fix:** Convert to a `while` loop:

```cpp
while (true) {
    // ... recv header + payload ...
    if (hdr.type == MsgType::LOG) { processLog(payload); continue; }
    if (hdr.type == MsgType::STATUS) { processStatus(payload); continue; }
    break;
}
```

---

### C2. `readSectors` assumes one RESP per range — protocol allows PARTIAL

**File:** `FysetcTcpServer.cpp:301-340`

```cpp
for (size_t i = 0; i < ranges.size(); ++i) {
    // ... recv one message per range ...
    if (hdr.type == MsgType::SECTOR_READ_ERR) {
        continue;  // skip this range
    }
    if (hdr.type != MsgType::SECTOR_READ_RESP) return false;
```

The loop expects **exactly one response per range**. But the Fysetc protocol spec and implementation allow:
1. **PARTIAL responses**: status=PARTIAL means only some sectors were read; the remaining sectors need re-requesting. The server discards the partial data position info.
2. **Mixed ERR/RESP**: after an ERR for range 0, the server reads the next message expecting range 1's RESP, but it could be range 2's RESP if the Fysetc skipped range 1.

The fundamental issue: responses are matched by position in the loop (`i`), not by `sector_lba` in the response. If the Fysetc sends responses out of order, skips a range, or sends PARTIAL, the server assigns sector data to the wrong file offset.

**Fix:** Match responses by `sector_lba` rather than loop index. Track which ranges have been fully delivered. Handle PARTIAL by requesting the remaining sectors.

---

### C3. `readSectors` data concatenation breaks on ERR gaps

**File:** `FysetcTcpServer.cpp:335-336`

```cpp
out_data.insert(out_data.end(), data_ptr, data_ptr + data_len);
out_delivered.push_back({sector_lba, count});
```

The caller (`FysetcSectorCollectorService::syncFile` and `makeSectorReader`) treats `out_data` as a contiguous byte stream starting from the requested LBA. But if any range returns an ERR (line 324: `continue`), the data for subsequent ranges is appended at the wrong offset. The `out_delivered` vector tracks what was actually delivered, but `makeSectorReader` only checks `delivered[0].second == count` — it does not verify that the data is contiguous or that all ranges succeeded.

For single-range requests (which `makeSectorReader` uses), this is fine. But `syncFile` sends multi-range batches where a gap from a skipped range corrupts the reconstructed file.

**Fix:** Either fail the entire request on any ERR, or use `out_delivered` to map data back to file offsets correctly.

---

### C4. `handleConnection` runs on the accept thread — blocks new connections

**File:** `FysetcTcpServer.cpp:103`

```cpp
client_fd_ = fd;
handleConnection(fd);
```

`handleConnection` runs to completion (HELLO handshake) on the `accept_thread_`. While it's running, no new `accept()` calls are made. If the HELLO handshake takes the full 10-second timeout (slow Fysetc, network issue), no new connections can be accepted during that time.

More critically: after `handleConnection` returns, the `acceptLoop` immediately goes back to `poll()` on the server socket. If the Fysetc disconnects and reconnects quickly, the old `client_fd_` is closed and replaced — but there is no synchronization with `readSectors` or `ping` which may be using the old fd from another thread.

**Fix:** Run handshake on a separate thread, or use the accept thread only for accept and let `readSectors`/`ping` handle the handshake lazily on first use.

---

## HIGH — Probable bugs that will bite in production

### H1. Race condition: `client_fd_` accessed from multiple threads without synchronization

**File:** `FysetcTcpServer.h:59`, `FysetcTcpServer.cpp` throughout

`client_fd_` is:
- Set by `acceptLoop` thread (line 100, 103)
- Read by `sendMessage` (guarded by `send_mutex_`)
- Read by `recvMessage` (guarded by `recv_mutex_`)
- Read by `isConnected()` (no mutex, used from BurstCollector thread)
- Read/written by `disconnect()` (no mutex, called from acceptLoop or external)

The `send_mutex_` and `recv_mutex_` protect the send/recv operations but **not** the `client_fd_` assignment. Timeline for a crash:

1. `readSectors` checks `client_fd_ >= 0` (true), enters send_mutex
2. `acceptLoop` accepts a new connection, calls `close(client_fd_)` on the old fd
3. `readSectors` calls `send()` on the now-closed fd — may send to wrong socket if fd was reused

**Fix:** Protect `client_fd_` with a dedicated mutex or use `std::atomic<int>`. Alternatively, use a single-threaded event loop pattern.

---

### H2. No keepalive/PING sent — stale connections undetected

**File:** `FysetcTcpServer.cpp`

The protocol spec says hms-cpap sends PING every 30 seconds. The `ping()` method exists but is **never called** from `BurstCollectorService`. The burst collector runs on a 65-second interval. Between collection cycles, there are no PING messages. If the Fysetc silently disconnects (e.g., WiFi drop without TCP RST), the server will not know until the next `readSectors` call fails — up to 65 seconds later.

Worse: TCP keepalive is not enabled on the client socket (no `SO_KEEPALIVE` setsockopt). Default TCP will hold a dead connection open indefinitely.

**Fix:** Either:
- Enable TCP keepalive with short intervals (`TCP_KEEPIDLE=30, TCP_KEEPINTVL=10, TCP_KEEPCNT=3`)
- Or spawn a background PING thread that fires every 30 seconds

---

### H3. FAT table read is one sector per cluster entry — O(n) TCP round-trips

**File:** `Fat32Parser.cpp:109-115`

```cpp
uint32_t Fat32Parser::fatEntryForCluster(uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat_start_lba_ + (fat_offset / 512);
    // ...
    if (!readSectors(fat_sector, 1, buf)) return 0x0FFFFFFF;
```

Each FAT entry lookup reads **one sector** (512 bytes = 128 FAT entries). `clusterChain()` calls this once per cluster. A 50MB EDF file with 8-sector clusters (4KB) has ~12,500 clusters. That is **12,500 individual TCP round-trips** to trace the cluster chain, each requiring a SECTOR_READ_REQ -> SECTOR_READ_RESP exchange over WiFi.

At ~20ms per round-trip (WiFi + MUX switch + read + response), that is **4 minutes** just to trace one file's cluster chain. This is the dominant bottleneck in the entire system.

**Fix:** Cache FAT sectors. Read FAT sectors in bulk (e.g., 64 sectors = 32KB = 8,192 entries per request). A simple LRU cache of the last N FAT sectors would reduce round-trips by 100x for contiguous files.

---

### H4. `syncFile` batch splitting exceeds 16-range protocol limit

**File:** `FysetcSectorCollectorService.cpp:110-122`

```cpp
for (size_t j = i; j < batch_end; ++j) {
    // Split large ranges into <=64 sectors per range
    uint32_t lba = ranges[j].lba;
    uint32_t remaining = ranges[j].count;
    while (remaining > 0) {
        uint16_t chunk = static_cast<uint16_t>(std::min(remaining, 64u));
        batch.push_back({lba, chunk});
        // ...
    }
}
```

The outer loop takes up to 16 `Fat32Parser::SectorRange`s, but the inner loop splits each one into <=64-sector chunks. A single FAT range could be hundreds of contiguous sectors (fragmented file). If a range has 256 sectors, it becomes 4 chunks. With 16 input ranges, the `batch` vector could have 64+ entries — but the protocol spec limits `range_count` to 16 per request (`uint8_t`, max 16).

The `encodeSectorReadReq` casts `ranges.size()` to `uint8_t` (in FysetcProtocol.h), silently truncating. The Fysetc firmware also validates `range_count <= MAX_RANGES_PER_REQ (16)`.

**Fix:** Limit `batch.size()` to 16 and send multiple requests if needed.

---

### H5. `decodeSectorReadResp` returns pointer into payload vector — fragile lifetime

**File:** `FysetcProtocol.h:139-145`

```cpp
inline bool decodeSectorReadResp(const uint8_t* payload, size_t len, ...,
                                  const uint8_t*& data_ptr, ...) {
    data_ptr = payload + 7;  // pointer into the caller's buffer
```

In `readSectors()`, the `payload` is a `std::vector<uint8_t>` that gets `resize`d on each `recvMessage` call. The `data_ptr` returned from `decodeSectorReadResp` points directly into this vector. The code then does:

```cpp
out_data.insert(out_data.end(), data_ptr, data_ptr + data_len);
```

This is safe **in the current code** because `data_ptr` is used immediately before `payload` is modified. But it is fragile — any refactor that stores `data_ptr` or reuses `payload` between the decode and the insert will create a dangling pointer.

---

## MEDIUM — Design issues that reduce reliability

### M1. `needs_full_sync` logic resets FAT cache but not boot_count tracking

**File:** `FysetcSectorCollectorService.cpp:183-187`

```cpp
if (tcp_.deviceState().needs_full_sync || needs_full_sync_) {
    fat_.reset();
    tracked_files_.clear();
    needs_full_sync_ = false;
}
```

After a full sync triggered by `boot_count` change, `needs_full_sync_` is cleared. But `device_state_.needs_full_sync` (set in `processHello`) stays true until the next HELLO handshake. If `collect()` is called multiple times before a reconnection, the FAT parser is re-created and `tracked_files_` is cleared every cycle — wasting all the incremental progress.

**Fix:** Clear `device_state_.needs_full_sync` after consuming it, or track it with a separate "sync completed" flag.

---

### M2. STR.edf sync sends unbounded batch — no 16-range limit

**File:** `FysetcSectorCollectorService.cpp:227-240`

The STR.edf sync builds a single `batch` from **all** sector ranges without the 16-range splitting logic used for DATALOG files:

```cpp
for (auto& r : ranges) {
    // ... split into 64-sector chunks ...
    batch.push_back({lba, chunk});
}
if (tcp_.readSectors(batch, data, delivered)) { ... }
```

STR.edf can be >2MB with a complex cluster chain. This batch could easily exceed 16 ranges. Same truncation issue as H4.

---

### M3. `writeFileData` offset gap creates zero-padded files on partial sync

**File:** `FysetcSectorCollectorService.cpp:157-165`

```cpp
if (!f.is_open() && offset > 0) {
    f.open(full_path, std::ios::binary | std::ios::out);
    // Pad to offset
    std::vector<uint8_t> pad(offset, 0);
    f.write(reinterpret_cast<const char*>(pad.data()), pad.size());
}
```

If the archive file does not exist but `offset > 0` (which should not happen in normal flow but could after a partial sync + crash), this creates a file with `offset` bytes of zeros followed by the new data. The EDF parser will see corrupt header data and potentially reject the file. A safer approach is to fail and trigger a full re-sync.

---

### M4. Unaligned `reinterpret_cast` in Fat32Parser — UB on strict-alignment architectures

**File:** `Fat32Parser.cpp:138-155` (multiple locations)

```cpp
partition_lba_ = *reinterpret_cast<uint32_t*>(&sector0[446 + 8]);
bpb_.bytes_per_sector = *reinterpret_cast<uint16_t*>(&sector0[11]);
```

The BPB fields are at odd byte offsets within the sector buffer. On x86 (NUC), unaligned access works but is technically undefined behavior. If this code is ever cross-compiled for ARM (Raspberry Pi, which is mentioned in `deploy_to_pi.sh`), unaligned 32-bit reads will SIGBUS.

**Fix:** Use `memcpy` instead:
```cpp
uint32_t tmp;
memcpy(&tmp, &sector0[446 + 8], sizeof(tmp));
partition_lba_ = tmp;
```

---

### M5. No validation of `hdr.length` underflow

**File:** `FysetcTcpServer.cpp:269`

```cpp
uint32_t payload_size = hdr.length - 4;
```

If a malformed message arrives with `hdr.length < 4`, this underflows to ~4 billion. The subsequent `if (hdr.length > 65536)` check is on `hdr.length`, not `payload_size`, so it will not catch this. The `payload.resize(payload_size)` will attempt to allocate ~4GB and throw `std::bad_alloc`, crashing the server.

**Fix:** Check `hdr.length >= 4` before the subtraction.

---

### M6. `collect()` re-reads root directory twice per cycle

**File:** `FysetcSectorCollectorService.cpp:195, 214`

`scanDatalogDir()` calls `refreshFatLayout()` which does `fat_->listDir(bpb_.root_cluster)` to find DATALOG. Then later, `collect()` does `fat_->listDir(bpb_.root_cluster)` again to find STR.edf. Each `listDir` is a full cluster-chain walk + sector reads over TCP. Root directory reads should be cached or combined.

---

### M7. No timeout on `sendMessage` — can block forever on a stalled socket

**File:** `FysetcTcpServer.cpp:215-230`

`sendMessage` uses blocking `send()` with no socket-level send timeout (`SO_SNDTIMEO`). If the Fysetc stops reading (e.g., busy with SD bus), the TCP send buffer fills and `send()` blocks indefinitely. The `send_mutex_` is held during this time, preventing any other thread from sending (including BYE on graceful shutdown).

**Fix:** Set `SO_SNDTIMEO` on the client socket, or use `poll()` with a timeout before each `send()`.

---

## Informational — Cleanup

### I1. `mergeRanges` function is dead code
`Fat32Parser.cpp` defines `mergeRanges()` which is never called.

### I2. `clusterChain` safety limit of 1,000,000 may be too high
A 1M-cluster chain at 4KB/cluster = 4GB file. ResMed SD cards are 4-8GB total. A runaway FAT chain (circular link from corruption) would loop for 1M iterations x 1 sector read each = millions of TCP round-trips before terminating. Consider a much lower safety limit (e.g., 50,000) or cycle detection.

---

## Cross-Cutting Issues (Server + Firmware Together)

### X1. Protocol version mismatch has no graceful handling
The firmware sends `fw_version` in HELLO but the server never checks it. The spec says "hms-cpap rejects connections with BYE(ERROR)" for incompatible versions, but no version check exists. A firmware downgrade to pre-TCP versions would send garbage and the server would try to decode it.

### X2. Session continuity after reconnect is fragile
When the Fysetc reconnects (same boot_count), `needs_full_sync` is false and the collector reuses its cached `tracked_files_`. But the FAT might have changed (CPAP wrote new data during disconnection). The server should at minimum re-read the DATALOG directory entries to detect new/changed files, which it does via `refreshFatLayout()` — but it reuses the old `Fat32Parser` whose internal state (cached sectors) may be stale.

### X3. No data integrity verification
Raw sectors from the Fysetc are trusted blindly. There are no checksums in the protocol. A single bit flip in a sector (SD read error, WiFi corruption, TCP stack bug) corrupts the archived file silently. The spec mentions "hms-cpap validates FAT structures (magic bytes, BPB signature) and EDF headers" but no EDF header validation exists in the collection path.

---

## Test Plan

1. **Basic connection:** Start server, boot Fysetc, verify HELLO/HELLO_ACK exchange and session log
2. **Sector read round-trip:** Request sector 0 (MBR), verify 0x55AA signature at bytes 510-511
3. **FAT parse:** Verify DATALOG directory listing matches known files on SD card
4. **File sync:** Let one full collection cycle run, compare archived EDF against known-good copy (md5sum)
5. **Reconnect:** Kill Fysetc WiFi, wait for reconnection, verify incremental sync resumes correctly
6. **Stress: LOG flood:** Have Fysetc emit 100+ log messages before first sector read, monitor server memory
7. **Range overflow:** Create a request with >16 ranges, verify the Fysetc handles truncation gracefully
8. **Long soak:** Run for 48 hours (overnight therapy), verify no file corruption, memory growth, or stale connections
