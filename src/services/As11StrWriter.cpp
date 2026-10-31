#include "services/As11StrWriter.h"
#include "services/As11NightWriter.h"

#include <cpapdash/parser/AirSense11Summary.h>
#include <cpapdash/parser/EDFWriter.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;
using namespace cpapdash::parser;

namespace hms_cpap {

namespace {

std::vector<uint8_t> readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool writeAtomic(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

// An EDF header field, space padded to its width.
void setField(std::vector<uint8_t>& h, size_t at, size_t width, const std::string& v) {
    for (size_t i = 0; i < width; i++) h[at + i] = i < v.size() ? static_cast<uint8_t>(v[i]) : ' ';
}

int digitsOf(const nlohmann::json& j) {
    std::string s = j.is_string() ? j.get<std::string>() : (j.is_number_integer() ? std::to_string(j.get<long>()) : "");
    std::string d;
    for (char c : s) if (c >= '0' && c <= '9') d += c;
    return d.empty() || d.size() > 9 ? 0 : std::stoi(d);
}

int64_t msOf(std::chrono::system_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

// Every kept Summary's days, a later file winning a day both carry; keyed by
// the day's start instant (the machine's noon).
std::map<int64_t, AirSense11Day> keptDays(const fs::path& root) {
    std::map<int64_t, AirSense11Day> days;
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& f : fs::directory_iterator(root / "AS11", ec))
        if (f.path().extension() == ".as11") files.push_back(f.path());
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
        std::error_code e;
        return fs::last_write_time(a, e) < fs::last_write_time(b, e);
    });
    for (const auto& f : files) {
        const auto bytes = readAll(f);
        if (bytes.empty()) continue;
        for (auto& d : AirSense11SummaryParser::parse(bytes.data(), bytes.size()))
            days[msOf(d.day_start)] = d;
    }
    return days;
}

int64_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

} // namespace

std::string strLayoutDir(const std::string& data_dir) {
    if (const char* d = std::getenv("HMS_CPAP_STR_LAYOUT_DIR"); d && *d) return d;
    return (fs::path(data_dir) / "str-layouts").string();
}

bool keepSummary(const std::string& root, const std::string& from, const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return false;
    std::string safe;
    for (char c : from) if ((c >= '0' && c <= '9') || c == '-') safe += c;
    if (safe.empty()) safe = "unknown";
    return writeAtomic(fs::path(root) / "AS11" / ("summary-" + safe + ".as11"), bytes);
}

bool machineOffsetFromSummaries(const std::string& root, int* offset_min) {
    *offset_min = 0;
    const auto days = keptDays(root);
    for (auto it = days.rbegin(); it != days.rend(); ++it) {
        if (auto off = machineOffsetFromDayStart(it->first)) { *offset_min = *off; return true; }
    }
    return false;
}

StrResult writeStrFromSummaries(const std::string& root_s, const std::string& layout_dir) {
    StrResult r;
    const fs::path root(root_s);

    std::ifstream idf(root / "Identification.json");
    if (!idf) { r.why = "no Identification.json yet"; return r; }
    std::stringstream ss; ss << idf.rdbuf();
    const auto ident = nlohmann::json::parse(ss.str(), nullptr, false);
    const auto prof = ident.is_discarded() ? nlohmann::json()
                      : ident.value("/FlowGenerator/IdentificationProfiles"_json_pointer, nlohmann::json());
    if (!prof.is_object()) { r.why = "Identification.json holds no IdentificationProfiles"; return r; }
    const auto product = prof.value("Product", nlohmann::json::object());
    const auto software = prof.value("Software", nlohmann::json::object());
    const std::string srn = product.value("SerialNumber", "");
    const int code = product.contains("ProductCode") ? digitsOf(product["ProductCode"]) : 0;
    const int mid = software.contains("PlatformIdentifier") ? digitsOf(software["PlatformIdentifier"]) : 0;
    const int vid = software.contains("VariantIdentifier") ? digitsOf(software["VariantIdentifier"]) : 0;
    if (srn.empty() || code == 0) { r.why = "the Identification names no serial or product code"; return r; }

    const fs::path layout_path = fs::path(layout_dir) / (std::to_string(code) + ".edfh");
    auto header = readAll(layout_path);
    if (header.size() < 256) {
        r.why = "no STR layout for product code " + std::to_string(code) + " (" + layout_path.string() + ")";
        return r;
    }
    const size_t header_bytes = static_cast<size_t>(std::atol(std::string(header.begin() + 184, header.begin() + 192).c_str()));
    if (header_bytes != header.size()) { r.why = "the layout for " + std::to_string(code) + " is not a whole header"; return r; }

    const auto days = keptDays(root);
    if (days.empty()) { r.why = "no Summary kept yet"; return r; }

    // Each Summary day by its calendar date on the machine's clock.
    std::map<int64_t, const AirSense11Day*> by_date;
    for (const auto& [start_ms, d] : days) {
        const int off = machineOffsetFromDayStart(start_ms).value_or(0);
        const auto wc = fixedOffsetClock(off)(start_ms);
        by_date[daysFromCivil(wc.year, wc.month, wc.day)] = &d;
    }
    const int64_t first = by_date.begin()->first, last = by_date.rbegin()->first;

    // The layout made this machine's: its recording field, the first day as
    // the start, the record count.
    const auto fd = fixedOffsetClock(0)(first * 86400000LL);
    char date[16];
    std::snprintf(date, sizeof(date), "%02d.%02d.%02d", fd.day, fd.month, fd.year % 100);
    setField(header, 8, 80, "X X X X");
    setField(header, 88, 80, resmedRecordingField(WallClock{fd.year, fd.month, fd.day, 12, 0, 0}, srn, mid, vid));
    setField(header, 168, 8, date);
    setField(header, 176, 8, "12.00.00");
    setField(header, 236, 8, std::to_string(last - first + 1));

    std::vector<uint8_t> out = header;
    for (int64_t day = first; day <= last; day++) {
        auto it = by_date.find(day);
        const STRDayValues values = it != by_date.end() ? AirSense11SummaryParser::toStrDay(*it->second) : STRDayValues{};
        const auto rec = writeResMedSTRRecord(header.data(), header.size(), {}, day, values);
        if (rec.empty()) { r.why = "the STR record writer refused day " + std::to_string(day); return r; }
        out.insert(out.end(), rec.begin(), rec.end());
        r.days++;
    }
    if (!writeAtomic(root / "STR.edf", out)) { r.why = "could not write STR.edf"; r.days = 0; return r; }
    r.written = true;
    return r;
}

} // namespace hms_cpap
