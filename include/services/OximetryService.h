#pragma once

#include "clients/IO2RingClient.h"
#include "database/IDatabase.h"
#include "parsers/CpapdashBridge.h"
#include <map>
#include <memory>
#include <set>
#include <string>

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

    /**
     * Poll live SpO2/HR from ring (connect on demand via mule).
     * @return live reading, check .valid before using
     */
    IO2RingClient::LiveReading pollLive();

    /** Get last live reading (from most recent pollLive call). */
    const IO2RingClient::LiveReading& getLastLive() const { return last_live_; }

private:
    std::shared_ptr<IO2RingClient> client_;
    std::shared_ptr<IDatabase> db_;
    std::string cpap_device_id_;
    std::set<std::string> processed_files_;
    /// SDD-029: ring files parsed once and found on a removed night, by
    /// filename -> night. Not downloaded again while that night stays removed;
    /// fetched again once a Reparse restores it.
    std::map<std::string, std::string> removed_files_;
    IO2RingClient::LiveReading last_live_;
};

} // namespace hms_cpap
