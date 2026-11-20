#include "services/OximetryOutbox.h"

#include "database/SqlDialect.h"
#include "services/CardFileFeed.h"
#include "utils/OximetryDevice.h"
#include "utils/TimeCompat.h"

#include <cpapdash/parser/O2RingSParser.h>

#include <json/json.h>
#include <openssl/evp.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace hms_cpap::oximetry_outbox {

namespace {

const char* kTag = "[cpapdash-oximetry] ";

std::mutex& mu() {
    static std::mutex m;
    return m;
}
std::string& rootRef() {
    static std::string r;
    return r;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string sha256Hex(const std::string& bytes) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, bytes.data(), bytes.size());
    EVP_DigestFinal_ex(ctx, digest, &len);
    EVP_MD_CTX_free(ctx);
    std::ostringstream o;
    for (unsigned int i = 0; i < len; ++i)
        o << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    return o.str();
}

std::string quoted(const std::string& s) { return Json::valueToQuotedString(s.c_str()); }

/// Written whole or not at all: a .tmp beside it, then a rename. Called
/// under mu().
bool writeWhole(const fs::path& p, const std::string& bytes) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool readAll(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

/// "YYYY-MM-DD HH:MM:SS" (or with a 'T') as the wall-clock-shaped time point
/// every oximetry row is stored in.
bool parseDbTime(const std::string& s, std::chrono::system_clock::time_point& out) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 6 &&
        std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 6)
        return false;
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = sec;
    out = std::chrono::system_clock::from_time_t(timegm_utc(&tm));
    return true;
}

int looseInt(const Json::Value& v, int fallback = 0) {
    if (v.isNumeric()) return v.asInt();
    if (v.isString()) {
        try {
            return std::stoi(v.asString());
        } catch (...) {
        }
    }
    return fallback;
}

std::string looseText(const Json::Value& v) { return v.isString() ? v.asString() : ""; }

/// The .meta and the file, in that order, so the uploader that sees the
/// file always finds what to say about it; then the uploader is told.
std::string writeRecording(const std::string& file, const std::string& body, const Meta& m) {
    std::string where;
    {
        std::lock_guard<std::mutex> lk(mu());
        where = rootRef();
        if (where.empty()) return "";
        const fs::path dir(where);
        if (!writeWhole(dir / (file + ".meta"), metaJson(m)) || !writeWhole(dir / file, body)) {
            std::cerr << kTag << "could not write " << file << " to " << where << std::endl;
            return "";
        }
    }
    card_feed::fileWritten(where, file, "", card_feed::kSourceOximetry);
    return file;
}

}  // namespace

std::string rootFor(const std::string& data_dir) {
    return (fs::path(data_dir) / "cpapdash" / "oximetry").string();
}

void setRoot(const std::string& r) {
    std::lock_guard<std::mutex> lk(mu());
    rootRef() = r;
}

std::string root() {
    std::lock_guard<std::mutex> lk(mu());
    return rootRef();
}

std::string wireName(const std::string& filename) {
    const bool chars_ok = std::all_of(filename.begin(), filename.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '_' || c == '-';
    });
    const bool ok = !filename.empty() && filename.size() <= 64 && chars_ok && filename[0] != '.' &&
                    !endsWith(filename, ".meta") && !endsWith(filename, ".tmp") &&
                    filename.rfind("live-", 0) != 0;
    if (ok) return filename;
    return "hms-" + sha256Hex(filename).substr(0, 16);
}

std::string ringFormat(const std::string& bytes) {
    return cpapdash::parser::O2RingSParser::looksLike(reinterpret_cast<const uint8_t*>(bytes.data()),
                                                      bytes.size())
               ? "o2s"
               : "vld";
}

std::string metaJson(const Meta& m) {
    // The frame's `oxi` object as it goes: unset keys are left out.
    std::string s = "{\"name\":" + quoted(m.name) + ",\"format\":" + quoted(m.format);
    if (!m.source.empty()) s += ",\"source\":" + quoted(m.source);
    if (!m.device_name.empty()) s += ",\"device_name\":" + quoted(m.device_name);
    return s + "}";
}

std::optional<Meta> parseMeta(const std::string& text) {
    Json::Value v;
    Json::CharReaderBuilder rb;
    std::istringstream in(text);
    if (!Json::parseFromStream(rb, in, &v, nullptr) || !v.isObject()) return std::nullopt;
    Meta m;
    m.name = looseText(v["name"]);
    m.format = looseText(v["format"]);
    m.source = looseText(v["source"]);
    m.device_name = looseText(v["device_name"]);
    if (m.name.empty() || m.format.empty()) return std::nullopt;
    return m;
}

bool isLiveSpool(const std::string& file) {
    if (file.rfind("live-", 0) != 0 || !endsWith(file, ".jsonl")) return false;
    const std::string day = file.substr(5, file.size() - 5 - 6);
    return day.size() == 8 && std::all_of(day.begin(), day.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::optional<Meta> metaFor(const std::string& outbox_root, const std::string& file) {
    if (file.empty() || file[0] == '.' || file.find('/') != std::string::npos ||
        file.find('\\') != std::string::npos || endsWith(file, ".meta") || endsWith(file, ".tmp"))
        return std::nullopt;
    if (isLiveSpool(file)) {
        Meta m;
        m.name = file.substr(0, file.size() - 6);   // live-<YYYYMMDD>
        m.format = "live-jsonl";
        return m;
    }
    std::string text;
    if (!readAll(fs::path(outbox_root) / (file + ".meta"), text)) return std::nullopt;
    return parseMeta(text);
}

std::string hmsJson(const cpapdash::parser::OximetrySession& s, const std::vector<long long>* utc_s) {
    const bool utc = utc_s && utc_s->size() == s.samples.size();
    long long interval = std::llround(s.sample_interval);
    interval = std::clamp<long long>(interval, 1, 3600);
    std::string out = "{\"format\":\"hms-oximetry\",\"v\":1,\"clock\":\"";
    out += utc ? "utc" : "wall";
    out += "\",\"interval_s\":" + std::to_string(interval) + ",\"samples\":[";
    for (std::size_t i = 0; i < s.samples.size(); ++i) {
        const auto& smp = s.samples[i];
        const long long t =
            utc ? (*utc_s)[i] : static_cast<long long>(std::chrono::system_clock::to_time_t(smp.timestamp));
        // Per field: an SpO2 that is not a reading is 0 (SDD-061 D4); a pulse
        // that is not a reading is null, so it never makes the SpO2 invalid.
        const bool sp_ok = smp.spo2 > 0 && smp.spo2 <= 100 && smp.invalid_flag == 0;
        const bool hr_ok = smp.heart_rate > 0 && smp.heart_rate < 255;
        if (i) out += ",";
        out += "[" + std::to_string(t) + "," + std::to_string(sp_ok ? smp.spo2 : 0) + "," +
               (hr_ok ? std::to_string(smp.heart_rate) : std::string("null")) + "," +
               std::to_string(static_cast<int>(smp.motion)) + "]";
    }
    return out + "]}";
}

std::string liveLine(long long t, int spo2, int hr, int motion) {
    return "{\"t\":" + std::to_string(t) + ",\"spo2\":" + std::to_string(spo2) +
           ",\"hr\":" + std::to_string(hr) + ",\"motion\":" + std::to_string(motion) + "}\n";
}

std::string liveSpoolName(std::chrono::system_clock::time_point wall_clock) {
    // live_<YYYYMMDD>.vld is the day's live session in the database.
    const std::string db_name = liveOximetryFilename(wall_clock);
    return "live-" + db_name.substr(5, 8) + ".jsonl";
}

std::string stored(const cpapdash::parser::OximetrySession& row,
                   const IDatabase::OximetryOrigin& origin, const std::string& raw,
                   const std::vector<long long>* utc_s) {
    if (isLiveOximetryFilename(row.filename)) return "";   // live rows go as the spool
    Meta m;
    m.name = wireName(row.filename);
    m.source = origin.source;
    m.device_name = origin.device_name;
    if (!raw.empty()) {
        // platform SDD-061 P2: the ring's own file, byte for byte.
        m.format = ringFormat(raw);
        return writeRecording(m.name, raw, m);
    }
    if (row.samples.empty()) return "";
    m.format = "hms-json";
    return writeRecording(m.name + ".json", hmsJson(row, utc_s), m);
}

std::string liveSample(std::chrono::system_clock::time_point read_at,
                       std::chrono::system_clock::time_point wall_clock, int spo2, int hr,
                       int motion) {
    const std::string file = liveSpoolName(wall_clock);
    std::string where;
    {
        std::lock_guard<std::mutex> lk(mu());
        where = rootRef();
        if (where.empty()) return "";
        std::error_code ec;
        fs::create_directories(where, ec);
        // One whole line per write; the uploader sends only whole lines.
        std::ofstream out(fs::path(where) / file, std::ios::binary | std::ios::app);
        const std::string line =
            liveLine(static_cast<long long>(std::chrono::system_clock::to_time_t(read_at)), spo2, hr, motion);
        out.write(line.data(), static_cast<std::streamsize>(line.size()));
        if (!out) {
            std::cerr << kTag << "could not append to " << file << std::endl;
            return "";
        }
    }
    card_feed::fileWritten(where, file, "", card_feed::kSourceOximetry);
    return file;
}

int backfill(IDatabase& db, const std::string& archive_dir) {
    const std::string where = root();
    if (where.empty()) return 0;
    const DbType dt = db.dbType();
    const std::string ts = dt == DbType::POSTGRESQL ? "::text" : "";
    Json::Value rows;
    try {
        rows = db.executeQuery("SELECT id, filename, source, device_name, sample_interval, start_time" + ts +
                                   " AS start_time FROM oximetry_sessions WHERE device_id = " +
                                   sql::param(1, dt) + " ORDER BY start_time",
                               {kOximetryDeviceId});
    } catch (const std::exception& e) {
        std::cerr << kTag << "the recordings could not be listed: " << e.what() << std::endl;
        return 0;
    }
    if (!rows.isArray()) return 0;
    int written = 0;
    for (const auto& r : rows) {
        const std::string filename = looseText(r["filename"]);
        if (filename.empty() || isLiveOximetryFilename(filename)) continue;
        const std::string name = wireName(filename);
        std::error_code ec;
        if (fs::exists(fs::path(where) / (name + ".meta"), ec) ||
            fs::exists(fs::path(where) / (name + ".json.meta"), ec))
            continue;   // already in the outbox

        IDatabase::OximetryOrigin origin;
        origin.source = looseText(r["source"]);
        if (origin.source.empty()) origin.source = kOximetrySourceRing;
        origin.device_name = looseText(r["device_name"]);
        std::chrono::system_clock::time_point start{};
        const bool have_start = parseDbTime(looseText(r["start_time"]), start);

        // The ring's raw file, kept beside the card, when it is that recording.
        if (!archive_dir.empty() && have_start) {
            const fs::path raw_path = fs::path(archive_dir) / "DATALOG" / fs::path(filename).filename();
            std::string raw;
            if (fs::is_regular_file(raw_path, ec) && readAll(raw_path, raw) && !raw.empty()) {
                const auto parsed = cpapdash::parser::parseOximetryFile(
                    reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), filename);
                if (parsed && std::chrono::system_clock::to_time_t(parsed->start_time) ==
                                  std::chrono::system_clock::to_time_t(start)) {
                    cpapdash::parser::OximetrySession row;
                    row.filename = filename;
                    if (!stored(row, origin, raw).empty()) ++written;
                    continue;
                }
            }
        }

        // Else its stored samples, on the wall clock they are stored in.
        cpapdash::parser::OximetrySession row;
        row.filename = filename;
        row.sample_interval = r["sample_interval"].isNumeric() ? r["sample_interval"].asDouble() : 0;
        Json::Value samples;
        try {
            samples = db.executeQuery("SELECT timestamp" + ts +
                                          " AS ts, spo2, heart_rate, motion FROM oximetry_samples"
                                          " WHERE oximetry_session_id = " + sql::param(1, dt) +
                                          " ORDER BY timestamp",
                                      {std::to_string(looseInt(r["id"]))});
        } catch (const std::exception& e) {
            std::cerr << kTag << "the samples of " << filename << " could not be read: " << e.what()
                      << std::endl;
            continue;
        }
        for (const auto& s : samples) {
            cpapdash::parser::OximetrySample smp{};
            if (!parseDbTime(looseText(s["ts"]), smp.timestamp)) continue;
            const int sp = looseInt(s["spo2"]);
            const int hr = looseInt(s["heart_rate"]);
            smp.spo2 = (sp > 0 && sp <= 100) ? static_cast<uint8_t>(sp) : 0xFF;
            smp.heart_rate = (hr > 0 && hr < 255) ? static_cast<uint8_t>(hr) : 0;
            smp.invalid_flag = smp.spo2 == 0xFF ? 1 : 0;
            smp.motion = static_cast<uint8_t>(std::clamp(looseInt(s["motion"]), 0, 255));
            row.samples.push_back(smp);
        }
        if (row.sample_interval <= 0 && row.samples.size() > 1)
            row.sample_interval = static_cast<double>(
                std::chrono::duration_cast<std::chrono::seconds>(row.samples[1].timestamp -
                                                                 row.samples[0].timestamp)
                    .count());
        if (!stored(row, origin, "").empty()) ++written;
    }
    if (written) std::cout << kTag << written << " recording(s) stored before put in the outbox" << std::endl;
    return written;
}

}  // namespace hms_cpap::oximetry_outbox
