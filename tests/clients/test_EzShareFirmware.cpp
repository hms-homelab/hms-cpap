#include <gtest/gtest.h>
#include "clients/EzShareClient.h"
#include "utils/ArchiveRecordCount.h"
#include "TestSocketCompat.h"
#include "PosixCompat.h"
#include <mutex>
#include <thread>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <atomic>

namespace hms_cpap {
namespace {
// All response bytes here are synthetic; no card captures or EDF payloads.
class FirmwareHttp {
public:
    std::map<std::string, std::string> files;
    std::string listing = "<html><form action='http://ezshare.card/publicdir/welcome.htm'></form></html>";
    std::mutex mutex;
    std::atomic<int> dir_requests{0};
    std::atomic<int> str_transfers{0};
    std::vector<std::string> listing_replies;
    void setFile(const std::string& path, const std::string& body) {
        std::lock_guard<std::mutex> lock(mutex);
        files[path] = body;
    }
    void setListing(const std::string& body) {
        std::lock_guard<std::mutex> lock(mutex);
        listing = body;
    }
    std::atomic<int> cut{0};
    std::atomic<int> short_reply{0};
    std::atomic<int> stalled{0};
    std::atomic<int> requests{0};
    std::atomic<size_t> largest{0};
    int fd = -1;
    std::thread worker;
    std::string url;

    void start() {
        // Synthetic EDFs still need the fixed header used by the real probe.
        for (auto& file : files) {
            const auto& name = file.first;
            if ((name.find(".edf") != std::string::npos || name.find(".EDF") != std::string::npos) &&
                file.second.size() < 256) file.second.resize(256, ' ');
        }
        fd = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(fd, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ASSERT_EQ(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        ASSERT_EQ(listen(fd, 8), 0);
        socklen_t len = sizeof(addr);
        ASSERT_EQ(getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len), 0);
        url = "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port));
        worker = std::thread([this] {
            for (;;) {
                int conn = accept(fd, nullptr, nullptr);
                if (conn < 0) return;
#ifdef SO_NOSIGPIPE
                int no_sigpipe = 1;
                setsockopt(conn, SOL_SOCKET, SO_NOSIGPIPE,
                           reinterpret_cast<const char*>(&no_sigpipe), sizeof(no_sigpipe));
#endif
                std::string request;
                char buf[2048];
                while (request.find("\r\n\r\n") == std::string::npos) {
                    auto n = recv(conn, buf, sizeof(buf), 0);
                    if (n <= 0) break;
                    request.append(buf, n);
                }
                ++requests;
                const auto end = request.find(' ', 4);
                auto path = request.substr(4, end - 4);
                std::unique_lock<std::mutex> lock(mutex);
                std::string body = listing, extra, status = "200 OK", type = "text/html";
                if (path.find("/dir?") == 0) {
                    ++dir_requests;
                    if (!listing_replies.empty()) {
                        body = listing_replies.front();
                        listing_replies.erase(listing_replies.begin());
                    }
                }
                auto found = files.find(path);
                bool truncate = false, stall = false, short_body = false;
                if (found != files.end()) {
                    size_t first = 0, last = found->second.size() - 1;
                    auto range = request.find("Range: bytes=");
                    if (range != std::string::npos) {
                        auto value = request.substr(range + 13);
                        first = std::stoull(value);
                        auto dash = value.find('-');
                        if (dash + 1 < value.size() && std::isdigit(value[dash + 1]))
                            last = std::min(last, static_cast<size_t>(std::stoull(value.substr(dash + 1))));
                        // Like firmware 4.4.0: a range spanning the whole
                        // file is a plain 200 with the full body.
                        if (first != 0 || last + 1 != found->second.size()) {
                            status = "206 Partial Content";
                            extra = "Content-Range: bytes " + std::to_string(first) + "-" +
                                std::to_string(last) + "/" + std::to_string(found->second.size()) + "\r\n";
                        }
                    }
                    type = "text/plain";
                    body = found->second.substr(first, last - first + 1);
                    if (path == "/STR.edf" && first == 0 && body.size() > 8) ++str_transfers;
                    if (body.size() > 16) {
                        largest = std::max(largest.load(), body.size());
                        truncate = cut.exchange(0) != 0;
                        short_body = short_reply.exchange(0) != 0;
                        stall = stalled.exchange(0) != 0;
                    }
                }
                lock.unlock();
                std::string response = "HTTP/1.1 " + status + "\r\nContent-Type: " + type +
                    "\r\nContent-Length: " + std::to_string(short_body ? 17 : body.size()) + "\r\n" + extra +
                    "Connection: close\r\n\r\n" + (truncate || stall || short_body ? body.substr(0, 17) : body);
                size_t sent = 0;
                while (sent < response.size()) {
#ifdef MSG_NOSIGNAL
                    constexpr int send_flags = MSG_NOSIGNAL;
#else
                    constexpr int send_flags = 0;
#endif
                    auto n = send(conn, response.data() + sent, response.size() - sent, send_flags);
                    if (n <= 0) break;
                    sent += n;
                }
                if (stall) std::this_thread::sleep_for(std::chrono::seconds(9));
                closeSocket(conn);
            }
        });
    }
    ~FirmwareHttp() {
        stopListening(fd);
        if (worker.joinable()) worker.join();
        if (fd >= 0) closeSocket(fd);
    }
};
}

class EzShareFirmwareTest : public testing::Test {
protected:
    EzShareClient client;
    void fallback() {
        client.no_listing_ = true;
        client.listing_checked_at_ = std::chrono::steady_clock::now();
    }
    void retryNow() { client.listing_checked_at_ = {}; }
    void expireSTR() { client.str_read_at_ = {}; client.sessions_read_at_ = {}; }
    uint64_t probe(const std::string& path) { return client.probe(path); }
    void seed(std::time_t start, size_t sessions = 1) {
        client.starts_[client.folderForStart(start)] = std::vector<std::time_t>(sessions, start);
    }
    std::string folder(std::time_t start) { return client.folderForStart(start); }
    bool fallbackEnabled() { return client.no_listing_; }
    std::time_t time(int day, int hour, int minute) {
        std::tm tm{}; tm.tm_year = 126; tm.tm_mon = 2; tm.tm_mday = day;
        tm.tm_hour = hour; tm.tm_min = minute; tm.tm_isdst = -1;
        return std::mktime(&tm);
    }
};

TEST_F(EzShareFirmwareTest, RedirectListingAndHtml200MissUsePlainPathProbes) {
    FirmwareHttp http;
    http.files["/Identification.json"] = "{}";
    http.start(); client.setBaseURL(http.url);
    auto root = client.listDir("");
    EXPECT_TRUE(fallbackEnabled());
    ASSERT_EQ(root.size(), 2u); // Identification and SETTINGS
    EXPECT_EQ(root[0].name, "Identification.json");
    EXPECT_EQ(probe("missing.edf"), 0u);
    EXPECT_EQ(probe("Identification.json"), 2u);
    http.setFile("/Identification.crc", "x");
    EXPECT_EQ(probe("Identification.crc"), 1u);
    const auto path = (std::filesystem::temp_directory_path() / "hms-fw440-small.json").string();
    ASSERT_TRUE(client.downloadRootFile("Identification.json", path)); // one 200 chunk
    std::ifstream in(path, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(in), {}), "{}");
    std::filesystem::remove(path);
}

TEST_F(EzShareFirmwareTest, STRMaskTimesDriveListingFreeDiscovery) {
    // One invented therapy day, with identity signal scaling.
    const std::vector<std::string> labels = {
        "Date", "MaskOn", "MaskOff", "MaskEvents", "Duration", "Mode"};
    std::string edf(256 + 256 * labels.size(), ' ');
    auto put = [&](size_t at, size_t width, const std::string& value) {
        edf.replace(at, std::min(width, value.size()), value.substr(0, width));
    };
    put(0, 8, "0"); put(8, 80, "X X X X");
    put(168, 8, "01.03.26"); put(176, 8, "12.00.00");
    put(184, 8, std::to_string(edf.size()));
    put(236, 8, "1"); put(244, 8, "86400"); put(252, 4, "6");
    for (size_t i = 0; i < labels.size(); ++i) {
        put(256 + i * 16, 16, labels[i]);
        put(256 + 104 * 6 + i * 8, 8, "-32768");
        put(256 + 112 * 6 + i * 8, 8, "32767");
        put(256 + 120 * 6 + i * 8, 8, "-32768");
        put(256 + 128 * 6 + i * 8, 8, "32767");
        put(256 + 216 * 6 + i * 8, 8, "1");
    }
    for (int sample : {20513, 616, 650, 2, 34, 1}) {
        edf += static_cast<char>(sample & 255);
        edf += static_cast<char>((sample >> 8) & 255);
    }
    FirmwareHttp http;
    http.files["/STR.edf"] = edf;
    http.files["/DATALOG/20260301/20260301_221626_BRP.edf"] = "synthetic";
    http.start(); client.setBaseURL(http.url);
    EXPECT_EQ(client.listDateFolders(), std::vector<std::string>{"20260301"});
    const int after_first = http.requests;
    EXPECT_EQ(client.listDateFolders(), std::vector<std::string>{"20260301"});
    EXPECT_EQ(http.requests, after_first); // STR is not re-read every burst
    const auto path = (std::filesystem::temp_directory_path() / "hms-str-cache.edf").string();
    ASSERT_TRUE(client.downloadRootFile("STR.edf", path));
    EXPECT_EQ(http.requests, after_first);
    EXPECT_EQ(http.str_transfers, 1);
    std::filesystem::remove(path);
    const auto files = client.listFiles("20260301");
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].name, "20260301_221626_BRP.edf");
}

TEST_F(EzShareFirmwareTest, SecondsOffsetsSA2AndCachedNames) {
    FirmwareHttp http;
    for (const auto* type : {"BRP", "PLD", "SA2"})
        http.files[std::string("/DATALOG/20260301/20260301_221626_") + type + ".edf"] = "synthetic";
    for (const auto* type : {"EVE", "CSL"})
        http.files[std::string("/DATALOG/20260301/20260301_221616_") + type + ".edf"] = "synthetic";
    http.start(); client.setBaseURL(http.url); fallback(); seed(time(1, 22, 16));
    auto files = client.listFiles("20260301");
    ASSERT_EQ(files.size(), 5u);
    int before = http.requests;
    files = client.listFiles("20260301");
    EXPECT_EQ(files.size(), 5u);
    // No repeated second search; only the three aliases past the last one.
    EXPECT_EQ(http.requests - before, 5 + 3 + 1); // optional TCV at the checkpoint prefix
}

TEST_F(EzShareFirmwareTest, NoonSplitAndAirSense10SAD) {
    EXPECT_EQ(folder(time(2, 0, 5)), "20260301");
    EXPECT_EQ(folder(time(2, 11, 59)), "20260301");
    EXPECT_EQ(folder(time(2, 12, 0)), "20260302");
    FirmwareHttp http;
    for (const auto* type : {"BRP", "PLD", "SAD", "EVE", "CSL"})
        http.files[std::string("/DATALOG/20260301/20260302_000510_") + type + ".edf"] = "synthetic";
    http.start(); client.setBaseURL(http.url); fallback(); seed(time(2, 0, 5));
    auto files = client.listFiles("20260301");
    ASSERT_EQ(files.size(), 5u);
    EXPECT_TRUE(std::any_of(files.begin(), files.end(), [](const auto& e) {
        return e.name == "20260302_000510_SAD.edf";
    }));
}

TEST_F(EzShareFirmwareTest, SessionsSharingAnSTRMinuteFindTheirOwnFiles) {
    // STR keeps minutes, so two short sessions can share a start minute.
    FirmwareHttp http;
    for (const auto* stamp : {"221603", "221657"})
        for (const auto* type : {"BRP", "PLD", "SA2", "EVE", "CSL"})
            http.files[std::string("/DATALOG/20260301/20260301_") + stamp + "_" + type + ".edf"] = "synthetic";
    http.start(); client.setBaseURL(http.url); fallback();
    const auto start = time(1, 22, 16);
    seed(start, 2);
    auto files = client.listFiles("20260301");
    ASSERT_EQ(files.size(), 10u);
    EXPECT_TRUE(std::any_of(files.begin(), files.end(), [](const auto& e) {
        return e.name == "20260301_221657_BRP.edf";
    }));
    files = client.listFiles("20260301");
    EXPECT_EQ(files.size(), 10u); // cached names stay with their own session
}

TEST_F(EzShareFirmwareTest, AliasesFindSessionsSTRDoesNotList) {
    // Synthetic 8.3 aliases in creation order, each a header whose bytes
    // 168-183 carry the start. No STR start is known for any of them.
    FirmwareHttp http;
    int number = 0;
    const auto put = [&](const std::string& stamp, const std::string& name) {
        std::string header(256, ' ');
        header.replace(168, 16, stamp);
        http.files["/DATALOG/20260301/" + name] = header;
        const auto digits = std::to_string(++number);
        http.files["/DATALOG/20260301/" + std::string("202603").substr(0, 7 - digits.size()) +
                   "~" + digits + ".EDF"] = header;
    };
    for (const auto* type : {"CSL", "EVE"}) put("01.03.2622.16.16", std::string("20260301_221616_") + type + ".edf");
    for (const auto* type : {"PLD", "SA2", "BRP"}) put("01.03.2622.16.26", std::string("20260301_221626_") + type + ".edf");
    // A mask-on that wrote no BRP, and a session after midnight.
    for (const auto* type : {"CSL", "EVE"}) put("01.03.2623.00.00", std::string("20260301_230000_") + type + ".edf");
    put("02.03.2601.02.03", "20260302_010203_BRP.edf");
    http.start(); client.setBaseURL(http.url); fallback();
    auto files = client.listFiles("20260301");
    ASSERT_EQ(files.size(), 8u);
    for (const auto* name : {"20260301_230000_EVE.edf", "20260302_010203_BRP.edf", "20260301_221626_BRP.edf"})
        EXPECT_TRUE(std::any_of(files.begin(), files.end(), [&](const auto& e) { return e.name == name; })) << name;
    const int before = http.requests;
    files = client.listFiles("20260301");
    EXPECT_EQ(files.size(), 8u);
    EXPECT_GT(http.requests - before, 8 + 3); // absent companions are retried // sizes, then aliases past ~8
}

TEST_F(EzShareFirmwareTest, BoundedChunksResumeAfterCutAndTimeout) {
    FirmwareHttp http;
    std::string bytes;
    for (int i = 0; i < 20000; ++i) bytes += static_cast<char>(i % 251);
    http.files["/STR.edf"] = bytes;
    http.cut = 1;
    http.start(); client.setBaseURL(http.url); fallback();
    const auto path = (std::filesystem::temp_directory_path() / "hms-fw440-chunks.edf").string();
    ASSERT_TRUE(client.downloadRootFile("STR.edf", path));
    std::ifstream in(path, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(in), {}), bytes);
    in.close();
    http.short_reply = 1;
    expireSTR();
    ASSERT_TRUE(client.downloadRootFile("STR.edf", path));
    std::ifstream short_read(path, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(short_read), {}), bytes);
    short_read.close();
    http.stalled = 1;
    size_t got = 0;
    ASSERT_TRUE(client.downloadFileRange("", "STR.edf", path, 0, got));
    EXPECT_EQ(got, bytes.size());
    std::ifstream resumed(path, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(resumed), {}), bytes);
    EXPECT_LE(http.largest, 8192u);
    std::filesystem::remove(path);
}

TEST_F(EzShareFirmwareTest, ListingFirmwareKeepsItsExistingBehaviour) {
    FirmwareHttp http;
    http.listing = "<pre>2026- 3- 1 22:16:26 <DIR> <a href=\"x\">20260301</a></pre>";
    http.files["/download?file=STR.edf"] = "synthetic root";
    http.start(); client.setBaseURL(http.url);
    EXPECT_EQ(client.listDateFolders(), std::vector<std::string>{"20260301"});
    EXPECT_FALSE(fallbackEnabled());
    const auto path = (std::filesystem::temp_directory_path() / "hms-fw440-listing.edf").string();
    EXPECT_TRUE(client.downloadRootFile("STR.edf", path));
    std::filesystem::remove(path);
}

TEST_F(EzShareFirmwareTest, EmptyAndCutShort200ListingsNeverEnableFallback) {
    FirmwareHttp http;
    http.listing = "";
    http.start(); client.setBaseURL(http.url);
    EXPECT_TRUE(client.listDateFolders().empty());
    EXPECT_FALSE(fallbackEnabled());
    http.setListing("<pre>2026- 3-");
    EXPECT_TRUE(client.listFiles("20260301").empty());
    EXPECT_TRUE(client.listDir("").empty());
    EXPECT_FALSE(fallbackEnabled());
    EXPECT_EQ(http.dir_requests, 3);
}

TEST_F(EzShareFirmwareTest, WelcomeMustRepeatAndRecoveredListingLeavesFallback) {
    FirmwareHttp http;
    const std::string valid = "<pre>2026- 3- 1 22:16:26 <DIR> <a href=\"x\">20260301</a></pre>";
    http.listing_replies = {http.listing, valid};
    http.start(); client.setBaseURL(http.url);
    EXPECT_EQ(client.listDateFolders(), std::vector<std::string>{"20260301"});
    EXPECT_FALSE(fallbackEnabled());
    client.listDir("");
    EXPECT_TRUE(fallbackEnabled());
    http.setListing(valid);
    retryNow();
    EXPECT_EQ(client.listDateFolders(), std::vector<std::string>{"20260301"});
    EXPECT_FALSE(fallbackEnabled());
}

TEST_F(EzShareFirmwareTest, NonResmedFormatsNeverUseResmedDiscovery) {
    FirmwareHttp http;
    http.start(); client.setBaseURL(http.url);
    const char* old = std::getenv("CPAP_FORMAT");
    const std::string saved = old ? old : "";
    for (const auto* format : {"sefam", "bmc", "lowenstein", "philips"}) {
        setenv("CPAP_FORMAT", format, 1);
        EXPECT_TRUE(client.listDir("").empty());
        EXPECT_TRUE(client.listDateFolders().empty());
        EXPECT_FALSE(fallbackEnabled());
    }
    if (old) setenv("CPAP_FORMAT", saved.c_str(), 1); else unsetenv("CPAP_FORMAT");
    EXPECT_EQ(http.requests, 8);
}

TEST_F(EzShareFirmwareTest, SameSizeEdfFinalisationAndCrcChangesRequireFullRefresh) {
    FirmwareHttp http;
    std::string edf(512, ' ');
    edf.replace(236, 8, "-1      ");
    const std::string remote = "DATALOG/20260301/20260301_221626_BRP.edf";
    http.files["/" + remote] = edf;
    http.files["/Identification.crc"] = "aaaa";
    http.files["/SETTINGS/CurrentSettings.crc"] = "cccc";
    http.start(); client.setBaseURL(http.url); fallback();
    const auto dir = std::filesystem::temp_directory_path() / "hms-fw-freshness";
    std::filesystem::create_directories(dir / "SETTINGS");
    const auto local = (dir / "sample.edf").string();
    std::ofstream(local, std::ios::binary) << edf;
    EXPECT_EQ(probe(remote), 512u);
    EXPECT_FALSE(client.needsFullRefresh(remote, local));
    edf.replace(236, 8, "13      ");
    http.setFile("/" + remote, edf);
    EXPECT_EQ(probe(remote), 512u);
    EXPECT_TRUE(client.needsFullRefresh(remote, local));
    std::ofstream(local, std::ios::binary) << edf;
    EXPECT_FALSE(client.needsFullRefresh(remote, local));
    for (const auto& name : {std::string("Identification"), std::string("SETTINGS/CurrentSettings")}) {
        const auto json = (dir / (name + ".json")).string();
        const auto crc = (dir / (name + ".crc")).string();
        std::ofstream(crc) << (name == "Identification" ? "aaaa" : "cccc");
        EXPECT_FALSE(client.needsFullRefresh(name + ".json", json));
        http.setFile("/" + name + ".crc", "bbbb");
        EXPECT_TRUE(client.needsFullRefresh(name + ".json", json));
    }
    std::filesystem::remove_all(dir);
}

TEST_F(EzShareFirmwareTest, AliasOffsetsAndUnresolvedAliasesKeepStrSearchAndTcv) {
    FirmwareHttp http;
    std::string alias(256, ' ');
    alias.replace(168, 16, "01.03.2622.16.10");
    http.files["/DATALOG/20260301/202603~1.EDF"] = alias;
    // Beyond the three-second alias tolerance: STR must still search the minute.
    http.files["/DATALOG/20260301/20260301_221630_BRP.edf"] = std::string(256, ' ');
    http.files["/DATALOG/20260301/20260301_221630_TCV.edf"] = std::string(256, ' ');
    // A second alias differs from its filename by two seconds.
    alias.replace(168, 16, "01.03.2623.00.10");
    http.files["/DATALOG/20260301/202603~2.EDF"] = alias;
    http.files["/DATALOG/20260301/20260301_230012_PLD.edf"] = std::string(256, ' ');
    http.start(); client.setBaseURL(http.url); fallback(); seed(time(1, 22, 16));
    testing::internal::CaptureStderr();
    const auto first = client.listFiles("20260301");
    const auto again = client.listFiles("20260301");
    const auto log = testing::internal::GetCapturedStderr();
    EXPECT_EQ(first.size(), 3u);
    EXPECT_EQ(again.size(), 3u);
    const auto warning = log.find("unresolved alias start");
    ASSERT_NE(warning, std::string::npos);
    EXPECT_EQ(log.find("unresolved alias start", warning + 1), std::string::npos);
}
TEST_F(EzShareFirmwareTest, AliasRetriesMissingCompanion) {
    FirmwareHttp http;
    std::string header(256, ' ');
    header.replace(168, 16, "01.03.2622.16.26");
    http.files["/DATALOG/20260301/202603~1.EDF"] = header;
    http.files["/DATALOG/20260301/20260301_221626_BRP.edf"] = header;
    http.start(); client.setBaseURL(http.url); fallback(); seed(time(1,22,16));
    ASSERT_EQ(client.listFiles("20260301").size(), 1u);
    http.setFile("/DATALOG/20260301/20260301_221626_PLD.edf", header);
    http.setFile("/DATALOG/20260301/202603~2.EDF", header);
    EXPECT_EQ(client.listFiles("20260301").size(), 2u);
}
TEST_F(EzShareFirmwareTest, RepairedArchiveIsNotARemoteRewrite) {
    FirmwareHttp http;
    std::string header(514, ' ');
    header.replace(184, 8, "512     "); header.replace(236, 8, "-1      ");
    header.replace(252, 4, "1   "); header.replace(472, 8, "1       ");
    const std::string remote = "DATALOG/20260301/20260301_221626_BRP.edf";
    http.files["/" + remote] = header;
    http.start(); client.setBaseURL(http.url); fallback();
    const auto path = (std::filesystem::temp_directory_path()/"hms-review-repaired_BRP.edf").string();
    std::ofstream(path, std::ios::binary) << header;
    ASSERT_TRUE(repairSignalEdfFile(path));
    ASSERT_EQ(probe(remote), 514u);
    EXPECT_FALSE(client.needsFullRefresh(remote, path));
    std::filesystem::remove(path);
}
TEST_F(EzShareFirmwareTest, ResolvedAliasCoversOnlyOneSessionInSharedStrMinute) {
    FirmwareHttp http;
    std::string alias(256, ' ');
    alias.replace(168, 16, "01.03.2622.16.10");
    http.files["/DATALOG/20260301/202603~1.EDF"] = alias;
    http.files["/DATALOG/20260301/20260301_221610_BRP.edf"] = alias;
    alias.replace(168, 16, "01.03.2622.16.20");
    http.files["/DATALOG/20260301/202603~2.EDF"] = alias;
    http.files["/DATALOG/20260301/20260301_221650_BRP.edf"] = alias;
    http.start(); client.setBaseURL(http.url); fallback(); seed(time(1, 22, 16), 2);
    const auto files = client.listFiles("20260301");
    ASSERT_EQ(files.size(), 2u);
    EXPECT_TRUE(std::any_of(files.begin(), files.end(), [](const auto& file) {
        return file.name == "20260301_221650_BRP.edf";
    }));
}
TEST_F(EzShareFirmwareTest, UnchangedEventCountsMatchWithoutSignalRepair) {
    const auto dir = std::filesystem::temp_directory_path() / "hms-fw-event-counts";
    std::filesystem::create_directories(dir);
    for (const auto* type : {"CSL", "EVE"}) {
        const auto path = (dir / (std::string("20260301_221626_") + type + ".edf")).string();
        std::string edf(256, ' ');
        edf.replace(236, 8, "13      ");
        std::ofstream(path, std::ios::binary) << edf;
        EXPECT_TRUE(signalEdfRecordCountMatches(path, "13      ")) << type;
        EXPECT_FALSE(signalEdfRecordCountMatches(path, "14      ")) << type;
    }
    std::filesystem::remove_all(dir);
}
} // namespace hms_cpap
