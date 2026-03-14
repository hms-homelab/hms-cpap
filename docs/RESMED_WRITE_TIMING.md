# ResMed AirSense 10 — SD Card Write Timing

## Discovery Method

No bus access or hardware probing needed. The ezShare WiFi SD card supports HTTP range requests, and EDF files have a `num_data_records` field at bytes 236-243 in the header. By range-requesting just the first 256 bytes every second, we can detect exactly when the machine flushes new data.

### Probe Command

```bash
# From the Pi (wlan1 connected to ezShare at 192.168.4.1)
URL="http://192.168.4.1/download?file=DATALOG%5C<DATE>%5C<LATEST_BRP>.edf"
LAST=""
for i in $(seq 1 180); do
    VAL=$(curl -s -r 0-255 --max-time 3 "$URL" 2>/dev/null | dd bs=1 skip=236 count=8 2>/dev/null | tr -d ' ')
    TS=$(date '+%H:%M:%S')
    if [ "$VAL" != "$LAST" ]; then
        echo "$TS  num_records=$VAL  ** CHANGED (was $LAST)"
        LAST="$VAL"
    fi
    sleep 1
done
```

### Key Insight

The ezShare HTML directory listing only reports file sizes in **KB** (integer), so sub-KB changes are invisible. But EDF headers contain the exact `num_data_records` count, and ezShare supports `Range: bytes=0-255` requests. This gives byte-accurate change detection for the cost of a 256-byte fetch — no need to download the full file.

## Results (March 12, 2026)

Machine: ResMed AirSense 10 AutoSet (SRN 23243570851)

```
22:28:26  num_records=28  (initial read)
22:28:46  num_records=29  ** CHANGED (~20s — caught mid-cycle)
22:29:47  num_records=30  ** CHANGED (61s)
22:30:45  num_records=31  ** CHANGED (58s)
22:31:47  num_records=32  ** CHANGED (62s)
```

## Findings

- **Write interval: exactly 60 seconds** (+-2s jitter)
- **One EDF data record = 1 minute of data** (BRP: 1500 flow samples at 25 Hz)
- **Write is atomic** — value jumps between consecutive 1-second polls, no partial state observed
- **Per-flush data sizes** (from range download deltas during 5s stress test):
  - BRP: +6002 bytes (~5.86 KB) per flush
  - PLD: +542 bytes (~0.53 KB) per flush
  - SAD: +242 bytes (~0.24 KB) per flush
  - Total: ~6.8 KB per minute of therapy

## Implications for BURST_INTERVAL

| Interval | Behavior |
|----------|----------|
| 5s | 12 wasted scans per 1 useful. Works but pointless. |
| 60s | Catches every write but could land exactly on a flush. |
| **65s** | Catches every write with 5s margin. Optimal. |
| 120s | Misses every other write. Still fine for non-realtime use. |

**Current production setting: BURST_INTERVAL=65**

## Potential Optimization

Could replace the KB-based change detection in `BurstCollectorService` with a 256-byte header probe:

1. HTML directory listing to detect new/removed files (still needed)
2. Range-request EDF header (256 bytes) on known growing files
3. Compare `num_data_records` to last known value
4. Only trigger full range download when records actually increased

This would eliminate false negatives from KB truncation on small files (PLD, SAD) and false positives from ezShare returning stale KB values. Not implemented yet — current KB detection works because BRP always grows by ~6 KB per flush which is enough to flip the integer KB value.
