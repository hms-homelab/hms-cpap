# QA Findings: Local Directory & Backfill Logic

Following up on the ezShare polling review, an analysis of the `local_dir` mode and the `BackfillService` has revealed several critical issues. 

## 1. 🚨 Timezone Rollover Desync (Live Polling Dropouts)
**Location:** `SessionDiscoveryService::discoverLocalSessions` (and `discoverNewSessions`)
**Issue:** When determining if a folder should be re-scanned for live, growing files, the code uses the system's local time:
```cpp
auto now = std::chrono::system_clock::now();
std::time_t now_time = std::chrono::system_clock::to_time_t(now);
std::tm* now_tm = std::localtime(&now_time);
char today_str[9];
std::strftime(today_str, sizeof(today_str), "%Y%m%d", now_tm);
std::string today(today_str);
// ...
bool is_today = (folder == today); 
```
**Impact:** If the `hms-cpap` backend is deployed in a Docker container (which defaults to UTC) and the user resides in North America (e.g., EST / UTC-5), the server's `today` string will roll over to the next day at 7:00 PM or 8:00 PM EST. When the user sleeps, their CPAP will create a folder for their local date. The backend will evaluate `is_today` as `false` because the server's UTC date has already advanced to tomorrow. This completely breaks live data polling for the majority of the night, failing to fetch updates until the session happens to trigger the `is_recent` 48-hour fallback (which operates correctly on pure `chrono::system_clock` diffs but is less optimized).

## 2. 🚨 Duplicated Core Logic Bugs (Session Gap & Map Reference Leaks)
**Location:** `SessionDiscoveryService::groupLocalFolder`
**Issue:** The local folder parsing logic appears to have been copy-pasted from the ezShare logic and contains the exact same two fatal flaws:
* **Gap Calculation Truncation:** `std::chrono::duration_cast<std::chrono::hours>` blindly truncates any gap less than 60 minutes to `0`, ignoring the configured `SESSION_GAP_MINUTES` if it's not perfectly aligned to hour intervals.
* **CSL/EVE Map Leak:** The loops that assign `.csl` and `.eve` files to grouped sessions use `break;` instead of `csl_files.erase(csl_prefix);`. Because the final session unconditionally matches `is_last_session`, a user with multiple sessions in one date folder will have the *first* session's CSL file assigned to the *last* session, discarding the actual data for the last session.

## 3. ⚠️ Frontend Backfill Polling Race Condition
**Location:** `SettingsComponent.ts` (`pollBackfillStatus()`)
**Issue:** 
```typescript
  private pollBackfillStatus(): void {
    let polls = 0;
    const interval = setInterval(() => {
      this.api.getBackfillStatus().subscribe({
        next: (status) => {
          this.backfillProgress = status;
          if (status.status !== 'running' || polls > 200) {
```
**Impact:** `setInterval` fires a new `getBackfillStatus()` HTTP request every 3 seconds regardless of whether the previous request completed. Since the `BackfillService` blocks the database heavily while parsing and inserting large EDF files, the backend event loop will frequently delay API responses. The browser will pile up concurrent HTTP requests. When the backend finally answers them, they may resolve out-of-order. This causes the UI progress bar to "jump backwards" or incorrectly trigger the `status !== 'running'` exit condition prematurely due to an out-of-order stale payload. 
**Fix:** Replace `setInterval` with an RxJS `timer(0, 3000).pipe(switchMap(() => this.api.getBackfillStatus()))`.

## 4. ⚠️ Potential Thread Safety / Lock contention on Progress Struct
**Location:** `BackfillService::executeBackfill`
**Issue:**
```cpp
{
    std::lock_guard<std::mutex> lock(progress_mutex_);
    progress_.sessions_parsed++;
}
```
**Impact:** Inside the hot loop of `executeBackfill`, `progress_mutex_` is locked and unlocked independently at least 3-4 times per session (for `folders_done`, `sessions_parsed`, `sessions_saved`, `errors`). While functionally correct, if the frontend is hammering the `/api/backfill/status` endpoint due to the bug mentioned in #3, it will cause unnecessary lock contention on the `progress_mutex_` between the main API event loop and the worker thread. It would be cleaner to lock once and update multiple progress metrics locally, or use `std::atomic` for counters like `sessions_parsed`, `errors`, etc., since they only represent scalar telemetry.