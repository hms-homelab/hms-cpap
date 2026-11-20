#pragma once

#include "parsers/CpapdashBridge.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hms_cpap {

// SDD-049: a BMC / React Health Luna card.
//
// Sibling of SefamIngestion, with the opposite shape: a Sefam card keeps one
// session per folder, a BMC card keeps EVERY session in the same few files
// (<serial>.000, .001, ... one packet per second, and <serial>.evt). So there
// is nothing to walk per session. The card is read whole and the parser splits
// it; a donor card holding 6.6 hours is about 6 MB, which is cheaper to re-read
// each burst than to track which shared file changed (D4).
class BmcIngestion {
public:
    explicit BmcIngestion(std::string card_root);

    /// The folder holding the card: [root] itself, or the first folder up to
    /// [max_depth] levels under it whose files the parser recognises as a BMC
    /// card (a zip often wraps the card in a folder or two).
    static std::optional<std::string> findCardDir(const std::string& root, int max_depth = 3);

    struct Read {
        bool ok = false;
        std::string error;                               // set when !ok
        std::string card_dir;
        std::vector<std::unique_ptr<CPAPSession>> sessions;   // oldest first
        /// SDD-058 D4: the card has its .evt. A card a Luna has not yet
        /// stopped on (a new card, rebuilt live) has only its .000.
        bool has_event_file = false;
    };

    /// Every session on the card, oldest first. Not ok when there is no card
    /// under the root or the parser cannot read it. A card whose .000 has no
    /// .evt beside it yet (the machine writes it at the stop) is read from
    /// its data files alone, with no machine events (SDD-058 D2, D4).
    /// [flow_apneas] also fills each session's computed_events (SDD-058 D3);
    /// off, the sessions are what they always were.
    Read readSessions(const std::string& device_id, const std::string& device_name,
                      bool flow_apneas = false) const;

#ifdef CPAPDASH_WITH_BMC
    /// The card in [dir] itself (no search): the parser's folder read, or,
    /// when the .000 has no .evt beside it yet, its data files and .USR alone.
    /// [has_event_file] says which. nullopt when [dir] holds no card.
    static std::optional<cpapdash::parser::BmcCard> readCard(const std::string& dir,
                                                             bool* has_event_file = nullptr);
#endif

private:
    std::string root_;
};

}  // namespace hms_cpap
