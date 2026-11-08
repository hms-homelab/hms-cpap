#pragma once
//
// A card learned on a computer (SDD-061): its layout and metadata sectors, as
// CpapDash Desktop reads them from the card once, so a Sleep Bridge's rebuild
// knows the card before the machine writes to it.
//
// The payload is CBL1, the `baseline.cbl1` entry of a .cpapcard file:
//
//   "CBL1"   u32 LE json_len   json statement   u32 LE count   count x (u32 LE lba, 512 bytes)
//
// LBAs absolute on the card, ascending, no repeats, the whole payload at most
// 32 MB. The statement names the layout (fat, partition_lba,
// sectors_per_cluster, data_begin_lba, root_cluster, root_dir_lbas,
// fat_begin_lba, boot_sha256, ...) and some labels (machine_serial, brand,
// volume_serial) that nothing here keys on: clone cards share them.
//
// The sectors are checked by content before anything is kept (D3): the boot
// sector at partition_lba parses, agrees with the statement and hashes to
// boot_sha256; the root directory's sectors are there and read as directory
// entries; the FAT's first two entries are the media and end-of-chain marks.
//
// Kept on disk, under the Sleep Bridge root:
//
//   learned/card.cbl1               the install's latest learn, as uploaded
//   learned/card.json               when, from which file, and its summary
//   <sender>/batches/learned.cbl1   the copy a bridge rebuilds from (D4, D5)
//   <sender>/batches/learned.json   its summary, so a check never inflates it
//
#include "sleepbridge/Rebuilder.h"

#include <json/json.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hms_cpap::sleepbridge {

/// The largest CBL1 payload taken, as the cloud takes it.
inline constexpr size_t kLearnedCardMax = 32u * 1024u * 1024u;

inline constexpr const char* kLearnedDir      = "learned";
inline constexpr const char* kLearnedCardFile = "card.cbl1";
inline constexpr const char* kLearnedCardMeta = "card.json";
inline constexpr const char* kBridgeLearned     = "learned.cbl1";
inline constexpr const char* kBridgeLearnedMeta = "learned.json";

struct LearnedCard {
    Json::Value statement{Json::objectValue};
    /// Ascending by LBA, each 512 bytes.
    std::vector<std::pair<uint32_t, std::string>> sectors;

    // From the statement.
    std::string fat;              // "fat16" or "fat32"
    std::string volume_serial;    // a label
    std::string machine_serial;   // a label, "" when the card names none
    std::string brand;            // a label, "" when the card names none
    std::string boot_sha256;
    std::string read_at;
    std::string desktop_version;
    uint32_t    partition_lba{0};
    uint32_t    sectors_per_cluster{0};
    uint32_t    data_begin_lba{0};
    uint32_t    root_cluster{0};   // 0 on FAT16
    uint32_t    fat_begin_lba{0};
    std::vector<uint32_t> root_dir_lbas;
    std::vector<std::string> folders;
    int         nights{0};

    // From the boot sector.
    uint32_t    fats{0};
    uint32_t    fat_sectors{0};
    uint32_t    cluster_count{0};

    bool fat32() const { return fat == "fat32"; }
};

/// The payload decoded and checked by content (D3). nullopt with `why` set
/// (a person's words, naming the check) when it is not one to keep.
std::optional<LearnedCard> decodeLearnedCard(const std::string& bytes, std::string* why);

/// A CBL1 payload from a statement and its sectors (ascending). The suites
/// build cards with it; the format is Desktop's, not this end's.
std::string encodeCbl1(const Json::Value& statement,
                       const std::vector<std::pair<uint32_t, std::string>>& sectors);

/// Lower-case hex SHA-256 of `bytes`.
std::string sha256HexOf(const std::string& bytes);

/// The learn's layout and a rebuild's agree: sectors per cluster and data
/// start, and the FAT start when both know it.
bool sameLayout(const LearnedCard& card, const RebuildGeometry& g);
bool sameLayout(const Json::Value& summary, const RebuildGeometry& g);

/// A bridge whose own stream confirmed this layout as its card's: read from a
/// boot sector, in a bridge session (session 0 is the learned card's own).
bool knowsOwnLayout(const RebuildGeometry& g);

/// What the answers and learned.json say about a learn: the labels, the
/// layout, the counts. No sectors.
Json::Value learnedSummary(const LearnedCard& card);

/// Keeps `bytes` as the install's latest learn (learned/card.cbl1 and
/// card.json, written durably). False with `why` when it could not be written.
bool keepLearnedCard(const std::string& root, const LearnedCard& card, const std::string& bytes,
                     const std::string& file_name, std::string* why);

/// learned/card.json, or null when nothing was learned.
Json::Value readLearnedMeta(const std::string& root);

/// A bridge's learned.json, or null when it holds no learned card.
Json::Value readBridgeLearned(const std::string& batches_dir);

/// Copies a learn into a bridge's batches/ (learned.cbl1 and learned.json).
bool giveBridgeLearned(const std::string& batches_dir, const std::string& bytes,
                       const Json::Value& summary, std::string* why);

/// How a bridge's rebuild used the learned card it holds.
struct LearnedUse {
    /// "" (holds none), "fed", "fat16", "another_layout", "session_zero",
    /// "unreadable".
    std::string state;
    std::string why;           // for every state but "" and "fed"
    int         sectors{0};    // fed this rebuild
    Json::Value summary{Json::nullValue};
    // SDD-061 D9: the learned files' contents fed after the sectors.
    int         files{0};
    int         file_sectors{0};
    std::vector<std::string> files_skipped;
};

/// The verdict on a bridge that holds a learned card (D8), from how the
/// rebuild used it and what its last look trusted.
struct LearnedVerdict {
    std::string verdict;   // waiting, latched, mismatch, not_used
    std::string reason;
};
LearnedVerdict learnedVerdict(const LearnedUse& use, bool stepped, const RebuildGeometry& geometry,
                              bool holding, const std::string& holding_reason);

}  // namespace hms_cpap::sleepbridge
