// SDD-056: a Toshiba FlashAir read like an ez Share. The listing lines are the
// card's own (a W-02 on firmware W2.00.02, 2026-10-29), and the client is run
// against a one-shot HTTP server that records what it was asked.
#include <gtest/gtest.h>
#include "clients/FlashAirClient.h"
#include "utils/FolderListingStore.h"

#include <arpa/inet.h>
#include <filesystem>
#include <fstream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace hms_cpap;
namespace fs = std::filesystem;

namespace {

class OneShotHttpServer {
public:
    OneShotHttpServer(std::string status, std::string body, std::string headers = "")
        : status_(std::move(status)), body_(std::move(body)), headers_(std::move(headers)) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_GE(listen_fd_, 0);
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        EXPECT_EQ(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        EXPECT_EQ(::listen(listen_fd_, 1), 0);
        socklen_t len = sizeof(addr);
        EXPECT_EQ(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len), 0);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~OneShotHttpServer() {
        if (listen_fd_ >= 0) ::shutdown(listen_fd_, SHUT_RDWR);
        if (thread_.joinable()) thread_.join();
        if (listen_fd_ >= 0) ::close(listen_fd_);
    }

    std::string baseUrl() const { return "http://127.0.0.1:" + std::to_string(port_); }

    /// The request as it arrived, once the client is done with it.
    std::string request() {
        if (thread_.joinable()) thread_.join();
        return request_;
    }

private:
    void serve() {
        int conn = ::accept(listen_fd_, nullptr, nullptr);
        if (conn < 0) return;
        char buf[4096];
        const ssize_t n = ::recv(conn, buf, sizeof(buf), 0);
        if (n > 0) request_.assign(buf, static_cast<size_t>(n));
        const std::string resp =
            "HTTP/1.1 " + status_ + "\r\n" + headers_ +
            "Content-Length: " + std::to_string(body_.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n" + body_;
        size_t sent = 0;
        while (sent < resp.size()) {
            const ssize_t s = ::send(conn, resp.data() + sent, resp.size() - sent, 0);
            if (s <= 0) break;
            sent += static_cast<size_t>(s);
        }
        ::close(conn);
    }

    std::string status_, body_, headers_, request_;
    int listen_fd_ = -1;
    uint16_t port_ = 0;
    std::thread thread_;
};

class FlashAirClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() / ("hms_flashair_" + std::to_string(::getpid()));
        fs::remove_all(dir_);
        fs::create_directories(dir_);
    }
    void TearDown() override { fs::remove_all(dir_); }

    std::string path(const std::string& name) const { return (dir_ / name).string(); }
    static std::string read(const std::string& p) {
        std::ifstream f(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    static void write(const std::string& p, const std::string& bytes) {
        std::ofstream(p, std::ios::binary) << bytes;
    }

    fs::path dir_;
};

}  // namespace

// ── The listing ─────────────────────────────────────────────────────────────

TEST(FlashAirListing, ANightFolderAsTheCardListsIt) {
    const std::string body =
        "WLANSD_FILELIST\r\n"
        "/DATALOG/20261016,20261016_235259_BRP.edf,1483518,32,23889,10374\r\n"
        "/DATALOG/20261016,20261016_235253_CSL.crc,8,32,23889,1741\r\n";
    const auto e = FlashAirClient::parseFileList(body);
    ASSERT_EQ(e.size(), 2u);

    EXPECT_EQ(e[0].name, "20261016_235259_BRP.edf");
    EXPECT_FALSE(e[0].is_dir);
    EXPECT_EQ(e[0].size_kb, 1448) << "1,483,518 bytes, rounded down as a local folder's are";
    EXPECT_EQ(e[0].size_bytes, 1483518u) << "and the exact size kept beside it (D2 b)";
    // fdate 23889, ftime 10374: FAT-packed 2026-10-17 05:04:12.
    EXPECT_EQ(e[0].year, 2026);
    EXPECT_EQ(e[0].month, 10);
    EXPECT_EQ(e[0].day, 17);
    EXPECT_EQ(e[0].hour, 5);
    EXPECT_EQ(e[0].minute, 4);
    EXPECT_EQ(e[0].second, 12);

    EXPECT_EQ(e[1].size_kb, 0) << "8 bytes is under a KB";
}

TEST(FlashAirListing, TheRootHasAnEmptyFolderAndDirectoriesAreMarked) {
    const std::string body =
        "WLANSD_FILELIST\r\n"
        ",DATALOG,0,16,23889,3513\r\n"
        ",STR.edf,34904,32,23889,20825\r\n";
    const auto e = FlashAirClient::parseFileList(body);
    ASSERT_EQ(e.size(), 2u);
    EXPECT_EQ(e[0].name, "DATALOG");
    EXPECT_TRUE(e[0].is_dir);
    EXPECT_EQ(e[0].size_bytes, 0u) << "a folder has no size";
    EXPECT_EQ(e[1].name, "STR.edf");
    EXPECT_EQ(e[1].size_kb, 34);
}

TEST(FlashAirListing, ANameWithACommaKeepsItsComma) {
    const auto e = FlashAirClient::parseFileList(
        "WLANSD_FILELIST\n/reports,night 1, part 2.pdf,2048,32,23889,0\n");
    ASSERT_EQ(e.size(), 1u);
    EXPECT_EQ(e[0].name, "night 1, part 2.pdf");
    EXPECT_EQ(e[0].size_kb, 2);
}

TEST(FlashAirListing, HiddenSystemAndVolumeEntriesAreSkipped) {
    const auto e = FlashAirClient::parseFileList(
        "WLANSD_FILELIST\n"
        ",SD_WLAN,0,18,23889,0\n"      // 0x10 | 0x02, hidden
        ",SYS,0,20,23889,0\n"          // 0x10 | 0x04, system
        ",LABEL,0,8,23889,0\n"         // volume label
        ",SETTINGS,0,16,23889,0\n");
    ASSERT_EQ(e.size(), 1u);
    EXPECT_EQ(e[0].name, "SETTINGS");
}

TEST(FlashAirListing, GarbageAndShortLinesAreSkipped) {
    const auto e = FlashAirClient::parseFileList(
        "WLANSD_FILELIST\n"
        "not a listing\n"
        "/DATALOG,x.edf,12,32\n"                  // too few fields
        "/DATALOG,y.edf,big,32,23889,0\n"         // size not a number
        "/DATALOG,z.edf,1024,32,23889,0");        // no final newline: still read
    ASSERT_EQ(e.size(), 1u);
    EXPECT_EQ(e[0].name, "z.edf");
    EXPECT_EQ(e[0].size_kb, 1);
}

TEST(FlashAirListing, AnEmptyBodyIsAnEmptyListing) {
    EXPECT_TRUE(FlashAirClient::parseFileList("").empty());
    EXPECT_TRUE(FlashAirClient::parseFileList("WLANSD_FILELIST\r\n").empty());
}

// A night folder as the card listed it, 2026-10-30, mask off mid-session.
TEST(FlashAirListing, ANightFolderListsEveryFileAtItsExactSize) {
    const auto e = FlashAirClient::parseFileList(
        "WLANSD_FILELIST\r\n"
        "/DATALOG/20261029,20261029_223212_PLD.edf,87910,32,23902,2343\r\n"
        "/DATALOG/20261029,20261029_222829_CSL.edf,800,32,23901,46031\r\n");
    ASSERT_EQ(e.size(), 2u);
    EXPECT_EQ(e[0].size_bytes, 87910u);
    EXPECT_EQ(e[0].size_kb, 85);
    EXPECT_EQ(e[1].size_bytes, 800u) << "under a KB, where KB alone reads 0";
    EXPECT_EQ(e[1].size_kb, 0);
}

// SDD-056 D2 (b): the stored listing (SDD-054 D9) keeps the exact size as an
// 11th element, and reads one written before it existed.
TEST(FlashAirListing, AStoredListingKeepsTheExactSize) {
    StoredListing s;
    EzShareFileEntry f;
    f.name = "20261029_223212_PLD.edf";
    f.size_kb = 85;
    f.size_bytes = 87910;
    s.files.push_back(f);
    StoredListing back;
    folder_listing::decode(folder_listing::encode(s), back);
    ASSERT_EQ(back.files.size(), 1u);
    EXPECT_EQ(back.files[0].size_bytes, 87910u);

    StoredListing old;
    folder_listing::decode(R"([["x.edf",3,0,2026,10,29,22,32,12,1]])", old);
    ASSERT_EQ(old.files.size(), 1u);
    EXPECT_EQ(old.files[0].size_bytes, 0u) << "an older row has no exact size";
    EXPECT_EQ(old.done_files.count("x.edf"), 1u) << "and its done mark still reads";
}

TEST(FlashAirCardPath, BackslashesBecomeSlashesAndTheRestIsEncoded) {
    EXPECT_EQ(FlashAirClient::encodeCardPath(""), "/");
    EXPECT_EQ(FlashAirClient::encodeCardPath("SETTINGS"), "/SETTINGS");
    EXPECT_EQ(FlashAirClient::encodeCardPath("SETTINGS\\AGL.tgt"), "/SETTINGS/AGL.tgt");
    EXPECT_EQ(FlashAirClient::encodeCardPath("System Volume Information "),
              "/System%20Volume%20Information%20");
}

// ── The client over HTTP ────────────────────────────────────────────────────

TEST_F(FlashAirClientTest, DateFoldersComeFromOp100OnDatalogSortedAndOnlyDates) {
    OneShotHttpServer srv("200 OK",
                          "WLANSD_FILELIST\r\n"
                          "/DATALOG,20261016,0,16,23889,20802\r\n"
                          "/DATALOG,20261007,0,16,23889,3516\r\n"
                          "/DATALOG,notes,0,16,23889,0\r\n"
                          "/DATALOG,20261008.edf,10,32,23889,0\r\n");
    FlashAirClient client;
    client.setBaseURL(srv.baseUrl());

    EXPECT_EQ(client.listDateFolders(), (std::vector<std::string>{"20261007", "20261016"}));
    EXPECT_NE(srv.request().find("GET /command.cgi?op=100&DIR=/DATALOG HTTP"), std::string::npos)
        << srv.request();
}

TEST_F(FlashAirClientTest, AMissingFolderIsRefusedNotEmpty) {
    OneShotHttpServer srv("404 Not Found", "");
    FlashAirClient client;
    client.setBaseURL(srv.baseUrl());
    EXPECT_THROW(client.listFiles("20991231"), ListingRefused);
    EXPECT_NE(srv.request().find("DIR=/DATALOG/20991231 HTTP"), std::string::npos);
}

TEST_F(FlashAirClientTest, TheRootIsListedAsSlash) {
    OneShotHttpServer srv("200 OK", "WLANSD_FILELIST\r\n,SETTINGS,0,16,23889,0\r\n");
    FlashAirClient client;
    client.setBaseURL(srv.baseUrl());
    const auto e = client.listDir("");
    ASSERT_EQ(e.size(), 1u);
    EXPECT_TRUE(e[0].is_dir);
    EXPECT_NE(srv.request().find("DIR=/ HTTP"), std::string::npos) << srv.request();
}

TEST_F(FlashAirClientTest, AFileIsFetchedByItsPathOnTheCard) {
    OneShotHttpServer srv("200 OK", "EDF-BYTES");
    FlashAirClient client;
    client.setBaseURL(srv.baseUrl());
    const auto p = path("BRP.edf");
    ASSERT_TRUE(client.downloadFile("20261016", "20261016_235259_BRP.edf", p));
    EXPECT_EQ(read(p), "EDF-BYTES");
    const auto req = srv.request();
    EXPECT_NE(req.find("GET /DATALOG/20261016/20261016_235259_BRP.edf HTTP"), std::string::npos)
        << req;
    EXPECT_EQ(req.find("Range:"), std::string::npos) << "a whole file asks for no range";
}

TEST_F(FlashAirClientTest, ARangeAppendsAfterTheBytesWeHad) {
    const auto p = path("BRP.edf");
    write(p, std::string(100, 'A'));
    OneShotHttpServer srv("206 Partial Content", std::string(50, 'B'),
                          "Content-Range: bytes 100-149/150\r\n");
    FlashAirClient client;
    client.setBaseURL(srv.baseUrl());

    size_t got = 0;
    ASSERT_TRUE(client.downloadFileRange("20261016", "20261016_235259_BRP.edf", p, 100, got));
    EXPECT_EQ(got, 50u);
    EXPECT_EQ(read(p), std::string(100, 'A') + std::string(50, 'B'));
    EXPECT_NE(srv.request().find("Range: bytes=100-"), std::string::npos);
    EXPECT_TRUE(client.supportsRange());
}

// The card answers a file it does not have with a 307 to http://flashair/.
TEST_F(FlashAirClientTest, AMissingFileIsNotFetchedAndLeavesNothing) {
    OneShotHttpServer srv("307 Temporary Redirect", "", "Location: http://flashair/\r\n");
    FlashAirClient client;
    client.setBaseURL(srv.baseUrl());
    const auto p = path("STR.edf");
    EXPECT_FALSE(client.downloadRootFile("STR.edf", p));
    EXPECT_FALSE(fs::exists(p));
}

TEST_F(FlashAirClientTest, AFailedRangeLeavesThePartialFileAsItWas) {
    const auto p = path("PLD.edf");
    write(p, std::string(100, 'A'));
    OneShotHttpServer srv("307 Temporary Redirect", "", "Location: http://flashair/\r\n");
    FlashAirClient client;
    client.setBaseURL(srv.baseUrl());
    size_t got = 0;
    EXPECT_FALSE(client.downloadFileRange("20261016", "PLD.edf", p, 100, got));
    EXPECT_EQ(read(p), std::string(100, 'A'));
}

TEST_F(FlashAirClientTest, AWholeFileForARangeIsCutBackAndRangesStop) {
    const auto p = path("SAD.edf");
    write(p, std::string(100, 'A'));
    OneShotHttpServer srv("200 OK", std::string(150, 'C'));
    FlashAirClient client;
    client.setBaseURL(srv.baseUrl());
    size_t got = 0;
    EXPECT_FALSE(client.downloadFileRange("20261016", "SAD.edf", p, 100, got));
    EXPECT_EQ(read(p), std::string(100, 'A'));
    EXPECT_FALSE(client.supportsRange());
}

TEST_F(FlashAirClientTest, AResidueFileIsFetchedByItsCardPath) {
    OneShotHttpServer srv("200 OK", "tgt");
    FlashAirClient client;
    client.setBaseURL(srv.baseUrl());
    ASSERT_TRUE(client.downloadByPath("SETTINGS\\AGL.tgt", path("AGL.tgt")));
    EXPECT_NE(srv.request().find("GET /SETTINGS/AGL.tgt HTTP"), std::string::npos);
}
