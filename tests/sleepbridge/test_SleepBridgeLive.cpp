// The live night: a rebuilt card's therapy writes put the mask on, its EVE
// and CSL growing by an annotation push one event each, the STR after
// therapy (or a long silence) takes the mask off. Nothing is counted twice.

#include "SleepBridgeTestUtil.h"

#include "sleepbridge/LiveNight.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace sbtest;
using hms_cpap::sleepbridge::LiveEvent;
using hms_cpap::sleepbridge::LiveNight;
using hms_cpap::sleepbridge::LiveSink;
using hms_cpap::sleepbridge::liveEventType;

namespace {

/// One annotation of an EDF+ EVE: onset from the recording's start, a
/// duration (none when negative), and the machine's label.
struct Ann {
    double      onset;
    double      duration;
    std::string text;
};

std::string field(const std::string& s, size_t width) {
    std::string f = s.substr(0, width);
    f.resize(width, ' ');
    return f;
}

/// A ResMed-shaped EDF+ annotation file: one "EDF Annotations" signal, one
/// data record per annotation, each record's TALs carrying the record's own
/// time stamp and the annotation.
std::string edfPlus(const std::vector<Ann>& anns, int records_in_header = -1) {
    constexpr int kBytesPerRecord = 120;   // 60 two-byte samples
    std::string h;
    h += field("0", 8);
    h += field("", 80);
    h += field("", 80);
    h += field("30.10.26", 8);
    h += field("22.30.00", 8);
    h += field("512", 8);
    h += field("EDF+C", 44);
    h += field(std::to_string(records_in_header < 0 ? -1 : records_in_header), 8);
    h += field("1", 8);
    h += field("1", 4);
    h += field("EDF Annotations", 16);
    h += field("", 80);
    h += field("", 8);
    h += field("-1", 8);
    h += field("1", 8);
    h += field("-32768", 8);
    h += field("32767", 8);
    h += field("", 80);
    h += field(std::to_string(kBytesPerRecord / 2), 8);
    h += field("", 32);
    std::string body;
    for (size_t i = 0; i < anns.size(); i++) {
        char onset[32];
        std::snprintf(onset, sizeof onset, "+%g", anns[i].onset);
        std::string rec = "+" + std::to_string(i) + "\x14\x14";
        rec.push_back('\0');
        rec += onset;
        if (anns[i].duration >= 0) {
            char dur[32];
            std::snprintf(dur, sizeof dur, "\x15%g", anns[i].duration);
            rec += dur;
        }
        rec += "\x14" + anns[i].text + "\x14";
        rec.push_back('\0');
        rec.resize(kBytesPerRecord, '\0');
        body += rec;
    }
    return h + body;
}

void put(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

struct Recorder {
    std::vector<LiveEvent>             events;
    std::vector<std::pair<std::map<std::string, int>, int>> counts;
    std::vector<bool>                  masks;
    LiveSink sink() {
        LiveSink s;
        s.event  = [this](const LiveEvent& e) { events.push_back(e); };
        s.counts = [this](const std::map<std::string, int>& c, int t) { counts.emplace_back(c, t); };
        s.mask   = [this](bool on) { masks.push_back(on); };
        return s;
    }
};

const std::string kEve = "DATALOG/20261030/20261031_003042_EVE.edf";
const std::string kCsl = "DATALOG/20261030/20261031_003042_CSL.edf";
const std::string kBrp = "DATALOG/20261030/20261031_003049_BRP.edf";

using namespace std::chrono_literals;
const auto t0 = std::chrono::system_clock::from_time_t(1800000000);

}  // namespace

TEST(SleepBridgeLive, TheMachinesLabelsBecomeShortTypes) {
    EXPECT_EQ(liveEventType("Obstructive apnea"), "OA");
    EXPECT_EQ(liveEventType("Central apnea"), "CA");
    EXPECT_EQ(liveEventType("Clear airway apnea"), "CA");
    EXPECT_EQ(liveEventType("Hypopnea"), "H");
    EXPECT_EQ(liveEventType("Unclassified apnea"), "UA");
    EXPECT_EQ(liveEventType("Apnea"), "UA");
    EXPECT_EQ(liveEventType("RERA"), "RERA");
    EXPECT_EQ(liveEventType("Cheyne-Stokes respiration"), "CSR");
    EXPECT_EQ(liveEventType("Flow limitation"), "FL");
    EXPECT_EQ(liveEventType("Something new"), "OTHER");
    EXPECT_EQ(liveEventType("Recording starts"), "");
    EXPECT_EQ(liveEventType(""), "");
}

TEST(SleepBridgeLive, TherapyWritesPutTheMaskOnAndTheStrTakesItOff) {
    TempDir t;
    Recorder r;
    LiveNight live(t.path.string(), r.sink());
    EXPECT_FALSE(live.maskOn());
    live.onWritten({"Identification.json", "SETTINGS/CurrentSettings.json"}, t0);
    EXPECT_FALSE(live.maskOn()) << "a mount's files are not therapy";
    live.onWritten({kBrp, "DATALOG/20261030/20261031_003049_PLD.edf"}, t0);
    EXPECT_TRUE(live.maskOn());
    live.onWritten({kBrp}, t0 + 60s);
    EXPECT_EQ(r.masks, std::vector<bool>{true}) << "said once";
    live.onWritten({"STR.edf"}, t0 + 120s);
    EXPECT_FALSE(live.maskOn());
    EXPECT_EQ(r.masks, (std::vector<bool>{true, false}));
    // Therapy again, then nothing for the quiet span.
    live.onWritten({kBrp}, t0 + 200s);
    live.tick(t0 + 400s);
    EXPECT_TRUE(live.maskOn());
    live.tick(t0 + 200s + 300s);
    EXPECT_FALSE(live.maskOn());
    EXPECT_EQ(r.masks, (std::vector<bool>{true, false, true, false}));
}

TEST(SleepBridgeLive, EachNewAnnotationIsOneEventOnce) {
    TempDir t;
    Recorder r;
    LiveNight live(t.path.string(), r.sink());
    put(t.path / kEve, edfPlus({{0, -1, "Recording starts"},
                                {120.5, 14, "Obstructive apnea"},
                                {300, 11, "Hypopnea"}}));
    live.onWritten({kEve}, t0);
    ASSERT_EQ(r.events.size(), 2u);
    EXPECT_EQ(r.events[0].type, "OA");
    EXPECT_EQ(r.events[0].description, "Obstructive apnea");
    EXPECT_DOUBLE_EQ(r.events[0].duration_s, 14);
    EXPECT_EQ(r.events[0].file, kEve);
    EXPECT_EQ(r.events[1].type, "H");
    EXPECT_EQ(r.events[1].at - r.events[0].at, 179500ms);
    ASSERT_EQ(r.counts.size(), 1u);
    EXPECT_EQ(r.counts[0].second, 2);
    EXPECT_EQ(r.counts[0].first.at("OA"), 1);

    // The same file written again, unchanged: nothing new.
    live.onWritten({kEve}, t0 + 10s);
    EXPECT_EQ(r.events.size(), 2u);
    EXPECT_EQ(r.counts.size(), 1u);

    // Grown by one: one event, and the counts again.
    put(t.path / kEve, edfPlus({{0, -1, "Recording starts"},
                                {120.5, 14, "Obstructive apnea"},
                                {300, 11, "Hypopnea"},
                                {420, 12, "Central apnea"}}));
    put(t.path / kCsl, edfPlus({{0, -1, "Recording starts"}, {500, 60, "Cheyne-Stokes respiration"}}));
    live.onWritten({kEve, kCsl, kBrp}, t0 + 70s);
    ASSERT_EQ(r.events.size(), 4u);
    EXPECT_EQ(r.events[2].type, "CA");
    EXPECT_EQ(r.events[3].type, "CSR");
    EXPECT_EQ(live.total(), 4);
    EXPECT_EQ(r.counts.back().second, 4);
    EXPECT_EQ(r.counts.back().first.at("CSR"), 1);
    EXPECT_TRUE(live.maskOn());

    // A fresh file under the same name (a new card): counted from its start.
    put(t.path / kEve, edfPlus({{0, -1, "Recording starts"}, {30, 10, "Hypopnea"}}));
    live.onWritten({kEve}, t0 + 140s);
    EXPECT_EQ(r.events.size(), 5u);
    EXPECT_EQ(r.events[4].type, "H");
}

TEST(SleepBridgeLive, AFileThatWillNotOpenIsNoEvent) {
    TempDir t;
    Recorder r;
    LiveNight live(t.path.string(), r.sink());
    put(t.path / kEve, "not an edf");
    live.onWritten({kEve, "DATALOG/20261030/missing_EVE.edf"}, t0);
    EXPECT_TRUE(r.events.empty());
    EXPECT_TRUE(r.counts.empty());
}
