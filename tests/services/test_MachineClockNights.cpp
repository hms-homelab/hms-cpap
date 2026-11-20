// SDD-074, end to end on the paths that turn instants into a machine's
// nights: an AirSense 11 over Bluetooth (D6), named on the machine's clock
// setting from the first byte; and issue #79's example (a host on
// Australia/Sydney in daylight saving, a machine on a fixed UTC+10, a night
// starting 22:00): stored 22:00, published to Home Assistant as
// 22:00+10:00, and settled / closed on the machine's clock. Every value is
// made up; no real night.
#include <gtest/gtest.h>

#include "database/SQLiteDatabase.h"
#include "services/As11NightImport.h"
#include "services/As11NightWriter.h"
#include "utils/MachineClocks.h"
#include "utils/MachineZone.h"
#include "utils/SessionEnd.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#endif

using namespace hms_cpap;
using namespace hms_cpap::as11;
namespace fs = std::filesystem;
using std::chrono::seconds;

namespace {

constexpr const char* kSerial = "23000000074";
constexpr const char* kDevice = "clock_night_dev";

long long L(int y, int mo, int d, int h, int mi = 0, int s = 0) {
    std::tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = s;
    return wall::labelOf(t);
}

StreamBlock second(int64_t start_ms) {
    StreamBlock b;
    b.start_ms = start_ms;
    b.serial = kSerial;
    b.channels.push_back({kFlow, 0, std::vector<int16_t>(25, 300)});
    b.channels.push_back({kMaskPressure, 0, std::vector<int16_t>(25, 900)});
    return b;
}

class HostZone {
public:
    explicit HostZone(const char* tz) {
        const char* old = std::getenv("TZ");
        had_ = old != nullptr;
        if (had_) old_ = old;
        set(tz);
    }
    ~HostZone() { had_ ? set(old_.c_str()) : unset(); }

private:
    static void set(const char* tz) {
#ifdef _WIN32
        _putenv_s("TZ", tz);
        _tzset();
#else
        setenv("TZ", tz, 1);
        tzset();
#endif
    }
    static void unset() {
#ifdef _WIN32
        _putenv_s("TZ", "");
        _tzset();
#else
        unsetenv("TZ");
        tzset();
#endif
    }
    bool had_{false};
    std::string old_;
};

class ClockNightTest : public ::testing::Test {
protected:
    fs::path dir_;
    std::shared_ptr<SQLiteDatabase> db_;

    void SetUp() override {
        machine_clocks::clear();
        machine_clocks::setPrimaryDevice(kDevice);
        dir_ = fs::temp_directory_path() /
               ("clock_night_" + std::to_string(::getpid()) + "_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir_);
        db_ = std::make_shared<SQLiteDatabase>((dir_ / "test.db").string());
        ASSERT_TRUE(db_->connect());
    }
    void TearDown() override {
        machine_clocks::clear();
        machine_clocks::setPrimaryDevice("");
        db_->disconnect();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    void setClock(const std::string& s) {
        std::string why;
        ASSERT_TRUE(machine_clocks::save(*db_, kDevice, s, &why)) << why;
    }

    /// [secs] seconds of stream from [first_utc_s], through a writer whose own
    /// clock is the host's (the default every install has).
    void stream(long long first_utc_s, int secs) {
        As11NightWriter w(dir_.string(), hostLocalClock());
        for (int i = 0; i < secs; i++) w.onBlock(second((first_utc_s + i) * 1000LL));
        w.onStreamEnd(kSerial);
    }

    std::vector<std::string> brpNames() {
        std::vector<std::string> out;
        for (auto& e : fs::recursive_directory_iterator(dir_ / "as11" / kSerial / "DATALOG")) {
            const auto n = e.path().filename().string();
            if (n.size() > 8 && n.substr(n.size() - 8) == "_BRP.edf")
                out.push_back(e.path().parent_path().filename().string() + "/" + n);
        }
        return out;
    }
};

}  // namespace

TEST_F(ClockNightTest, WithASettingTheFirstNightIsNamedOnItBeforeAnySummary) {
    setClock("+10:00");
    // 2026-01-15 12:00:00 UTC: 22:00 on a UTC+10 machine, whatever the host.
    stream(L(2026, 1, 15, 12), 30);
    const auto names = brpNames();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "20260115/20260115_220000_BRP.edf");
}

TEST_F(ClockNightTest, WithNoSettingTheNightIsNamedOnTheHostsClockAsBefore) {
    stream(L(2026, 1, 15, 12), 30);
    const auto names = brpNames();
    ASSERT_EQ(names.size(), 1u);
    const long long wall_label = MachineZone::host().toWall(L(2026, 1, 15, 12));
    const std::tm t = wall::fieldsOf(wall_label);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &t);
    EXPECT_NE(names[0].find(std::string(stamp) + "_BRP.edf"), std::string::npos) << names[0];
}

TEST_F(ClockNightTest, TheSettingWinsOverAClockSetForTheSerial) {
    // Decision 7 (a Mule's POSIX TZ) and the Summary's offset both arrive as
    // a clock set for the serial; the owner's setting is read first.
    setClock("+10:00");
    As11NightWriter w(dir_.string(), hostLocalClock());
    w.setClock(kSerial, fixedOffsetClock(-300));
    for (int i = 0; i < 30; i++) w.onBlock(second((L(2026, 1, 15, 12) + i) * 1000LL));
    w.onStreamEnd(kSerial);
    const auto names = brpNames();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "20260115/20260115_220000_BRP.edf");
}

TEST_F(ClockNightTest, TheIssuesExampleIsStoredPublishedAndClosedOnTheMachinesClock) {
#ifdef _WIN32
    // The Windows C runtime reads no IANA name from TZ ("Australia/Sydney" is
    // UTC there); January's Sydney in the shape it reads.
    HostZone tz("AEDT-11");
#else
    HostZone tz("Australia/Sydney");   // AEDT, UTC+11, in January
#endif
    setClock("+10:00");
    const long long start_utc = L(2026, 1, 15, 12);
    stream(start_utc, 60);

    As11NightImport imp(dir_.string(), db_, kDevice, "AirSense 11");
    const auto r = imp.run({});
    ASSERT_EQ(r.imported, 1);
    const auto start = *r.saved[0].session_start;

    // Stored 22:00, the machine's own clock (not 23:00, the host's).
    const auto rows = db_->executeQuery("SELECT session_start, machine_offset_s FROM cpap_sessions", {});
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0]["session_start"].asString().substr(0, 19), "2026-01-15 22:00:00");
    EXPECT_EQ(std::atoll(rows[0]["machine_offset_s"].asString().c_str()), 36000);

    // D8: Home Assistant gets 22:00+10:00, the right instant.
    const long long instant = machine_clocks::instantOf(kDevice, wall::seconds(start));
    EXPECT_EQ(instant, start_utc);
    EXPECT_EQ(machine_clocks::zoneFor(kDevice).isoText(instant), "2026-01-15T22:00:00+10:00");

    // D7: settled at the right instant: two hours after the real start it
    // is two hours old on the machine's clock, not one.
    const auto machine_now = machine_clocks::nowWall(kDevice, wall::point(start_utc + 2 * 3600));
    EXPECT_EQ(wall::seconds(machine_now), L(2026, 1, 16, 0));
    EXPECT_TRUE(localNightHasSettled(start, machine_now, std::chrono::hours(2)));
    EXPECT_FALSE(localNightHasSettled(start, machine_now, std::chrono::hours(2) + seconds(1)));
}
