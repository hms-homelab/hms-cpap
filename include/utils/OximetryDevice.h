#pragma once

namespace hms_cpap {

/**
 * The device_id every oximetry row is stored under.
 *
 * Oximetry is namespaced by device in the schema (oximetry_sessions.device_id
 * is NOT NULL), but there is only ever one logical ring: the BLE path, the file
 * import and the CSV upload all write the same value, and every reader filters
 * on it. Before this constant that agreement was nine unrelated string literals
 * in eight files, and a mismatch between any writer and any reader would have
 * produced an empty chart with nothing logged — the same silent-empty failure
 * as issue #18.
 *
 * NOT interchangeable with two other "o2ring" strings in this codebase:
 *   - O2RingBleClient's KNOWN_NAME_SUBSTRINGS, which matches what a ring
 *     ADVERTISES over BLE ("o2ring", "checkme", "viatom", ...). That is device
 *     discovery, it is deliberately a list of vendor names, and it has nothing
 *     to do with how a session is keyed once stored.
 *   - The "o2ring" object in config.json (AppConfig). That is a serialization
 *     contract with files already on disk; renaming this constant must never
 *     rename that key.
 */
inline constexpr const char* kOximetryDeviceId = "o2ring";

}  // namespace hms_cpap
