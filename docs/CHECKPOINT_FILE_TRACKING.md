# HMS-CPAP Checkpoint File Tracking & Session Stop Detection

**Version:** 1.0.0
**Date:** February 9, 2026
**Status:** ✅ Production Ready

## Overview

Intelligent checkpoint file tracking system that detects when CPAP sessions end by monitoring individual file sizes on ez Share SD card, eliminating unnecessary downloads and enabling real-time session status updates in Home Assistant.

## Problem Statement

### Initial Issues

1. **Session status stuck:** session_active stayed ON in MQTT even after mask removal
2. **Unnecessary downloads:** Re-downloaded 2.5 MB every 2 minutes even when nothing changed
3. **No stop detection:** No way to detect when user stops using CPAP

### Root Causes

- EVE file presence doesn't mean session ended (EVE written during active sessions)
- Total file size unreliable (EVE/CSL can update after checkpoints stop)
- No individual file tracking (couldn't detect when checkpoint files stopped growing)

## Solution Architecture

### Key Concepts

**Checkpoint Files (BRP/PLD/SAD):**
- Written continuously during active session
- Source of truth for session activity
- Stop growing when user removes mask

**Summary Files (CSL/EVE):**
- Written separately from checkpoints
- Valuable data but NOT for stop detection
- Can update even after session ends

### Implementation

#### 1. Database Schema

**New column:** `checkpoint_files` (JSONB)

```sql
ALTER TABLE cpap_sessions ADD COLUMN checkpoint_files JSONB DEFAULT '{}'::jsonb;
```

**Example data:**
```json
{
  "20260209_003608_BRP.edf": 576,
  "20260209_003609_PLD.edf": 55,
  "20260209_003609_SAD.edf": 25,
  "20260209_021753_BRP.edf": 1660,
  "20260209_021754_PLD.edf": 153,
  "20260209_021754_SAD.edf": 68
}
```

#### 2. New Database Methods

**DatabaseService.h:**

```cpp
// Get stored checkpoint file sizes
std::map<std::string, int> getCheckpointFileSizes(
    const std::string& device_id,
    const std::chrono::system_clock::time_point& session_start);

// Update checkpoint file sizes
bool updateCheckpointFileSizes(
    const std::string& device_id,
    const std::chrono::system_clock::time_point& session_start,
    const std::map<std::string, int>& file_sizes);

// Mark session as completed
bool markSessionCompleted(
    const std::string& device_id,
    const std::chrono::system_clock::time_point& session_start);
```

**Features:**
- JSON parsing/building for JSONB storage
- 5-second timestamp tolerance for lookups
- Sets session_end to current timestamp when stopped

#### 3. MQTT Status Publishing

**DataPublisherService.h:**

```cpp
// Publish session completed status
bool publishSessionCompleted();
```

**Publishes:**
- `session_status` → "completed"
- `session_active` → "OFF"

**Called when:** Session stop detected (checkpoint files unchanged)

#### 4. Session Stop Detection Logic

**BurstCollectorService.cpp (lines 242-341):**

```
For each session discovered on ez Share:
├─ Session NOT in DB?
│  ├─ YES: Download all files
│  │      Store checkpoint sizes (BRP/PLD/SAD only)
│  │      Parse and publish
│  └─ NO: Continue to stop detection
│
├─ Get stored checkpoint sizes from DB
├─ Get current checkpoint sizes from ez Share HTML
│
├─ Compare:
│  ├─ All checkpoint files same size?
│  ├─ No new checkpoint files?
│  │
│  ├─ YES (session stopped):
│  │  ├─ Mark session_end in DB
│  │  ├─ Publish session_active=OFF to MQTT
│  │  └─ Skip download (no changes)
│  │
│  └─ NO (session active):
│     ├─ Download all files
│     ├─ Update checkpoint sizes in DB
│     └─ Parse and publish
```

## Benefits

### Performance

**Before checkpoint tracking:**
- Every cycle: Download 2.5 MB (~2 minutes at 23 KB/s)
- Cycle time: ~120 seconds
- Network usage: High (repeated downloads)

**After checkpoint tracking:**
- Active session: Download when files grow (~2 minutes)
- Stopped session: Skip download (~2 seconds)
- Cycle time: 2 seconds when stopped
- Network usage: Minimal (only when changed)

**Savings:** ~98% faster when session stopped

### Accuracy

✅ **Session stop detection:** Detects within 2 minutes (one burst cycle)
✅ **MQTT updates:** session_active reflects actual state
✅ **Database consistency:** session_end timestamp stored
✅ **No false positives:** Ignores EVE/CSL updates

## Data Flow

### New Session

```
ez Share → SessionDiscoveryService → BurstCollectorService
  ├─ Session not in DB
  ├─ Download all files (CSL, EVE, BRP×2, PLD×2, SAD×2)
  ├─ Extract checkpoint sizes: {BRP: [576, 1660], PLD: [55, 153], SAD: [25, 68]}
  ├─ Store in DB: checkpoint_files JSONB
  ├─ Parse EDF files
  └─ Publish to MQTT: session_active=ON, session_status=in_progress
```

### Active Session (files growing)

```
ez Share → SessionDiscoveryService → BurstCollectorService
  ├─ Compare checkpoint sizes
  ├─ Size changed: 1660 KB → 2100 KB (BRP file growing)
  ├─ Download updated files
  ├─ Update checkpoint sizes in DB
  ├─ Parse new data
  └─ Publish to MQTT: session_active=ON
```

### Stopped Session (mask removed)

```
ez Share → SessionDiscoveryService → BurstCollectorService
  ├─ Compare checkpoint sizes
  ├─ All sizes unchanged: {BRP: [576, 1660], PLD: [55, 153], SAD: [25, 68]}
  ├─ No new checkpoint files
  ├─ Mark session_end in DB
  ├─ Publish to MQTT: session_active=OFF, session_status=completed
  └─ Skip download (no changes)
```

## Testing

### Test Scenarios

**✅ Test 1: Normal session end**
- Start mask → session_active=ON
- Remove mask → wait 2 minutes
- Verify: session_active=OFF, session_end in DB

**✅ Test 2: Resume session**
- Remove mask → session marked COMPLETED
- Put mask back on → new checkpoint files appear
- Verify: Re-download, parse, session_active=ON

**✅ Test 3: New session 2+ hours later**
- Complete session → marked COMPLETED
- Start new session later (different session_prefix)
- Verify: Treated as new session, all files downloaded

**✅ Test 4: EVE/CSL updates after stop**
- Session stopped (checkpoints unchanged)
- EVE file updates (late event processing)
- Verify: Session stays COMPLETED (not affected by EVE/CSL)

### Logs

**Session stop detected:**
```
🛑 CPAP: Session 20260209_003608 stopped (all checkpoint files unchanged, no new files)
✅ Marked as COMPLETED in database
📤 MQTT: Publishing session completed status...
  ✓ Session status: completed
  ✓ Session active: OFF
⏭️  Skipping download (no changes)
```

**MQTT verification:**
```bash
$ mosquitto_sub -h 192.168.2.15 -t 'cpap/.../realtime/session_active' -C 1
cpap/cpap_resmed_23243570851/realtime/session_active OFF

$ mosquitto_sub -h 192.168.2.15 -t 'cpap/.../realtime/session_status' -C 1
cpap/cpap_resmed_23243570851/realtime/session_status completed
```

**Database verification:**
```sql
SELECT session_start, session_end, checkpoint_files
FROM cpap_sessions
WHERE device_id = 'cpap_resmed_23243570851'
ORDER BY session_start DESC LIMIT 1;

-- Result:
-- session_start    | session_end         | checkpoint_files
-- 2026-02-09 00:36 | 2026-02-09 08:12:46 | {"20260209_003608_BRP.edf": 576, ...}
```

## Configuration

### Environment Variables

```bash
CPAP_DEVICE_ID=cpap_resmed_23243570851
CPAP_DEVICE_NAME=ResMed AirSense 10
MQTT_BROKER=192.168.2.15
MQTT_PORT=1883
DB_HOST=localhost
DB_NAME=cpap_monitoring
```

### Burst Interval

**Default:** 120 seconds (2 minutes)

**Considerations:**
- Faster: More responsive stop detection, more network usage
- Slower: Less responsive, less network usage

**Recommended:** 120 seconds (good balance)

## Future Enhancements

### Considered but Not Implemented

**1. CSL/EVE separate tracking** (not needed yet)
- Could track CSL/EVE sizes separately
- Download if changed even when checkpoints stopped
- Benefit: Capture late event updates
- Decision: Wait for real use case

**2. Configurable stop detection threshold** (premature)
- Allow "X burst cycles unchanged = stopped"
- Benefit: More confidence before marking stopped
- Decision: Current logic (1 cycle) works fine

**3. WebSocket real-time updates** (out of scope)
- Replace polling with WebSocket connection to ez Share
- Benefit: Instant updates when files change
- Decision: ez Share doesn't support WebSockets

## Maintenance

### Monitoring

**Check service status:**
```bash
systemctl status hms-cpap
journalctl -u hms-cpap -f
```

**Check database:**
```sql
-- Sessions without checkpoint_files (migration artifact)
SELECT COUNT(*) FROM cpap_sessions WHERE checkpoint_files = '{}'::jsonb;

-- Recent session status
SELECT session_start, session_end, checkpoint_files
FROM cpap_sessions
WHERE device_id = 'cpap_resmed_23243570851'
ORDER BY session_start DESC LIMIT 5;
```

### Troubleshooting

**Problem:** session_active stuck ON after mask removal

**Diagnosis:**
```bash
# Check MQTT
mosquitto_sub -h 192.168.2.15 -t 'cpap/.../realtime/session_active' -C 1

# Check database
psql -d cpap_monitoring -c "SELECT session_end FROM cpap_sessions WHERE ..."
```

**Solution:** Restart HMS-CPAP (will republish status on next cycle)

---

**Problem:** Session not detected as stopped

**Diagnosis:**
```bash
# Check logs for stop detection
journalctl -u hms-cpap | grep "stopped"

# Check checkpoint sizes in DB
psql -d cpap_monitoring -c "SELECT checkpoint_files FROM cpap_sessions WHERE ..."
```

**Solution:** Verify ez Share accessible, checkpoint_files populated

## Contributors

- User: Architecture design, requirements, testing
- Claude: Implementation, documentation

## License

Part of HMS-CPAP service (maestro_hub project)

## See Also

- `HMS_CPAP_QUICKREF.md` - Service overview
- `DatabaseService.h` - Database API
- `DataPublisherService.h` - MQTT publishing API
- `BurstCollectorService.cpp` - Main collection logic
