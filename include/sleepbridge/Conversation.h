#pragma once
//
// One Sleep Bridge connection, as frames in and frames out. No Drogon here:
// the route hands it each frame and sends what it returns, so the unit suite
// drives every branch.
//
//   up    text   hello {state}, state {state} (every 30 s)
//         binary 12-byte header, kind 7: one batch body (BatchStore::handle)
//   down  text   ack (to hello), ack_sectors {seq, status, ...the answer},
//                error {error}
//
// Local mode sends no commands: no cmd frame ever leaves here. A result, a log
// text (kind 4) or a ring file (kind 5) is ignored, and said so in the log.
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

struct Effects {
    std::vector<std::string>   texts;   // frames to send, in order
    std::vector<std::string>   log;     // lines for the service log
    std::optional<Json::Value> state;   // a hello's or state's "state" object
    bool hello{false};
    bool banked{false};     // a batch with records was stored: rebuild soon
    bool card{false};       // a card statement was stored
};

class Conversation {
public:
    Conversation(BatchStore& store, Admission who);

    Effects onText(const std::string& frame);
    Effects onBinary(const std::string& frame);

    const Admission& who() const { return who_; }

private:
    BatchStore& store_;
    Admission   who_;
};

}  // namespace hms_cpap::sleepbridge
