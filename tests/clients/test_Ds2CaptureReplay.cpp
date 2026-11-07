// SDD-064: a real capture of a DreamStation 2 sync, replayed through the
// reader and the night import end to end. A capture holds a person's therapy
// records, so none is kept here: DS2BLE_CAPTURE points at one, and without it
// the test is skipped. It prints each night it imported (session start,
// therapy seconds, events by type) for a person to compare with another
// reading of the same nights; it asserts only what holds for any capture.

#include "Ds2TestMachine.h"
#include "clients/Ds2BleReader.h"
#include "database/SQLiteDatabase.h"
#include "services/Ds2NightImport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>

using namespace hms_cpap;
namespace fs = std::filesystem;
using ds2test::Bytes;

namespace {

std::vector<std::pair<bool, Bytes>> readCapture(const std::string& path) {
    std::vector<std::pair<bool, Bytes>> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string t, who, hex;
        if (!(ss >> t >> who >> hex) || (who != "APP>" && who != "<DEV")) continue;
        out.emplace_back(who == "APP>", cpapdash::ds2ble::fromHex(hex));
    }
    return out;
}

std::string utc(std::chrono::system_clock::time_point t) {
    const std::time_t s = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &s);
#else
    gmtime_r(&s, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%SZ", &tm);
    return b;
}

}  // namespace

TEST(Ds2CaptureReplay, ReadThenImport) {
    const char* path = std::getenv("DS2BLE_CAPTURE");
    if (!path) GTEST_SKIP() << "DS2BLE_CAPTURE not set";
    // The first night to import (SDD-064 D9), "YYYY-MM-DD"; earlier records
    // (a used machine's previous owner) are walked past and never kept.
    const std::string from = std::getenv("DS2BLE_IMPORT_FROM") ? std::getenv("DS2BLE_IMPORT_FROM") : "1970-01-02";

    const fs::path dir = fs::temp_directory_path() /
                         ("ds2_capture_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);

    auto replay = std::make_shared<ds2test::ReplayMachine>(readCapture(path));
    auto t = std::make_shared<ds2test::Transport>();
    t->answerer = [replay](const Bytes& b) { return replay->answer(b); };
    Ds2Timeouts to;
    to.scan_ms = 50;
    to.answer_ms = 500;
    Ds2BleReader reader(dir.string(), "", t, to);
    ASSERT_TRUE(reader.pairStart());
    for (int i = 0; i < 400 && reader.pairStatus().state != Ds2BleReader::PairState::Confirm; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_TRUE(reader.pairAnswer(true));
    while (reader.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_TRUE(reader.paired());
    ASSERT_TRUE(reader.store().setImportFrom(from));

    const auto res = reader.read();
    ASSERT_TRUE(res.ok) << res.error;
    std::cout << "  " << res.identity.model << " firmware " << res.identity.firmware << ": " << res.records
              << " records kept, " << res.walked_past << " walked past (before " << from
              << "), newest session " << res.identity.newest_session << std::endl;
    EXPECT_GT(res.records, 0);

    auto db = std::make_shared<SQLiteDatabase>((dir / "test.db").string());
    ASSERT_TRUE(db->connect());
    Ds2NightImport imp(dir.string(), db, "capture_dev", "DreamStation 2");
    const auto r = imp.run({});
    EXPECT_EQ(r.failed, 0);
    EXPECT_GT(r.imported, 0);
    for (const auto& s : r.saved) {
        std::map<std::string, int> by;
        for (const auto& e : s.events) ++by[std::to_string(static_cast<int>(e.event_type))];
        std::cout << "  " << utc(*s.session_start) << "  " << s.duration_seconds.value_or(0) << " s  events";
        for (const auto& [k, v] : by) std::cout << " t" << k << "=" << v;
        std::cout << std::endl;
    }
    std::cout << "  imported " << r.imported << ", skipped " << r.skipped << std::endl;

    // Nothing new on a second read and a second import.
    const auto again = reader.read();
    ASSERT_TRUE(again.ok) << again.error;
    EXPECT_EQ(again.records, 0);
    EXPECT_EQ(imp.run({}).imported, 0);

    db->disconnect();
    std::error_code ec;
    fs::remove_all(dir, ec);
}
