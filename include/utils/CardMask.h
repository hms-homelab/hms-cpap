#pragma once
//
// The mask, read from a card: an ez Share, a FlashAir or a local folder.
//
// A ResMed writes one BRP per mask-on and adds a record to it every record
// duration while the mask is on; nothing on the card says the mask came off.
// SDD-068 decides it from what the card shows, never on a timer:
//
//   on   a new BRP with at least one record grew in a burst (D3; a BRP that
//        is only its header is not a mask-on): occurred_at is the session's
//        start
//   off  a listing taken after the BRP's last growth shows it at EOF, and two
//        bursts end with its listed size and stamp unchanged (D1): the
//        machine's next record never came. Or a later session began, which
//        is the off of the one before. occurred_at is the end of its last
//        record
//
// A listing that shows more than is stored (a partial BRP) or a card that does
// not answer decides nothing (D2): the mask stays as it is. When one burst
// proves several changes (a reconnect: the old session ended, a new one
// began), each is returned, in the order they occurred (D4).
//
// Only what is being written now: a session whose data ended more than
// kRecentS ago is an old night read late (an import, a catch-up), and says
// nothing. Pure: the caller gives every time, so every rule is a unit test.
//
#include <optional>
#include <string>
#include <vector>

namespace hms_cpap {

class CardMask {
public:
    static constexpr long long kRecentS        = 600;
    static constexpr int       kUnchangedLists = 2;      // D1: two bursts unchanged at EOF

    struct Change {
        bool        on{false};
        std::string session;      // the BRP's stem, e.g. 20261104_231502
        long long   occurred_s{0};
    };

    /// A session's BRP grew in this burst. start_s and end_s are its first
    /// and last record, UTC epoch seconds on the machine's clock read on the
    /// real one; end_s 0 when the file could not be read. now_s: the host's.
    /// records: the stored BRP's records, -1 when it could not be read. A new
    /// session whose BRP has no record yet (only its header) is not a
    /// mask-on (D3).
    std::vector<Change> grew(const std::string& session, long long start_s, long long end_s,
                             long long now_s, long long records = -1);

    /// This burst's listing shows a BRP, before anything is downloaded.
    /// at_eof: what is stored holds what the listing shows (within its grain);
    /// sig: its listed size and stamp. The off of D1, once proven.
    std::optional<Change> listed(const std::string& session, bool at_eof, const std::string& sig);

    bool on() const { return on_; }
    const std::string& session() const { return session_; }

private:
    bool        on_{false};
    std::string session_;
    long long   start_s_{0}, end_s_{0}, grew_at_{0};
    std::string listed_sig_;      // the open session's last listed size and stamp
    int         unchanged_{0};    // listings in a row at EOF with that same sig
    std::string pending_session_; // a newer BRP listed before it grew into the open one
    std::string pending_sig_;
};

}  // namespace hms_cpap
