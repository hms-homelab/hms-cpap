// SDD-071: the capture file, one JSON object per line, in order.
#include <gtest/gtest.h>

#include "clients/As11Capture.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using hms_cpap::as11::As11Capture;
namespace fs = std::filesystem;

namespace {

constexpr std::time_t kAt = 1795042862;   // 2026-11-18 23:01:02 UTC

fs::path tempDir() {
    auto d = fs::temp_directory_path() /
             ("as11-capture-" +
              std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(d);
    return d;
}

std::vector<nlohmann::json> lines(const std::string& path) {
    std::vector<nlohmann::json> out;
    std::ifstream f(path);
    for (std::string l; std::getline(f, l);) out.push_back(nlohmann::json::parse(l));
    return out;
}

}  // namespace

TEST(As11Capture, ThePathIsUnderTheMachinesFolderNamedByUtcTime) {
    EXPECT_EQ(As11Capture::pathFor("/data", "23000000001", kAt),
              (fs::path("/data") / "as11" / "23000000001" / "capture" / "20261118-230102.jsonl").string());
    EXPECT_NE(As11Capture::pathFor("/data", "", 0).find("unknown"), std::string::npos);
}

TEST(As11Capture, EveryMessageIsOneLineInOrderAsSent) {
    const auto dir = tempDir();
    As11Capture c;
    const auto path = As11Capture::pathFor(dir.string(), "23000000001", kAt);
    ASSERT_TRUE(c.open(path));
    c.record("out", R"({"id":5,"method":"StartStream","params":{"dataIds":["_RFL","_TGT"]}})");
    c.record("in", R"({"id":5,"result":{"_TGT":"OK"}})");
    c.record("in", "not json at all");
    c.note("asked again with the 13");
    EXPECT_TRUE(c.isOpen());
    EXPECT_GT(c.bytes(), 0);
    c.close();

    const auto got = lines(path);
    ASSERT_EQ(got.size(), 4u);
    EXPECT_EQ(got[0]["dir"], "out");
    EXPECT_EQ(got[0]["msg"]["params"]["dataIds"][1], "_TGT") << "kept as JSON, not as a string";
    EXPECT_EQ(got[1]["msg"]["result"]["_TGT"], "OK");
    EXPECT_EQ(got[2]["msg"], "not json at all") << "text that does not parse is kept as text";
    EXPECT_EQ(got[3]["dir"], "note");
    EXPECT_EQ(got[3]["note"], "asked again with the 13");
    for (const auto& l : got) EXPECT_EQ(l["t"].get<std::string>().back(), 'Z');
    EXPECT_EQ(c.path(), path) << "the kept file stays named after it closes";
    EXPECT_EQ(c.bytes(), static_cast<long long>(fs::file_size(path)));
    fs::remove_all(dir);
}

TEST(As11Capture, AConnectThatNeverStreamedLeavesNoFile) {
    const auto dir = tempDir();
    As11Capture c;
    const auto kept = As11Capture::pathFor(dir.string(), "23000000001", kAt);
    ASSERT_TRUE(c.open(kept));
    c.record("in", "{}");
    c.close(true);
    const auto empty = As11Capture::pathFor(dir.string(), "23000000001", kAt + 3600);
    ASSERT_TRUE(c.open(empty));
    c.close(false);
    EXPECT_FALSE(fs::exists(empty));
    EXPECT_TRUE(fs::exists(kept));
    EXPECT_EQ(c.path(), kept) << "the status still names the last file kept";
    fs::remove_all(dir);
}
