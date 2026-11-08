#pragma once
//
// SDD-061 addendum (D9): the file contents of a card learned on a computer.
//
// A .cpapcard carries the card's own files beside its layout (CBL1). CBL1
// gives the rebuild the card's boot sector, FAT and folders; the files'
// contents it does not carry. A machine that appends to the same files every
// night (a BMC Luna: <serial>.000, .USR, .evt, .idx, .log) wrote most of their
// sectors before a Sleep Bridge was listening, so the rebuild holds them for
// good: every night adds a few sectors to files whose start it never saw.
//
// So the learn keeps the files too, and a fresh rebuilder is fed each file's
// data, right after the learned sectors and before the bank, as READ records
// in the same session 0, at the sectors the file occupies on the card: its
// first cluster and size from its directory entry in the learned folders, its
// cluster chain from the learned FAT, under the learned layout. Only a file
// whose directory entry and whole chain are in the learn is fed. Every
// captured write outranks these sectors (a higher sequence), exactly as it
// outranks a learned CBL1 sector.
//
// Kept on disk, under the Sleep Bridge root:
//
//   learned/card.files              the install's latest learn's files
//   <sender>/batches/learned.files  the copy a bridge rebuilds from
//
// The blob: "CBF1" u32 LE count, then count x (u16 LE path_len, path,
// u32 LE size, bytes). Paths card-relative, '/' separated; the whole blob at
// most kLearnedFilesMax.
//
#include "sleepbridge/LearnedCard.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hms_cpap::sleepbridge {

/// The largest file blob kept (all of a learn's files together).
inline constexpr size_t kLearnedFilesMax = 64u * 1024u * 1024u;

inline constexpr const char* kLearnedFilesFile  = "card.files";
inline constexpr const char* kBridgeLearnedFiles = "learned.files";

struct LearnedFile {
    std::string path;    // card-relative, '/' separated
    std::string bytes;   // the file as the computer read it
};

std::string encodeLearnedFiles(const std::vector<LearnedFile>& files);
/// nullopt with `why` set for a blob that is not one, or a path that would
/// leave the card.
std::optional<std::vector<LearnedFile>> decodeLearnedFiles(const std::string& blob, std::string* why);

/// Where the learned files' bytes sit on the card.
struct LearnedSeed {
    /// (lba, 512 bytes), each file's sectors in order, then the next file's.
    std::vector<std::pair<uint32_t, std::string>> sectors;
    int files{0};
    /// Card paths left out, with why ("not in the learned folders", ...).
    std::vector<std::string> skipped;
};

/// Each file found in the learned folders (by long or 8.3 name, case
/// ignored), with its chain from the learned FAT: its first min(content,
/// directory size) bytes at its sectors, the last one padded with zeros.
/// FAT32 only (an empty seed for any other card).
LearnedSeed seedLearnedFiles(const LearnedCard& card, const std::vector<LearnedFile>& files);

}  // namespace hms_cpap::sleepbridge
