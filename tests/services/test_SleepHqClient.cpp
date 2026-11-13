// Issue #66: the O2 Ring export to SleepHQ. SleepHQ refused every ring CSV
// ("We couldn't find any CPAP machine data in this upload") while hms-cpap
// logged "exported". Ported from cpapdash-sleephq-lib 1.0.3, 1.0.4 and 1.0.6,
// where the cloud fixed the same three things: the import's device type, the
// CSV named after the team's own ring, and the upload's part filename.
#include <gtest/gtest.h>
#include "TestSocketCompat.h"
#include "services/SleepHqClient.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

using namespace hms_cpap;
using json = nlohmann::json;

namespace {

// One request on a loopback socket, answered with `response`; its bytes kept.
// Reads the headers, then exactly Content-Length bytes of body.
std::string serveOne(const std::string& response,
                     const std::function<void(const std::string& base_url)>& client) {
    const int srv = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    EXPECT_GE(srv, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    EXPECT_EQ(bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof addr), 0);
    EXPECT_EQ(listen(srv, 1), 0);
    socklen_t len = sizeof addr;
    getsockname(srv, reinterpret_cast<sockaddr*>(&addr), &len);

    std::string request;
    std::thread server([&] {
        const int c = static_cast<int>(accept(srv, nullptr, nullptr));
        if (c < 0) return;
        char buf[4096];
        size_t need = std::string::npos;
        for (;;) {
            const auto n = recv(c, buf, sizeof buf, 0);
            if (n <= 0) break;
            request.append(buf, static_cast<size_t>(n));
            const auto head_end = request.find("\r\n\r\n");
            if (head_end == std::string::npos) continue;
            if (need == std::string::npos) {
                size_t body = 0;
                const auto cl = request.find("Content-Length: ");
                if (cl != std::string::npos && cl < head_end)
                    body = std::strtoul(request.c_str() + cl + 16, nullptr, 10);
                need = head_end + 4 + body;
            }
            if (request.size() >= need) break;
        }
        send(c, response.data(), static_cast<int>(response.size()), 0);
        closeSocket(c);
    });
    client("http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port)));
    server.join();
    closeSocket(srv);
    return request;
}

const std::string kCreated =
    "HTTP/1.1 201 Created\r\nContent-Length: 20\r\nConnection: close\r\n\r\n"
    "{\"data\":{\"id\":\"17\"}}";

}  // namespace

// SleepHQ decides from the import's device TYPE what its files are. With none
// it reads them as a CPAP card and refuses a ring CSV.
TEST(SleepHqClientImport, TheRingImportNamesTheO2RingAndTheCardNamesNone) {
    EXPECT_EQ(SleepHqClient::kO2RingDeviceId, 69184);
    EXPECT_EQ(SleepHqClient::importForm(SleepHqClient::kO2RingDeviceId),
              "programmatic=true&device_id=69184");
    EXPECT_EQ(SleepHqClient::importForm(0), "programmatic=true")
        << "the card export sends no device type, as before";
}

TEST(SleepHqClientImport, TheFormGoesOnTheWire) {
    for (const int device : {SleepHqClient::kO2RingDeviceId, 0}) {
        std::string id, err;
        const auto req = serveOne(kCreated, [&](const std::string& base) {
            SleepHqClient c("id", "secret");
            c.setToken("tok", "team-9");
            c.setBaseUrl(base);
            id = device ? c.createImport(err, device) : c.createImport(err);
        });
        EXPECT_EQ(id, "17") << err;
        EXPECT_NE(req.find("POST /teams/team-9/imports "), std::string::npos) << req;
        const auto body = req.substr(req.find("\r\n\r\n") + 4);
        EXPECT_EQ(body, device ? "programmatic=true&device_id=69184" : "programmatic=true");
    }
}

// SleepHQ files a ring CSV by the digits after "O2Ring " in its name: the
// team's own ring's serial, or none (and then a team that has a ring with a
// serial refuses the file).
TEST(SleepHqClientO2RingSerial, TheTeamsLatestDigitOnlyViatomRingIsTheName) {
    const auto j = json::parse(R"({"data":[
        {"attributes":{"model":"AirSense11AutoSet","brand":"ResMed","serial_number":"23223281654",
                       "updated_at":"2026-10-23 09:13:33 UTC"}},
        {"attributes":{"model":"O2 Ring","brand":"Viatom","serial_number":"5950",
                       "updated_at":"2026-10-21 15:45:08 UTC"}},
        {"attributes":{"model":"O2 Ring","brand":"Viatom","serial_number":"6011",
                       "updated_at":"2026-10-22 01:00:00 UTC"}},
        {"attributes":{"model":"O2 Ring","brand":"Viatom","serial_number":"4233A",
                       "updated_at":"2026-10-24 01:00:00 UTC"}},
        {"attributes":{"model":"O2 Ring","brand":"Viatom","serial_number":null,
                       "updated_at":"2026-10-25 01:00:00 UTC"}}]})");
    EXPECT_EQ(SleepHqClient::pickO2RingSerial(j), "6011")
        << "the CPAP, a serial with a letter and no serial are all passed over";
    EXPECT_EQ(SleepHqClient::pickO2RingSerial(json::parse(R"({"data":[]})")), "")
        << "no ring: the CSV names none";
    EXPECT_EQ(SleepHqClient::pickO2RingSerial(json("junk")), "");
}

TEST(SleepHqClientUpload, TheFileGoesUpUnderItsNameNotTheTempFiles) {
    // Written where the service writes a ring CSV: a temp name with a prefix.
    const auto tmp = std::filesystem::temp_directory_path() /
                     "hms_oxi_20261022_O2Ring 5950_20261023000921.csv";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << "Time,Oxygen Level\r\n";
    }
    bool ok = false;
    std::string err;
    const auto req = serveOne(
        "HTTP/1.1 201 Created\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}",
        [&](const std::string& base) {
            SleepHqClient c("id", "secret");
            c.setToken("tok", "team-9");
            c.setBaseUrl(base);
            ok = c.uploadFile("17", "O2Ring 5950_20261023000921.csv", "", tmp.string(), err);
        });
    std::filesystem::remove(tmp);
    EXPECT_TRUE(ok) << err;
    // SleepHQ stores the part under its filename and then looks for `name`
    // in it: "We could not find <name> in your upload" when they differ.
    EXPECT_NE(req.find("filename=\"O2Ring 5950_20261023000921.csv\""), std::string::npos) << req;
    EXPECT_EQ(req.find("hms_oxi_"), std::string::npos) << req;
}

TEST(SleepHqClientImportStatus, AFailedImportReadsAsFailedWithSleepHqsReason) {
    std::string status, reason;
    EXPECT_TRUE(SleepHqClient::parseImportStatus(json::parse(R"({"data":{"id":"17",
        "attributes":{"status":"failed",
                      "failed_reason":"We couldn't find any CPAP machine data in this upload"}}})"),
        status, reason));
    EXPECT_EQ(status, "failed");
    EXPECT_EQ(reason, "We couldn't find any CPAP machine data in this upload");

    EXPECT_TRUE(SleepHqClient::parseImportStatus(json::parse(
        R"({"data":{"attributes":{"status":"complete","failed_reason":null}}})"), status, reason));
    EXPECT_EQ(status, "complete");
    EXPECT_EQ(reason, "");

    EXPECT_FALSE(SleepHqClient::parseImportStatus(json::parse(R"({"errors":[]})"), status, reason));
    EXPECT_FALSE(SleepHqClient::parseImportStatus(json("junk"), status, reason));
}

TEST(SleepHqClientImportStatus, TheStatusIsReadFromTheImport) {
    const std::string body =
        R"({"data":{"attributes":{"status":"failed","failed_reason":"nope"}}})";
    std::string status, reason, err;
    bool read = false;
    const auto req = serveOne(
        "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\nConnection: close\r\n\r\n" + body,
        [&](const std::string& base) {
            SleepHqClient c("id", "secret");
            c.setToken("tok", "team-9");
            c.setBaseUrl(base);
            read = c.importStatus("17", status, reason, err);
        });
    EXPECT_TRUE(read) << err;
    EXPECT_NE(req.find("GET /imports/17 "), std::string::npos) << req;
    EXPECT_EQ(status, "failed");
    EXPECT_EQ(reason, "nope");
}
