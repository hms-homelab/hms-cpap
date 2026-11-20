#pragma once
//
// SDD-081: hms-cpap's end of the OSCAR helper (OscarHelperWire.h has the
// frames).
//
// Trust on first use (D4). The first helper to connect while none is
// remembered gets a token, silently; only the token's SHA-256 and the PC's
// name are kept (<state_dir>/helper.json). Later connections carry the token.
// Any other is refused at the upgrade, HTTP 401 {"error":"has_helper",
// "helper":"<PC name>"}, or {"error":"forgotten"} for a token while none is
// remembered (a helper that was forgotten never takes the place back by
// itself). Forget drops the hash: the next helper with no token is remembered.
//
// The read (D5) is the reader's thread calling read(): the changed P-SERIES
// files, the import, the wanted keys, the small oscar.db back. It waits on the
// helper's frames and on its connection ending, nothing else: no timer. The
// connection's own silence rule (90 s, as the device streams') ends a helper
// that went away, and the read with it, as helper_offline.
//
// Every helper hello is an event (D6): `on_hello` asks for a read.
//
// No Drogon here: the socket glue (controllers/OscarHelperSocket) feeds frames
// in and hands outlets over, so the suite runs both ends in one process.
//
#include "services/OscarCardReader.h"
#include "services/OscarHelperWire.h"

#include <json/json.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hms_cpap::oscar::helper {

/// The remembered helper, kept on disk.
class Trust {
public:
    explicit Trust(std::string file);
    /// The remembered PC's name, or nullopt.
    std::optional<std::string> remembered() const;
    /// The token is the remembered one.
    bool matches(const std::string& token) const;
    /// Remembers `pc_name` with a new token, only while none is remembered.
    /// The token, or "" when one already is.
    std::string pair(const std::string& pc_name);
    /// Forget: drops the hash. False when none was remembered.
    bool forget();

private:
    void load();
    bool save() const;
    std::string file_;
    mutable std::mutex mu_;
    std::string name_;
    std::string hash_;
    std::string paired_at_;
};

/// What a connection was admitted as.
enum class Admit { Refused, Probe, Pairing, Helper };

struct Admission {
    Admit       as{Admit::Refused};
    int         status{200};   ///< 401 when refused
    Json::Value body{Json::objectValue};
    std::string pc_name;
};

struct Outlets {
    std::function<bool(const std::string&)> text;
    std::function<bool(const std::string&)> binary;
    std::function<void()>                   close;
};

class Host : public IRemoteOscar {
public:
    struct Options {
        std::string state_dir;    ///< <data_dir>/oscar-helper
        std::string install_id;
        std::string version;
        std::string name;         ///< this hms-cpap's name, for the installer's list
        std::size_t chunk_bytes{kChunkBytes};
        /// A helper said hello (D6). Never called with a lock held.
        std::function<void(const std::string& why)> on_hello;
    };

    explicit Host(Options o);
    ~Host() override;

    /// The upgrade's decision: the Authorization header ("Bearer <token>" or
    /// ""), X-Hms-Helper and whether X-Hms-Probe was set.
    Admission admit(const std::string& authorization, const std::string& pc_name, bool probe);

    /// An admitted connection opened. Its id.
    uint64_t open(const Admission& a, Outlets out);
    void onText(uint64_t id, const std::string& text);
    void onBinary(uint64_t id, const std::string& bytes);
    void closed(uint64_t id);

    /// Forget (D4). False when none was remembered. A helper connected now
    /// is closed.
    bool forget();

    /// Ends every wait (shutdown).
    void shutdown();

    // IRemoteOscar
    bool remembered() const override;
    Json::Value status() const override;
    RemoteRead read(const std::string& card_root, const Want& want, const std::string& slice_dir) override;

private:
    struct Conn {
        uint64_t    id{0};
        Admit       as{Admit::Refused};
        std::string pc_name;
        Outlets     out;
        bool        hello{false};     ///< answered: a session
        bool        open{true};
        std::string version;
        std::map<std::string, FileStamp> files;   ///< the helper's copy
        Assembly    incoming;
        std::map<uint32_t, Json::Value> replies;  ///< result, file_ack, slice_ready by req_id
        std::map<uint32_t, std::string> slices;   ///< the bytes of a slice_ready
    };

    Json::Value ackBody() const;
    void sendText(Conn& c, const Json::Value& v);
    /// Waits for req_id's reply on the active connection, or its end.
    std::optional<Json::Value> await(std::unique_lock<std::mutex>& lk, const std::shared_ptr<Conn>& c, uint32_t req);

    Options opt_;
    Trust   trust_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::map<uint64_t, std::shared_ptr<Conn>> conns_;
    std::shared_ptr<Conn> active_;    ///< the helper's session
    uint64_t next_id_{1};
    uint32_t next_req_{1};
    bool stopping_{false};
    // D8: the last hello spoke another protocol.
    bool update_needed_{false};
    std::string update_version_;
    std::string last_version_;
};

}  // namespace hms_cpap::oscar::helper
