#include "services/As11NightWriter.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using cpapdash::parser::WallClock;

namespace hms_cpap {

namespace {

// ── civil time, no libc zone involved ──

int64_t floorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

int64_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

WallClock civilFromSeconds(int64_t secs) {
    const int64_t days = floorDiv(secs, 86400);
    const int64_t sod = secs - days * 86400;
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    WallClock wc;
    wc.day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    wc.month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    wc.year = static_cast<int>(yoe + era * 400 + (wc.month <= 2));
    wc.hour = static_cast<int>(sod / 3600);
    wc.minute = static_cast<int>((sod % 3600) / 60);
    wc.second = static_cast<int>(sod % 60);
    return wc;
}

bool writeFileAtomic(const fs::path& path, const std::vector<uint8_t>& bytes) {
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

// Appends one length-prefixed record and makes it durable before returning:
// a source may free its copy of the block once this returns.
bool appendRecord(const fs::path& path, const std::vector<uint8_t>& rec) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    FILE* f = std::fopen(path.string().c_str(), "ab");
    if (!f) return false;
    const uint32_t n = static_cast<uint32_t>(rec.size());
    const uint8_t len[4] = {static_cast<uint8_t>(n), static_cast<uint8_t>(n >> 8),
                            static_cast<uint8_t>(n >> 16), static_cast<uint8_t>(n >> 24)};
    bool ok = std::fwrite(len, 1, 4, f) == 4 && std::fwrite(rec.data(), 1, rec.size(), f) == rec.size() &&
              std::fflush(f) == 0;
#ifdef _WIN32
    ok = ok && _commit(_fileno(f)) == 0;
#else
    ok = ok && ::fsync(fileno(f)) == 0;
#endif
    return std::fclose(f) == 0 && ok;
}

int intOf(const nlohmann::json& j) {
    if (j.is_number_integer()) return j.get<int>();
    if (j.is_string()) {
        const auto& s = j.get_ref<const std::string&>();
        if (!s.empty() && std::all_of(s.begin(), s.end(), ::isdigit)) return std::stoi(s);
    }
    return 0;
}

// The machine's model and variant ids, from the Identification beside the
// night (0 when there is none): they go in the EVE/CSL recording field.
void modelOf(const fs::path& root, int* mid, int* vid) {
    *mid = 0; *vid = 0;
    std::ifstream in(root / "Identification.json");
    if (!in) return;
    std::stringstream ss; ss << in.rdbuf();
    const auto j = nlohmann::json::parse(ss.str(), nullptr, false);
    if (j.is_discarded()) return;
    const auto sw = j.value("/FlowGenerator/IdentificationProfiles/Software"_json_pointer, nlohmann::json());
    if (!sw.is_object()) return;
    if (sw.contains("PlatformIdentifier")) *mid = intOf(sw["PlatformIdentifier"]);
    if (sw.contains("VariantIdentifier")) *vid = intOf(sw["VariantIdentifier"]);
}

// The machine's event name as the card's EVE writes it; "" for a CSR mark.
std::string eveText(const std::string& e) {
    if (e == "ObstructiveApneaEnd") return "Obstructive Apnea";
    if (e == "CentralApneaEnd") return "Central Apnea";
    if (e == "ApneaEnd") return "Apnea";
    if (e == "HypopneaEnd") return "Hypopnea";
    if (e == "ReraEnd") return "Arousal";
    return e;
}
bool isCsr(const std::string& e) { return e.rfind("Csr", 0) == 0 || e.rfind("CSR", 0) == 0; }

} // namespace

// ── clocks ──

WallClockFn fixedOffsetClock(int offset_min_east) {
    return [offset_min_east](int64_t utc_ms) {
        return civilFromSeconds(floorDiv(utc_ms, 1000) + static_cast<int64_t>(offset_min_east) * 60);
    };
}

WallClockFn hostLocalClock() {
    return [](int64_t utc_ms) {
        const std::time_t t = static_cast<std::time_t>(floorDiv(utc_ms, 1000));
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        WallClock wc;
        wc.year = tm.tm_year + 1900; wc.month = tm.tm_mon + 1; wc.day = tm.tm_mday;
        wc.hour = tm.tm_hour; wc.minute = tm.tm_min; wc.second = tm.tm_sec;
        return wc;
    };
}

std::optional<int> machineOffsetFromDayStart(int64_t day_start_utc_ms) {
    const int64_t mod = floorDiv(day_start_utc_ms, 60000) % 1440;
    int off = 720 - static_cast<int>(mod < 0 ? mod + 1440 : mod);
    if (off > 840) off -= 1440;
    if (off <= -720) off += 1440;
    if (off % 15 != 0) return std::nullopt;
    return off;
}

std::string sleepDayFolder(const WallClock& wc) {
    int64_t days = daysFromCivil(wc.year, wc.month, wc.day);
    if (wc.hour < 12) days -= 1;
    const WallClock d = civilFromSeconds(days * 86400);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d", d.year, d.month, d.day);
    return buf;
}

// ── the writer ──

As11NightWriter::As11NightWriter(std::string data_dir, WallClockFn default_clock)
    : data_dir_(std::move(data_dir)), default_clock_(std::move(default_clock)) {}

void As11NightWriter::setClock(const std::string& serial, WallClockFn clock) {
    std::lock_guard<std::mutex> lk(mtx_);
    clocks_[serial] = std::move(clock);
}

WallClockFn As11NightWriter::clockFor(const std::string& serial) const {
    auto it = clocks_.find(serial);
    return it != clocks_.end() ? it->second : default_clock_;
}

std::string As11NightWriter::rootFor(const std::string& serial) const {
    return (fs::path(data_dir_) / "as11" / (serial.empty() ? "unknown" : serial)).string();
}

std::vector<as11::StreamBlock> As11NightWriter::readSegment(const std::string& file) const {
    std::vector<as11::StreamBlock> out;
    std::ifstream in(file, std::ios::binary);
    if (!in) return out;
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::set<int64_t> seen;
    for (size_t at = 0; at + 4 <= buf.size();) {
        const uint32_t n = buf[at] | (buf[at + 1] << 8) | (buf[at + 2] << 16) |
                           (static_cast<uint32_t>(buf[at + 3]) << 24);
        at += 4;
        if (at + n > buf.size()) break;   // a torn last record: dropped
        if (auto b = as11::decodeCds1(buf.data() + at, n); b && seen.insert(b->start_ms).second)
            out.push_back(std::move(*b));
        at += n;
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.start_ms < b.start_ms; });
    return out;
}

void As11NightWriter::load(const std::string& serial, Machine& m) {
    m.loaded = true;
    const fs::path root = rootFor(serial);
    std::error_code ec;
    if (fs::exists(root / "stream", ec)) {
        for (const auto& night : fs::directory_iterator(root / "stream", ec)) {
            if (!night.is_directory()) continue;
            for (const auto& f : fs::directory_iterator(night.path(), ec)) {
                if (f.path().extension() != ".cds1s") continue;
                Segment s;
                s.night = night.path().filename().string();
                s.file = f.path().string();
                for (const auto& b : readSegment(s.file)) {
                    if (s.starts.empty()) s.start_ms = b.start_ms;
                    s.starts.insert(b.start_ms);
                    s.end_ms = std::max(s.end_ms, b.endMs());
                }
                if (!s.starts.empty()) { s.written_end = s.end_ms; m.segments.push_back(std::move(s)); }
            }
        }
    }
    std::sort(m.segments.begin(), m.segments.end(),
              [](const Segment& a, const Segment& b) { return a.start_ms < b.start_ms; });
    std::ifstream ev(root / "stream" / "events.jsonl");
    for (std::string line; std::getline(ev, line);) {
        const auto j = nlohmann::json::parse(line, nullptr, false);
        if (j.is_object()) m.event_keys.insert(j.value("reportTime", "") + "|" + j.value("event", ""));
    }
}

As11NightWriter::Machine& As11NightWriter::machine(const std::string& serial) {
    auto& m = machines_[serial];
    if (!m.loaded) load(serial, m);
    return m;
}

void As11NightWriter::onBlock(const as11::StreamBlock& b) {
    if (b.start_ms <= 0) return;
    std::lock_guard<std::mutex> lk(mtx_);
    Machine& m = machine(b.serial);

    Segment* seg = nullptr;
    for (auto& s : m.segments)
        if (b.start_ms >= s.start_ms - kSegmentGapMs && b.start_ms < s.end_ms + kSegmentGapMs) seg = &s;
    if (seg && seg->starts.count(b.start_ms)) return;   // a resend: kept once
    if (!seg) {
        Segment s;
        s.night = sleepDayFolder(clockFor(b.serial)(b.start_ms));
        s.file = (fs::path(rootFor(b.serial)) / "stream" / s.night /
                  (std::to_string(b.start_ms) + ".cds1s")).string();
        s.start_ms = b.start_ms;
        s.written_end = b.start_ms;   // the first live write after 10 s of it
        m.segments.push_back(std::move(s));
        std::sort(m.segments.begin(), m.segments.end(),
                  [](const Segment& x, const Segment& y) { return x.start_ms < y.start_ms; });
        for (auto& s2 : m.segments) if (s2.start_ms == b.start_ms && s2.starts.empty()) seg = &s2;
    }
    if (!appendRecord(seg->file, as11::encodeCds1(b))) {
        std::cerr << "AirSense11 night: could not store a block for " << b.serial << std::endl;
        return;
    }
    seg->starts.insert(b.start_ms);
    seg->start_ms = std::min(seg->start_ms, b.start_ms);
    seg->end_ms = std::max(seg->end_ms, b.endMs());
    m.nights_touched.insert(seg->night);

    // A segment this block can no longer join is closed: its files are final.
    for (auto& s : m.segments) {
        if (&s == seg || s.written_end == s.end_ms) continue;
        if (b.start_ms >= s.end_ms + kSegmentGapMs) { writeSegment(b.serial, s); s.written_end = s.end_ms; }
    }
    // The open segment: live, every 10 s of stream.
    if (seg->end_ms - seg->written_end >= kLiveEveryMs) {
        writeSegment(b.serial, *seg);
        seg->written_end = seg->end_ms;
    }
}

void As11NightWriter::onEvent(const std::string& serial, const as11::RespEvent& e) {
    if (e.event.empty() || e.reportTime.empty()) return;
    std::lock_guard<std::mutex> lk(mtx_);
    Machine& m = machine(serial);
    if (!m.event_keys.insert(e.reportTime + "|" + e.event).second) return;
    nlohmann::ordered_json j;
    j["event"] = e.event;
    j["reportTime"] = e.reportTime;
    j["durationSeconds"] = e.durationSeconds;
    j["backdateSeconds"] = e.backdateSeconds;
    const fs::path path = fs::path(rootFor(serial)) / "stream" / "events.jsonl";
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::app);
    out << j.dump() << "\n";
}

void As11NightWriter::onStreamEnd(const std::string& serial) {
    std::lock_guard<std::mutex> lk(mtx_);
    Machine& m = machine(serial);
    for (auto& s : m.segments) {
        if (s.written_end == s.end_ms || !m.nights_touched.count(s.night)) continue;
        writeSegment(serial, s);
        s.written_end = s.end_ms;
    }
    for (const auto& night : m.nights_touched) writeEvents(serial, night);
    m.nights_touched.clear();
}

void As11NightWriter::writeNight(const std::string& serial, const std::string& night) {
    std::lock_guard<std::mutex> lk(mtx_);
    Machine& m = machine(serial);
    for (auto& s : m.segments) {
        if (s.night != night) continue;
        writeSegment(serial, s);
        s.written_end = s.end_ms;
    }
    writeEvents(serial, night);
}

void As11NightWriter::writeSegment(const std::string& serial, const Segment& seg) {
    using namespace cpapdash::parser;
    const auto blocks = readSegment(seg.file);
    if (blocks.empty()) return;
    const int64_t t0 = blocks.front().start_ms;
    int64_t t_end = t0;
    for (const auto& b : blocks) t_end = std::max(t_end, b.endMs());
    const WallClock wc = clockFor(serial)(t0);
    const fs::path dir = fs::path(rootFor(serial)) / "DATALOG" / seg.night;
    const std::string srn = blocks.front().serial;

    // BRP: 25 Hz, a gap inside the segment filled with zeros so later
    // samples stay at their time.
    StreamedBRP brp;
    brp.start = wc;
    brp.serial = srn;
    int64_t cursor = t0;
    for (const auto& b : blocks) {
        const auto* fl = b.channel(as11::kFlow);
        const auto* pr = b.channel(as11::kMaskPressure);
        if (!fl && !pr) continue;
        const int iv = b.interval_ms > 0 ? b.interval_ms : as11::kIntervalMs;
        if (b.start_ms > cursor) {
            const size_t gap = static_cast<size_t>((b.start_ms - cursor) / iv);
            brp.flow_lpm.insert(brp.flow_lpm.end(), gap, 0.0);
            brp.press_cmh2o.insert(brp.press_cmh2o.end(), gap, 0.0);
            cursor += static_cast<int64_t>(gap) * iv;
        }
        const size_t n = std::max(fl ? fl->values.size() : 0, pr ? pr->values.size() : 0);
        // Samples before the cursor overlap what is already laid: skipped.
        const size_t skip = b.start_ms < cursor ? static_cast<size_t>((cursor - b.start_ms) / iv) : 0;
        for (size_t i = skip; i < n; i++) {
            const double f = fl && i < fl->values.size() ? as11::physical(as11::kFlow, fl->values[i]) : NAN;
            const double p = pr && i < pr->values.size() ? as11::physical(as11::kMaskPressure, pr->values[i]) : NAN;
            brp.flow_lpm.push_back(std::isnan(f) ? 0.0 : f * 60.0);   // L/s to L/min
            brp.press_cmh2o.push_back(std::isnan(p) ? 0.0 : p);
        }
        cursor = std::max(cursor, b.start_ms + static_cast<int64_t>(n) * iv);
    }
    if (brp.flow_lpm.empty()) return;
    writeFileAtomic(dir / resmedFileName(wc, "BRP"), writeResMedBRP(brp));

    // The slow channels on their own grid: PLD every 2 s, SAD every 1 s. A
    // value covers its 2 s; where none came, NaN (written as the gap).
    const size_t slots2 = static_cast<size_t>((t_end - t0 + 1999) / 2000);
    std::map<uint8_t, std::vector<double>> pld;
    std::vector<double> spo2(slots2 * 2, NAN), pulse(slots2 * 2, NAN);
    bool have_pld = false, have_sad = false;
    for (const auto& b : blocks) {
        for (const auto& c : b.channels) {
            if (c.cadence != 1) continue;
            for (size_t i = 0; i < c.values.size(); i++) {
                const int64_t t = b.start_ms + static_cast<int64_t>(i) * as11::kSlowCadenceMs;
                const double v = as11::physical(static_cast<as11::Channel>(c.id), c.values[i]);
                if (std::isnan(v) || t < t0) continue;
                if (c.id >= 3 && c.id <= 11) {
                    auto& col = pld[c.id];
                    if (col.empty()) col.assign(slots2, NAN);
                    const size_t k = static_cast<size_t>((t - t0) / 2000);
                    if (k < col.size()) { col[k] = v; have_pld = true; }
                } else if (c.id == as11::kSpO2 || c.id == as11::kHeartRate) {
                    auto& col = c.id == as11::kSpO2 ? spo2 : pulse;
                    const size_t k = static_cast<size_t>((t - t0) / 1000);
                    for (size_t s = k; s < k + 2 && s < col.size(); s++) col[s] = v;
                    have_sad = true;
                }
            }
        }
    }
    if (have_pld) {
        StreamedPLD p;
        p.start = wc;
        p.serial = srn;
        auto col = [&](uint8_t id) { auto it = pld.find(id); return it == pld.end() ? std::vector<double>{} : it->second; };
        p.mask_press_cmh2o = col(as11::kMaskPressure2s);
        p.press_cmh2o = col(as11::kInspPressure2s);
        p.epr_press_cmh2o = col(as11::kExpPressure2s);
        p.leak_lps = col(as11::kLeak);
        p.resp_rate = col(as11::kRespRate);
        p.tidal_volume_l = col(as11::kTidalVolume);
        p.minute_vent_lpm = col(as11::kMinuteVent);
        p.snore = col(as11::kSnore);
        p.flow_limitation = col(as11::kFlowLimitation);
        writeFileAtomic(dir / resmedFileName(wc, "PLD"), writeResMedPLD(p));
    }
    if (have_sad) {
        StreamedSAD s;
        s.start = wc;
        s.serial = srn;
        s.spo2_pct = spo2;
        s.pulse_bpm = pulse;
        writeFileAtomic(dir / resmedFileName(wc, "SAD"), writeResMedSAD(s));
    }
}

void As11NightWriter::writeEvents(const std::string& serial, const std::string& night) {
    using namespace cpapdash::parser;
    Machine& m = machines_[serial];
    std::vector<const Segment*> segs;
    for (const auto& s : m.segments)
        if (s.night == night && !s.starts.empty()) segs.push_back(&s);
    if (segs.empty()) return;
    const Segment* head = segs.front();
    const fs::path root = rootFor(serial);

    ResMedAnnotationFile eve, csl;
    eve.start = csl.start = clockFor(serial)(head->start_ms);
    eve.serial = csl.serial = serial;
    modelOf(root, &eve.model_id, &eve.vid);
    csl.model_id = eve.model_id;
    csl.vid = eve.vid;

    std::ifstream in(root / "stream" / "events.jsonl");
    for (std::string line; std::getline(in, line);) {
        const auto j = nlohmann::json::parse(line, nullptr, false);
        if (!j.is_object()) continue;
        const std::string name = j.value("event", "");
        int64_t t = as11::isoToMs(j.value("reportTime", ""));
        if (name.empty() || t == 0) continue;
        const bool csr = isCsr(name);
        // Scored events are stamped at their end, as the card's are; a CSR
        // mark is moved back to when the stretch began.
        if (csr) t -= std::max(0, j.value("backdateSeconds", 0)) * 1000LL;
        const Segment* home = nullptr;
        for (const auto* s : segs) if (s->start_ms <= t) home = s;
        if (!home || t > home->end_ms + kEventTailMs) continue;
        const double onset = static_cast<double>((t - head->start_ms + 500) / 1000);
        if (csr) {
            csl.annotations.push_back({onset, 0.0, name.find("Start") != std::string::npos ? "CSR Start" : "CSR End"});
        } else {
            eve.annotations.push_back({onset, static_cast<double>(std::max(0, j.value("durationSeconds", 0))),
                                       eveText(name)});
        }
    }
    auto byOnset = [](const EDFWriteAnnotation& a, const EDFWriteAnnotation& b) { return a.onset_s < b.onset_s; };
    std::sort(eve.annotations.begin(), eve.annotations.end(), byOnset);
    std::sort(csl.annotations.begin(), csl.annotations.end(), byOnset);
    const fs::path dir = root / "DATALOG" / night;
    writeFileAtomic(dir / resmedFileName(eve.start, "EVE"), writeResMedAnnotations(eve));
    writeFileAtomic(dir / resmedFileName(csl.start, "CSL"), writeResMedAnnotations(csl));
}

bool writeIdentification(const std::string& root, const std::string& result_json) {
    const auto r = nlohmann::json::parse(result_json, nullptr, false);
    if (r.is_discarded()) return false;
    const auto ptr = "/IdentificationProfiles/Product/SerialNumber"_json_pointer;
    if (!r.contains(ptr) || !r.at(ptr).is_string() || r.at(ptr).get<std::string>().empty()) return false;
    const std::string serial = r.at(ptr).get<std::string>();
    const fs::path path = fs::path(root) / "Identification.json";
    std::error_code ec;
    if (fs::exists(path, ec)) {
        // A card's copy, or ours, is already there: never replaced.
        std::ifstream in(path);
        std::stringstream ss; ss << in.rdbuf();
        const auto have = nlohmann::json::parse(ss.str(), nullptr, false);
        const auto hp = "/FlowGenerator/IdentificationProfiles/Product/SerialNumber"_json_pointer;
        const bool same = !have.is_discarded() && have.contains(hp) && have.at(hp) == serial;
        if (!same) std::cerr << "AirSense11: Identification.json there names another machine: left alone" << std::endl;
        return same;
    }
    const std::string text = "{\"FlowGenerator\":" + result_json + "}";
    return writeFileAtomic(path, std::vector<uint8_t>(text.begin(), text.end()));
}

} // namespace hms_cpap
