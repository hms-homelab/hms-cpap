#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace cpapdash::supervisor {

/**
 * SDD-073: the Sleep Bridge setup steps as data, like SourceChoice.
 *
 * The window reads the OS (volumes, interfaces) and the service's watch
 * socket, and asks these what they mean. Every wait in the steps is for an
 * event (a drive appearing, the bridge's own hello, its card statement);
 * nothing here knows a clock.
 */

/// D3: where the bridge is told to stream.
struct AddressChoice {
    std::string primary;              ///< "" when this computer has no usable address
    std::vector<std::string> others;  ///< behind "Not this one?"
};

/**
 * The address on the interface with the default route (`default_route_ip`,
 * the source address the OS picks for the internet) when it is one of
 * `ipv4` and usable; otherwise the first private one (lanAddress). The rest
 * that are usable go behind "Not this one?", private ones first.
 */
AddressChoice chooseBridgeAddress(const std::vector<std::string>& ipv4, const std::string& default_route_ip);

/// D3: the port is mentioned only when it is not the bridge's default, 8894.
bool mentionPort(int port);

/**
 * D1: a CPAP machine's card by the names at its root (any case): ResMed's
 * STR.edf with DATALOG, or its Identification file.
 */
bool looksLikeCpapCard(const std::vector<std::string>& root_names);

/**
 * D4: the bridge a watch frame says has arrived, or "": a "hello" event's
 * sender; in a snapshot (the socket just opened), a bridge online whose own
 * hello came at or after `since` (epoch s), so one that connected while the
 * socket was down still counts. A test hello is never a frame at all.
 */
std::string arrivedBridge(const nlohmann::json& frame, long long since);

/// The bridge's entry in a status body (GET /api/sleepbridge), or null.
nlohmann::json bridgeEntry(const nlohmann::json& status, const std::string& sender);

/// D5: what the bridge's card statement says.
enum class CardSeen {
    Waiting,   ///< no statement since `stated_before`
    Seen,      ///< the machine mounted the card
    Mismatch,  ///< it did, and it is not the card learned on this computer
};

/// D5, from a status body: a card statement newer than `stated_before`
/// (card_stated_at), and the learned card's verdict.
CardSeen cardSeen(const nlohmann::json& status, const std::string& sender, long long stated_before);

/// D5: the format to set again, or "" when nothing to change: the bridge's
/// card is now recognised (card_format) as another than the one set.
std::string formatToFix(const nlohmann::json& status, const std::string& sender, const std::string& format_set);

}  // namespace cpapdash::supervisor
