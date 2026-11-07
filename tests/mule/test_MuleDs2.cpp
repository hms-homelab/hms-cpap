// SDD-065: a DreamStation 2 read through a Mule & Miner over the local socket.
// A fake unit answers ds2_read with a synthetic .ds2r (push-c3 SDD-043 D4's
// layout), built here from made-up records the Philips parser decodes. No
// machine wrote any of these bytes; no real night is in this file.
//
// HMS_CPAP_DS2R_CAPTURE=<a .ds2r a unit sent>, a file kept OUTSIDE this
// repository, also runs that file through the same store; without it that
// test is skipped.

#include "../clients/Ds2TestMachine.h"
#include "database/SQLiteDatabase.h"
#include "mule/Ds2rFile.h"
#include "mule/MuleDs2.h"
#include "mule/MuleService.h"
#include "services/As11NightWriter.h"
#include "services/Ds2NightImport.h"
#include "services/Ds2Store.h"

#include <cpapdash/ds2ble/Protocol.h>
#include <gtest/gtest.h>
#include <json/json.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace hms_cpap;
using namespace hms_cpap::mule;
namespace fs  = std::filesystem;
namespace ds2 = cpapdash::ds2ble;

namespace {

constexpr const char* kUnit    = "FE-C3-00000009";
constexpr const char* kSerial  = "TEST0000000009";   // made up
constexpr const char* kPeer    = "AA:BB:CC:00:00:09";
constexpr uint32_t    kT0      = 1893456000;          // 2030-01-01 00:00:00 UTC, made up
constexpr uint32_t    kDaySecs = 86400;

using Bytes = std::vector<uint8_t>;

Json::Value parse(const std::string& text) {
    Json::Value v;
    Json::CharReaderBuilder rb;
    std::istringstream ss(text);
    Json::parseFromStream(rb, ss, &v, nullptr);
    return v;
}

std::string compact(const Json::Value& v) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, v);
}

// ── a .ds2r, written the way the miner's ds2_walk.c writes it ──

constexpr uint8_t kItemAnswer    = 0x01;
constexpr uint8_t kItemRecord    = 0x02;
constexpr uint8_t kItemStreamEnd = 0x03;
constexpr uint8_t kItemEnd       = 0x04;
constexpr uint8_t kItemSealed    = 0x05;
constexpr uint8_t kHowNone       = 0x00;
constexpr uint8_t kHowStopped    = 0xFF;

/// A made-up stand-in for a card file: opaque bytes that name what they
/// stand for. This host only places them; it never looks inside.
Bytes cardBytes(const std::string& what) {
    Bytes b(32, 0xC5);
    b.insert(b.end(), what.begin(), what.end());
    return b;
}

class Ds2rWriter {
public:
    Ds2rWriter() { out_ = {'D', 'S', '2', 'R', 0x01}; }
    Ds2rWriter& answer(uint16_t cmd, uint8_t sub, uint8_t status, const Bytes& data) {
        Bytes p = {static_cast<uint8_t>(cmd >> 8), static_cast<uint8_t>(cmd), sub, status};
        p.insert(p.end(), data.begin(), data.end());
        return item(kItemAnswer, p);
    }
    Ds2rWriter& identity(const std::string& serial, uint32_t newest) {
        auto z = [](const std::string& s) {
            Bytes b(s.begin(), s.end());
            b.push_back(0);
            return b;
        };
        answer(ds2::kCmdSerial, 0x01, 0, z(serial));
        answer(ds2::kCmdCommands, 0x00, 0, {0x00, 0x2b, 0x00, 0x9a});
        answer(ds2::kCmdFirmware, 0x03, 0, z("V9.9.9"));
        answer(ds2::kCmdModel, 0x01, 0, z("TESTMODEL"));
        answer(ds2::kCmdCursor, ds2::kCurNewest, 0,
               {static_cast<uint8_t>(newest >> 24), static_cast<uint8_t>(newest >> 16),
                static_cast<uint8_t>(newest >> 8), static_cast<uint8_t>(newest)});
        return answer(ds2::kCmdCounters, 0x00, 0, Bytes(7, 1));
    }
    Ds2rWriter& record(uint8_t stream, uint32_t session, const Bytes& bookmark, const Bytes& body) {
        Bytes p = {stream, static_cast<uint8_t>(session >> 24), static_cast<uint8_t>(session >> 16),
                   static_cast<uint8_t>(session >> 8), static_cast<uint8_t>(session)};
        p.insert(p.end(), bookmark.begin(), bookmark.end());
        p.insert(p.end(), body.begin(), body.end());
        return item(kItemRecord, p);
    }
    Ds2rWriter& sealed(uint8_t stream, uint32_t session, const Bytes& bookmark, const std::string& path,
                       const Bytes& card) {
        Bytes p = {stream, static_cast<uint8_t>(session >> 24), static_cast<uint8_t>(session >> 16),
                   static_cast<uint8_t>(session >> 8), static_cast<uint8_t>(session)};
        p.insert(p.end(), bookmark.begin(), bookmark.end());
        p.push_back(static_cast<uint8_t>(path.size()));
        p.insert(p.end(), path.begin(), path.end());
        p.insert(p.end(), card.begin(), card.end());
        return item(kItemSealed, p);
    }
    Ds2rWriter& streamEnd(uint8_t stream, uint8_t how, uint16_t records, const Bytes& bookmark) {
        Bytes p = {stream, how, static_cast<uint8_t>(records), static_cast<uint8_t>(records >> 8)};
        p.insert(p.end(), bookmark.begin(), bookmark.end());
        return item(kItemStreamEnd, p);
    }
    Ds2rWriter& end(uint8_t result, bool more, uint8_t status = 0) {
        return item(kItemEnd, {result, static_cast<uint8_t>(more ? 1 : 0), status});
    }
    std::string bytes() const { return std::string(out_.begin(), out_.end()); }

private:
    Ds2rWriter& item(uint8_t kind, const Bytes& p) {
        const uint32_t n = static_cast<uint32_t>(p.size());
        out_.insert(out_.end(), {kind, static_cast<uint8_t>(n), static_cast<uint8_t>(n >> 8),
                                 static_cast<uint8_t>(n >> 16), static_cast<uint8_t>(n >> 24)});
        out_.insert(out_.end(), p.begin(), p.end());
        return *this;
    }
    Bytes out_;
};

Bytes bm(uint8_t stream, uint8_t n) { return {stream, n, 0, 0, 0, 0, 0, 0, 0, 0x5a}; }
const Bytes kNone(10, 0);

std::string pPath(uint8_t stream, uint32_t session) {
    char b[32];
    std::snprintf(b, sizeof b, "P0/%08X.B0%u", session, static_cast<unsigned>(stream));
    return b;
}

/// Nights `sessions` (one a day from kT0 + day * 86400), each a summary and
/// its events. `card`: "plain" (RECORD only), "both" (RECORD, then the card
/// file as SEALED), "sealed" (SEALED only), as the miner writes them. `logs`
/// adds that many D log records (stream 3), numbered from 000 in this read as
/// the miner numbers them, with bookmarks from `first_log`.
Ds2rWriter nights(const std::vector<std::pair<uint32_t, uint32_t>>& sessions, bool more = false,
                  uint8_t first_bm = 1, const std::string& card = "plain", int logs = 0,
                  uint8_t first_log = 1) {
    Ds2rWriter w;
    w.identity(kSerial, sessions.empty() ? 0 : sessions.back().first);
    const bool plain = card != "sealed", seal = card != "plain";
    uint8_t n = first_bm;
    for (const auto& [session, day] : sessions) {
        auto [s, e] = ds2test::night(session, kT0 + day * kDaySecs, 3600);
        if (plain) w.record(1, session, bm(1, n), s);
        if (seal) w.sealed(1, session, bm(1, n), pPath(1, session), cardBytes(pPath(1, session)));
        if (plain) w.record(2, session, bm(2, n), e);
        if (seal) w.sealed(2, session, bm(2, n), pPath(2, session), cardBytes(pPath(2, session)));
        ++n;
    }
    for (int i = 0; i < logs; ++i) {
        const Bytes b = bm(3, static_cast<uint8_t>(first_log + i));
        char path[16];
        std::snprintf(path, sizeof path, "D/%03d.B03", i);
        if (plain) w.record(3, 0, b, Bytes(24, static_cast<uint8_t>(0x30 + i)));
        if (seal) w.sealed(3, 0, b, path, cardBytes("log " + std::to_string(first_log + i)));
    }
    const uint16_t c = static_cast<uint16_t>(sessions.size());
    const Bytes last1 = sessions.empty() ? kNone : bm(1, static_cast<uint8_t>(n - 1));
    const Bytes last2 = sessions.empty() ? kNone : bm(2, static_cast<uint8_t>(n - 1));
    w.streamEnd(1, more ? kHowStopped : kHowNone, c, last1);
    w.streamEnd(2, more ? kHowStopped : kHowNone, c, last2);
    w.streamEnd(3, kHowNone, static_cast<uint16_t>(logs),
                logs > 0 ? bm(3, static_cast<uint8_t>(first_log + logs - 1)) : kNone);
    w.streamEnd(4, kHowNone, 0, kNone);   // read to its end, empty
    return w;
}

/// The card-level files the miner seals on a read that carried card_meta.
Ds2rWriter& withCardMeta(Ds2rWriter& w) {
    w.sealed(0, 0, kNone, "PROP.BIN", cardBytes("PROP.BIN"));
    return w.sealed(0, 0, kNone, "LOG.SEQ", cardBytes("LOG.SEQ"));
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
std::string asString(const Bytes& b) { return std::string(b.begin(), b.end()); }

// The unit's end of one connection, in DreamStation 2 mode.
class FakeUnit {
public:
    explicit FakeUnit(MuleService& svc) : svc_(svc) {
        conn_ = svc_.open(kUnit, [this](const std::string& t) {
            down.push_back(parse(t));
            return true;
        });
    }
    ~FakeUnit() { close(); }
    void close() {
        if (conn_) svc_.closed(conn_);
        conn_ = 0;
    }
    void text(const Json::Value& v) { svc_.onText(conn_, compact(v)); }

    static Json::Value ds2State(bool enabled, bool bonded, const std::string& peer = kPeer) {
        Json::Value s(Json::objectValue);
        s["mode"]                = "cloud";
        s["ds2"]["enabled"]      = enabled;
        s["ds2"]["read"]         = true;
        s["ds2"]["bonded"]       = bonded;
        s["ds2"]["peer"]         = bonded ? peer : "";
        s["ds2"]["machine"]      = "";
        s["ds2"]["busy"]         = false;
        s["ds2"]["last_error"]   = "";
        s["ds2"]["last_records"] = 0;
        return s;
    }
    void hello(bool enabled = true, bool bonded = true, const std::string& peer = kPeer) {
        Json::Value h(Json::objectValue);
        h["t"]        = "hello";
        h["fw"]       = "9.9.9";
        h["miner_fw"] = "9.9.9";
        h["mode"]     = "card";   // what a mule says in DreamStation 2 mode
        h["state"]    = ds2State(enabled, bonded, peer);
        text(h);
    }
    void state(const Json::Value& s) {
        Json::Value f(Json::objectValue);
        f["t"]     = "state";
        f["state"] = s;
        text(f);
    }
    /// The newest command frame sent down, of `cmd`.
    Json::Value last(const std::string& cmd) const {
        for (auto it = down.rbegin(); it != down.rend(); ++it)
            if ((*it)["t"].asString() == "cmd" && (*it)["cmd"].asString() == cmd) return *it;
        return Json::Value(Json::nullValue);
    }
    size_t count(const std::string& cmd) const {
        size_t n = 0;
        for (const auto& f : down)
            if (f["t"].asString() == "cmd" && f["cmd"].asString() == cmd) ++n;
        return n;
    }
    /// A ds2_read's answer: the file as kind 7 frames, the empty last frame,
    /// then the result.
    void answerRead(uint64_t req_id, const std::string& bytes, size_t chunk = 333) {
        const auto rid = static_cast<uint32_t>(req_id);
        uint32_t seq   = 0;
        for (size_t at = 0; at < bytes.size(); at += chunk) {
            std::string f = encodeHeader({rid, seq++, 0, kKindDs2});
            f += bytes.substr(at, chunk);
            svc_.onBinary(conn_, f);
        }
        svc_.onBinary(conn_, encodeHeader({rid, seq, 1, kKindDs2}));
        Json::Value r(Json::objectValue);
        r["t"]      = "result";
        r["req_id"] = static_cast<Json::UInt64>(req_id);
        r["ok"]     = true;
        text(r);
    }
    void answerFailed(uint64_t req_id, const std::string& token) {
        Json::Value r(Json::objectValue);
        r["t"]                          = "result";
        r["req_id"]                     = static_cast<Json::UInt64>(req_id);
        r["ok"]                         = false;
        r["error"]                      = token;
        r["result"]["ok"]               = false;
        r["result"]["error"]            = token;
        r["result"]["ds2_read"]["ok"]   = false;
        r["result"]["ds2_read"]["error"] = token;
        text(r);
    }
    void answerBond(uint64_t req_id, bool bonded, const std::string& peer, const std::string& code = "") {
        Json::Value r(Json::objectValue);
        r["t"]                         = "result";
        r["req_id"]                    = static_cast<Json::UInt64>(req_id);
        r["ok"]                        = true;
        r["result"]["ok"]              = true;
        r["result"]["ds2_bond"]["ok"]  = true;
        r["result"]["ds2_bond"]["error"] = "";
        r["result"]["ds2_bond"]["bonded"] = bonded;
        r["result"]["ds2_bond"]["peer"]   = peer;
        if (!code.empty()) {
            r["result"]["ds2_bond"]["bonding"]["code"]      = code;
            r["result"]["ds2_bond"]["bonding"]["expires_s"] = 25;
        }
        text(r);
    }

    std::vector<Json::Value> down;

private:
    MuleService& svc_;
    uint64_t     conn_{0};
};

class MuleDs2Test : public ::testing::Test {
protected:
    fs::path dir_;
    std::unique_ptr<MuleService> svc_;
    std::unique_ptr<MuleDs2> ds2_;
    int imports_due_ = 0;

    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("mule_ds2_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir_);
        MuleService::Options o;
        o.data_dir = dir_.string();
        o.root     = (dir_ / "mule").string();
        svc_ = std::make_unique<MuleService>(o, std::make_shared<As11NightWriter>(o.data_dir, hostLocalClock()));
        MuleDs2::Options d;
        d.data_dir   = o.data_dir;
        d.units_root = o.root;
        ds2_ = std::make_unique<MuleDs2>(d, *svc_);
        svc_->setAnswerHook([this](const MuleService::Answered& a) { ds2_->onAnswer(a); });
        ds2_->setImportDue([this] { ++imports_due_; });
    }
    void TearDown() override {
        ds2_.reset();
        svc_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    uint64_t reqOf(const Json::Value& frame) const { return frame["req_id"].asUInt64(); }
    Ds2Store store() const { return Ds2Store(dir_.string()); }
    Json::Value unitCommand(uint64_t req) const {
        const Json::Value status = svc_->status();
        for (const auto& u : status["units"])
            for (const auto& c : u["commands"])
                if (c["req_id"].asUInt64() == req) return c;
        return Json::Value(Json::nullValue);
    }
};

}  // namespace

// ── the file ────────────────────────────────────────────────────────────────

namespace {
ds2::ReadFile readOf(const std::string& bytes) {
    return ds2::parseReadFile(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}
}  // namespace

// What the miner writes, as the library this host pins reads it (v0.1.1).
TEST(Ds2rFile, TheLibraryReadsTheMinersFileRecordsCardFilesAndTheIdentity) {
    auto [s, e] = ds2test::night(7, kT0, 600);
    const std::string bytes = Ds2rWriter()
                                  .identity(kSerial, 7)
                                  .record(1, 7, bm(1, 1), s)
                                  .sealed(1, 7, bm(1, 1), pPath(1, 7), cardBytes("s7"))
                                  .record(2, 7, bm(2, 1), e)
                                  .streamEnd(1, kHowNone, 1, bm(1, 1))
                                  .streamEnd(3, 0x06, 0, kNone)
                                  .sealed(0, 0, kNone, "PROP.BIN", cardBytes("PROP.BIN"))
                                  .end(0, false)
                                  .bytes();
    const auto f = readOf(bytes);
    ASSERT_EQ(f.records.size(), 2u);
    EXPECT_EQ(f.records[0].bookmark, bm(1, 1));
    EXPECT_EQ(f.records[0].body, s);
    ASSERT_EQ(f.sealed.size(), 2u);
    EXPECT_EQ(f.sealed[0].path, "P0/00000007.B01");
    EXPECT_EQ(f.sealed[0].card, cardBytes("s7"));
    EXPECT_EQ(f.sealed[1].stream, 0);
    EXPECT_EQ(f.sealed[1].path, "PROP.BIN");
    ASSERT_TRUE(f.end);
    const auto id = ds2r::identity(f);
    EXPECT_EQ(id.serial, kSerial);
    EXPECT_EQ(id.model, "TESTMODEL");
    EXPECT_EQ(id.firmware, "V9.9.9");
    EXPECT_EQ(id.newest_session, 7u);
    EXPECT_EQ(id.commands, (std::vector<uint16_t>{0x002b, 0x009a}));
}

TEST(Ds2rFile, ACutFileIsRefusedWholeAndAnUnknownItemSkipped) {
    auto [s, e] = ds2test::night(7, kT0, 600);
    std::string bytes = Ds2rWriter().identity(kSerial, 7).record(1, 7, bm(1, 1), s).record(2, 7, bm(2, 1), e).bytes();
    bytes.resize(bytes.size() - 5);   // the stream broke inside the second record
    EXPECT_THROW(readOf(bytes), ds2::ReadFileError);
    EXPECT_THROW(readOf("DS2Q\x01"), ds2::ReadFileError);
    std::string later = Ds2rWriter().identity(kSerial, 1).bytes();
    later += std::string("\x09\x02\x00\x00\x00zz", 7);   // a kind a later version adds
    later += Ds2rWriter().end(0, false).bytes().substr(5);
    const auto f = readOf(later);
    EXPECT_EQ(f.skipped_items, 1u);
    EXPECT_TRUE(f.end);
}

TEST(Ds2rFile, TokensAndBookmarks) {
    EXPECT_EQ(ds2r::resultToken(0), "");
    EXPECT_EQ(ds2r::resultToken(3), "not_found");
    EXPECT_EQ(ds2r::resultToken(11), "link_lost");
    EXPECT_EQ(ds2r::resultToken(12), "other_machine");
    EXPECT_EQ(ds2r::resultToken(7), "protocol");
    EXPECT_EQ(ds2r::resultToken(7, 0x0b), "status_0b");
    EXPECT_EQ(ds2r::toHex(bm(1, 2)), "010200000000000000" "5a");
    EXPECT_EQ(*ds2r::fromHex("010200000000000000" "5A"), bm(1, 2));
    EXPECT_FALSE(ds2r::fromHex("0102"));
    EXPECT_FALSE(ds2r::fromHex("zz0200000000000000" "5a"));
}

// ── the commands ────────────────────────────────────────────────────────────

TEST_F(MuleDs2Test, TheCommandsAreCheckedBeforeTheyGo) {
    FakeUnit unit(*svc_);
    unit.hello();
    auto q = [&](const Json::Value& r) { return svc_->queueCommand(kUnit, r).status; };
    Json::Value r;
    r["cmd"] = "ds2_bond";
    r["action"] = "pair";
    EXPECT_EQ(q(r), 400);
    r["action"] = "confirm";
    r["code"]   = "12345";
    EXPECT_EQ(q(r), 400);
    r["code"] = "12345a";
    EXPECT_EQ(q(r), 400);
    r = Json::Value();
    r["cmd"] = "ds2_bond";
    r["action"] = "cancel";
    r["just_works"] = true;   // start only
    EXPECT_EQ(q(r), 400);

    Json::Value rd;
    rd["cmd"]             = "ds2_read";
    rd["streams"]["5"]    = "";
    EXPECT_EQ(q(rd), 400);
    rd["streams"]         = Json::Value(Json::objectValue);
    rd["streams"]["1"]    = "abc";
    EXPECT_EQ(q(rd), 400);
    rd["streams"]["1"]    = "";
    rd["machine"]         = "bad serial!";
    EXPECT_EQ(q(rd), 400);
    rd.removeMember("machine");
    rd["card"] = "everything";   // plain, both or sealed
    EXPECT_EQ(q(rd), 400);
    rd["card"]                   = "both";
    rd["card_meta"]["pfn"]       = -1;
    EXPECT_EQ(q(rd), 400);
    rd["card_meta"]["pfn"]       = 4;
    rd["card_meta"]["dfn"]       = 2;
    rd["card_meta"]["efn"]       = 0;
    rd["card_meta"]["fd"]        = static_cast<Json::Int64>(kT0);
    rd["card_meta"]["extra"]     = 1;
    EXPECT_EQ(q(rd), 400);
    rd["card_meta"].removeMember("extra");
    EXPECT_EQ(q(rd), 200);
    EXPECT_EQ(unit.last("ds2_read")["card_meta"]["fd"].asInt64(), static_cast<Json::Int64>(kT0));
    unit.answerFailed(reqOf(unit.last("ds2_read")), "not_found");   // one command at a time

    Json::Value c;
    c["cmd"]                   = "config";
    c["config"]["ble_machine"] = "resmed";
    EXPECT_EQ(q(c), 400);
    c["config"]["ble_machine"] = "ds2";
    c["config"]["o2_enabled"]  = true;   // one thing at a time
    EXPECT_EQ(q(c), 400);
    c["config"].removeMember("o2_enabled");
    EXPECT_EQ(q(c), 200);
    EXPECT_EQ(unit.last("config")["config"]["ble_machine"].asString(), "ds2");
}

TEST_F(MuleDs2Test, PairingGoesAsTheUnitTakesIt) {
    FakeUnit unit(*svc_);
    unit.hello(true, false);
    Json::Value r;
    r["cmd"]    = "ds2_bond";
    r["action"] = "start";
    ASSERT_EQ(svc_->queueCommand(kUnit, r).status, 200);
    const Json::Value start = unit.last("ds2_bond");
    EXPECT_EQ(start["action"].asString(), "start");
    EXPECT_FALSE(start.isMember("just_works"));
    unit.answerBond(reqOf(start), false, "", "042517");

    r["action"] = "confirm";
    r["code"]   = "042517";
    ASSERT_EQ(svc_->queueCommand(kUnit, r).status, 200);
    const Json::Value confirm = unit.last("ds2_bond");
    EXPECT_EQ(confirm["code"].asString(), "042517");
    unit.answerBond(reqOf(confirm), true, kPeer);
    // A bond seen here starts the import day: today.
    const auto v = ds2_->view(kUnit);
    EXPECT_FALSE(v["import_from_default"].asBool());
    EXPECT_EQ(v["import_from"].asString(), Ds2Store::localDate(std::time(nullptr)));
}

TEST_F(MuleDs2Test, AUnitInDreamStationModeIsTakenAndACardUnitIsNot) {
    {
        FakeUnit unit(*svc_);
        unit.hello(true, true);
        EXPECT_TRUE(svc_->brief(kUnit).online);
        EXPECT_EQ(svc_->status()["units"][0]["mode"].asString(), "ds2_ble");
    }
    MuleService::Options o;
    o.data_dir = (dir_ / "other").string();
    o.root     = (dir_ / "other" / "mule").string();
    MuleService other(o, std::make_shared<As11NightWriter>(o.data_dir, hostLocalClock()));
    FakeUnit card(other);
    card.hello(false, false);
    ASSERT_FALSE(card.down.empty());
    EXPECT_EQ(card.down.back()["t"].asString(), "error");
    EXPECT_FALSE(other.brief(kUnit).online);
}

// ── a read ──────────────────────────────────────────────────────────────────

TEST_F(MuleDs2Test, AFirstReadStoresTheNightsAndTheBookmarksAndImportsThem) {
    FakeUnit unit(*svc_);
    unit.hello();
    ASSERT_EQ(ds2_->setImportFrom(kUnit, "2029-12-31").status, 200);
    const auto a = ds2_->readNow(kUnit);
    ASSERT_EQ(a.status, 200) << compact(a.body);
    EXPECT_EQ(a.body["result"].asString(), "reading");

    // The first read of a machine: every stream from the oldest, no serial check.
    const Json::Value cmd = unit.last("ds2_read");
    ASSERT_TRUE(cmd.isObject());
    EXPECT_EQ(cmd["max_records"].asInt(), 32);
    EXPECT_EQ(cmd["card"].asString(), "both");   // the default: records and card files
    EXPECT_FALSE(cmd.isMember("card_meta"));
    EXPECT_FALSE(cmd.isMember("machine"));
    for (const char* s : {"1", "2", "3", "4"}) EXPECT_EQ(cmd["streams"][s].asString(), "") << s;

    unit.answerRead(reqOf(cmd), nights({{500, 0}, {501, 1}}).end(0, false).bytes());

    // Records on disk, then the bookmarks.
    const auto st = store().load();
    ASSERT_TRUE(st.machines.count(kSerial));
    const auto& m = st.machines.at(kSerial);
    EXPECT_EQ(m.bookmarks.at(1), bm(1, 2));
    EXPECT_EQ(m.bookmarks.at(2), bm(2, 2));
    EXPECT_EQ(m.model, "TESTMODEL");
    EXPECT_TRUE(fs::exists(dir_ / "ds2" / kSerial / "P0" / ds2::cardFileName(1, 500)));
    EXPECT_TRUE(fs::exists(dir_ / "ds2" / kSerial / "P0" / ds2::cardFileName(2, 501)));
    EXPECT_EQ(store().readIdentity(kSerial).serial, kSerial);
    EXPECT_EQ(imports_due_, 1);

    const auto c = unitCommand(reqOf(cmd));
    EXPECT_EQ(c["state"].asString(), "answered");
    EXPECT_GT(c["bytes"].asUInt64(), 0u);
    EXPECT_NE(c["outcome"].asString().find("4 record(s) kept"), std::string::npos);

    const auto v = ds2_->view(kUnit);
    EXPECT_EQ(v["machine"].asString(), kSerial);
    EXPECT_EQ(v["last_read"]["records"].asInt(), 4);
    EXPECT_EQ(v["last_read"]["error"].asString(), "");
    EXPECT_GT(v["last_read"]["at"].asInt64(), 0);
    EXPECT_TRUE(v["first_read_done"].asBool());
    EXPECT_EQ(v["trigger"].asString(), "manual");

    // The same import as the direct read's makes the nights.
    auto db = std::make_shared<SQLiteDatabase>((dir_ / "t.db").string());
    ASSERT_TRUE(db->connect());
    Ds2NightImport imp(dir_.string(), db, "mule_ds2_dev", "DreamStation 2");
    const auto r = imp.run({});
    EXPECT_EQ(r.imported, 2);
    EXPECT_EQ(r.failed, 0);
    db->disconnect();
}

TEST_F(MuleDs2Test, TheNextReadResumesFromTheStoredBookmarks) {
    FakeUnit unit(*svc_);
    unit.hello();
    ds2_->setImportFrom(kUnit, "2029-12-31");
    ASSERT_EQ(ds2_->readNow(kUnit).status, 200);
    unit.answerRead(reqOf(unit.last("ds2_read")), nights({{500, 0}}).end(0, false).bytes());

    ASSERT_EQ(ds2_->readNow(kUnit).status, 200);
    const Json::Value cmd = unit.last("ds2_read");
    EXPECT_EQ(cmd["machine"].asString(), kSerial);
    EXPECT_EQ(cmd["streams"]["1"].asString(), ds2r::toHex(bm(1, 1)));
    EXPECT_EQ(cmd["streams"]["2"].asString(), ds2r::toHex(bm(2, 1)));
    EXPECT_EQ(cmd["streams"]["3"].asString(), "");
    // Nothing new: the bookmarks stay, nothing to import.
    unit.answerRead(reqOf(cmd), nights({}).end(0, false).bytes());
    EXPECT_EQ(store().load().machines.at(kSerial).bookmarks.at(1), bm(1, 1));
    EXPECT_EQ(imports_due_, 1);
    EXPECT_EQ(ds2_->view(kUnit)["last_read"]["records"].asInt(), 0);
}

TEST_F(MuleDs2Test, NightsBeforeTheImportDayAreWalkedPastNeverKept) {
    FakeUnit unit(*svc_);
    unit.hello();
    // 2030-01-03, local: the nights of the 1st and 2nd are someone else's.
    ds2_->setImportFrom(kUnit, "2030-01-03");
    ASSERT_EQ(ds2_->readNow(kUnit).status, 200);
    unit.answerRead(reqOf(unit.last("ds2_read")),
                    nights({{500, 0}, {501, 1}, {502, 4}}).end(0, false).bytes());
    EXPECT_FALSE(fs::exists(dir_ / "ds2" / kSerial / "P0" / ds2::cardFileName(1, 500)));
    EXPECT_FALSE(fs::exists(dir_ / "ds2" / kSerial / "P0" / ds2::cardFileName(2, 501)));
    EXPECT_TRUE(fs::exists(dir_ / "ds2" / kSerial / "P0" / ds2::cardFileName(1, 502)));
    // Walked past still moves the bookmark: they are never asked again.
    EXPECT_EQ(store().load().machines.at(kSerial).bookmarks.at(1), bm(1, 3));
    const auto v = ds2_->view(kUnit);
    EXPECT_EQ(v["last_read"]["records"].asInt(), 2);
    EXPECT_EQ(v["last_read"]["walked_past"].asInt(), 4);
}

TEST_F(MuleDs2Test, MoreToReadAsksTheNextBatchAtOnceUntilTheMachineIsDone) {
    FakeUnit unit(*svc_);
    unit.hello();
    ds2_->setImportFrom(kUnit, "2029-12-31");
    ASSERT_EQ(ds2_->readNow(kUnit).status, 200);
    unit.answerRead(reqOf(unit.last("ds2_read")), nights({{500, 0}}, true, 1).end(0, true).bytes());
    // The next batch went out on that answer, from the bookmarks just saved.
    ASSERT_EQ(unit.count("ds2_read"), 2u);
    const Json::Value next = unit.last("ds2_read");
    EXPECT_EQ(next["streams"]["1"].asString(), ds2r::toHex(bm(1, 1)));
    EXPECT_EQ(ds2_->view(kUnit)["chain"].asInt(), 1);
    unit.answerRead(reqOf(next), nights({{501, 1}}, false, 2).end(0, false).bytes());
    EXPECT_EQ(unit.count("ds2_read"), 2u);   // done: no third
    EXPECT_EQ(store().load().machines.at(kSerial).bookmarks.at(1), bm(1, 2));
    EXPECT_EQ(imports_due_, 2);
}

TEST_F(MuleDs2Test, ACutFileIsNotStoredAtAllAndItsBookmarksStay) {
    FakeUnit unit(*svc_);
    unit.hello();
    ds2_->setImportFrom(kUnit, "2029-12-31");
    ASSERT_EQ(ds2_->readNow(kUnit).status, 200);
    auto [s, e] = ds2test::night(500, kT0, 3600);
    std::string bytes = Ds2rWriter().identity(kSerial, 500).record(1, 500, bm(1, 1), s).record(2, 500, bm(2, 1), e).bytes();
    bytes.resize(bytes.size() - 3);   // the link went inside the events record
    unit.answerRead(reqOf(unit.last("ds2_read")), bytes);
    // All or nothing: the next read asks from the same place again.
    EXPECT_TRUE(store().load().machines.empty());
    EXPECT_FALSE(fs::exists(dir_ / "ds2" / kSerial / "P0" / ds2::cardFileName(1, 500)));
    EXPECT_EQ(ds2_->view(kUnit)["last_read"]["error"].asString(), "bad_file");
    // The miner's own outcome, after whole records: link_lost, kept as it said.
    ASSERT_EQ(ds2_->readNow(kUnit).status, 200);
    unit.answerRead(reqOf(unit.last("ds2_read")), nights({}).end(11, false).bytes());
    EXPECT_EQ(ds2_->view(kUnit)["last_read"]["error"].asString(), "link_lost");
}

TEST_F(MuleDs2Test, AReadTheMinerCouldNotMakeMovesNothing) {
    FakeUnit unit(*svc_);
    unit.hello();
    ASSERT_EQ(ds2_->readNow(kUnit).status, 200);
    unit.answerFailed(reqOf(unit.last("ds2_read")), "not_found");
    const auto v = ds2_->view(kUnit);
    EXPECT_EQ(v["last_read"]["error"].asString(), "not_found");
    EXPECT_FALSE(v["last_read"].isMember("at"));
    EXPECT_TRUE(store().load().machines.empty());
    EXPECT_EQ(imports_due_, 0);
}

TEST_F(MuleDs2Test, ReadNowSaysWhyNot) {
    EXPECT_EQ(ds2_->readNow(kUnit).status, 404);
    FakeUnit unit(*svc_);
    unit.hello(true, false);
    EXPECT_EQ(ds2_->readNow(kUnit).body["result"].asString(), "not_paired");
    unit.state(FakeUnit::ds2State(false, false));
    EXPECT_EQ(ds2_->readNow(kUnit).body["result"].asString(), "mode_off");
    unit.state(FakeUnit::ds2State(true, true));
    ASSERT_EQ(ds2_->readNow(kUnit).status, 200);
    const auto busy = ds2_->readNow(kUnit);
    EXPECT_EQ(busy.status, 409);
    EXPECT_EQ(busy.body["result"].asString(), "busy");
    unit.close();
    EXPECT_EQ(ds2_->readNow(kUnit).body["result"].asString(), "offline");
}

TEST_F(MuleDs2Test, ANightsEndReadsEveryBondedDreamStationUnit) {
    FakeUnit unit(*svc_);
    unit.hello(true, false);
    EXPECT_EQ(ds2_->nightEnded("sleep_bridge"), 0);   // not bonded: nothing
    unit.state(FakeUnit::ds2State(true, true));
    EXPECT_EQ(ds2_->nightEnded("sleep_bridge"), 1);
    const auto c = unitCommand(reqOf(unit.last("ds2_read")));
    EXPECT_TRUE(c["automatic"].asBool());
    EXPECT_EQ(ds2_->nightEnded("o2ring"), 0);   // one is under way already
}

TEST_F(MuleDs2Test, ANewBondStartsANewDayAndTheSameMachineKeepsItsOwn) {
    FakeUnit unit(*svc_);
    unit.hello();
    ds2_->setImportFrom(kUnit, "2029-12-31");
    ASSERT_EQ(ds2_->readNow(kUnit).status, 200);   // the same peer: the day stays
    EXPECT_EQ(ds2_->view(kUnit)["import_from"].asString(), "2029-12-31");
    unit.answerRead(reqOf(unit.last("ds2_read")), nights({{500, 0}}).end(0, false).bytes());
    EXPECT_EQ(ds2_->view(kUnit)["machine"].asString(), kSerial);

    Json::Value r;
    r["cmd"]    = "ds2_bond";
    r["action"] = "start";
    ASSERT_EQ(svc_->queueCommand(kUnit, r).status, 200);
    unit.answerBond(reqOf(unit.last("ds2_bond")), true, "AA:BB:CC:00:00:99");
    const auto v = ds2_->view(kUnit);
    EXPECT_EQ(v["import_from"].asString(), Ds2Store::localDate(std::time(nullptr)));
    EXPECT_EQ(v["machine"].asString(), "");   // not known until it is read
}

TEST_F(MuleDs2Test, AnotherMachineIsNotReadAndItsSerialIsForgotten) {
    FakeUnit unit(*svc_);
    unit.hello();
    ds2_->setImportFrom(kUnit, "2029-12-31");
    ASSERT_EQ(ds2_->readNow(kUnit).status, 200);
    unit.answerRead(reqOf(unit.last("ds2_read")), nights({{500, 0}}).end(0, false).bytes());
    ASSERT_EQ(ds2_->readNow(kUnit).status, 200);
    unit.answerFailed(reqOf(unit.last("ds2_read")), "other_machine");
    ASSERT_EQ(ds2_->readNow(kUnit).status, 200);
    EXPECT_FALSE(unit.last("ds2_read").isMember("machine"));
}

// ── SDD-065 D8: the card tree from the miner's card files ─────────────────

namespace {
fs::path cardDir(const fs::path& data) {
    return data / "ds2card" / kSerial / "P-SERIES" / Ds2CardTree::folderFor(kSerial);
}
}  // namespace

TEST_F(MuleDs2Test, ABothReadPlacesTheCardFilesThenAsksForTheCardsOwnFiles) {
    FakeUnit unit(*svc_);
    unit.hello();
    ds2_->setImportFrom(kUnit, "2029-12-31");
    ASSERT_EQ(ds2_->readNow(kUnit).status, 200);
    const Json::Value first = unit.last("ds2_read");
    unit.answerRead(reqOf(first), nights({{500, 0}, {501, 1}}, false, 1, "both", 2).end(0, false).bytes());

    // The miner's bytes, as they came, at their card paths; the records too.
    const fs::path m = cardDir(dir_);
    EXPECT_EQ(readAll(m / "P0" / "000001F4.B01"), asString(cardBytes(pPath(1, 500))));
    EXPECT_EQ(readAll(m / "P0" / "000001F5.B02"), asString(cardBytes(pPath(2, 501))));
    EXPECT_EQ(readAll(m / "D" / "000.B03"), asString(cardBytes("log 1")));
    EXPECT_EQ(readAll(m / "D" / "001.B03"), asString(cardBytes("log 2")));
    EXPECT_EQ(readAll(dir_ / "ds2card" / kSerial / "P-SERIES" / "LAST.TXT"), Ds2CardTree::folderFor(kSerial));
    EXPECT_TRUE(fs::exists(dir_ / "ds2" / kSerial / "P0" / ds2::cardFileName(1, 500)));
    EXPECT_EQ(imports_due_, 1);

    // The sync is over and the tree grew: one more read, every stream from a
    // bookmark of our own (stream 4, read empty to its end, from the oldest),
    // with the tree's counts, for the card's PROP.BIN and LOG.SEQ.
    ASSERT_EQ(unit.count("ds2_read"), 2u);
    const Json::Value meta = unit.last("ds2_read");
    EXPECT_EQ(meta["streams"]["1"].asString(), ds2r::toHex(bm(1, 2)));
    EXPECT_EQ(meta["streams"]["2"].asString(), ds2r::toHex(bm(2, 2)));
    EXPECT_EQ(meta["streams"]["3"].asString(), ds2r::toHex(bm(3, 2)));
    EXPECT_EQ(meta["streams"]["4"].asString(), "");
    EXPECT_EQ(meta["card_meta"]["pfn"].asInt(), 4);
    EXPECT_EQ(meta["card_meta"]["dfn"].asInt(), 2);
    EXPECT_EQ(meta["card_meta"]["efn"].asInt(), 0);
    EXPECT_EQ(meta["card_meta"]["fd"].asInt64(), static_cast<Json::Int64>(kT0));
    EXPECT_TRUE(unitCommand(reqOf(meta))["automatic"].asBool());

    Ds2rWriter w = nights({}, false, 1, "both");
    unit.answerRead(reqOf(meta), withCardMeta(w).end(0, false).bytes());
    EXPECT_EQ(readAll(m / "PROP.BIN"), asString(cardBytes("PROP.BIN")));
    EXPECT_EQ(readAll(m / "LOG.SEQ"), asString(cardBytes("LOG.SEQ")));
    EXPECT_EQ(unit.count("ds2_read"), 2u);   // done: nothing new came
}

TEST_F(MuleDs2Test, LogFilesAreNumberedOnAcrossTheReadsOfASync) {
    FakeUnit unit(*svc_);
    unit.hello();
    ds2_->setImportFrom(kUnit, "2029-12-31");
    ASSERT_EQ(ds2_->readNow(kUnit).status, 200);
    unit.answerRead(reqOf(unit.last("ds2_read")), nights({{500, 0}}, true, 1, "both", 2, 1).end(0, true).bytes());
    // The next batch numbers its logs from 000 again; here they go on at 002.
    unit.answerRead(reqOf(unit.last("ds2_read")), nights({{501, 1}}, false, 2, "both", 2, 3).end(0, false).bytes());
    const fs::path m = cardDir(dir_);
    EXPECT_EQ(readAll(m / "D" / "002.B03"), asString(cardBytes("log 3")));
    EXPECT_EQ(readAll(m / "D" / "003.B03"), asString(cardBytes("log 4")));
    EXPECT_FALSE(fs::exists(m / "D" / "004.B03"));
    // The card's counts are across the whole sync.
    const Json::Value meta = unit.last("ds2_read");
    ASSERT_TRUE(meta.isMember("card_meta"));
    EXPECT_EQ(meta["card_meta"]["pfn"].asInt(), 4);
    EXPECT_EQ(meta["card_meta"]["dfn"].asInt(), 4);
}

TEST_F(MuleDs2Test, ACardMetaReadThatBringsFilesIsAskedAgainWithTheNewCounts) {
    FakeUnit unit(*svc_);
    unit.hello();
    ds2_->setImportFrom(kUnit, "2029-12-31");
    ASSERT_EQ(ds2_->readNow(kUnit).status, 200);
    unit.answerRead(reqOf(unit.last("ds2_read")), nights({{500, 0}}, false, 1, "both").end(0, false).bytes());
    const Json::Value meta = unit.last("ds2_read");
    EXPECT_EQ(meta["card_meta"]["pfn"].asInt(), 2);
    // A night closed meanwhile: its files came with PROP.BIN, counted before them.
    Ds2rWriter w = nights({{501, 1}}, false, 2, "both");
    unit.answerRead(reqOf(meta), withCardMeta(w).end(0, false).bytes());
    ASSERT_EQ(unit.count("ds2_read"), 3u);
    EXPECT_EQ(unit.last("ds2_read")["card_meta"]["pfn"].asInt(), 4);
}

TEST_F(MuleDs2Test, NoCardMetaReadUntilEveryStreamHasABookmarkOfOurOwn) {
    FakeUnit unit(*svc_);
    unit.hello();
    ds2_->setImportFrom(kUnit, "2029-12-31");
    ASSERT_EQ(ds2_->readNow(kUnit).status, 200);
    auto [s, e] = ds2test::night(500, kT0, 3600);
    // Stream 2 stopped without a word of where it is (no STREAM_END for it).
    const std::string bytes = Ds2rWriter()
                                  .identity(kSerial, 500)
                                  .record(1, 500, bm(1, 1), s)
                                  .sealed(1, 500, bm(1, 1), pPath(1, 500), cardBytes("x"))
                                  .streamEnd(1, kHowNone, 1, bm(1, 1))
                                  .streamEnd(3, kHowNone, 0, kNone)
                                  .streamEnd(4, kHowNone, 0, kNone)
                                  .end(0, false)
                                  .bytes();
    unit.answerRead(reqOf(unit.last("ds2_read")), bytes);
    EXPECT_EQ(unit.count("ds2_read"), 1u);
    EXPECT_TRUE(fs::exists(cardDir(dir_) / "P0" / "000001F4.B01"));
}

TEST_F(MuleDs2Test, ASealedOnlyReadKeepsTheCardFilesAndItsBookmarksButNoNights) {
    ds2_->setCardMode([] { return std::string("sealed"); });
    FakeUnit unit(*svc_);
    unit.hello();
    ds2_->setImportFrom(kUnit, "2029-12-31");
    ASSERT_EQ(ds2_->readNow(kUnit).status, 200);
    EXPECT_EQ(unit.last("ds2_read")["card"].asString(), "sealed");
    unit.answerRead(reqOf(unit.last("ds2_read")), nights({{500, 0}}, false, 1, "sealed").end(0, false).bytes());
    EXPECT_TRUE(fs::exists(cardDir(dir_) / "P0" / "000001F4.B02"));
    EXPECT_FALSE(fs::exists(dir_ / "ds2" / kSerial / "P0" / ds2::cardFileName(1, 500)));
    EXPECT_EQ(imports_due_, 0);
    EXPECT_EQ(unit.last("ds2_read")["streams"]["1"].asString(), ds2r::toHex(bm(1, 1)));
}

TEST_F(MuleDs2Test, APlainReadAsksForNoCardFiles) {
    ds2_->setCardMode([] { return std::string("plain"); });
    FakeUnit unit(*svc_);
    unit.hello();
    ASSERT_EQ(ds2_->readNow(kUnit).status, 200);
    EXPECT_EQ(unit.last("ds2_read")["card"].asString(), "plain");
}

TEST_F(MuleDs2Test, ANightBeforeTheImportDayLeavesNoCardFile) {
    FakeUnit unit(*svc_);
    unit.hello();
    ds2_->setImportFrom(kUnit, "2030-01-03");
    ASSERT_EQ(ds2_->readNow(kUnit).status, 200);
    unit.answerRead(reqOf(unit.last("ds2_read")), nights({{500, 0}, {502, 4}}, false, 1, "both").end(0, false).bytes());
    EXPECT_FALSE(fs::exists(cardDir(dir_) / "P0" / "000001F4.B01"));
    EXPECT_FALSE(fs::exists(cardDir(dir_) / "P0" / "000001F4.B02"));
    EXPECT_TRUE(fs::exists(cardDir(dir_) / "P0" / "000001F6.B01"));
    // FD is the oldest session kept.
    EXPECT_EQ(unit.last("ds2_read")["card_meta"]["fd"].asInt64(), static_cast<Json::Int64>(kT0 + 4 * kDaySecs));
}

TEST_F(MuleDs2Test, OnlyBookmarksFromThisUnitsOwnReadsAreSent) {
    FakeUnit unit(*svc_);
    unit.hello();
    ds2_->setImportFrom(kUnit, "2029-12-31");
    ASSERT_EQ(ds2_->readNow(kUnit).status, 200);
    unit.answerRead(reqOf(unit.last("ds2_read")), nights({{500, 0}}).end(0, false).bytes());
    // A bookmark from elsewhere (another reader's era) lands in the shared store.
    ASSERT_TRUE(store().saveBookmark(kSerial, 2, bm(2, 0x77)));
    ASSERT_TRUE(store().saveBookmark(kSerial, 4, bm(4, 0x77)));
    ASSERT_EQ(ds2_->readNow(kUnit).status, 200);
    const Json::Value cmd = unit.last("ds2_read");
    EXPECT_EQ(cmd["streams"]["2"].asString(), ds2r::toHex(bm(2, 1)));
    EXPECT_EQ(cmd["streams"]["4"].asString(), "");
}

// ── SDD-065 D4a: the owner's daily read, the same choice as the direct read ──

namespace {
ds2time::Input dailyAt(const std::string& at, bool mask_on = false, const std::string& mode = "daily") {
    ds2time::Input in;
    in.now       = static_cast<long long>(std::time(nullptr));
    in.read_mode = mode;
    in.read_at   = at;   // "00:00": today's time has always come
    in.mask_on   = mask_on;
    return in;
}
}  // namespace

TEST_F(MuleDs2Test, TheDailyReadGoesOnceTheTimeHasComeAndOnceADay) {
    FakeUnit unit(*svc_);
    unit.hello();
    ds2_->setImportFrom(kUnit, "2029-12-31");
    EXPECT_EQ(ds2_->dailyCheck(dailyAt("00:00")), 1);
    const Json::Value cmd = unit.last("ds2_read");
    ASSERT_TRUE(cmd.isObject());
    EXPECT_TRUE(unitCommand(reqOf(cmd))["automatic"].asBool());
    EXPECT_EQ(ds2_->dailyCheck(dailyAt("00:00")), 0);   // under way
    unit.answerRead(reqOf(cmd), nights({{500, 0}}).end(0, false).bytes());
    EXPECT_EQ(ds2_->dailyCheck(dailyAt("00:00")), 0);   // read today already
    EXPECT_EQ(unit.count("ds2_read"), 1u);
}

TEST_F(MuleDs2Test, TheDailyReadWaitsForTheMaskAndForTheTime) {
    FakeUnit unit(*svc_);
    unit.hello();
    EXPECT_EQ(ds2_->dailyCheck(dailyAt("00:00", /*mask_on=*/true)), 0);
    auto later = dailyAt("00:00");
    later.read_at = "23:59";
    later.now     = ds2time::todayAt(later.now, 23 * 60 + 59) - 60;   // a minute before the time
    EXPECT_EQ(ds2_->dailyCheck(later), 0);
    EXPECT_EQ(unit.count("ds2_read"), 0u);
    EXPECT_EQ(ds2_->dailyCheck(dailyAt("00:00")), 1);   // the mask is off, the time has come
}

TEST_F(MuleDs2Test, OnlyWhenAskedReadsNothingDailyButTheNightsEndStillReads) {
    FakeUnit unit(*svc_);
    unit.hello();
    EXPECT_EQ(ds2_->dailyCheck(dailyAt("00:00", false, "manual")), 0);
    EXPECT_EQ(unit.count("ds2_read"), 0u);
    EXPECT_EQ(ds2_->nightEnded("sleep_bridge"), 1);
}

TEST_F(MuleDs2Test, AnUnbondedUnitIsNotReadDaily) {
    FakeUnit unit(*svc_);
    unit.hello(true, false);
    EXPECT_EQ(ds2_->dailyCheck(dailyAt("00:00")), 0);
}

TEST_F(MuleDs2Test, TheCardSaysTheChoiceAndTheNextRead) {
    FakeUnit unit(*svc_);
    unit.hello();
    ds2_->setReadSchedule([] { return std::make_pair(std::string("daily"), std::string("23:59")); });
    auto v = ds2_->view(kUnit);
    EXPECT_EQ(v["read_mode"].asString(), "daily");
    EXPECT_EQ(v["read_at"].asString(), "23:59");
    EXPECT_GT(v["next_read_at"].asInt64(), 0);
    ds2_->setReadSchedule([] { return std::make_pair(std::string("manual"), std::string("11:00")); });
    v = ds2_->view(kUnit);
    EXPECT_EQ(v["read_mode"].asString(), "manual");
    EXPECT_EQ(v["next_read_at"].asInt64(), 0);
}

TEST_F(MuleDs2Test, ACaptureAUnitSentIsStoredLikeTheSyntheticOne) {
    const char* path = std::getenv("HMS_CPAP_DS2R_CAPTURE");
    if (!path || !*path) GTEST_SKIP() << "HMS_CPAP_DS2R_CAPTURE not set (a .ds2r kept outside this repository)";
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in) << path;
    std::stringstream ss;
    ss << in.rdbuf();
    FakeUnit unit(*svc_);
    unit.hello();
    const char* from = std::getenv("HMS_CPAP_DS2R_IMPORT_FROM");
    ds2_->setImportFrom(kUnit, from && *from ? from : "2000-01-01");
    const auto f = readOf(ss.str());
    const auto r = ds2_->apply(kUnit, ss.str());
    EXPECT_TRUE(r.stored_ok);
    // Every record is kept or walked past; every card file of a kept one is placed.
    EXPECT_GE(r.records + r.walked_past, static_cast<int>(f.records.size()));
    std::cout << "capture: " << f.records.size() << " record(s), " << f.sealed.size() << " card file(s); "
              << r.records << " kept, " << r.card_files << " placed, " << r.walked_past
              << " walked past, error \"" << r.error << "\"" << std::endl;
}
