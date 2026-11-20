// The backfill worker is stopped on the shutdown path main() really takes.
//
// With the web UI, Drogon installs its own SIGTERM handler, so a stop ends
// drogon::app().run() without requestShutdown(). The backfill worker was then
// joined only by its static destructor during exit(), and a re-parse caught
// mid-save kept writing after the database client's statics were destroyed:
// on PostgreSQL the COMMIT went out as an empty statement, the save printed
// success and rolled back, and the night the re-parse had already deleted was
// gone from the database. main() is not linked into the test binary, so this
// reads it and checks the order of the shutdown path.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::filesystem::path sourceRoot() {
#ifdef HMS_CPAP_SOURCE_DIR
    return HMS_CPAP_SOURCE_DIR;
#else
    for (const char* p : {".", "..", "../.."}) {
        if (std::filesystem::exists(std::filesystem::path(p) / "src" / "main.cpp")) return p;
    }
    return ".";
#endif
}

std::string mainSource() {
    std::ifstream in(sourceRoot() / "src" / "main.cpp");
    EXPECT_TRUE(in.good()) << "cannot read src/main.cpp";
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

TEST(ShutdownOrder, TheBackfillStopsRightAfterTheWebServerReturns) {
    const std::string src = mainSource();
    const auto run = src.find("drogon::app().run();");
    ASSERT_NE(run, std::string::npos);
    const auto stop = src.find("backfill_service->stop();", run);
    ASSERT_NE(stop, std::string::npos) << "nothing stops the backfill after run() returns";
    // Before the services it saves through are stopped.
    const auto stream_stop = src.find("cpapdash_stream->stop();", run);
    ASSERT_NE(stream_stop, std::string::npos);
    EXPECT_LT(stop, stream_stop);
}

TEST(ShutdownOrder, TheBackfillIsGoneBeforeTheCollectorAndBeforeMainReturns) {
    const std::string src = mainSource();
    const auto done = src.find("HMS-CPAP service stopped cleanly");
    ASSERT_NE(done, std::string::npos);
    const auto cleanup = src.rfind("// Cleanup", done);
    ASSERT_NE(cleanup, std::string::npos);
    const auto reset = src.find("backfill_service.reset();", cleanup);
    ASSERT_NE(reset, std::string::npos) << "the backfill is left to a static destructor";
    EXPECT_LT(reset, done);
    // Its saved-sessions callback calls into burst_service.
    const auto burst_reset = src.find("burst_service.reset();", cleanup);
    ASSERT_NE(burst_reset, std::string::npos);
    EXPECT_LT(reset, burst_reset);
}
