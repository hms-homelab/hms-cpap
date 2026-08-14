// SDD-016: the database probe the Qt supervisor can reach without a server.
//
// These cover the request/response contract rather than the engines themselves
// -- probeDatabase() and provisionDatabase() already have their own suites. The
// thing worth pinning here is that a GUI gets a usable answer for EVERY input,
// including the malformed ones, because the supervisor has to render something
// either way and an exception would leave it with nothing to show.

#include <gtest/gtest.h>

#include "utils/DbProbeCli.h"
#include "services/SetupService.h"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <string>

using hms_cpap::runDbProbeCli;
using hms_cpap::SetupService;

namespace {

nlohmann::json parse(const std::string& s) {
    return nlohmann::json::parse(s);
}

}  // namespace

// ── The shape, on every path ────────────────────────────────────────────────
//
// The supervisor reads these four keys unconditionally. A response that omits
// one on some paths would make the GUI branch on which failure it hit.

TEST(DbProbeCli, EveryAnswerCarriesTheSameFourKeys) {
    const std::string inputs[] = {
        R"({"type":"sqlite","sqlite_path":":memory:"})",
        R"({"type":"nonsense"})",
        R"(not json at all)",
        R"([1,2,3])",
        R"({})",
    };
    for (const auto& in : inputs) {
        const auto r = runDbProbeCli(in, false);
        nlohmann::json j;
        ASSERT_NO_THROW(j = parse(r.json)) << "response was not valid JSON for: " << in;
        EXPECT_TRUE(j.contains("ok"))             << in;
        EXPECT_TRUE(j.contains("error"))          << in;
        EXPECT_TRUE(j.contains("schema_present")) << in;
        EXPECT_TRUE(j.contains("session_count"))  << in;
    }
}

TEST(DbProbeCli, TheExitCodeIsTheVerdict) {
    // Same contract as --preflight, because the supervisor blocks on both the
    // same way and should not need two ways to read an answer.
    const auto bad = runDbProbeCli(R"({"type":"nonsense"})", false);
    EXPECT_EQ(bad.exit_code, 1);
    EXPECT_FALSE(parse(bad.json)["ok"].get<bool>());
}

// ── Malformed input is an answer, not a crash ───────────────────────────────

TEST(DbProbeCli, GarbageJsonExplainsItselfRatherThanThrowing) {
    const auto r = runDbProbeCli("{ this is not json", false);
    EXPECT_EQ(r.exit_code, 1);
    const auto j = parse(r.json);
    EXPECT_FALSE(j["ok"].get<bool>());
    EXPECT_NE(j["error"].get<std::string>().find("not valid JSON"), std::string::npos)
        << "the message should say what is wrong with the request: "
        << j["error"].get<std::string>();
}

TEST(DbProbeCli, ANonObjectRequestIsRefused) {
    const auto r = runDbProbeCli(R"(["postgresql"])", false);
    EXPECT_FALSE(parse(r.json)["ok"].get<bool>());
    EXPECT_NE(parse(r.json)["error"].get<std::string>().find("JSON object"),
              std::string::npos);
}

TEST(DbProbeCli, AnUnsupportedBackendNamesTheBackend) {
    const auto r = runDbProbeCli(R"({"type":"oracle"})", false);
    const auto err = parse(r.json)["error"].get<std::string>();
    EXPECT_NE(err.find("oracle"), std::string::npos)
        << "the user needs to know WHICH backend this build cannot open: " << err;
}

// ── Widgets are allowed to disagree about types ─────────────────────────────
//
// A spin box hands back 5432 and a text field hands back "5432". Both mean the
// same thing, and which one arrives depends on how the form was built, not on
// what the user meant.

TEST(DbProbeCli, APortGivenAsAStringIsAccepted) {
    // sqlite ignores the port, so this reaches the probe without needing a
    // server: what is being pinned is that a string port does not blow up
    // parsing before it gets there.
    const auto num = runDbProbeCli(R"({"type":"sqlite","port":5432,"sqlite_path":":memory:"})", false);
    const auto txt = runDbProbeCli(R"({"type":"sqlite","port":"5432","sqlite_path":":memory:"})", false);
    EXPECT_EQ(parse(num.json)["ok"], parse(txt.json)["ok"]);
}

TEST(DbProbeCli, AMissingTypeDefaultsToSqliteForAProbe) {
    // The wizard's first screen offers SQLite as the zero-configuration answer,
    // so an omitted type means that rather than an error.
    const auto tmp = std::filesystem::temp_directory_path() /
                     ("hms_probe_default_" + std::to_string(::getpid()) + ".db");
    std::filesystem::remove(tmp);

    nlohmann::json req;
    req["sqlite_path"] = tmp.string();   // no "type" at all
    const auto r = runDbProbeCli(req.dump(), false);

    EXPECT_TRUE(parse(r.json)["ok"].get<bool>())
        << "an omitted type should be read as sqlite, got: " << r.json;
    EXPECT_EQ(parse(r.json)["error"].get<std::string>(), "");
}

TEST(DbProbeCli, AMissingTypeIsRefusedForProvisioning) {
    // There is no server to create a database on, so silently assuming sqlite
    // would answer a question the caller did not ask.
    const auto r = runDbProbeCli(R"({"name":"cpap","user":"cpap_user"})", true);
    EXPECT_EQ(r.exit_code, 1);
    const auto err = parse(r.json)["error"].get<std::string>();
    EXPECT_NE(err.find("no database type"), std::string::npos) << err;
}

// ── SQLite actually answers ─────────────────────────────────────────────────

TEST(DbProbeCli, AnAbsentSqliteFileIsNotAFailure) {
    // A database that does not exist yet is the normal first-run case: it will
    // be created at first start. Reporting it as broken would send the user
    // hunting for a problem they do not have.
    const auto tmp = std::filesystem::temp_directory_path() /
                     ("hms_probe_absent_" + std::to_string(::getpid()) + ".db");
    std::filesystem::remove(tmp);

    nlohmann::json req;
    req["type"] = "sqlite";
    req["sqlite_path"] = tmp.string();
    const auto r = runDbProbeCli(req.dump(), false);

    EXPECT_TRUE(parse(r.json)["ok"].get<bool>()) << r.json;
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_FALSE(parse(r.json)["schema_present"].get<bool>());
    EXPECT_FALSE(std::filesystem::exists(tmp))
        << "a PROBE must not create the database it was asked to look at";
}

TEST(DbProbeCli, AFileThatIsNotADatabaseIsReportedAsSuch) {
    const auto tmp = std::filesystem::temp_directory_path() /
                     ("hms_probe_junk_" + std::to_string(::getpid()) + ".db");
    { std::ofstream f(tmp); f << "this is not a sqlite file"; }

    nlohmann::json req;
    req["type"] = "sqlite";
    req["sqlite_path"] = tmp.string();
    const auto r = runDbProbeCli(req.dump(), false);

    EXPECT_FALSE(parse(r.json)["ok"].get<bool>())
        << "a text file opened as a database should not read as healthy";
    EXPECT_FALSE(parse(r.json)["error"].get<std::string>().empty())
        << "a failure must carry a message the user can act on";

    std::filesystem::remove(tmp);
}

// ── Secrets do not come back ────────────────────────────────────────────────

TEST(DbProbeCli, AdminCredentialsAreNeverEchoed) {
    // They are request-scoped by contract. A response that reflected them would
    // put a database superuser password into whatever the caller logs.
    nlohmann::json req;
    req["type"]           = "postgresql";
    req["host"]           = "127.0.0.1";
    req["port"]           = 1;            // nothing listens: this fails fast
    req["name"]           = "cpap";
    req["user"]           = "cpap_user";
    req["password"]       = "SECRET_OWNER_PW";
    req["admin_user"]     = "postgres";
    req["admin_password"] = "SECRET_ADMIN_PW";

    const auto r = runDbProbeCli(req.dump(), true);
    if (!SetupService::supportsBackend("postgresql")) GTEST_SKIP()
        << "this build has no PostgreSQL support";

    EXPECT_EQ(r.json.find("SECRET_ADMIN_PW"), std::string::npos)
        << "the admin password came back in the response";
    EXPECT_EQ(r.json.find("SECRET_OWNER_PW"), std::string::npos)
        << "the owner password came back in the response";
}
