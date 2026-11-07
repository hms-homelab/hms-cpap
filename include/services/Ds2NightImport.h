#pragma once

#include "database/IDatabase.h"
#include "parsers/CpapdashBridge.h"
#include "services/Ds2Store.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace hms_cpap {

/**
 * SDD-064 D6: the DreamStation 2 records read over Bluetooth become sessions.
 *
 * Walks every machine under <data_dir>/ds2/, hands each session's summary and
 * events records to the Philips parser, and saves what it returns the way a
 * BMC session is saved (save, close at its data end, the night's daily row).
 * Rows record their files under "ds2/", which is how a Bluetooth DS2 row is
 * told apart. A session is read once per process while its files are
 * unchanged, and again when they changed (a re-read rewrote them).
 *
 * There is no SleepHQ export of these nights (SDD-064 D8): nothing here marks
 * a day for it, and their files are not under DATALOG/, which is all the
 * export reads.
 */
class Ds2NightImport {
public:
    Ds2NightImport(std::string data_dir, std::shared_ptr<IDatabase> db, std::string device_id,
                   std::string device_name);

    struct Result {
        int imported = 0;   ///< sessions stored for the first time
        int updated = 0;    ///< stored sessions whose records changed
        int skipped = 0;    ///< no usable summary (machine on, no mask-on) or removed
        int failed = 0;     ///< could not be saved
        std::vector<CPAPSession> saved;   ///< oldest first; the last is the night to publish
    };

    /// One pass over every machine. [removed] is SDD-029's set.
    Result run(const std::set<std::string>& removed);

    /// The sessions of one machine's stored records, parsed, oldest first.
    /// Public for tests.
    std::vector<std::unique_ptr<CPAPSession>> parseMachine(const std::string& serial,
                                                           std::map<uint32_t, uint64_t>* sizes = nullptr) const;

    static constexpr const char* kRelPrefix = "ds2/";

private:
    std::string data_dir_;
    std::shared_ptr<IDatabase> db_;
    std::string device_id_;
    std::string device_name_;
    Ds2Store store_;
    /// "serial/session" -> its files' total bytes when last saved.
    std::map<std::string, uint64_t> seen_;
};

}  // namespace hms_cpap
