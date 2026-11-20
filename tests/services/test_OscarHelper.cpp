// SDD-081: a DreamStation 2 card is read by an OSCAR helper on a home PC.
//
// Both ends run in this process: hms-cpap's Host and reader on one side, the
// helper on the other, joined by a pipe that stands where the WebSocket is
// (the frames are the same text and binary frames). OSCAR is SDD-080's fake
// and its database is built by the suite's own SQL (OscarFakes.h). The card
// files are made-up bytes: nothing is a card, nothing is decrypted.

#include "database/SQLiteDatabase.h"
#include "services/OscarCardReader.h"
#include "services/OscarDb.h"
#include "services/OscarHelper.h"
#include "services/OscarHelperHost.h"
#include "services/OscarHelperWire.h"
#include "OscarFakes.h"

#include <gtest/gtest.h>
#include <json/json.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

using namespace hms_cpap;
using namespace hms_cpap::oscar;
using namespace hms_cpap::oscar::helper;
using namespace oscar_fakes;
namespace fs = std::filesystem;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// ── the pipe: a CpapDashLink whose far end is the Host, in process ────────

struct Inbox {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<CpapDashLink::Frame> q;
    void push(CpapDashLink::Frame f) {
        {
            std::lock_guard<std::mutex> lk(mu);
            q.push_back(std::move(f));
        }
        cv.notify_all();
    }
};

struct Wire {
    std::atomic<int> file_ready{0};   // what the host sent the helper
    std::atomic<int> imports{0};
};

class PipeLink : public CpapDashLink {
public:
    PipeLink(Host& h, CpapDashEndpoint ep, Wire* w) : host_(h), ep_(std::move(ep)), wire_(w) {}
    ~PipeLink() override { close(); }

    Connect connect(const std::string& token, const std::string&, const std::string&,
                    std::chrono::milliseconds) override {
        Connect out;
        std::string pc;
        bool probe = false;
        for (const auto& [k, v] : ep_.headers) {
            if (k == kHeaderName) pc = v;
            if (k == kHeaderProbe) probe = true;
        }
        const Admission a = host_.admit(token.empty() ? "" : "Bearer " + token, pc, probe);
        if (a.as == Admit::Refused) {
            out.http_status = a.status;
            out.http_body = frame(a.body);
            out.error = "the server refused the connection";
            return out;
        }
        auto in = inbox_;
        auto w = wire_;
        Outlets o;
        o.text = [in, w](const std::string& t) {
            if (w && t.find("\"file_ready\"") != std::string::npos) ++w->file_ready;
            if (w && t.find("\"import\"") != std::string::npos) ++w->imports;
            CpapDashLink::Frame f;
            f.text = t;
            in->push(std::move(f));
            return true;
        };
        o.binary = [in](const std::string& b) {
            CpapDashLink::Frame f;
            f.kind = CpapDashLink::Frame::Kind::Binary;
            f.text = b;
            in->push(std::move(f));
            return true;
        };
        o.close = [in] {
            CpapDashLink::Frame f;
            f.kind = CpapDashLink::Frame::Kind::Closed;
            in->push(std::move(f));
        };
        id_ = host_.open(a, std::move(o));
        open_ = true;
        out.ok = true;
        return out;
    }
    bool sendText(const std::string& f) override {
        if (!open_) return false;
        host_.onText(id_, f);
        return true;
    }
    bool sendBinary(const std::string& f) override {
        if (!open_) return false;
        host_.onBinary(id_, f);
        return true;
    }
    std::optional<Frame> next(std::chrono::milliseconds wait) override {
        std::unique_lock<std::mutex> lk(inbox_->mu);
        if (inbox_->q.empty()) inbox_->cv.wait_for(lk, wait, [&] { return !inbox_->q.empty(); });
        if (inbox_->q.empty()) return std::nullopt;
        Frame f = std::move(inbox_->q.front());
        inbox_->q.pop_front();
        return f;
    }
    void close() override {
        if (!open_) return;
        open_ = false;
        host_.closed(id_);
    }

private:
    Host& host_;
    CpapDashEndpoint ep_;
    Wire* wire_;
    std::shared_ptr<Inbox> inbox_ = std::make_shared<Inbox>();
    uint64_t id_{0};
    bool open_{false};
};

// ── one PC with the user's OSCAR, and its helper ──────────────────────────

class Pc {
public:
    Pc(const fs::path& dir, const std::string& name, Host& host, Wire* wire) : dir_(dir), name_(name) {
        oscar_db_ = std::make_unique<FakeOscarDb>(dir / "OSCAR_Data");
        HelperConfig c;
        c.host = "hms-cpap.test";
        c.oscar.program = "/opt/oscar/OSCAR20";
        c.oscar.data_dir = (dir / "OSCAR_Data").string();
        saveHelperConfig(config(), c);
        Helper::Options o;
        o.data_dir = dir.string();
        o.load = [this] { return loadHelperConfig(config()); };
        o.save = [this](const HelperConfig& h) { saveHelperConfig(config(), h); };
        o.factory = [&host, wire](const CpapDashEndpoint& ep) { return std::make_unique<PipeLink>(host, ep, wire); };
        o.runner = oscar_;
        o.version = "7.0.0";
        o.pc_name = name;
        o.backoff_min = std::chrono::milliseconds(10);
        o.backoff_max = std::chrono::milliseconds(40);
        o.chunk_bytes = 7;   // many chunks for a few bytes
        o.on_state = [this](const Json::Value& v) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                states_.push_back(v["state"].asString());
                last_ = v;
            }
            cv_.notify_all();
        };
        helper_ = std::make_unique<Helper>(o);
    }
    ~Pc() { helper_->stop(); }

    std::string config() const { return (dir_ / "config.json").string(); }
    Helper& helper() { return *helper_; }
    FakeOscarDb& oscarDb() { return *oscar_db_; }
    std::shared_ptr<FakeOscar> oscar() { return oscar_; }

    /// Waits for the state (a test's bound, not a schedule).
    bool waitFor(const std::string& state) {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, std::chrono::seconds(10), [&] { return last_["state"].asString() == state; });
    }
    Json::Value last() {
        std::lock_guard<std::mutex> lk(mu_);
        return last_;
    }

private:
    fs::path dir_;
    std::string name_;
    std::unique_ptr<FakeOscarDb> oscar_db_;
    std::shared_ptr<FakeOscar> oscar_ = std::make_shared<FakeOscar>();
    std::unique_ptr<Helper> helper_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::string> states_;
    Json::Value last_;
};

class OscarHelperTest : public ::testing::Test {
protected:
    fs::path dir_, card_;
    std::shared_ptr<SQLiteDatabase> db_;
    std::shared_ptr<Host> host_;
    std::atomic<int> hellos_{0};
    std::mutex hello_mu_;
    std::condition_variable hello_cv_;
    Wire wire_;

    bool waitHellos(int n) {
        std::unique_lock<std::mutex> lk(hello_mu_);
        return hello_cv_.wait_for(lk, std::chrono::seconds(10), [&] { return hellos_.load() >= n; });
    }

    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("oscar_sdd081_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        card_ = dir_ / "hms" / "sleepbridge" / "TAP-S3-00000081";
        cardFile(card_, "P-SERIES/1A2B3C4D/PROP.BIN", "made-up identity bytes");
        cardFile(card_, "P-SERIES/1A2B3C4D/P0/0000001E.B01", "made-up closing bytes");
        cardFile(card_, "P-SERIES/1A2B3C4D/P0/0000001E.B05", std::string(100, 'x'));
        setMtimeSeconds((card_ / "P-SERIES/1A2B3C4D/P0/0000001E.B05").string(), 1893456000);
        db_ = std::make_shared<SQLiteDatabase>((dir_ / "hms.db").string());
        ASSERT_TRUE(db_->connect());
        Host::Options ho;
        ho.state_dir = (dir_ / "hms" / "oscar-helper").string();
        ho.install_id = "00000000-0000-4000-8000-000000000081";
        ho.version = "7.0.0";
        ho.name = "hub";
        ho.chunk_bytes = 5;
        ho.on_hello = [this](const std::string&) {
            {
                std::lock_guard<std::mutex> lk(hello_mu_);
                ++hellos_;
            }
            hello_cv_.notify_all();
        };
        host_ = std::make_shared<Host>(ho);
    }
    void TearDown() override {
        host_->shutdown();
        db_->disconnect();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    OscarCardReader::Options options() {
        OscarCardReader::Options o;
        o.settings = [] { return Settings{}; };   // nothing of OSCAR on this computer
        o.card_root = [this] { return card_.string(); };
        o.db = db_;
        o.device_id = kDevice;
        o.device_name = "DreamStation 2";
        o.containerised = true;
        o.remote = host_;
        o.slice_dir = (dir_ / "hms" / "oscar-helper" / "slice").string();
        return o;
    }

    std::unique_ptr<Pc> pc(const std::string& name) {
        return std::make_unique<Pc>(dir_ / name, name, *host_, &wire_);
    }

    int sessions() {
        const auto rows = db_->executeQuery("SELECT COUNT(*) AS v FROM cpap_sessions WHERE device_id = ?", {kDevice});
        const Json::Value& v = rows[0]["v"];
        return v.isString() ? std::stoi(v.asString()) : v.asInt();
    }
};

}  // namespace

// ── the wire ──────────────────────────────────────────────────────────────

TEST(OscarHelperWire, OnlyARelativePathUnderPSeriesIsTaken) {
    EXPECT_TRUE(safeCardPath("P-SERIES/1A2B3C4D/P0/0000001E.B01"));
    EXPECT_TRUE(safeCardPath("P-SERIES/1A2B3C4D/PROP.BIN"));
    EXPECT_TRUE(safeCardPath("p-series/x/LOG.SEQ"));
    for (const char* bad : {"", "P-SERIES", "P-SERIES/", "/P-SERIES/x", "P-SERIES/../x", "P-SERIES/a/../../x",
                            "P-SERIES/./x", "P-SERIES//x", "DATALOG/x.edf", "P-SERIES\\x", "C:/P-SERIES/x",
                            "../P-SERIES/x", "P-SERIES/x/.."})
        EXPECT_FALSE(safeCardPath(bad)) << bad;
}

TEST(OscarHelperWire, AFileIsChangedByItsMtimeAndItsSizeNeverItsSizeAlone) {
    const fs::path d = fs::temp_directory_path() / "oscar_sdd081_changed";
    fs::remove_all(d);
    cardFile(d, "P-SERIES/M/P0/00000001.B05", "abcd");
    cardFile(d, "P-SERIES/M/PROP.BIN", "p");
    cardFile(d, "DATALOG/not-taken.edf", "e");
    setMtimeSeconds((d / "P-SERIES/M/P0/00000001.B05").string(), 1000);
    setMtimeSeconds((d / "P-SERIES/M/PROP.BIN").string(), 1000);
    auto held = listCard(d.string());
    ASSERT_EQ(held.size(), 2u) << "P-SERIES only";
    EXPECT_TRUE(changedFiles(d.string(), held).empty());
    // Same size, another mtime: changed.
    setMtimeSeconds((d / "P-SERIES/M/PROP.BIN").string(), 2000);
    auto ch = changedFiles(d.string(), held);
    ASSERT_EQ(ch.size(), 1u);
    EXPECT_EQ(ch[0].path, "P-SERIES/M/PROP.BIN");
    // Same mtime, another size: changed.
    held = listCard(d.string());
    cardFile(d, "P-SERIES/M/P0/00000001.B05", "abcdef");
    setMtimeSeconds((d / "P-SERIES/M/P0/00000001.B05").string(), 1000);
    ch = changedFiles(d.string(), held);
    ASSERT_EQ(ch.size(), 1u);
    EXPECT_EQ(ch[0].path, "P-SERIES/M/P0/00000001.B05");
    // A list from the wire keeps only safe paths.
    Json::Value w = toJson(held);
    Json::Value bad(Json::objectValue);
    bad["path"] = "../../etc/passwd";
    w.append(bad);
    EXPECT_EQ(filesFromJson(w).size(), held.size());
    fs::remove_all(d);
}

TEST(OscarHelperWire, ChunksComeBackWholeAndOutOfOrderIsBroken) {
    const std::string bytes = "the encrypted bytes, as the machine wrote them";
    Assembly a;
    const auto parts = chunks(9, bytes, 4);
    EXPECT_EQ(parts.size(), (bytes.size() + 3) / 4);
    for (const auto& p : parts) {
        const auto c = parseChunk(p);
        ASSERT_TRUE(c);
        EXPECT_EQ(c->kind, kKindBytes);
        EXPECT_TRUE(a.add(*c));
    }
    EXPECT_EQ(a.count(9), parts.size());
    EXPECT_EQ(a.take(9), bytes);
    const auto c1 = parseChunk(parts[1]);
    EXPECT_FALSE(a.add(*c1)) << "a chunk before its first";
    EXPECT_EQ(chunks(1, "", 4).size(), 1u) << "an empty file is one frame";
    EXPECT_EQ(newToken().size(), 64u);
    EXPECT_NE(newToken(), newToken());
}

// ── the small oscar.db (D5.4) ─────────────────────────────────────────────

TEST_F(OscarHelperTest, TheSliceHoldsOnlyTheWantedSessionsAndReadsAsTheWholeDoes) {
    const fs::path data = dir_ / "pc" / "OSCAR_Data";
    FakeOscarDb o(data);
    o.session(o.user_, 0x1E, kT0, 10);          // the user's own profile: never sent
    o.session(o.hms_, 0x1E, kT0, 10, 2);
    o.session(o.hms_, 0x1F, kT0 + 86400000, 10, 3);
    o.exec("PRAGMA wal_checkpoint(TRUNCATE)");
    const std::string before = readFile(data / "oscar.db");

    std::string why;
    auto db = OscarDb::open(data.string(), &why);
    ASSERT_TRUE(db) << why;
    const fs::path slice = dir_ / "slice" / "oscar.db";
    ASSERT_TRUE(db->writeSlice(kDefaultProfile, {std::string(kSerial) + "/31"}, slice.string(), &why)) << why;
    EXPECT_EQ(readFile(data / "oscar.db"), before) << "the user's oscar.db is never written";

    auto s = OscarDb::open(slice.parent_path().string(), &why);
    ASSERT_TRUE(s) << why;
    EXPECT_TRUE(s->gate().ok) << "the version and schema rows came along";
    EXPECT_EQ(s->profiles(), std::vector<std::string>{kDefaultProfile});
    const auto heads = s->sessions(kDefaultProfile);
    ASSERT_EQ(heads.size(), 1u);
    EXPECT_EQ(heads[0].session_id, 0x1F);

    // Read from the slice exactly as from the whole.
    const auto whole_heads = db->sessions(kDefaultProfile);
    ASSERT_EQ(whole_heads.size(), 2u);
    auto a = db->read(whole_heads[1], kDevice, "x", &why);
    auto b = s->read(heads[0], kDevice, "x", &why);
    ASSERT_TRUE(a && b) << why;
    EXPECT_EQ(a->events.size(), b->events.size());
    EXPECT_EQ(a->breaths.size(), b->breaths.size());
    EXPECT_EQ(a->breathing_summary.size(), b->breathing_summary.size());
    ASSERT_TRUE(a->metrics && b->metrics);
    EXPECT_EQ(a->metrics->obstructive_apneas, b->metrics->obstructive_apneas);
    EXPECT_EQ(b->metrics->obstructive_apneas, 3);
    EXPECT_EQ(a->metrics->pressure_p95, b->metrics->pressure_p95);
}

// ── trust on first use (D4) ───────────────────────────────────────────────

TEST_F(OscarHelperTest, TheFirstHelperIsRememberedAndOnlyItsTokenIsLetIn) {
    // A connection that never says hello pairs nothing.
    const auto first = host_->admit("", "ALBIN-PC", false);
    EXPECT_EQ(first.as, Admit::Pairing);
    host_->closed(host_->open(first, {[](const std::string&) { return true; }, nullptr, nullptr}));
    EXPECT_FALSE(host_->remembered());

    // The hello is what pairs: the token goes in the answer, silently.
    std::vector<std::string> sent;
    const uint64_t id = host_->open(host_->admit("", "ALBIN-PC", false),
                                    {[&](const std::string& t) { sent.push_back(t); return true; }, nullptr, nullptr});
    host_->onText(id, R"({"t":"hello","proto":1,"version":"7.0.0","files":[]})");
    ASSERT_GE(sent.size(), 2u);
    const auto paired = parse(sent[0]);
    ASSERT_TRUE(paired);
    EXPECT_EQ((*paired)["t"].asString(), "paired");
    const std::string token = (*paired)["token"].asString();
    EXPECT_EQ(token.size(), 64u);
    EXPECT_EQ((*parse(sent[1]))["t"].asString(), "ack");
    EXPECT_EQ(hellos_.load(), 1) << "the hello is a read";

    // Kept: the hash and the name, never the token.
    const std::string kept = readFile(dir_ / "hms" / "oscar-helper" / "helper.json");
    EXPECT_EQ(kept.find(token), std::string::npos);
    EXPECT_NE(kept.find(sha256Hex(token)), std::string::npos);
    EXPECT_NE(kept.find("ALBIN-PC"), std::string::npos);

    EXPECT_EQ(host_->admit("Bearer " + token, "whatever", false).as, Admit::Helper);
    const auto other = host_->admit("", "OTHER-PC", false);
    EXPECT_EQ(other.as, Admit::Refused);
    EXPECT_EQ(other.status, 401);
    EXPECT_EQ(other.body["error"].asString(), "has_helper");
    EXPECT_EQ(other.body["helper"].asString(), "ALBIN-PC");
    EXPECT_EQ(host_->admit("Bearer " + std::string(64, 'a'), "OTHER-PC", false).body["helper"].asString(),
              "ALBIN-PC");
    EXPECT_EQ(host_->admit("", "PROBE", true).as, Admit::Probe) << "a probe is always answered";

    // A new process remembers it.
    Host::Options ho;
    ho.state_dir = (dir_ / "hms" / "oscar-helper").string();
    Host restarted(ho);
    EXPECT_TRUE(restarted.remembered());
    EXPECT_EQ(restarted.admit("Bearer " + token, "", false).as, Admit::Helper);

    EXPECT_TRUE(host_->forget());
    EXPECT_FALSE(host_->remembered());
    const auto forgotten = host_->admit("Bearer " + token, "ALBIN-PC", false);
    EXPECT_EQ(forgotten.as, Admit::Refused);
    EXPECT_EQ(forgotten.body["error"].asString(), "forgotten") << "a forgotten PC never takes the place back";
    EXPECT_EQ(host_->admit("", "OTHER-PC", false).as, Admit::Pairing);
}

TEST_F(OscarHelperTest, AProbeIsAnsweredWithWhoThisIsAndIsNeverASession) {
    std::vector<std::string> sent;
    bool closed = false;
    const auto a = host_->admit("", "ALBIN-PC", true);
    const uint64_t id = host_->open(
        a, {[&](const std::string& t) { sent.push_back(t); return true; }, nullptr, [&] { closed = true; }});
    host_->onText(id, R"({"t":"hello","probe":true,"proto":1})");
    ASSERT_EQ(sent.size(), 1u);
    const auto ack = parse(sent[0]);
    EXPECT_EQ((*ack)["t"].asString(), "ack");
    EXPECT_EQ((*ack)["hms_cpap"]["name"].asString(), "hub");
    EXPECT_EQ((*ack)["hms_cpap"]["install_id"].asString(), "00000000-0000-4000-8000-000000000081");
    EXPECT_EQ((*ack)["hms_cpap"]["helper"].asString(), "");
    EXPECT_TRUE(closed);
    EXPECT_FALSE(host_->remembered()) << "nothing paired";
    EXPECT_EQ(hellos_.load(), 0) << "no read";
}

TEST_F(OscarHelperTest, AnotherProtocolIsRefusedAndTheTileSaysUpdate) {
    std::vector<std::string> sent;
    const uint64_t id = host_->open(host_->admit("", "ALBIN-PC", false),
                                    {[&](const std::string& t) { sent.push_back(t); return true; }, nullptr, nullptr});
    host_->onText(id, R"({"t":"hello","proto":2,"version":"8.0.0","files":[]})");
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ((*parse(sent[0]))["error"].asString(), "proto");
    EXPECT_FALSE(host_->remembered()) << "a helper it cannot speak to is not remembered";
    EXPECT_TRUE(host_->status()["update"].asBool());

    // The remembered helper, updated to a protocol this hms-cpap does not
    // speak: refused, and the read says what to do.
    auto albin = pc("ALBIN-PC");
    albin->helper().start();
    ASSERT_TRUE(albin->waitFor("connected"));
    albin->helper().stop();
    const std::string token = loadHelperConfig(albin->config()).token;
    sent.clear();
    const uint64_t id2 = host_->open(host_->admit("Bearer " + token, "ALBIN-PC", false),
                                     {[&](const std::string& t) { sent.push_back(t); return true; }, nullptr, nullptr});
    host_->onText(id2, R"({"t":"hello","proto":2,"version":"8.0.0","files":[]})");
    host_->closed(id2);
    OscarCardReader reader(options());
    const auto r = reader.readOnce();
    EXPECT_EQ(r.outcome, Outcome::HelperUpdate);
    EXPECT_NE(r.message.find("update hms-cpap on ALBIN-PC"), std::string::npos) << r.message;
    EXPECT_EQ(reader.status()["state"].asString(), "helper_update");
}

// ── the read, end to end over the two in-process ends (D5) ────────────────

TEST_F(OscarHelperTest, AReadSendsTheCardImportsOnThePcAndStoresWhatCameBackAsCard) {
    auto albin = pc("ALBIN-PC");
    // The user's OSCAR on the PC, faked: it imports from the helper's copy.
    std::string imported_from;
    albin->oscar()->on_run = [&](const std::vector<std::string>& argv) {
        imported_from = argv.back();
        if (albin->oscar()->runs.size() == 1) albin->oscarDb().session(albin->oscarDb().hms_, 0x1E, kT0, 10, 2);
        return exited(0);
    };
    albin->helper().start();
    ASSERT_TRUE(albin->waitFor("connected"));
    EXPECT_TRUE(host_->remembered());
    EXPECT_EQ(host_->status()["name"].asString(), "ALBIN-PC");
    EXPECT_FALSE(loadHelperConfig(albin->config()).token.empty()) << "the token is kept in config.json";
    EXPECT_EQ(loadHelperConfig(albin->config()).install_id, "00000000-0000-4000-8000-000000000081");
    EXPECT_EQ(hellos_.load(), 1);

    OscarCardReader reader(options());
    auto r = reader.readOnce();
    EXPECT_EQ(r.outcome, Outcome::Imported) << r.message;
    EXPECT_EQ(r.stored, 1);
    EXPECT_EQ(r.version, "2.1.1+00000000");
    EXPECT_EQ(sessions(), 1);
    EXPECT_TRUE(db_->sessionExists(kDevice, sessionClock(kT0)));
    // Stored as the card's (SDD-080 D5): its files are this card's own.
    const auto rows = db_->executeQuery(
        "SELECT COUNT(*) AS v FROM cpap_session_files f JOIN cpap_sessions s ON s.id = f.session_id "
        "WHERE s.device_id = ? AND f.source = 'card'",
        {kDevice});
    EXPECT_GE(rows[0]["v"].isString() ? std::stoi(rows[0]["v"].asString()) : rows[0]["v"].asInt(), 1);

    // The PC's copy is the card, byte for byte, with its times.
    const fs::path copy = albin->helper().cardRoot("00000000-0000-4000-8000-000000000081");
    EXPECT_EQ(imported_from, copy.string()) << "OSCAR is pointed at the copy's card root";
    EXPECT_EQ(albin->oscar()->runs[0], importCommand(loadHelperConfig(albin->config()).oscar, copy.string()));
    for (const char* rel : {"P-SERIES/1A2B3C4D/PROP.BIN", "P-SERIES/1A2B3C4D/P0/0000001E.B01",
                            "P-SERIES/1A2B3C4D/P0/0000001E.B05"}) {
        EXPECT_EQ(readFile(copy / rel), readFile(card_ / rel)) << rel;
        EXPECT_EQ(mtimeSeconds((copy / rel).string()), mtimeSeconds((card_ / rel).string())) << rel;
    }
    EXPECT_EQ(wire_.file_ready.load(), 3);
    EXPECT_FALSE(fs::exists(dir_ / "hms" / "oscar-helper" / "slice")) << "kept for the one pass";
    EXPECT_FALSE(fs::exists(dir_ / "ALBIN-PC" / "oscar-helper" / "slice"));
    EXPECT_EQ(reader.status()["where"].asString(), "helper");
    EXPECT_EQ(reader.status()["helper"]["name"].asString(), "ALBIN-PC");

    // Nothing changed: nothing sent, a cheap "nothing new".
    r = reader.readOnce();
    EXPECT_EQ(r.outcome, Outcome::NothingNew) << r.message;
    EXPECT_EQ(wire_.file_ready.load(), 3);
    EXPECT_EQ(sessions(), 1);

    // A night: the new files go, and only they.
    cardFile(card_, "P-SERIES/1A2B3C4D/P0/0000001F.B01", "made-up closing bytes, night two");
    albin->oscar()->on_run = [&](const std::vector<std::string>&) {
        albin->oscarDb().session(albin->oscarDb().hms_, 0x1F, kT0 + 86400000, 10, 1);
        return exited(0);
    };
    r = reader.readOnce();
    EXPECT_EQ(r.outcome, Outcome::Imported) << r.message;
    EXPECT_EQ(r.stored, 1) << "only the new session came back";
    EXPECT_EQ(wire_.file_ready.load(), 4);
    EXPECT_EQ(sessions(), 2);
}

TEST_F(OscarHelperTest, ThePcOffWaitsAndItsHelloReadsTheNight) {
    // Remembered once, then off.
    auto albin = pc("ALBIN-PC");
    albin->helper().start();
    ASSERT_TRUE(albin->waitFor("connected"));
    albin->helper().stop();
    ASSERT_TRUE(host_->remembered());
    EXPECT_FALSE(host_->status()["connected"].asBool());
    OscarCardReader reader(options());
    const auto off = reader.readOnce();
    EXPECT_EQ(off.outcome, Outcome::HelperOffline);
    EXPECT_NE(off.message.find("ALBIN-PC"), std::string::npos);
    EXPECT_EQ(reader.status()["state"].asString(), "helper_offline");
    EXPECT_EQ(albin->oscar()->runs.size(), 0u) << "nothing read";

    // Switched on: its hello is the event (D6), nothing else.
    const int before = hellos_.load();
    albin->oscar()->on_run = [&](const std::vector<std::string>&) {
        albin->oscarDb().session(albin->oscarDb().hms_, 0x1E, kT0, 10);
        return exited(0);
    };
    albin->helper().start();
    ASSERT_TRUE(waitHellos(before + 1));
    EXPECT_EQ(reader.readOnce().stored, 1);
}

TEST_F(OscarHelperTest, OscarBusyOnThePcIsSaidAndNothingIsStored) {
    auto albin = pc("ALBIN-PC");
    albin->oscar()->on_run = [](const std::vector<std::string>&) { return exited(75); };
    albin->helper().start();
    ASSERT_TRUE(albin->waitFor("connected"));
    OscarCardReader reader(options());
    const auto r = reader.readOnce();
    EXPECT_EQ(r.outcome, Outcome::Busy);
    EXPECT_EQ(sessions(), 0);
    EXPECT_EQ(reader.status()["state"].asString(), "busy");
    EXPECT_EQ(albin->helper().state()["last_outcome"].asString(), "busy");
}

TEST_F(OscarHelperTest, ASecondPcIsRefusedThenForgetLetsItInAndTheFirstIsTold) {
    auto first = pc("ALBIN-PC");
    first->helper().start();
    ASSERT_TRUE(first->waitFor("connected"));

    auto second = pc("OTHER-PC");
    second->helper().start();
    ASSERT_TRUE(second->waitFor("refused"));
    EXPECT_EQ(second->last()["helper"].asString(), "ALBIN-PC") << "hms-cpap already reads OSCAR on ALBIN-PC";
    EXPECT_TRUE(loadHelperConfig(second->config()).token.empty());

    // Forget on the Devices page: the first PC, still on, is refused and
    // says so, and does not take the place back by itself.
    ASSERT_TRUE(host_->forget());
    ASSERT_TRUE(first->waitFor("forgotten"));
    EXPECT_FALSE(host_->remembered());

    second->helper().connectAgain();
    ASSERT_TRUE(second->waitFor("connected"));
    EXPECT_EQ(host_->status()["name"].asString(), "OTHER-PC");

    // "Connect again" on the first PC now meets the second.
    first->helper().connectAgain();
    ASSERT_TRUE(first->waitFor("refused"));
    EXPECT_EQ(first->last()["helper"].asString(), "OTHER-PC");
    EXPECT_EQ(host_->status()["name"].asString(), "OTHER-PC");
}

// ── a helper end the suite scripts, for what a real helper never sends ────

namespace {

struct Scripted {
    Host& host;
    uint64_t id{0};
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::string> texts;
    bool closed{false};

    explicit Scripted(Host& h) : host(h) {
        id = host.open(host.admit("", "ROGUE-PC", false),
                       {[this](const std::string& t) {
                            {
                                std::lock_guard<std::mutex> lk(mu);
                                texts.push_back(t);
                            }
                            cv.notify_all();
                            return true;
                        },
                        [](const std::string&) { return true; },
                        [this] {
                            {
                                std::lock_guard<std::mutex> lk(mu);
                                closed = true;
                            }
                            cv.notify_all();
                        }});
        host.onText(id, R"({"t":"hello","proto":1,"version":"7.0.0","files":[]})");
    }
    Json::Value next() {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait_for(lk, std::chrono::seconds(10), [&] { return !texts.empty(); });
        if (texts.empty()) return Json::Value();
        const auto t = texts.front();
        texts.pop_front();
        return parse(t).value_or(Json::Value());
    }
    void say(const Json::Value& v) { host.onText(id, frame(v)); }
};

}  // namespace

TEST_F(OscarHelperTest, TheReturnedFilePassesHmsCpapsGateOrIsNotRead) {
    Scripted h(*host_);
    EXPECT_EQ(h.next()["t"].asString(), "paired");
    EXPECT_EQ(h.next()["t"].asString(), "ack");

    // The slice it will send: from an OSCAR this build was not proven on.
    const fs::path other = dir_ / "other";
    {
        FakeOscarDb o(other, "2.2.0+0", 19);
        o.session(o.hms_, 0x1E, kT0, 5);
    }
    std::string why;
    auto odb = OscarDb::open(other.string(), &why);
    ASSERT_TRUE(odb);
    const fs::path slice = dir_ / "rogue-slice" / "oscar.db";
    ASSERT_TRUE(odb->writeSlice(kDefaultProfile, {std::string(kSerial) + "/30"}, slice.string(), &why)) << why;
    odb.reset();
    const std::string bytes = readFile(slice);

    OscarCardReader reader(options());
    OscarCardReader::ReadResult r;
    std::thread t([&] { r = reader.readOnce(); });
    for (;;) {
        const auto f = h.next();
        ASSERT_TRUE(f.isObject()) << "the read stopped talking";
        const std::string kind = f["t"].asString() == "cmd" ? f["cmd"].asString() : f["t"].asString();
        Json::Value a(Json::objectValue);
        a["req_id"] = f["req_id"];
        if (kind == "file_ready") {
            a["t"] = "file_ack";
            a["ok"] = true;
            h.say(a);
        } else if (kind == "import") {
            a["t"] = "result";
            a["outcome"] = "imported";
            a["version"] = "2.1.1+00000000";
            Json::Value hd(Json::objectValue);
            hd["key"] = std::string(kSerial) + "/30";
            hd["start_ms"] = static_cast<Json::Int64>(kT0);
            hd["end_ms"] = static_cast<Json::Int64>(kT0 + 300000);
            a["heads"].append(hd);
            h.say(a);
        } else if (kind == "want") {
            const auto parts = chunks(f["req_id"].asUInt(), bytes, 1024);
            for (const auto& p : parts) host_->onBinary(h.id, p);
            a["t"] = "slice_ready";
            a["chunks"] = static_cast<Json::UInt>(parts.size());
            a["size"] = static_cast<Json::UInt64>(bytes.size());
            a["md5"] = md5Hex(bytes);
            h.say(a);
        } else if (kind == "file_ack") {
            EXPECT_TRUE(f["ok"].asBool()) << "it arrived whole";
            break;
        }
    }
    t.join();
    EXPECT_EQ(r.outcome, Outcome::NotSupported) << r.message;
    EXPECT_NE(r.message.find("2.2.0"), std::string::npos);
    EXPECT_EQ(sessions(), 0);
    EXPECT_FALSE(fs::exists(dir_ / "hms" / "oscar-helper" / "slice")) << "deleted after the pass";
}

TEST_F(OscarHelperTest, TheConnectionDroppingDuringAReadEndsItOffline) {
    Scripted h(*host_);
    h.next();
    h.next();
    OscarCardReader reader(options());
    OscarCardReader::ReadResult r;
    std::thread t([&] { r = reader.readOnce(); });
    // The first file arrives; the PC goes to sleep before answering.
    EXPECT_EQ(h.next()["t"].asString(), "file_ready");
    host_->closed(h.id);
    t.join();
    EXPECT_EQ(r.outcome, Outcome::HelperOffline);
    EXPECT_EQ(sessions(), 0);
}

TEST_F(OscarHelperTest, AFileOutsidePSeriesIsRefusedByTheHelper) {
    auto rogue = std::make_shared<Inbox>();
    std::vector<std::string> answers;
    std::mutex mu;
    // A far end that answers the hello, then offers a path out of the copy.
    class RogueLink : public CpapDashLink {
    public:
        RogueLink(std::shared_ptr<Inbox> in, std::vector<std::string>* out, std::mutex* mu)
            : in_(std::move(in)), out_(out), mu_(mu) {}
        Connect connect(const std::string&, const std::string&, const std::string&,
                        std::chrono::milliseconds) override {
            Connect c;
            c.ok = true;
            return c;
        }
        bool sendText(const std::string& f) override {
            std::lock_guard<std::mutex> lk(*mu_);
            out_->push_back(f);
            return true;
        }
        bool sendBinary(const std::string&) override { return true; }
        std::optional<Frame> next(std::chrono::milliseconds wait) override {
            std::unique_lock<std::mutex> lk(in_->mu);
            if (in_->q.empty()) in_->cv.wait_for(lk, wait, [&] { return !in_->q.empty(); });
            if (in_->q.empty()) return std::nullopt;
            Frame f = std::move(in_->q.front());
            in_->q.pop_front();
            return f;
        }
        void close() override {}

    private:
        std::shared_ptr<Inbox> in_;
        std::vector<std::string>* out_;
        std::mutex* mu_;
    };
    const fs::path pcdir = dir_ / "PC";
    HelperConfig c;
    c.host = "hms-cpap.test";
    c.token = "t";
    saveHelperConfig((pcdir / "config.json").string(), c);
    Helper::Options o;
    o.data_dir = pcdir.string();
    o.load = [&] { return loadHelperConfig((pcdir / "config.json").string()); };
    o.save = [&](const HelperConfig& x) { saveHelperConfig((pcdir / "config.json").string(), x); };
    o.factory = [&](const CpapDashEndpoint&) { return std::make_unique<RogueLink>(rogue, &answers, &mu); };
    o.backoff_min = std::chrono::milliseconds(10);
    Helper helper(o);
    const auto push = [&](CpapDashLink::Frame f) { rogue->push(std::move(f)); };
    CpapDashLink::Frame ack;
    ack.text = R"({"t":"ack","hms_cpap":{"name":"hub","install_id":"x-1","version":"7.0.0","helper":"PC"}})";
    push(ack);
    for (const char* path : {"P-SERIES/../../outside.bin", "DATALOG/x.edf"}) {
        for (const auto& part : chunks(7, "evil", 64)) {
            CpapDashLink::Frame b;
            b.kind = CpapDashLink::Frame::Kind::Binary;
            b.text = part;
            push(b);
        }
        Json::Value fr(Json::objectValue);
        fr["t"] = "file_ready";
        fr["req_id"] = 7;
        fr["path"] = path;
        fr["chunks"] = 1;
        fr["size"] = 4;
        fr["md5"] = md5Hex("evil");
        fr["mtime"] = 1;
        CpapDashLink::Frame t;
        t.text = frame(fr);
        push(t);
    }
    CpapDashLink::Frame bye;
    bye.kind = CpapDashLink::Frame::Kind::Closed;
    push(bye);
    EXPECT_EQ(helper.runOnce(), Helper::End::Closed);
    int refused = 0;
    for (const auto& a : answers) {
        const auto v = parse(a);
        if (v && (*v)["t"].asString() == "file_ack") {
            EXPECT_FALSE((*v)["ok"].asBool());
            ++refused;
        }
    }
    EXPECT_EQ(refused, 2);
    EXPECT_FALSE(fs::exists(pcdir / "oscar-helper" / "outside.bin"));
    EXPECT_FALSE(fs::exists(pcdir / "outside.bin"));
    EXPECT_FALSE(fs::exists(pcdir / "oscar-helper" / "x-1" / "DATALOG"));
}

TEST_F(OscarHelperTest, TheHelperConfigKeepsEveryOtherKeyAndSaysItsRole) {
    const fs::path cfg = dir_ / "cfg" / "config.json";
    fs::create_directories(cfg.parent_path());
    std::ofstream(cfg) << R"({"web_port": 8893, "logging": {"enabled": true}})";
    HelperConfig c;
    c.host = "192.168.2.15";
    c.token = "secret";
    c.oscar.data_dir = "/home/u/Documents/OSCAR_Data";
    ASSERT_TRUE(saveHelperConfig(cfg.string(), c));
    EXPECT_EQ(configRole(cfg.string()), "oscar_helper");
    const auto back = loadHelperConfig(cfg.string());
    EXPECT_EQ(back.host, "192.168.2.15");
    EXPECT_EQ(back.token, "secret");
    EXPECT_EQ(back.port, 8894);
    EXPECT_EQ(back.oscar.profile, kDefaultProfile);
    const std::string text = readFile(cfg);
    EXPECT_NE(text.find("\"web_port\": 8893"), std::string::npos);
}
