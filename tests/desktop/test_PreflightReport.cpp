// SDD-016: the supervisor's reading of --preflight.
//
// The fixtures below are REAL output, captured from the built binary rather
// than written from memory. That matters more than usual here: the format is
// enforced by nothing but PreflightService::Report::render(), no test pins the
// whole string, and a parser written against an imagined format would pass its
// own tests and fail on the first machine.

#include <gtest/gtest.h>

#include "PreflightReport.h"

using namespace cpapdash::supervisor;

namespace {

// Captured from: HMS_CPAP_DATA_DIR=/tmp/... hms_cpap --preflight
// (ezShare source, no archive_dir -- the ordinary healthy-with-a-warning case)
const char* kHealthyWithWarning = R"(Preflight checks:
  ok    data_dir: /tmp/hms_pf_test is writable
  ok    web_port: port 8893 is free
  ok    database: sqlite reachable, schema will be created on first start
  ok    source: source is 'ezshare' (network); reachability is checked while running, not at startup; see archive_dir below
  warn  archive_dir: source 'ezshare' downloads files but no archive directory is set, so nothing is written to disk: OSCAR has nothing to import and SleepHQ export stays blocked
        -> Set the Archive Directory in Settings under Data Source, or "archive_dir" in config.json, to a folder for the card layout (for example ~/CPAPData). Nights already collected stay in the database and are unaffected.
OK: configuration looks usable.
)";

const char* kPortTaken = R"(Preflight checks:
  ok    data_dir: /home/me/.hms-cpap is writable
  FAIL  web_port: port 8893 is already in use
        -> Stop whatever is listening on 8893, or set "web_port" in config.json to a free port.
  ok    database: sqlite reachable, schema will be created on first start
FAILED: fix the items marked FAIL above.
)";

}  // namespace

// ── The ordinary case ───────────────────────────────────────────────────────

TEST(PreflightReportTest, AHealthyRunParsesEveryCheck) {
    const auto r = parsePreflight(kHealthyWithWarning, "", 0);
    EXPECT_EQ(r.verdict, Verdict::Ok);
    ASSERT_EQ(r.checks.size(), 5u);
    EXPECT_EQ(r.checks[0].name, "data_dir");
    EXPECT_EQ(r.checks[1].name, "web_port");
    EXPECT_EQ(r.checks[2].name, "database");
    EXPECT_EQ(r.checks[3].name, "source");
    EXPECT_EQ(r.checks[4].name, "archive_dir");
}

TEST(PreflightReportTest, WarningsDoNotBlock) {
    // preflight exits 0 with a warning present, and the supervisor must agree:
    // treating warn as a failure would refuse to start a service the service
    // itself is happy to start.
    const auto r = parsePreflight(kHealthyWithWarning, "", 0);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(r.failures().empty());
    ASSERT_EQ(r.warnings().size(), 1u);
    EXPECT_EQ(r.warnings()[0].name, "archive_dir");
}

// ── The two parsing traps ───────────────────────────────────────────────────

TEST(PreflightReportTest, ADetailContainingAColonSurvivesIntact) {
    // archive_dir's detail has a colon in the middle of the sentence. Splitting
    // on the last one would cut the explanation in half and leave the user with
    // "OSCAR has nothing to import and SleepHQ export stays blocked" as the
    // NAME of a check.
    const auto r = parsePreflight(kHealthyWithWarning, "", 0);
    const auto* c = r.find("archive_dir");
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->name, "archive_dir");
    EXPECT_NE(c->detail.find("nothing is written to disk: OSCAR has nothing to import"),
              std::string::npos)
        << "the detail was truncated at its own colon: " << c->detail;
}

TEST(PreflightReportTest, ARemedyAttachesToTheCheckAboveIt) {
    const auto r = parsePreflight(kPortTaken, "", 1);
    const auto* port = r.find("web_port");
    ASSERT_NE(port, nullptr);
    EXPECT_EQ(port->status, CheckStatus::Fail);
    EXPECT_NE(port->remedy.find("Stop whatever is listening on 8893"), std::string::npos)
        << "remedy was: " << port->remedy;

    // ...and NOT to the one after it.
    const auto* db = r.find("database");
    ASSERT_NE(db, nullptr);
    EXPECT_TRUE(db->remedy.empty())
        << "the remedy leaked onto the following check: " << db->remedy;
}

// ── Failure ─────────────────────────────────────────────────────────────────

TEST(PreflightReportTest, AFailingCheckIsNamedSoTheUiCanPointAtTheField) {
    // This is the whole reason for parsing rather than showing the blob: the
    // configurator puts "port 8893 is already in use" under the port field.
    const auto r = parsePreflight(kPortTaken, "", 1);
    EXPECT_EQ(r.verdict, Verdict::Failed);
    EXPECT_FALSE(r.ok());
    ASSERT_EQ(r.failures().size(), 1u);
    EXPECT_EQ(r.failures()[0].name, "web_port");
    EXPECT_NE(r.failures()[0].detail.find("8893"), std::string::npos);
}

TEST(PreflightReportTest, TheExitCodeDecidesNotOurCount) {
    // If the service ever exits non-zero for a reason not expressed as a FAIL
    // line, the supervisor must still refuse to start it. The process owns the
    // verdict; we only render its reasoning.
    const auto r = parsePreflight(kHealthyWithWarning, "", 1);
    EXPECT_EQ(r.verdict, Verdict::Failed);
    EXPECT_FALSE(r.ok());
}

// ── The case that must never be confused with a bad setting ─────────────────

TEST(PreflightReportTest, AnUnparseableConfigIsItsOwnVerdict) {
    // main.cpp bails BEFORE preflight, to stderr, with no header. If this came
    // back as an ordinary failure the supervisor would offer to rewrite
    // config.json -- overwriting a file the user hand-edited and still wants.
    const char* err =
        "Refusing to start. The configuration file is not valid JSON:\n"
        "  /home/me/.hms-cpap/config.json\n"
        "  [json.exception.parse_error.101] unexpected end of input\n"
        "Your file has NOT been changed.\n";
    const auto r = parsePreflight("", err, 1);
    EXPECT_EQ(r.verdict, Verdict::ConfigUnparseable);
    EXPECT_TRUE(r.checks.empty());
    EXPECT_NE(r.message.find("not valid JSON"), std::string::npos);
}

TEST(PreflightReportTest, SilenceIsReportedRatherThanReadAsSuccess) {
    // A crashed or killed process prints nothing. Reading that as "no checks
    // failed, therefore fine" would start a service against a config nobody
    // ever validated.
    const auto r = parsePreflight("", "", 0);
    EXPECT_EQ(r.verdict, Verdict::Unreadable);
    EXPECT_FALSE(r.ok());
    EXPECT_FALSE(r.message.empty());
}

TEST(PreflightReportTest, TheRawTextIsAlwaysKept) {
    // Whatever the parser makes of it, the user can still be shown exactly what
    // the process said. A format change should degrade to "here is the output",
    // not to an empty dialog.
    const auto r = parsePreflight(kPortTaken, "some warning on stderr", 1);
    EXPECT_NE(r.raw.find("port 8893 is already in use"), std::string::npos);
    EXPECT_NE(r.raw.find("some warning on stderr"), std::string::npos)
        << "stderr was dropped; the interesting failures write to both streams";
}

TEST(PreflightReportTest, WindowsLineEndingsParse) {
    // The Windows build is the first target, and its output arrives with CRLF.
    std::string crlf;
    for (const char* p = kPortTaken; *p; ++p) {
        if (*p == '\n') crlf += '\r';
        crlf += *p;
    }
    const auto r = parsePreflight(crlf, "", 1);
    ASSERT_EQ(r.checks.size(), 3u);
    const auto* port = r.find("web_port");
    ASSERT_NE(port, nullptr);
    EXPECT_EQ(port->detail, "port 8893 is already in use")
        << "a stray carriage return survived into the detail";
}
