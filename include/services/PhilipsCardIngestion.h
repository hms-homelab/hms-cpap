#pragma once

#include "parsers/CpapdashBridge.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hms_cpap {

// A Philips DreamStation card whose files are plain, read from a folder: a
// card in a reader, a card folder shared on the network, or the copy an ez
// Share or a Mule & Miner in proxy mode keeps (PhilipsCardMirror).
//
// The card is PROP.BIN (or PROP.TXT) beside the P0 .. Pn folders, usually under
// P-SERIES/<folder>/. A session's files are <session>.B01 (summary), .B02
// (events) and .B05 (flow); a file can sit in any P folder, so the folders are
// read together and grouped by the session number in the chunk headers.
//
// The machine appends the open session's .B05 as it goes and writes its .B01
// and .B02 only at mask-off, so a session with flow and no summary is the one
// being recorded: it is read as flow only, its pressure not measured.
//
// A DreamStation 2 encrypts its card's files. hms-cpap does not decrypt them:
// an encrypted card is reported and gives no session.
class PhilipsCardIngestion {
public:
    explicit PhilipsCardIngestion(std::string card_root);

    /// The folder holding the card: [root] itself, or the first folder up to
    /// [max_depth] levels under it that holds a PROP.BIN or PROP.TXT and a P
    /// folder (P-SERIES/<folder>/ on a DreamStation card).
    static std::optional<std::string> findCardDir(const std::string& root, int max_depth = 3);

    struct Session {
        std::unique_ptr<CPAPSession> parsed;
        uint32_t number = 0;
        bool closed = false;   ///< its summary (.B01) is on the card
    };

    struct Read {
        bool ok = false;
        bool encrypted = false;   ///< the card is encrypted; nothing was read
        std::string error;        ///< set when !ok
        std::string card_dir;
        std::vector<Session> sessions;   ///< oldest first
    };

    /// Every session on the card, oldest first.
    Read readSessions(const std::string& device_id, const std::string& device_name) const;

private:
    std::string root_;
};

}  // namespace hms_cpap
