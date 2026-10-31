#include "services/As11StrWriter.h"
#include "services/As11NightWriter.h"
#include "services/StrLayouts.h"

#include <cpapdash/parser/AirSense11Summary.h>
#include <cpapdash/parser/EDFWriter.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

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

std::string field(const std::vector<uint8_t>& h, size_t at, size_t width) {
    if (h.size() < at + width) return {};
    std::string s(h.begin() + static_cast<long>(at), h.begin() + static_cast<long>(at + width));
    const auto e = s.find_last_not_of(' ');
    return e == std::string::npos ? std::string() : s.substr(0, e + 1);
}

long fieldLong(const std::vector<uint8_t>& h, size_t at, size_t width) {
    const std::string s = field(h, at, width);
    if (s.empty()) return -1;
    for (char c : s) if ((c < '0' || c > '9') && c != '-' && c != '+' && c != ' ') return -1;
    return std::atol(s.c_str());
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

std::string isoDay(int64_t day) {
    const auto wc = fixedOffsetClock(0)(day * 86400000LL);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", wc.year, wc.month, wc.day);
    return buf;
}

// The machine named by Identification.json, and the layout it takes.
struct Machine {
    std::string srn;
    int code = 0, mid = 0, vid = 0;
};

bool readMachine(const fs::path& root, Machine* m, std::string* why) {
    std::ifstream idf(root / "Identification.json");
    if (!idf) { *why = "no Identification.json yet"; return false; }
    std::stringstream ss; ss << idf.rdbuf();
    const auto ident = nlohmann::json::parse(ss.str(), nullptr, false);
    const auto prof = ident.is_discarded() ? nlohmann::json()
                      : ident.value("/FlowGenerator/IdentificationProfiles"_json_pointer, nlohmann::json());
    if (!prof.is_object()) { *why = "Identification.json holds no IdentificationProfiles"; return false; }
    const auto product = prof.value("Product", nlohmann::json::object());
    const auto software = prof.value("Software", nlohmann::json::object());
    m->srn = product.value("SerialNumber", "");
    m->code = product.contains("ProductCode") ? digitsOf(product["ProductCode"]) : 0;
    m->mid = software.contains("PlatformIdentifier") ? digitsOf(software["PlatformIdentifier"]) : 0;
    m->vid = software.contains("VariantIdentifier") ? digitsOf(software["VariantIdentifier"]) : 0;
    if (m->srn.empty() || m->code == 0) { *why = "the Identification names no serial or product code"; return false; }
    return true;
}

// Writes `bytes` at `at` in an existing file, then makes it durable.
bool pwriteSync(FILE* f, long at, const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return true;
    if (std::fseek(f, at, SEEK_SET) != 0) return false;
    if (std::fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size()) return false;
    if (std::fflush(f) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(f)) == 0;
#else
    return ::fsync(fileno(f)) == 0;
#endif
}

} // namespace

std::string strLayoutDir(const std::string& data_dir) {
    if (const char* d = std::getenv("HMS_CPAP_STR_LAYOUT_DIR"); d && *d) return d;
    return (fs::path(data_dir) / "str-layouts").string();
}

std::vector<uint8_t> strLayoutFor(int product_code, const std::string& layout_dir) {
    if (!layout_dir.empty()) {
        auto own = readAll(fs::path(layout_dir) / (std::to_string(product_code) + ".edfh"));
        if (own.size() >= 256) return own;
    }
    size_t n = 0;
    const BuiltinStrLayout* all = builtinStrLayouts(&n);
    for (size_t i = 0; i < n; i++)
        if (all[i].product_code == product_code)
            return std::vector<uint8_t>(all[i].bytes, all[i].bytes + all[i].size);
    return {};
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

StrHeaderInfo readStrHeader(const std::vector<uint8_t>& file) {
    StrHeaderInfo h;
    if (file.size() < 256) return h;
    const long ns = fieldLong(file, 252, 4);
    const long header_bytes = fieldLong(file, 184, 8);
    const long records = fieldLong(file, 236, 8);
    const double duration = std::atof(field(file, 244, 8).c_str());
    if (ns <= 0 || header_bytes != 256 + 256 * ns || static_cast<size_t>(header_bytes) > file.size()) return h;
    if (records < 0 || std::lround(duration) != 86400) return h;
    // dd.mm.yy, the EDF rule for the century: 85 to 99 is 1985 to 1999.
    const std::string date = field(file, 168, 8);
    if (date.size() != 8 || date[2] != '.' || date[5] != '.') return h;
    const int dd = std::atoi(date.substr(0, 2).c_str()), mm = std::atoi(date.substr(3, 2).c_str()),
              yy = std::atoi(date.substr(6, 2).c_str());
    if (dd < 1 || dd > 31 || mm < 1 || mm > 12) return h;
    h.first_day = daysFromCivil(yy >= 85 ? 1900 + yy : 2000 + yy, mm, dd);
    const size_t base = 256;
    const size_t spr_at = base + static_cast<size_t>(ns) * (16 + 80 + 8 + 8 + 8 + 8 + 8 + 80);
    for (long i = 0; i < ns; i++) {
        h.labels.push_back(field(file, base + static_cast<size_t>(i) * 16, 16));
        const long spr = fieldLong(file, spr_at + static_cast<size_t>(i) * 8, 8);
        if (spr < 0) return StrHeaderInfo{};
        h.samples_per_record.push_back(static_cast<int>(spr));
        h.record_bytes += static_cast<size_t>(spr) * 2;
    }
    if (h.record_bytes == 0) return StrHeaderInfo{};
    h.header_bytes = static_cast<size_t>(header_bytes);
    h.records = records;
    h.recording = field(file, 88, 80);
    h.ok = true;
    return h;
}

StrResult writeStrFromSummaries(const std::string& root_s, const std::string& layout_dir) {
    StrResult r;
    const fs::path root(root_s);

    Machine m;
    if (!readMachine(root, &m, &r.why)) return r;

    const auto days = keptDays(root);
    if (days.empty()) { r.why = "no Summary kept yet"; return r; }

    // Each Summary day by its calendar date on the machine's clock.
    std::map<int64_t, const AirSense11Day*> by_date;
    for (const auto& [start_ms, d] : days) {
        const int off = machineOffsetFromDayStart(start_ms).value_or(0);
        const auto wc = fixedOffsetClock(off)(start_ms);
        by_date[daysFromCivil(wc.year, wc.month, wc.day)] = &d;
    }

    const auto layout = strLayoutFor(m.code, layout_dir);
    const fs::path path = root / "STR.edf";
    std::error_code ec;
    const bool have_file = fs::exists(path, ec) && fs::file_size(path, ec) >= 256;

    if (!have_file) {
        // ── A new STR.edf, from the layout made this machine's ──
        if (layout.size() < 256) {
            r.why = "no STR layout for product code " + std::to_string(m.code) +
                    " (built in, or " + (fs::path(layout_dir) / (std::to_string(m.code) + ".edfh")).string() + ")";
            return r;
        }
        auto header = layout;
        const auto lh = readStrHeader(header);
        if (!lh.ok || lh.header_bytes != header.size()) {
            r.why = "the layout for " + std::to_string(m.code) + " is not a whole STR header";
            return r;
        }
        const int64_t first = by_date.begin()->first, last = by_date.rbegin()->first;
        const auto fd = fixedOffsetClock(0)(first * 86400000LL);
        char date[16];
        std::snprintf(date, sizeof(date), "%02d.%02d.%02d", fd.day, fd.month, fd.year % 100);
        setField(header, 8, 80, "X X X X");
        setField(header, 88, 80, resmedRecordingField(WallClock{fd.year, fd.month, fd.day, 12, 0, 0}, m.srn, m.mid, m.vid));
        setField(header, 168, 8, date);
        setField(header, 176, 8, "12.00.00");
        setField(header, 236, 8, std::to_string(last - first + 1));

        std::vector<uint8_t> out = header;
        for (int64_t day = first; day <= last; day++) {
            auto it = by_date.find(day);
            const STRDayValues values = it != by_date.end() ? AirSense11SummaryParser::toStrDay(*it->second) : STRDayValues{};
            const auto rec = writeResMedSTRRecord(header.data(), header.size(), {}, day, values);
            if (rec.empty()) { r.why = "the STR record writer refused day " + isoDay(day); return r; }
            out.insert(out.end(), rec.begin(), rec.end());
            r.days++;
        }
        if (!writeAtomic(path, out)) { r.why = "could not write STR.edf"; r.days = 0; return r; }
        r.written = r.created = true;
        r.appended = r.days;
        r.total = r.days;
        return r;
    }

    // ── An STR.edf is there: its records stay, the new days go after them ──
    const auto file = readAll(path);
    const auto fh = readStrHeader(file);
    if (!fh.ok) { r.why = "the STR.edf there is not an STR this reads: left alone"; return r; }
    if (fh.recording.find("SRN=" + m.srn) == std::string::npos) {
        r.why = "the STR.edf there names another machine (" + fh.recording + "): left alone";
        return r;
    }
    if (layout.size() >= 256) {
        const auto lh = readStrHeader(layout);
        if (lh.ok && (lh.labels != fh.labels || lh.samples_per_record != fh.samples_per_record)) {
            r.why = "the STR.edf there has " + std::to_string(fh.labels.size()) + " signals that differ from the layout of " +
                    std::to_string(m.code) + " (" + std::to_string(lh.labels.size()) + "): nothing appended";
            return r;
        }
    }
    const std::vector<uint8_t> header(file.begin(), file.begin() + static_cast<long>(fh.header_bytes));
    const size_t rec_bytes = fh.record_bytes;
    long records = fh.records;
    const size_t whole = fh.header_bytes + static_cast<size_t>(records) * rec_bytes;
    if (file.size() < whole) {
        r.why = "the STR.edf there is shorter than its " + std::to_string(records) + " records: left alone";
        return r;
    }
    int64_t last_day = fh.first_day + records - 1;

    std::vector<uint8_t> replaced;     // the last record, rewritten
    std::vector<uint8_t> appended;     // the records after it
    for (const auto& [day, d] : by_date) {
        const STRDayValues values = AirSense11SummaryParser::toStrDay(*d);
        if (day < last_day || (day == last_day && records == 0)) { r.skipped++; continue; }
        if (day == last_day) {
            // The machine revises the day it is on: the file's record of
            // that day is the base, so what the Summary does not carry stays.
            const auto at = file.begin() + static_cast<long>(fh.header_bytes + static_cast<size_t>(records - 1) * rec_bytes);
            const std::vector<uint8_t> base(at, at + static_cast<long>(rec_bytes));
            auto rec = writeResMedSTRRecord(header.data(), header.size(), base, day, values);
            if (rec.empty()) { r.why = "the STR record writer refused day " + isoDay(day); return r; }
            if (rec != base) replaced = std::move(rec);
            continue;
        }
        for (int64_t gap = last_day + 1; gap < day; gap++) {
            const auto rec = writeResMedSTRRecord(header.data(), header.size(), {}, gap, {});
            if (rec.empty()) { r.why = "the STR record writer refused day " + isoDay(gap); return r; }
            appended.insert(appended.end(), rec.begin(), rec.end());
        }
        const auto rec = writeResMedSTRRecord(header.data(), header.size(), {}, day, values);
        if (rec.empty()) { r.why = "the STR record writer refused day " + isoDay(day); return r; }
        appended.insert(appended.end(), rec.begin(), rec.end());
        last_day = day;
    }
    r.replaced = replaced.empty() ? 0 : 1;
    r.appended = static_cast<int>(appended.size() / rec_bytes);
    r.days = r.replaced + r.appended;
    r.total = static_cast<int>(records) + r.appended;
    if (r.skipped > 0)
        std::cout << "AirSense11 STR: " << r.skipped << " Summary day(s) before the file's last day ("
                  << isoDay(fh.first_day + records - 1) << ") left as the file has them" << std::endl;
    if (r.days == 0) { r.why = "every Summary day is in the STR.edf already"; return r; }

    // The records first, then the count: a reader that sees the old count
    // sees a whole file, and a torn write past the count is overwritten by
    // the next append (which starts at count * record, not at the end).
    FILE* f = std::fopen(path.string().c_str(), "r+b");
    if (!f) { r.why = "could not open STR.edf for writing"; r.days = 0; return r; }
    bool ok = true;
    if (!replaced.empty())
        ok = pwriteSync(f, static_cast<long>(fh.header_bytes + static_cast<size_t>(records - 1) * rec_bytes), replaced);
    if (ok && !appended.empty()) {
        ok = pwriteSync(f, static_cast<long>(whole), appended);
        if (ok) {
            std::vector<uint8_t> count(8, ' ');
            setField(count, 0, 8, std::to_string(r.total));
            ok = pwriteSync(f, 236, count);
        }
    }
    std::fclose(f);
    if (!ok) { r.why = "could not write into STR.edf"; r.days = 0; return r; }
    r.written = true;
    return r;
}

} // namespace hms_cpap
