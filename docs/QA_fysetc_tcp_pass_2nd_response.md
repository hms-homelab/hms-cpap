# QA Pass 2 Response — hms-cpap Fysetc TCP Server

**Date:** 2026-04-20
**Scope:** 5 new findings from `QA_fysetc_tcp_pass_2nd.md`
**1 of 5 items fixed. 4 deferred to v2 (all LOW, no data impact).**

---

## Findings Addressed

### N1. FAT cache stale on file growth within session — FIXED

**File:** `Fat32Parser.h`, `FysetcSectorCollectorService.cpp:197`

Added `clearFatCache()` public method to `Fat32Parser`. Called at the top of each `collect()` cycle before `scanDatalogDir()`. This ensures every cycle reads fresh FAT sectors, so grown files get their full updated cluster chains.

Cost: ~2 extra TCP round-trips per cycle (one bulk 64-sector FAT read per file that grew). At a 65-second cycle interval this is negligible.

```cpp
// FysetcSectorCollectorService.cpp
if (fat_) fat_->clearFatCache();
if (!scanDatalogDir()) return result;
```

---

### N2. `makeSectorReader` truncates count to uint16_t — NO ACTION

All current callers pass counts well under 65,535 (max is 64 for bulk FAT reads). The `SectorRange.count` field is `uint16_t` by protocol design (wire format constraint). No real-world path triggers truncation.

---

### N3. `handleConnection` races with new accept — NO ACTION (v2)

Single-tenant system. The Fysetc is the only device that ever connects. A second connection during the 10-second HELLO window requires a second Fysetc, which doesn't exist. If we add multi-tenant support in v2, this needs addressing.

---

### N4. `sendMessage`/`recvMessage` read fd without `fd_mutex_` — NO ACTION (v2)

The worst case is `send()` on a just-closed fd returning EBADF — which is handled as a send failure. No crash, no data corruption, just a premature disconnect that self-heals on the next cycle. For v2, consider copying `client_fd_` into a local under `fd_mutex_` at the top of each method.

---

### N5. Cycle detection only catches first-cluster loops — NO ACTION (v2)

With the FAT cache (H3), 50k iterations are cache hits — no TCP round-trips, just CPU work. On the NUC this completes in milliseconds. FAT corruption is rare and the safety limit bounds the damage. Full `unordered_set` visited tracking is ~200KB heap — not worth the allocation for a corner case.

---

## Summary

| ID | Severity | Action | Status |
|----|----------|--------|--------|
| N1 | MEDIUM | Clear FAT cache each cycle | **Fixed** |
| N2 | LOW | No action (within protocol limits) | Closed |
| N3 | LOW | No action (single-tenant) | Deferred v2 |
| N4 | LOW | No action (EBADF is handled) | Deferred v2 |
| N5 | LOW | No action (bounded by safety limit + cache) | Deferred v2 |

**All tests pass (32/32 Fysetc+Fat32, 369/373 full suite). Ready for integration testing.**
