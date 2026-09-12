# QA Findings: Polling Mode & Frontend Logic (ezShare)

## 1. 🚨 Critical Precision Loss Bug in Session Gap Calculation
**Location:** `SessionDiscoveryService::groupSessionsInFolder` and `groupLocalFolder`
**Issue:** The service relies on calculating time gaps to split sessions. However, the time difference is improperly cast to `std::chrono::hours` before comparing it against a `std::chrono::minutes` threshold.
```cpp
const std::chrono::minutes SESSION_GAP_THRESHOLD(ConfigManager::getInt("SESSION_GAP_MINUTES", 60));
// ...
auto gap = std::chrono::duration_cast<std::chrono::hours>(
    checkpoints[i].timestamp - checkpoints[i-1].timestamp
);

if (gap >= SESSION_GAP_THRESHOLD) { 
    // ...
```
**Impact:** Because `duration_cast<hours>` aggressively truncates (e.g., 59 minutes becomes `0 hours` -> `0 mins`), any gap less than 60 minutes is evaluated as `0`. Conversely, if the configured threshold is set to e.g. 90 minutes, an 80-minute gap will be cast to `1 hour` (`60 mins`), which is less than 90 mins, so it will falsely skip the gap. If the gap was 119 minutes, it would also be cast to `1 hour` (`60 mins`), entirely ignoring the split. 
**Fix:** Cast to `std::chrono::minutes` instead:
```cpp
auto gap = std::chrono::duration_cast<std::chrono::minutes>(
    checkpoints[i].timestamp - checkpoints[i-1].timestamp
);
```

## 2. 🚨 Critical Reference Leak during CSL / EVE Session Assignment
**Location:** `SessionDiscoveryService::groupSessionsInFolder` and `groupLocalFolder` (Step 4)
**Issue:** When matching `_CSL.edf` (Session files) and `_EVE.edf` (Event files) to the corresponding session groups, the logic unconditionally consumes the first item in the map if it's evaluating the last session.
```cpp
bool is_last_session = (group_idx == session_groups.size() - 1);

for (const auto& [csl_prefix, csl_file] : csl_files) {
    auto csl_time = parseSessionTime(csl_prefix);
    auto time_diff = std::chrono::abs(csl_time - session_start);
    bool time_match = time_diff < std::chrono::hours(12);

    if (is_last_session || time_match) {
        session.csl_file = csl_file.name;
        // ...
        break; // BUG: We didn't remove it from the map!
    }
}
```
**Impact:** Because matched items are never `.erase()`'d from the `csl_files` or `eve_files` map, the very *first* `CSL` file inside `csl_files` is blindly assigned to the *last* session (since `is_last_session` overrides the `time_match`). This means if a user has multiple CPAP sessions in a single day, the **first session** properly matches its `CSL` file, but the **last session** inherits the exact same `CSL` file from the *first session* instead of its own, thoroughly corrupting the dataset.

## 3. ⚠️ Potential Faulty Directory Detection in `EzShareClient`
**Location:** `EzShareClient::parseDirectoryListing`
**Issue:** The regex looks for `&lt;DIR&gt;`:
```cpp
R"((?:(\d+)KB|&lt;DIR&gt;)\s+)"
```
**Impact:** Cheap ezShare devices return raw HTML directly injected into `<pre>` tags. Standard web servers encode `<DIR>` into `&lt;DIR&gt;`, but ezShare often sends literal `<DIR>` characters over the network. If the device firmware actually sends `<DIR>`, this Regex silently ignores all directory transversals and the ezShare scanner falls into an infinite loop or fails to pull date folders entirely.
**Recommendation:** Expand the group to match both: `(?:(\d+)KB|&lt;DIR&gt;|<DIR>)\s+` 

## 4. ⚠️ Growing File HTTP 206 Handling Flaw
**Location:** `EzShareClient::downloadFileRange`
**Issue:** 
```cpp
long http_code = 0;
curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);
if (http_code != 200 && http_code != 206) {
```
**Impact:** When polling a live, growing session CPAP file directly over the Wi-Fi SD card, sometimes the byte range request reaches outside the currently flushed buffer, prompting the ezShare embedded web server to unexpectedly close the connection resulting in `CURLE_PARTIAL_FILE`. In `downloadFile` you capture this elegantly, but in `downloadFileRange`, it blindly fails since it only checks `http_code`. The partially read block is lost instead of being successfully persisted.

## 5. Frontend Polling: ML State Desync Race Condition
**Location:** `SettingsComponent.ts` (`trainNow()`)
**Issue:** In the frontend, triggering the ML training kicks off an immediate `setInterval` firing every 5000ms:
```typescript
const interval = setInterval(() => {
    this.loadMlStatus();
    // ...
```
**Impact:** `loadMlStatus()` creates a new HTTP `Observable` call every 5 seconds but doesn't map or cancel pending requests (`switchMap`). If the backend takes longer than 5 seconds to reply to the status call under high load, overlapping network requests will resolve out of order causing race conditions leading to the UI flashing "Training complete" prematurely.