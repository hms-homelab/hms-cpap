#pragma once
//
// SDD-081 D1, D2, D4, D5: the OSCAR helper, the home PC's end.
//
// `hms_cpap --oscar-helper`: no web server, no collector, no database. It
// connects out to hms-cpap (ws://<host>:8894/oscar/v1/helper), says hello
// with the list of its copy of the card, and then does what hms-cpap asks and
// only that (D5):
//
//   - file_ready: writes one P-SERIES file into its copy (a safe relative path
//     only, checked against its md5, its mtime set);
//   - import: runs the user's OSCAR exactly as the native reader does
//     (runImport: the gate, the profile, the signature, the headless import,
//     the job limit, the exit) on its copy, and answers with the outcome and
//     the profile's session heads;
//   - want: writes a small oscar.db of the named sessions (OscarDb::writeSlice,
//     a second connection; the user's oscar.db stays query-only) and sends it.
//
// No program, flag or path comes from the wire. The copy lives in
// <data_dir>/oscar-helper/<hms-cpap install id>/P-SERIES/; nothing else is
// kept: no nights, no database, no CpapDash link. The card's files are never
// opened here except to copy them; only OSCAR reads them.
//
// Trust (D4): the first hello carries no token and hms-cpap answers with one,
// kept in config.json and never logged. A refused token is never replaced by
// itself: the helper stops and says so ("hms-cpap forgot this PC"), and only
// connectAgain() (the tray's "Connect again") sends a hello with no token.
//
// The reconnect after a drop is the transport's only clock (D2): a doubling
// wait, as the CpapDash stream's. No read is started by it; hms-cpap starts
// one on the hello it leads to.
//
#include "services/CpapDashLink.h"
#include "services/OscarCard.h"
#include "services/OscarHelperWire.h"

#include <json/json.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace hms_cpap::oscar::helper {

/// The helper's part of config.json (D1): "role": "oscar_helper", the
/// SDD-080 "oscar" fields, and "oscar_helper".
struct HelperConfig {
    std::string host;           ///< hms-cpap's address
    int         port{kDefaultPort};
    std::string token;          ///< D4, never logged
    std::string install_id;     ///< the hms-cpap it reads for, learned from its ack
    std::string hms_name;
    Settings    oscar;
};

/// config.json <-> HelperConfig, every other key kept as it is.
HelperConfig loadHelperConfig(const std::string& config_path);
bool saveHelperConfig(const std::string& config_path, const HelperConfig& c, bool set_role = true);
/// The file's "role" ("" when none).
std::string configRole(const std::string& config_path);

class Helper {
public:
    using LinkFactory = std::function<std::unique_ptr<CpapDashLink>(const CpapDashEndpoint&)>;

    struct Options {
        std::string data_dir;                      ///< the helper's data folder
        std::function<HelperConfig()> load;
        std::function<void(const HelperConfig&)> save;
        LinkFactory factory;
        std::shared_ptr<IProcessRunner> runner;    ///< null: the system's
        std::string version;
        std::string pc_name;
        /// Every change of state (the tray reads it from status.json).
        std::function<void(const Json::Value&)> on_state;
        std::chrono::milliseconds connect_timeout{10000};
        std::chrono::milliseconds backoff_min{2000};
        std::chrono::milliseconds backoff_max{300000};
        std::size_t chunk_bytes{kChunkBytes};
    };

    /// How one connection ended.
    enum class End { Stop, Closed, Unreachable, Refused, Update, NoAddress };

    explicit Helper(Options o);
    ~Helper();

    void start();
    void stop();
    /// The tray's "Connect again" (D4): the token is dropped and a hello with
    /// none is sent.
    void connectAgain();

    /// {"state","host","port","hms_name","helper","last_import_at","last_outcome","message"}.
    /// state: no_address | connecting | connected | importing | unreachable |
    /// refused | forgotten | update
    Json::Value state() const;

    /// One connection, start to end, on the calling thread (the worker's
    /// body; the suite drives it).
    End runOnce();

    /// Where the copy of the card of `install_id` lives.
    std::string cardRoot(const std::string& install_id) const;

private:
    void run();
    void setState(const std::string& s, const std::string& message = "");
    void handle(CpapDashLink& link, const Json::Value& f, HelperConfig& cfg);
    void sendText(CpapDashLink& link, const Json::Value& v);

    Options opt_;
    std::shared_ptr<IProcessRunner> runner_;
    std::thread worker_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool stop_{false};
    uint64_t gen_{0};          ///< connectAgain
    Json::Value state_{Json::objectValue};
    Assembly incoming_;
    bool link_lost_{false};
};

}  // namespace hms_cpap::oscar::helper
