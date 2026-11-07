// SDD-064 D6: DreamStation 2 records stored by a Bluetooth read become
// sessions through the Philips parser. Synthetic records only.

#include "../clients/Ds2TestMachine.h"
#include "database/SQLiteDatabase.h"
#include "services/Ds2NightImport.h"
#include "services/Ds2Store.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

using namespace hms_cpap;
namespace fs = std::filesystem;
namespace ds2 = cpapdash::ds2ble;
using std::chrono::seconds;
using std::chrono::system_clock;

namespace {

constexpr uint32_t kT0 = 1893456000;   // 2030-01-01 00:00:00 UTC, made up
constexpr const char* kDevice = "ds2_import_dev";
constexpr const char* kSerial = "TEST0000000003";

class Ds2Import : public ::testing::Test {
protected:
    fs::path dir_;
    std::shared_ptr<SQLiteDatabase> db_;

    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("ds2_import_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir_);
        db_ = std::make_shared<SQLiteDatabase>((dir_ / "test.db").string());
        ASSERT_TRUE(db_->connect());
    }
    void TearDown() override {
        db_->disconnect();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    void store(uint32_t session, uint32_t t, unsigned secs, int oa = 1) {
        Ds2Store st(dir_.string());
        ds2::Identity id;
        id.serial = kSerial;
        id.model = "520X110C";
        id.firmware = "V1";
        st.writeIdentity(id);
        auto [s, e] = ds2test::night(session, t, secs, oa);
        ds2::Record r1{1, session, s, {}, {}};
        ds2::Record r2{2, session, e, {}, {}};
        ASSERT_FALSE(st.writeRecord(kSerial, r1).empty());
        ASSERT_FALSE(st.writeRecord(kSerial, r2).empty());
    }

    Ds2NightImport importer() { return Ds2NightImport(dir_.string(), db_, kDevice, "DreamStation 2"); }
};

}  // namespace

TEST_F(Ds2Import, RecordsBecomeSessions) {
    store(500, kT0, 3600, 2);
    store(501, kT0 + 86400, 1800);
    auto imp = importer();
    const auto r = imp.run({});
    EXPECT_EQ(r.imported, 2);
    EXPECT_EQ(r.failed, 0);
    ASSERT_EQ(r.saved.size(), 2u);
    const auto& first = r.saved.front();
    ASSERT_TRUE(first.session_start);
    // Mask on one second after the summary's start.
    EXPECT_EQ(*first.session_start, system_clock::time_point(seconds(kT0 + 1)));
    EXPECT_EQ(first.duration_seconds.value_or(0), 3600);
    EXPECT_TRUE(db_->sessionExists(kDevice, *first.session_start));
    EXPECT_TRUE(db_->sessionExists(kDevice, system_clock::time_point(seconds(kT0 + 86400 + 1))));
}

TEST_F(Ds2Import, ARunWithNothingNewSavesNothing) {
    store(500, kT0, 3600);
    auto imp = importer();
    EXPECT_EQ(imp.run({}).imported, 1);
    const auto again = imp.run({});
    EXPECT_EQ(again.imported, 0);
    EXPECT_EQ(again.updated, 0);
    // A new night is imported alone.
    store(501, kT0 + 86400, 1800);
    const auto next = imp.run({});
    EXPECT_EQ(next.imported, 1);
    EXPECT_EQ(next.saved.size(), 1u);
}

TEST_F(Ds2Import, ARestartDoesNotImportStoredNightsAgain) {
    store(500, kT0, 3600);
    EXPECT_EQ(importer().run({}).imported, 1);
    const auto r = importer().run({});   // a new process
    EXPECT_EQ(r.imported, 0);
    EXPECT_EQ(r.updated, 0);
}

TEST_F(Ds2Import, ARemovedNightIsNotPutBack) {
    store(500, kT0, 3600);
    store(501, kT0 + 86400, 1800);
    auto parsed = importer().parseMachine(kSerial);
    ASSERT_EQ(parsed.size(), 2u);
    // SDD-029: the set holds nights as strDayForSessionStart writes them.
    const std::set<std::string> removed{strDayForSessionStart(*parsed.front()->session_start)};
    const auto r = importer().run(removed);
    EXPECT_EQ(r.imported, 1);
    EXPECT_EQ(r.skipped, 1);
    EXPECT_FALSE(db_->sessionExists(kDevice, *parsed.front()->session_start));
}

TEST_F(Ds2Import, AMachineSwitchedOnWithNoMaskIsNoSession) {
    Ds2Store st(dir_.string());
    ds2::Identity id;
    id.serial = kSerial;
    id.model = "520X110C";
    st.writeIdentity(id);
    const auto body = ds2test::v3Block(1, 900, kT0, {ds2test::equipmentOn(), ds2test::settingsAuto(),
                                                     ds2test::equipmentOff(20)});
    ASSERT_FALSE(st.writeRecord(kSerial, ds2::Record{1, 900, body, {}, {}}).empty());
    const auto r = importer().run({});
    EXPECT_EQ(r.imported, 0);
    EXPECT_TRUE(r.saved.empty());
}

TEST_F(Ds2Import, RowsNameTheirFilesUnderDs2) {
    store(500, kT0, 3600);
    importer().run({});
    const auto rows = db_->executeQuery("SELECT f.kind, f.rel_path FROM cpap_session_files f "
                                        "JOIN cpap_sessions s ON s.id = f.session_id WHERE s.device_id = ?",
                                        {kDevice});
    ASSERT_EQ(rows.size(), 2u);
    for (const auto& row : rows) {
        EXPECT_EQ(row["rel_path"].asString().rfind("ds2/TEST0000000003/P0/000001F4.B0", 0), 0u);
        // Not under DATALOG/: the SleepHQ export never sees these (D8).
        EXPECT_EQ(row["rel_path"].asString().find("DATALOG/"), std::string::npos);
    }
}

TEST_F(Ds2Import, StoreRoundTrip) {
    Ds2Store st(dir_.string());
    Ds2Store::State s;
    Ds2Store::Paired p;
    p.address = "aa:bb";
    p.name = "PR BT 1";
    p.serial = kSerial;
    s.paired = p;
    s.machines[kSerial].bookmarks[1] = ds2::Bookmark(10, 7);
    s.machines[kSerial].newest_session = 12;
    ASSERT_TRUE(st.save(s));
    const auto back = st.load();
    ASSERT_TRUE(back.paired);
    EXPECT_EQ(back.paired->serial, kSerial);
    EXPECT_EQ(back.machines.at(kSerial).bookmarks.at(1), ds2::Bookmark(10, 7));
    EXPECT_EQ(back.machines.at(kSerial).newest_session, 12u);
    EXPECT_EQ(Ds2Store::safeSerial("../a b/C1"), "abC1");
    ds2::Record log{3, 0, {1, 2}, {}, ds2::Bookmark(10, 0xab)};
    EXPECT_EQ(Ds2Store::recordPath(log), "D/abababababababababab.B03");
}
