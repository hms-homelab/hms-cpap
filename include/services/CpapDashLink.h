#pragma once
//
// platform SDD-059 D2: the WebSocket hms-cpap holds to CpapDash.
//
// One address only: wss://api.cpapdash.com/socket/v1/hms, CpapDash's gateway.
// The token goes on the upgrade (Authorization: Bearer, the SDD-057 hms token)
// with the install id (X-Hms-Install). The frames are the device socket's:
// text frames are one JSON object with `t`, binary frames a 12-byte header
// then the bytes.
//
// The link is an interface so the uploader runs the same against a local fake
// server in the tests. makeCpapDashLink() is Drogon's WebSocket client in a
// build with the web server, and nullptr without one.
//
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hms_cpap {

struct CpapDashEndpoint {
    bool        tls  = true;
    std::string host = "api.cpapdash.com";
    uint16_t    port = 443;
    std::string path = "/socket/v1/hms";
    /// A WebSocket ping this often, so an idle socket is never silent: the
    /// gateway closes one that has heard nothing from hms-cpap for 90 s.
    double      ping_every_s = 30.0;
    /// More headers on the upgrade (SDD-081: the OSCAR helper's PC name and
    /// probe mark). An empty token sends no Authorization, an empty install
    /// id no X-Hms-Install.
    std::vector<std::pair<std::string, std::string>> headers;
};

class CpapDashLink {
public:
    struct Connect {
        bool        ok = false;
        int         http_status = 0;   ///< the upgrade's answer when it was refused, 0 when unknown
        std::string error;             ///< why not, for Settings
        std::string http_body;         ///< the refused upgrade's body, when it was learned
    };
    struct Frame {
        enum class Kind { Text, Ping, Closed, Binary };
        Kind        kind = Kind::Text;
        std::string text;              ///< Text: the frame; Binary: its bytes (SDD-081)
        int         close_code = 0;    ///< Closed: the peer's close code, 0 when none was given
        std::string close_reason;      ///< Closed: the peer's close reason
    };

    virtual ~CpapDashLink() = default;

    /// Opens the socket. Blocks until it is open, refused, or [timeout] passed.
    virtual Connect connect(const std::string& token, const std::string& install_id,
                            const std::string& user_agent, std::chrono::milliseconds timeout) = 0;
    virtual bool sendText(const std::string& frame) = 0;
    virtual bool sendBinary(const std::string& frame) = 0;
    /// The next frame from the server, waiting up to [wait]; nullopt when none came.
    virtual std::optional<Frame> next(std::chrono::milliseconds wait) = 0;
    virtual void close() = 0;
};

/// Drogon's WebSocket client to [ep]; nullptr in a build without the web server.
std::unique_ptr<CpapDashLink> makeCpapDashLink(const CpapDashEndpoint& ep);

}  // namespace hms_cpap
