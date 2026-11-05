#pragma once

#include "clients/IO2RingClient.h"
#include "database/IDatabase.h"
#include "parsers/CpapdashBridge.h"
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace hms_cpap {

/**
 * OximetryService - Orchestrator for O2 Ring oximetry data collection.
 *
 * Polls the O2 Ring mule for new .vld files, parses them with VLDParser,
 * stores results in the database, and tracks which files have been processed.
 */
class OximetryService {
public:
    /// [cpap_device_id] is whose removed nights (SDD-029) the ring's files
    /// are checked against; empty checks none.
    OximetryService(std::shared_ptr<IO2RingClient> client,
                    std::shared_ptr<IDatabase> db,
                    std::string cpap_device_id = "");

    /**
     * Poll for new files, parse, and store.
     * @return true if any new data was processed
     */
    bool collectAndPublish();

    /// Where the ring's raw file is kept: <archive>/DATALOG/<filename>, beside
    /// the CPAP nights but flat, since every folder in DATALOG is read as a
    /// night. Asked on each save so a Settings change applies at once; empty
    /// keeps nothing.
    void setArchiveDirProvider(std::function<std::string()> p) { archive_dir_ = std::move(p); }

    /// Writes [bytes] to <archive_dir>/DATALOG/<basename of filename>. Returns
    /// false (and writes nothing) for an empty archive_dir or name. An existing
    /// copy with identical bytes is left alone; a different one is replaced.
    /// Public so it can be tested.
    static bool retainRawFile(const std::string& archive_dir, const std::string& filename,
                              const std::vector<uint8_t>& bytes);

    /**
     * Poll live SpO2/HR from ring (connect on demand via mule).
     * @return live reading, check .valid before using
     */
    IO2RingClient::LiveReading pollLive();

    /** Get last live reading (from most recent pollLive call). */
    const IO2RingClient::LiveReading& getLastLive() const { return last_live_; }

    /// What the last talk with the ring found, for GET /api/oximetry/status.
    /// Recorded when this service polls or reads the ring; reading it never
    /// talks to the ring. checked_at 0: not asked since this start.
    struct Facts {
        long long   checked_at = 0;
        bool        connected = false;
        int         battery = -1;          // -1: not known
        long long   last_read_at = 0;      // the last file stored
        std::string last_file;
        IO2RingClient::LiveReading live;
    };
    Facts facts() const;

private:
    void noteFacts(bool connected);
    mutable std::mutex facts_mtx_;
    Facts facts_;
    std::shared_ptr<IO2RingClient> client_;
    std::shared_ptr<IDatabase> db_;
    std::string cpap_device_id_;
    std::function<std::string()> archive_dir_;
    std::set<std::string> processed_files_;
    /// SDD-029: ring files parsed once and found on a removed night, by
    /// filename -> night. Not downloaded again while that night stays removed;
    /// fetched again once a Reparse restores it.
    std::map<std::string, std::string> removed_files_;
    IO2RingClient::LiveReading last_live_;
};

} // namespace hms_cpap
