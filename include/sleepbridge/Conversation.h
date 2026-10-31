#pragma once
//
// One Sleep Bridge connection, as frames in and frames out. No Drogon here:
// the route hands it each frame and sends what it returns, so the unit suite
// drives every branch.
//
//   up    text   hello {state}, state {state} (every 30 s),
//                result {req_id, result} (the answer to a command)
//         binary 12-byte header, kind 7: one batch body (BatchStore::handle)
//                kind 4: a command's log text, header req_id its command's
//   down  text   ack (to hello), ack_sectors {seq, status, ...the answer},
//                error {error}
//
// A hello whose state says "probe" or "portal_test" is a test of this
// address (the bridge's setup page, or a bridge about to switch here): it is
// answered with ack and nothing else, its state is not kept, and it is not the
// bridge's session. The bridge closes it within seconds.
//
// Commands go down from SleepBridgeService, not from here; this side reads
// their results and log text. A ring file (kind 5) is ignored.
//
// No authentication on the LAN. A bridge is named by its X-Coyote-Sender
// header and nothing else; an Authorization header is never read, so it can
// be neither stored nor logged.
//
#include "sleepbridge/BatchStore.h"

#include <json/json.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hms_cpap::sleepbridge {

/// Who a connection says it is, from the upgrade's headers.
struct Admission {
    std::string sender;    // X-Coyote-Sender
    std::string machine;   // X-Coyote-Machine, may be empty
    std::string chip_id;   // X-Chip-Id, shown only
};

/// The admission, or nullopt with `why` set. `header` returns "" for a header
/// the request does not carry. Only the three headers above are asked for.
std::optional<Admission> admit(const std::function<std::string(const std::string&)>& header,
                               std::string* why);

/// A bridge's answer to a command: {"t":"result","req_id":N,"result":{...}}.
struct CommandResult {
    uint64_t    req_id{0};
    Json::Value result{Json::objectValue};
};

/// One piece of a command's log text (binary kind 4), ahead of its result.
struct LogChunk {
    uint32_t    req_id{0};
    uint32_t    seq{0};
    bool        last{false};
    std::string text;
};

struct Effects {
    std::vector<std::string>   texts;   // frames to send, in order
    std::vector<std::string>   log;     // lines for the service log
    std::optional<Json::Value> state;   // a hello's or state's "state" object
    bool hello{false};      // the bridge's own hello: its session starts
    bool probe{false};      // a test hello: answered, nothing else
    bool banked{false};     // a batch with records was stored: rebuild soon
    bool card{false};       // a card statement was stored
    uint64_t conn{0};       // the connection these came from
    std::optional<CommandResult> result;
    std::optional<LogChunk>      log_text;
};

class Conversation {
public:
    Conversation(BatchStore& store, Admission who, uint64_t conn = 0);

    Effects onText(const std::string& frame);
    Effects onBinary(const std::string& frame);

    const Admission& who() const { return who_; }
    uint64_t conn() const { return conn_; }

private:
    BatchStore& store_;
    Admission   who_;
    uint64_t    conn_{0};
};

}  // namespace hms_cpap::sleepbridge
