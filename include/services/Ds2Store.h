#pragma once

#include <cpapdash/ds2ble/Record.h>
#include <cpapdash/ds2ble/SyncSession.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hms_cpap {

/**
 * SDD-064 D6, D7: what a DreamStation 2 read over Bluetooth leaves on disk.
 *
 *   <data_dir>/ds2.json                     the paired machine, and per serial
 *                                           the bookmark of every stream (0600)
 *   <data_dir>/ds2/<serial>/PROP.TXT        SN=, MN=, SV= (what the parser reads)
 *   <data_dir>/ds2/<serial>/P0/<HEX>.B01    a session's summary, as the record came
 *   <data_dir>/ds2/<serial>/P0/<HEX>.B02    its events
 *   <data_dir>/ds2/<serial>/D|E/<bm>.B03|4  the two machine logs, one file per record
 *
 * The order is what makes a read safe to interrupt: a record's file is
 * written (to a temporary name, then renamed) BEFORE its stream's bookmark is
 * saved, so a read cut anywhere re-reads at most one record, never skips one.
 */
class Ds2Store {
public:
    explicit Ds2Store(std::string data_dir);

    /// The machine hms-cpap is bonded with (one at a time, SDD-064 D4).
    struct Paired {
        std::string address;   // MAC (Linux, Windows) or the CoreBluetooth identifier
        std::string name;      // "PR BT XXXX"
        std::string id;        // the backend's handle when it was paired
        long long paired_at = 0;
        std::string serial;    // learned on its first read; "" until then
    };

    /// What is kept per machine serial.
    struct Machine {
        std::string serial, model, firmware;
        std::map<uint8_t, cpapdash::ds2ble::Bookmark> bookmarks;
        uint32_t newest_session = 0;
        long long last_read_at = 0;     // epoch s, a read that finished
        int last_records = 0;           // records that read brought
        std::string last_error;         // "" when the last read finished
        long long last_attempt_at = 0;  // epoch s, any read, finished or not
    };

    struct State {
        std::optional<Paired> paired;
        std::map<std::string, Machine> machines;   // by serial
    };

    State load() const;
    bool save(const State& s) const;

    /// The serial as a folder name: letters and digits only.
    static std::string safeSerial(const std::string& serial);
    std::string machineDir(const std::string& serial) const;
    /// <data_dir>/ds2
    std::string root() const;
    std::string jsonPath() const;

    /// Writes PROP.TXT from what the machine said about itself.
    bool writeIdentity(const cpapdash::ds2ble::Identity& id) const;
    /// Writes one record's file. Its path relative to the data directory, or
    /// "" when it could not be written (or the stream is unknown).
    std::string writeRecord(const std::string& serial, const cpapdash::ds2ble::Record& r) const;
    /// The file a record goes to, relative to the machine's folder.
    static std::string recordPath(const cpapdash::ds2ble::Record& r);

    /// Saves one stream's bookmark for [serial], keeping everything else.
    bool saveBookmark(const std::string& serial, uint8_t stream, const cpapdash::ds2ble::Bookmark& b) const;

    /// A record read back from disk: streams 1 and 2 only (the nights).
    struct StoredRecord {
        uint8_t stream = 0;
        uint32_t session = 0;
        std::vector<uint8_t> body;
        std::string rel_path;   // relative to the data directory
    };
    /// Every summary and events file of a machine, by session then stream.
    std::vector<StoredRecord> sessionRecords(const std::string& serial) const;
    /// Every serial with a folder under ds2/.
    std::vector<std::string> serials() const;
    /// SN/MN/SV from PROP.TXT.
    cpapdash::ds2ble::Identity readIdentity(const std::string& serial) const;

private:
    bool writeAtomic(const std::string& path, const std::vector<uint8_t>& bytes, bool private_file) const;
    std::string data_dir_;
};

}  // namespace hms_cpap
