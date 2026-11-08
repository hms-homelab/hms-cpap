#pragma once
//
// SDD-061: a .cpapcard upload. CpapDash Desktop's "Export for HMS CPAP" writes
// one zip of a card: the card's own files at their card paths (stored), and
// `baseline.cbl1` at the root (deflated), the card's layout as a CBL1 payload
// (sleepbridge/LearnedCard.h).
//
// The upload route reads the layout out of the zip before it queues the card,
// so the person's answer says at once what became of it; the importer then
// takes the entry out of the extracted card before it is read, so the layout
// never lands in the archive as if the machine had written it. Everything
// else is the card-zip upload as it was.
//
#include <optional>
#include <string>

namespace hms_cpap {

inline constexpr const char* kCardLayoutEntry = "baseline.cbl1";

/// The layout entry of an uploaded zip: at its root, or inside one folder
/// that wraps the card. nullopt with `why` empty when the zip has none (a
/// plain card zip); nullopt with `why` set when it has one this end will not
/// read (over 32 MB unpacked, or it does not unpack).
std::optional<std::string> readCardLayout(const std::string& zip_path, std::string* why);

/// Removes every layout entry from an extracted card; how many were removed.
int removeCardLayout(const std::string& extracted_root);

/// SDD-061 D9: the card's own files in an uploaded zip, as the learned files
/// blob (sleepbridge/LearnedFiles.h): every entry but the layout, at its card
/// path (the folder that wraps the card, if any, taken off), '/' separated.
/// A path that would leave the card is left out, and so is every file once
/// the blob would pass 64 MB (`why` says so). "" when the zip has no files.
std::string readCardFilesBlob(const std::string& zip_path, std::string* why);

}  // namespace hms_cpap
