#pragma once
//
// The mask, read from a card: an ez Share, a FlashAir or a local folder.
//
// A ResMed writes one BRP per mask-on and adds a record to it every minute
// while the mask is on; nothing on the card says the mask came off. So, as the
// CpapDash cloud reads it (its mask.on / mask.off):
//
//   on   a session's BRP grew in a burst: occurred_at is the session's start
//   off  the open session has not grown for kMaskOffAfterS, or a later session
//        began: occurred_at is the end of its last record
//
// A burst sees the card every minute or two and a change needs the next burst
// to show, so a card says it a burst or two late; the change carries both
// times and the reader sees the delay.
//
// Only what is being written now: a session whose data ended more than
// kRecentS ago is an old night read late (an import, a catch-up), and says
// nothing. Pure: the caller gives every time, so every rule is a unit test.
//
#include <optional>
#include <string>

namespace hms_cpap {

class CardMask {
public:
    static constexpr long long kMaskOffAfterS = 135;   // two missed minutes and a margin
    static constexpr long long kRecentS       = 600;

    struct Change {
        bool        on{false};
        std::string session;      // the BRP's stem, e.g. 20261104_231502
        long long   occurred_s{0};
    };

    /// A session's BRP grew in this burst. start_s and end_s are its first
    /// and last record, UTC epoch seconds on the machine's clock read on the
    /// real one; end_s 0 when the file could not be read. now_s: the host's.
    std::optional<Change> grew(const std::string& session, long long start_s, long long end_s,
                               long long now_s);
    /// The off a burst with no growth can bring; nothing before kMaskOffAfterS.
    std::optional<Change> tick(long long now_s);

    bool on() const { return on_; }
    const std::string& session() const { return session_; }

private:
    bool        on_{false};
    std::string session_;
    long long   start_s_{0}, end_s_{0}, grew_at_{0};
};

}  // namespace hms_cpap
