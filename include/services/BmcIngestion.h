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
    };

    /// Every session on the card, oldest first. Not ok when there is no card
    /// under the root or the parser cannot read it.
    Read readSessions(const std::string& device_id, const std::string& device_name) const;

    const std::string& root() const { return root_; }

private:
    std::string root_;
};

}  // namespace hms_cpap
