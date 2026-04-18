#pragma once

#include "clients/O2RingClient.h"
#include "database/IDatabase.h"
#include "parsers/CpapdashBridge.h"
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
    OximetryService(std::shared_ptr<O2RingClient> client,
                    std::shared_ptr<IDatabase> db);

    /**
     * Poll for new files, parse, and store.
     * @return true if any new data was processed
     */
    bool collectAndPublish();

private:
    std::shared_ptr<O2RingClient> client_;
    std::shared_ptr<IDatabase> db_;
    std::set<std::string> processed_files_;
};

} // namespace hms_cpap
