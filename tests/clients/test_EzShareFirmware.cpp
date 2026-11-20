#include <gtest/gtest.h>
#include "clients/EzShareClient.h"
#include "utils/ArchiveRecordCount.h"
#include "services/ListingPlan.h"
#include "utils/MachineClock.h"
#include "utils/TimeCompat.h"
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
    std::set<std::string> unavailable;
    std::map<std::string, int> path_requests;
    void setUnavailable(const std::string& path, bool fail) {
        std::lock_guard<std::mutex> lock(mutex);
        if (fail) unavailable.insert(path); else unavailable.erase(path);
    }
    int calls(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex);
        return path_requests[path];
    }
    void setFile(const std::string& path, const std::string& body) {
        std::lock_guard<std::mutex> lock(mutex);
        files[path] = body;
    }
    void setListing(const std::string& body) {
        std::lock_guard<std::mutex> lock(mutex);
        listing = body;
    }
    void removeFile(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex);
        files.erase(path);
    }
    // SDD-069: a file whose transfers stall: any reply longer than a probe's
    // 16 bytes sends its headers and then nothing.
    std::set<std::string> dead;
    void setDead(const std::string& path, bool on) {
        std::lock_guard<std::mutex> lock(mutex);
        if (on) dead.insert(path); else dead.erase(path);
    }
    std::atomic<int> cut{0};
    std::atomic<int> short_reply{0};
    std::atomic<int> stalled{0};
    std::atomic<int> requests{0};
    std::atomic<size_t> largest{0};
    int fd = -1;
    sockaddr_in address{};
    std::atomic<bool> stopping{false};
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
        address = addr;
        url = "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port));
        worker = std::thread([this] {
            for (;;) {
                int conn = accept(fd, nullptr, nullptr);
                if (conn < 0) return;
                if (stopping) { closeSocket(conn); return; }
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
                ++path_requests[path];
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
                if (unavailable.count(path)) {
                    status = "503 Service Unavailable";
                    type = "text/plain";
                    body = "temporary failure";
                    extra.clear();
                }
                const bool silent = found != files.end() && dead.count(path) && body.size() > 16;
                lock.unlock();
                std::string response = "HTTP/1.1 " + status + "\r\nContent-Type: " + type +
                    "\r\nContent-Length: " + std::to_string(short_body ? 17 : body.size()) + "\r\n" + extra +
                    "Connection: close\r\n\r\n" +
                    (silent ? std::string() : truncate || stall || short_body ? body.substr(0, 17) : body);
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
        if (worker.joinable()) {
            stopping = true;
            // A real connection wakes accept() on macOS as well as Winsock.
            // The worker observes stopping before reading from this socket.
            int wake = socket(AF_INET, SOCK_STREAM, 0);
            if (wake >= 0) {
                EXPECT_EQ(connect(wake, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
                closeSocket(wake);
            }
            worker.join();
        }
        stopListening(fd);
        if (fd >= 0) closeSocket(fd);
    }
};
}

class EzShareFirmwareTest : public testing::Test {
protected:
    EzShareClient client;
    void fallback() {
        client.no_listing_ = true;
        client.listing_attempts_ = 0;
    }
    void retryNow() { client.beginRefresh(); }
    void expireSTR() { client.beginRefresh(); }
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
    // SDD-069: what the collector tells the client of each burst.
    void burst(long long now, bool full = false, bool growing = false, bool checkpoint = false) {
        IDataSource::BurstScope s;
        s.now_s = now; s.full_scan = full; s.night_growing = growing; s.str_checkpoint = checkpoint;
        client.setBurstScope(s);
        client.listing_attempts_ = 0;   // SDD-076 D2's /dir check is not under test here
    }
    bool offset(long long& off) { return client.takeMachineOffset(off); }
};

namespace {
// A synthetic EDF: its start (168-183), its record count (236-243) and its
// record duration (244-251), padded to [size] bytes.
std::string syntheticEdf(const std::string& start, int records, size_t size) {
    std::string edf(std::max<size_t>(size, 256), ' ');
    edf.replace(168, 16, start);
    const auto count = std::to_string(records);
    edf.replace(236, count.size(), count);
    edf.replace(244, 2, "60");
    return edf;
}
}  // namespace

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
    in.close();   // Windows will not remove a file that is still open
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
    EXPECT_EQ(http.requests, after_first); // reuse only within this refresh
    const auto path = (std::filesystem::temp_directory_path() / "hms-str-cache.edf").string();
    ASSERT_TRUE(client.downloadRootFile("STR.edf", path));
    EXPECT_EQ(http.requests, after_first);
    EXPECT_EQ(http.str_transfers, 1);
    std::filesystem::remove(path);
    const auto files = client.listFiles("20260301");
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].name, "20260301_221626_BRP.edf");
    // A new refresh sees new STR content immediately, without advancing time.
    edf[256 + 256 * labels.size() + 2] = static_cast<char>(617 & 255);
    edf[256 + 256 * labels.size() + 3] = static_cast<char>(617 >> 8);
    http.setFile("/STR.edf", edf);
    client.beginRefresh();
    EXPECT_EQ(client.listDateFolders(), std::vector<std::string>{"20260301"});
    EXPECT_EQ(http.str_transfers, 2);
    ASSERT_TRUE(client.downloadRootFile("STR.edf", path));
    std::ifstream updated(path, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(updated), {}), edf);
    EXPECT_EQ(http.str_transfers, 2);
    updated.close();
    std::filesystem::remove(path);
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
    EXPECT_EQ(http.requests - before, 8 + 3); // cached sizes, then aliases past ~8
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
    resumed.close();   // Windows will not remove a file that is still open
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
    testing::internal::CaptureStderr();
    EXPECT_TRUE(client.listDateFolders().empty());
    EXPECT_TRUE(client.listFiles("20260301").empty());
    EXPECT_TRUE(client.listDir("").empty());
    const auto log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("empty response listing DATALOG"), std::string::npos);
    EXPECT_NE(log.find("empty response for 20260301"), std::string::npos);
    EXPECT_NE(log.find("empty response listing ''"), std::string::npos);
    EXPECT_FALSE(fallbackEnabled());
    http.setListing("<pre>2026- 3-");
    EXPECT_TRUE(client.listDateFolders().empty());
    EXPECT_TRUE(client.listFiles("20260301").empty());
    EXPECT_TRUE(client.listDir("").empty());
    EXPECT_FALSE(fallbackEnabled());
    EXPECT_EQ(http.dir_requests, 6);
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

TEST_F(EzShareFirmwareTest, ListingRecoveryRetriesAfterBoundedListingAttempts) {
    FirmwareHttp http;
    http.start(); client.setBaseURL(http.url); fallback();
    http.setListing("<pre>2026- 3- 1 22:16:26 <DIR> <a href=\"x\">20260301</a></pre>");
    for (int attempt = 0; attempt < 3; ++attempt) client.listDir("unknown");
    EXPECT_EQ(http.dir_requests, 0);
    EXPECT_EQ(client.listDateFolders(), std::vector<std::string>{"20260301"});
    EXPECT_FALSE(fallbackEnabled());
    EXPECT_EQ(http.dir_requests, 2); // recovery check, then normal listing
}

TEST_F(EzShareFirmwareTest, MissingTypesAreCachedButFailuresAndLateFilesAreRetried) {
    FirmwareHttp http;
    std::string header(256, ' ');
    header.replace(168, 16, "01.03.2622.16.26");
    const std::string prefix = "/DATALOG/20260301/20260301_221626_";
    http.files["/DATALOG/20260301/202603~1.EDF"] = header;
    http.files[prefix + "BRP.edf"] = header;
    http.setUnavailable(prefix + "PLD.edf", true);
    http.start(); client.setBaseURL(http.url); fallback();
    ASSERT_EQ(client.listFiles("20260301").size(), 1u);
    EXPECT_EQ(http.calls(prefix + "TCV.edf"), 1);
    http.setUnavailable(prefix + "PLD.edf", false);
    http.setFile(prefix + "PLD.edf", header);
    ASSERT_EQ(client.listFiles("20260301").size(), 2u);
    EXPECT_EQ(http.calls(prefix + "TCV.edf"), 1) << "confirmed absence avoids another seven offsets";
    EXPECT_EQ(http.calls(prefix + "PLD.edf"), 3) << "retry resolves PLD, then reads its current size";
    http.setFile(prefix + "TCV.edf", header);
    ASSERT_EQ(client.listFiles("20260301").size(), 2u); // cached on third scan
    EXPECT_EQ(client.listFiles("20260301").size(), 3u); // bounded retry finds late file without alias
}

TEST_F(EzShareFirmwareTest, JournalIsHeldAcrossRefreshesButFailedTransferRetries) {
    FirmwareHttp http;
    http.files["/journal.jnl"] = std::string(32, 'j');
    http.files["/STR.edf"] = std::string(256, ' ');
    http.start(); client.setBaseURL(http.url); fallback();
    const auto path = (std::filesystem::temp_directory_path()/"hms-fw440-journal.jnl").string();
    ASSERT_TRUE(client.downloadByPath("journal.jnl", path));
    const auto before = http.calls("/journal.jnl");
    client.beginRefresh();
    EXPECT_FALSE(client.needsFullRefresh("journal.jnl", path));
    client.beginRefresh();
    EXPECT_FALSE(client.needsFullRefresh("journal.jnl", path));
    EXPECT_EQ(http.calls("/journal.jnl"), before);
    http.setUnavailable("/journal.jnl", true);
    EXPECT_FALSE(client.downloadByPath("journal.jnl", path));
    client.beginRefresh();
    EXPECT_TRUE(client.needsFullRefresh("journal.jnl", path));
    http.setUnavailable("/journal.jnl", false);
    ASSERT_TRUE(client.downloadByPath("journal.jnl", path));
    EXPECT_FALSE(client.needsFullRefresh("journal.jnl", path));
    std::filesystem::remove(path);
}

TEST_F(EzShareFirmwareTest, IdleServerTeardownWakesAcceptWithoutAnHttpRequest) {
    const auto before = std::chrono::steady_clock::now();
    { FirmwareHttp http; http.start(); }
    EXPECT_LT(std::chrono::steady_clock::now() - before, std::chrono::seconds(2));
}

// ── SDD-069: a live night on a card with no listing ──────────────────────────
// Synthetic headers only. A session at 22:16:26 has a BRP and a PLD, each
// served by its long name and by its alias in creation order.
namespace {
const std::string kDir = "/DATALOG/20260301/";
const std::string kBrp = kDir + "20260301_221626_BRP.edf";
const std::string kStart = "01.03.2622.16.26";

void putFile(FirmwareHttp& http, const std::string& stamp, const std::string& name, int alias,
             int records, size_t size = 0) {
    const auto body = syntheticEdf(stamp, records, size ? size : 256 + records * 100);
    http.setFile(kDir + name, body);
    http.setFile(kDir + "202603~" + std::to_string(alias) + ".EDF", body);
}

void putSessionA(FirmwareHttp& http, int records) {
    putFile(http, kStart, "20260301_221626_BRP.edf", 1, records);
    putFile(http, kStart, "20260301_221626_PLD.edf", 2, 10);
}
}  // namespace

TEST_F(EzShareFirmwareTest, Sdd069BeforeTheNightABurstSendsOneProbePerStem) {
    FirmwareHttp http;
    http.start(); client.setBaseURL(http.url); fallback();
    const long long t0 = time(10, 18, 0);
    burst(t0);
    int before = http.requests;
    EXPECT_TRUE(client.listFiles("20260310").empty());
    EXPECT_EQ(http.requests - before, 1) << "mid-month both stems share one alias numbering";
    EXPECT_EQ(http.calls("/DATALOG/20260310/202603~1.EDF"), 1);
    // A month's last night has two stems: one probe each, and nothing else.
    burst(t0 + 65);
    before = http.requests;
    EXPECT_TRUE(client.listFiles("20260331").empty());
    EXPECT_EQ(http.requests - before, 2);
    EXPECT_EQ(http.calls("/DATALOG/20260331/202603~1.EDF"), 1);
    EXPECT_EQ(http.calls("/DATALOG/20260331/202604~1.EDF"), 1);
    // The full scan (the index, SDD-054 D6) keeps the three-miss tolerance.
    burst(t0 + 130, true);
    before = http.requests;
    EXPECT_TRUE(client.listFiles("20260310").empty());
    EXPECT_EQ(http.requests - before, 3);
}

TEST_F(EzShareFirmwareTest, Sdd069ANewBrpIsTheOpenSessionAndTheOneBeforeWaitsForItsCheckpoint) {
    FirmwareHttp http;
    putSessionA(http, 10);
    http.start(); client.setBaseURL(http.url); fallback();
    long long now = time(1, 22, 30);
    burst(now, true);
    ASSERT_EQ(client.listFiles("20260301").size(), 2u);
    // The open session grows: its files are probed every burst, and the card
    // is asked for the next alias once.
    for (int records = 11; records <= 13; ++records) {
        putSessionA(http, records);
        now += 65;
        burst(now);
        const int brp = http.calls(kBrp), alias = http.calls(kDir + "202603~3.EDF");
        EXPECT_EQ(client.listFiles("20260301").size(), 2u);
        EXPECT_EQ(http.calls(kBrp), brp + 1);
        EXPECT_EQ(http.calls(kDir + "202603~3.EDF"), alias + 1);
    }
    const long long grew = now;
    // A new BRP mid-night, found by that one alias probe.
    putFile(http, "01.03.2623.40.00", "20260301_234000_BRP.edf", 3, 2);
    now += 65;
    burst(now);
    auto files = client.listFiles("20260301");
    ASSERT_EQ(files.size(), 3u);
    EXPECT_TRUE(std::any_of(files.begin(), files.end(),
                            [](const auto& e) { return e.name == "20260301_234000_BRP.edf"; }));
    // Session A is followed by a new BRP: not probed again before its first
    // checkpoint, 5 min after its last growth. The new session is, every burst.
    const std::string b = kDir + "20260301_234000_BRP.edf";
    const int a_calls = http.calls(kBrp);
    int b_records = 3;
    for (now = grew + 130; now < grew + 300; now += 65) {
        putFile(http, "01.03.2623.40.00", "20260301_234000_BRP.edf", 3, b_records++);
        burst(now);
        const int b_calls = http.calls(b);
        EXPECT_EQ(client.listFiles("20260301").size(), 3u);
        EXPECT_EQ(http.calls(b), b_calls + 1) << "the open session is probed every burst";
    }
    EXPECT_EQ(http.calls(kBrp), a_calls) << "a session followed by a new BRP waits for a checkpoint";
    burst(now);
    EXPECT_EQ(client.listFiles("20260301").size(), 3u);
    EXPECT_EQ(http.calls(kBrp), a_calls + 1) << "SDD-054 D4's 5-min checkpoint";
}

TEST_F(EzShareFirmwareTest, Sdd069AtEofTwoUnchangedBurstsCloseTheSessionAndAMovedCountKeepsItOpen) {
    FirmwareHttp http;
    putSessionA(http, 10);
    http.start(); client.setBaseURL(http.url); fallback();
    long long now = time(1, 22, 30);
    burst(now, true);
    ASSERT_EQ(client.listFiles("20260301").size(), 2u);
    const auto probedOnce = [&](bool expected, const char* why) {
        now += 65;
        burst(now);
        const int before = http.calls(kBrp);
        ASSERT_EQ(client.listFiles("20260301").size(), 2u);
        EXPECT_EQ(http.calls(kBrp) - before, expected ? 1 : 0) << why;
    };
    putSessionA(http, 11);
    probedOnce(true, "a BRP that grows is probed");
    // Held to its end.
    const auto local = (std::filesystem::temp_directory_path() / "hms-sdd069-eof_BRP.edf").string();
    size_t got = 0;
    ASSERT_TRUE(client.downloadFileRange("20260301", "20260301_221626_BRP.edf", local, 0, got));
    probedOnce(true, "one unchanged burst is not two");
    // The record count moves at the same size (a file finalised in place).
    putFile(http, kStart, "20260301_221626_BRP.edf", 1, 12, 256 + 11 * 100);
    probedOnce(true, "a moved count is a change");
    probedOnce(true, "one unchanged burst after it");
    probedOnce(true, "two unchanged bursts after it: this probe proves it");
    probedOnce(false, "at EOF and unchanged across two bursts, the session is over");
    std::filesystem::remove(local);
}

TEST_F(EzShareFirmwareTest, Sdd069AStalledRangeKeepsTheSessionAsItIsThroughAnHour) {
    FirmwareHttp http;
    putSessionA(http, 10);
    http.start(); client.setBaseURL(http.url); fallback();
    long long now = time(1, 22, 30);
    burst(now, true);
    ASSERT_EQ(client.listFiles("20260301").size(), 2u);
    putSessionA(http, 11);
    now += 65;
    burst(now);
    client.listFiles("20260301");
    http.setDead(kBrp, true);
    const auto local = (std::filesystem::temp_directory_path() / "hms-sdd069-stall_BRP.edf").string();
    size_t got = 0;
    EXPECT_FALSE(client.downloadFileRange("20260301", "20260301_221626_BRP.edf", local, 0, got));
    // An hour of bursts that see nothing move: a copy short of its end keeps
    // the session open, probed every burst. No clock turns it off.
    for (int i = 0; i < 56; ++i) {
        now += 65;
        burst(now);
        const int before = http.calls(kBrp);
        ASSERT_EQ(client.listFiles("20260301").size(), 2u);
        EXPECT_EQ(http.calls(kBrp), before + 1) << "burst " << i;
    }
    // The rest arrives: the unchanged bursts already counted stand (P4).
    http.setDead(kBrp, false);
    ASSERT_TRUE(client.downloadFileRange("20260301", "20260301_221626_BRP.edf", local, 0, got));
    now += 65;
    burst(now);
    const int before = http.calls(kBrp);
    client.listFiles("20260301");
    EXPECT_EQ(http.calls(kBrp), before);
    std::filesystem::remove(local);
}

TEST_F(EzShareFirmwareTest, Sdd069AnHtmlReplyForAKnownFileKeepsItAsItWasThroughAnHour) {
    FirmwareHttp http;
    putSessionA(http, 10);
    http.start(); client.setBaseURL(http.url); fallback();
    long long now = time(1, 22, 30);
    burst(now, true);
    ASSERT_EQ(client.listFiles("20260301").size(), 2u);
    putSessionA(http, 11);
    now += 65;
    burst(now);
    client.listFiles("20260301");
    now += 65;
    burst(now);
    client.listFiles("20260301");   // one unchanged burst
    const auto held = http.files[kBrp];
    http.removeFile(kBrp);           // the card answers its welcome page
    for (int i = 0; i < 56; ++i) {
        now += 65;
        burst(now);
        const int before = http.calls(kBrp);
        const auto files = client.listFiles("20260301");
        ASSERT_EQ(files.size(), 2u) << "a known file that answers HTML is still listed";
        const auto brp = std::find_if(files.begin(), files.end(),
                                      [](const auto& e) { return e.name == "20260301_221626_BRP.edf"; });
        ASSERT_NE(brp, files.end());
        EXPECT_EQ(brp->size_bytes, held.size()) << "with the size it had";
        EXPECT_EQ(http.calls(kBrp), before + 1) << "and still open, burst " << i;
    }
    // Back as it was: the streak resumes where it stood, so this is the second.
    http.setFile(kBrp, held);
    now += 65;
    burst(now);
    client.listFiles("20260301");
    now += 65;
    burst(now);
    const int before = http.calls(kBrp);
    client.listFiles("20260301");
    EXPECT_EQ(http.calls(kBrp), before);
}

TEST_F(EzShareFirmwareTest, Sdd069TheMachineClockFromAGrowingBrpMovesTonightsFolder) {
    // The host's 11:30 on 2 March; the machine runs an hour ahead, so its
    // 12:30 ends record 150 of a BRP it started at 10:00.
    std::tm w{};
    w.tm_year = 126; w.tm_mon = 2; w.tm_mday = 2; w.tm_hour = 11; w.tm_min = 30;
    const long long wall = timegm_utc(&w);
    const long long now = wall - hostZoneOffset(wall);
    FirmwareHttp http;
    putFile(http, "02.03.2610.00.00", "20260302_100000_BRP.edf", 1, 149);
    http.start(); client.setBaseURL(http.url); fallback();
    burst(now, true);
    ASSERT_EQ(client.listFiles("20260301").size(), 1u);
    long long off = 0;
    EXPECT_FALSE(offset(off)) << "a first sighting is no growth";
    putFile(http, "02.03.2610.00.00", "20260302_100000_BRP.edf", 1, 150);
    burst(now + 65);
    client.listFiles("20260301");
    ASSERT_TRUE(offset(off));
    EXPECT_EQ(off, 3600 + hostZoneOffset(now));
    EXPECT_EQ(tonightFolder(now + 65, hostZoneOffset(now)), "20260301") << "the host's zone";
    EXPECT_EQ(tonightFolder(now + 65, off), "20260302") << "the machine's day";
    EXPECT_FALSE(offset(off)) << "said once";
}

TEST_F(EzShareFirmwareTest, Sdd069ASettledSessionIsProbedOnlyAtItsCheckpoints) {
    FirmwareHttp http;
    putSessionA(http, 10);
    http.start(); client.setBaseURL(http.url); fallback();
    long long now = time(1, 22, 30);
    burst(now, true);
    client.listFiles("20260301");
    putSessionA(http, 11);
    for (int i = 0; i < 3; ++i) {   // grows, then two unchanged bursts
        now += 65;
        burst(now);
        client.listFiles("20260301");
    }
    const long long grew = now - 130;
    const int settled = http.calls(kBrp);
    for (now += 65; now <= grew + 70 * 60; now += 65) {
        burst(now);
        ASSERT_EQ(client.listFiles("20260301").size(), 2u);
    }
    EXPECT_EQ(http.calls(kBrp) - settled, 3) << "once each at 5, 35 and 65 min after its growth";
}

TEST_F(EzShareFirmwareTest, Sdd069NoStrWhileTheNightGrowsAndOneAtTheSixtyFiveMinuteCheckpoint) {
    FirmwareHttp http;
    http.files["/STR.edf"] = std::string(256, ' ');
    http.start(); client.setBaseURL(http.url); fallback();
    const auto path = (std::filesystem::temp_directory_path() / "hms-sdd069-str.edf").string();
    long long now = time(1, 23, 0);
    for (int i = 0; i < 5; ++i, now += 65) {
        burst(now, false, true);
        client.beginRefresh();
        client.listDateFolders();
        client.listFiles("20260301");
        EXPECT_FALSE(client.downloadRootFile("STR.edf", path));
    }
    EXPECT_EQ(http.str_transfers, 0) << "no STR while the night grows";
    // Growth stopped: the bursts before the checkpoint read none of their own.
    burst(now);
    client.beginRefresh();
    client.listFiles("20260301");
    EXPECT_EQ(http.str_transfers, 0);
    // The 65-min checkpoint: one read, which the collector's own shares.
    burst(now + 3900, false, false, true);
    client.beginRefresh();
    EXPECT_EQ(http.str_transfers, 1);
    ASSERT_TRUE(client.downloadRootFile("STR.edf", path));
    EXPECT_EQ(http.str_transfers, 1);
    std::filesystem::remove(path);
}
} // namespace hms_cpap
