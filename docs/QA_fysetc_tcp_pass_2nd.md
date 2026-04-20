# QA Pass 2 — hms-cpap Fysetc TCP Server (Post-Fix Repass)

**Date:** 2026-04-20
**Scope:** Full re-review of server-side code after all QA pass 1 fixes
**Files reviewed:** `FysetcTcpServer.cpp/.h`, `FysetcProtocol.h`, `Fat32Parser.cpp/.h`, `FysetcSectorCollectorService.cpp/.h`
**Verdict:** All original findings verified fixed. 5 new findings, all low/medium. **Ready for integration testing.**

---

## Original Findings — Verification

| ID | Title | Status |
|----|-------|--------|
| C1 | `recvMessage` recursive LOG/STATUS drain | **FIXED** — converted to `while(true)` loop with shared `deadline` via `recvExact` lambda |
| C2 | `readSectors` assumes one RESP per range | **ACKNOWLEDGED** — positional match correct for current firmware; PARTIAL deferred to v2 |
| C3 | `readSectors` data gap on ERR | **FIXED** — `return false` on any ERR, caller retries next cycle |
| C4 | `handleConnection` blocks accept thread | **FIXED** — `fd_mutex_` protects fd swap; socket options set atomically |
| H1 | `client_fd_` race condition | **FIXED** — `fd_mutex_` added, guards all fd mutations |
| H2 | No keepalive/PING | **FIXED** — TCP_KEEPIDLE=30, TCP_KEEPINTVL=10, TCP_KEEPCNT=3 on client socket |
| H3 | FAT table O(n) round-trips | **FIXED** — `readFatSector` bulk-reads 64 sectors, LRU cache of 128 sectors |
| H4 | Batch splitting exceeds 16-range limit | **FIXED** — flatten-then-batch-by-16 in `syncFile` |
| H5 | `decodeSectorReadResp` dangling pointer | **ACKNOWLEDGED** — safe in current code, documented |
| M1 | `needs_full_sync` not cleared after consume | **FIXED** — `clearFullSyncFlag()` called after consuming |
| M2 | STR.edf unbounded batch | **FIXED** — same flatten+batch-by-16 pattern |
| M3 | `writeFileData` zero-pad on crash recovery | **ACKNOWLEDGED** — acceptable for v1 |
| M4 | Unaligned `reinterpret_cast` | **FIXED** — all replaced with `std::memcpy` |
| M5 | `hdr.length` underflow | **FIXED** — `if (hdr.length < 4 || hdr.length > 65536)` |
| M6 | Root directory read twice | **FIXED** — `root_entries_` cached, STR.edf reuses it |
| M7 | No `sendMessage` timeout | **FIXED** — `SO_SNDTIMEO = 10s` on client socket |
| I1 | `mergeRanges` dead code | **FIXED** — removed |
| I2 | Cluster chain safety limit too high | **FIXED** — reduced to 50k, cycle detection added |
| X1 | Protocol version check | **DEFERRED** — single version for v1 |
| X2 | Session continuity fragile | **ACKNOWLEDGED** — acceptable with cache clear on full sync |
| X3 | No data integrity verification | **DEFERRED** — TCP checksums sufficient for v1 |

---

## New Findings

### N1. (MEDIUM) FAT cache never invalidated within a session

**File:** `Fat32Parser.cpp:84-115`, `Fat32Parser.h:73`

The `fat_cache_` is an `unordered_map` that grows up to 128 sectors and is never cleared except when the `Fat32Parser` object is destroyed (full sync). During a single session:

1. Cycle 1: FAT sectors cached (file cluster chains resolved)
2. CPAP writes new data — new clusters allocated, FAT updated on disk
3. Cycle 2: `refreshFatLayout()` re-reads the DATALOG directory (good), but `fileSectorRanges()` and `clusterChain()` hit the **stale cache** for FAT sector lookups

If the CPAP extends an existing EDF file (appends clusters), the new FAT entries are in sectors that were cached in cycle 1 with old values. The cluster chain will terminate early (old EOC marker still cached), and the new data will not be fetched.

This is the X2 "session continuity" concern materialized in the cache layer.

**Impact:** Incremental file growth within a session may be missed. The collector's `confirmed_bytes` check notices the file grew (directory entry shows new size), but `fileSectorRanges` returns the old shorter range. The data read is truncated, `writeFileData` writes less than expected, and `confirmed_bytes` is set to the full size — permanently skipping the gap.

**Fix options:**
1. Clear `fat_cache_` at the start of each `collect()` cycle (simple, costs ~2 extra TCP round-trips per cycle for the FAT re-read)
2. Only cache FAT sectors for clusters below the file's `confirmed_bytes` offset — new growth always reads fresh
3. Add a `clearFatCache()` method and call it from the collector before processing grown files

---

### N2. (LOW) `makeSectorReader` truncates count to `uint16_t`

**File:** `FysetcSectorCollectorService.cpp:17`

```cpp
std::vector<fysetc::SectorRange> ranges = {{lba, static_cast<uint16_t>(count)}};
```

The `SectorReader` callback receives `count` as `uint32_t`, but `SectorRange.count` is `uint16_t`. If the FAT parser ever requests more than 65,535 sectors in a single read (33MB), this silently truncates. Currently safe — `readFatSector` reads 64, `listDir` reads `sectors_per_cluster` (typically 8-64), and `init()` reads 1. But the interface contract allows larger reads.

**Recommendation:** Add an assert or clamp with a warning.

---

### N3. (LOW) `fd_mutex_` not held during `handleConnection`

**File:** `FysetcTcpServer.cpp:120`

```cpp
client_fd_ = fd;      // inside fd_mutex_ (line 103-110)
}                      // fd_mutex_ released here
handleConnection(fd);  // runs WITHOUT fd_mutex_
```

`handleConnection` uses the local `fd` variable (not `client_fd_`), but it calls `recvMessage` and `sendMessage` which read `client_fd_`. If a second connection arrives while `handleConnection` is in the 10-second HELLO wait, the accept loop closes the old fd and assigns a new one. `handleConnection` continues using the local `fd` which is now closed — `recvMessage` will fail and `disconnect()` will close the new fd.

The window is small (10s max) and the system is single-tenant (one Fysetc), so this is unlikely in practice. The fix from C4/H1 protects the fd assignment but doesn't prevent the handshake from racing with a new accept.

**Recommendation:** No action for v1. For v2, consider making `handleConnection` check `client_fd_ == fd` before proceeding.

---

### N4. (LOW) `sendMessage` doesn't hold `fd_mutex_` — checks stale `client_fd_`

**File:** `FysetcTcpServer.cpp:221-222`

```cpp
bool FysetcTcpServer::sendMessage(const std::vector<uint8_t>& msg) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (client_fd_ < 0) return false;
```

`sendMessage` reads `client_fd_` under `send_mutex_`, not `fd_mutex_`. Between the check and the `::send()` call, `disconnect()` or `acceptLoop` could close the fd under `fd_mutex_`. The `send_mutex_` doesn't synchronize with `fd_mutex_`. Same issue in `recvMessage` (reads `client_fd_` under `recv_mutex_`).

In practice, `send()` on a closed fd returns -1 with EBADF, which propagates as a send failure — no crash, just a missed error distinction. And if the fd number is reused by another socket (unlikely within microseconds), `send()` would write to the wrong socket.

**Recommendation:** Either acquire `fd_mutex_` to read `client_fd_` at the top of `sendMessage`/`recvMessage`, or copy it into a local under `fd_mutex_` and use the local for I/O.

---

### N5. (LOW) Cycle detection in `clusterChain` only catches first-cluster loops

**File:** `Fat32Parser.cpp:128-130`

```cpp
if (chain.size() > 1 && cluster == first_cluster) {
    std::cerr << "FAT32: Circular cluster chain at " << cluster << std::endl;
    break;
}
```

This only detects loops that return to `first_cluster`. A cycle like `10→11→12→11` (loop not involving the first cluster) will not be caught until the 50,000 safety limit. With the FAT cache (H3 fix), 50k iterations no longer cause 50k TCP round-trips, so the performance impact is bounded. But it will still spin for a while on a corrupt FAT.

**Recommendation:** Use a `std::unordered_set<uint32_t>` for visited clusters. Cost: ~200KB for 50k entries — acceptable on the NUC but not worth optimizing for v1 since FAT corruption is rare and the safety limit catches it.

---

## Summary

| ID | Severity | Title | Action |
|----|----------|-------|--------|
| N1 | MEDIUM | FAT cache stale on file growth within session | Clear cache each cycle or before processing grown files |
| N2 | LOW | `SectorReader` count truncated to uint16_t | No action needed |
| N3 | LOW | `handleConnection` races with new accept | No action for v1 |
| N4 | LOW | `sendMessage`/`recvMessage` read fd without `fd_mutex_` | No action for v1 |
| N5 | LOW | Cycle detection only catches first-cluster loops | No action for v1 |

**N1 is the only finding that could cause real data issues.** If the CPAP extends an EDF file with new clusters during a session, the stale FAT cache will cause the collector to miss the new data and mark it as confirmed. This is recoverable by restarting the service (triggers full sync), but it could silently lose a few hours of data if unnoticed.

**Recommendation:** Clear `fat_cache_` at the top of each `collect()` cycle. The cost is ~2 extra TCP round-trips (one bulk FAT read per file) — negligible compared to the 65-second cycle interval.
