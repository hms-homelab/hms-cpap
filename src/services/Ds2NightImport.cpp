#include "services/Ds2NightImport.h"

#include "services/RemovedNights.h"
#include "utils/SessionEnd.h"

#ifdef CPAPDASH_WITH_PHILIPS
#include <cpapdash/parser/PhilipsParser.h>
#if __has_include(<cpapdash/parser/PhilipsBleCard.h>)
#include <cpapdash/parser/PhilipsBleCard.h>
#define HMS_DS2_PARSE_RECORDS 1
#endif
#endif

#include <iostream>
#include <iterator>

namespace hms_cpap {

namespace {
constexpr const char* kTag = "DS2 Bluetooth: ";
}

Ds2NightImport::Ds2NightImport(std::string data_dir, std::shared_ptr<IDatabase> db, std::string device_id,
                               std::string device_name)
    : data_dir_(std::move(data_dir)), db_(std::move(db)), device_id_(std::move(device_id)),
      device_name_(std::move(device_name)), store_(data_dir_) {}

std::vector<std::unique_ptr<CPAPSession>> Ds2NightImport::parseMachine(const std::string& serial,
                                                                       std::map<uint32_t, uint64_t>* sizes) const {
    std::vector<std::unique_ptr<CPAPSession>> out;
#ifdef CPAPDASH_WITH_PHILIPS
    const auto stored = store_.sessionRecords(serial);
    if (stored.empty()) return out;
    const auto id = store_.readIdentity(serial);
    if (sizes)
        for (const auto& r : stored) (*sizes)[r.session] += r.body.size();

#ifdef HMS_DS2_PARSE_RECORDS
    // The parser's own records-to-sessions path: the same session its card
    // reader gives for the same files.
    std::vector<cpapdash::parser::Ds2BleRecord> records;
    records.reserve(stored.size());
    for (const auto& r : stored) records.push_back({r.stream, r.session, r.body});
    cpapdash::parser::Ds2MachineIdentity ident{id.serial.empty() ? serial : id.serial, id.model, id.firmware};
    cpapdash::parser::PhilipsParseWarnings warnings;
    auto sessions = cpapdash::parser::parseDs2BleRecords(records, ident, device_id_, device_name_, &warnings);
    if (warnings.total() > 0)
        std::cerr << kTag << serial << ": " << warnings.total() << " parse warning(s)" << std::endl;
    for (auto& s : sessions) out.push_back(std::move(s));
#else
    // An older parser: one session at a time, from its files in memory.
    const std::string prop = "SN=" + (id.serial.empty() ? serial : id.serial) + "\r\nMN=" + id.model +
                             "\r\nSV=" + id.firmware + "\r\n";
    std::map<uint32_t, std::vector<const Ds2Store::StoredRecord*>> by_session;
    for (const auto& r : stored) by_session[r.session].push_back(&r);
    cpapdash::parser::PhilipsParser parser;
    for (const auto& [session, recs] : by_session) {
        std::map<std::string, std::pair<const uint8_t*, size_t>> bufs;
        bufs["PROP.TXT"] = {reinterpret_cast<const uint8_t*>(prop.data()), prop.size()};
        bool summary = false;
        for (const auto* r : recs) {
            bufs[cpapdash::ds2ble::cardFileName(r->stream, r->session)] = {r->body.data(), r->body.size()};
            summary = summary || r->stream == 1;
        }
        if (!summary) continue;
        if (auto s = parser.parseSessionFromBuffers(bufs, device_id_, device_name_)) out.push_back(std::move(s));
    }
#endif
#else
    (void)serial;
    (void)sizes;
    std::cerr << kTag << "this build has no Philips parser" << std::endl;
#endif
    return out;
}

Ds2NightImport::Result Ds2NightImport::run(const std::set<std::string>& removed) {
    Result r;
    if (!db_) return r;
    // SDD-064 D9: never a night before the paired machine's import date
    // (records from before it are not kept, but a date moved later may leave
    // some on disk).
    const long long cutoff = store_.importCutoff();
    for (const auto& serial : store_.serials()) {
        // Session number -> its files' bytes, and the files themselves.
        std::map<uint32_t, uint64_t> sizes;
        auto sessions = parseMachine(serial, &sizes);
        const auto stored = store_.sessionRecords(serial);
        // A session's number, from the time its summary starts: the session
        // whose summary began last at or before the parsed start.
        std::map<long long, uint32_t> summary_start;
        for (const auto& rec : stored) {
            if (rec.stream != 1) continue;
            try {
                const auto blocks = cpapdash::ds2ble::verifyBody(rec.body);
                if (!blocks.empty()) summary_start[blocks.front().timestamp] = rec.session;
            } catch (const std::exception&) {
                // the parser has already refused it; nothing to map
            }
        }
        for (auto& sp : sessions) {
            CPAPSession& s = *sp;
            if (!s.session_start) {
                ++r.skipped;
                continue;
            }
            const auto start = *s.session_start;
            if (isRemovedNight(removed, start)) {   // SDD-029
                ++r.skipped;
                continue;
            }
            if (cutoff > 0 &&
                std::chrono::duration_cast<std::chrono::seconds>(start.time_since_epoch()).count() < cutoff) {
                ++r.skipped;
                continue;
            }
            const long long start_s =
                std::chrono::duration_cast<std::chrono::seconds>(start.time_since_epoch()).count();
            auto at = summary_start.upper_bound(start_s);
            const uint32_t number = at == summary_start.begin() ? 0 : std::prev(at)->second;
            const std::string key = serial + "/" + std::to_string(number);
            const uint64_t bytes = sizes[number];
            const bool exists = db_->sessionExists(device_id_, start);
            if (exists) {
                auto it = seen_.find(key);
                if (it == seen_.end() || it->second == bytes) {
                    seen_[key] = bytes;
                    continue;
                }
            }
            if (!db_->saveSession(s)) {
                std::cerr << kTag << "could not save session " << number << std::endl;
                ++r.failed;
                continue;
            }
            std::vector<SessionFileRef> refs;
            for (const auto& rec : stored)
                if (rec.session == number) refs.push_back({rec.stream == 1 ? "b01" : "b02", rec.rel_path});
            db_->replaceSessionFiles(device_id_, start, refs);
            closeWithDataEnd(*db_, device_id_, start, s);   // SDD-037 D2
            seen_[key] = bytes;
            if (exists) ++r.updated; else ++r.imported;
            r.saved.push_back(s);
            std::cout << kTag << (exists ? "updated " : "imported ") << "session " << number << " ("
                      << s.duration_seconds.value_or(0) << " s of therapy)" << std::endl;
        }
    }
    // SDD-026: the nights' daily rows follow their sessions.
    if (r.imported + r.updated > 0) db_->aggregateDailySummaryFromSessions(device_id_);
    return r;
}

}  // namespace hms_cpap
