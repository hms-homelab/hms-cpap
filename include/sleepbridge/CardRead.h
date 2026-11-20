#pragma once
//
// SDD-073 D1: the installer reads a machine's card once, in this computer, and
// learns its layout the way CpapDash Desktop's Export for HMS CPAP does
// (SDD-061): the CBL1 baseline of sleepbridge/LearnedCard.h. What it reads is
// the card's metadata, never its nights (platform SDD-042 D1):
//
//   - LBA 0 (the partition table) when the volume starts later, the
//     partition's boot sector, and on FAT32 its FSInfo and backup boot sector;
//   - FAT #1 over the clusters in use (up to the highest allocated cluster,
//     rounded to whole sectors); FAT #2 is a copy and is not read into it;
//   - every directory's sectors, walked from the root (long names included).
//
// FAT16 and FAT32. exFAT, NTFS and FAT12 are refused in plain words.
//
// Pure: the sectors come through `ReadSectors`, so the suite reads a card
// image built in memory and the command line (`hms_cpap --read-card`) reads
// the raw device, read-only.
//
#include <json/json.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hms_cpap::sleepbridge {

/// Reads `count` sectors from `lba` into `out` (count x 512 bytes); false
/// when the device would not give them.
using ReadSectors = std::function<bool(uint32_t lba, uint32_t count, std::string& out)>;

/// Sectors read so far and the sectors the read expects in all (which grows
/// as folders are found), called every few hundred sectors.
using ReadProgress = std::function<void(size_t done, size_t total)>;

struct CardBaseline {
    Json::Value statement{Json::objectValue};
    std::vector<std::pair<uint32_t, std::string>> sectors;   // ascending, 512 bytes each
};

/// The card's baseline, or nullopt with `why` (a person's words).
/// `labels` are added to the statement as they are (machine_serial, brand,
/// reader): nothing keys on them.
std::optional<CardBaseline> readCardBaseline(const ReadSectors& read, std::string* why,
                                             const Json::Value& labels = Json::Value(Json::objectValue),
                                             const ReadProgress& progress = {});

/// The whole disk a volume's device lives on, for the raw read: the card's
/// own LBA 0 is on the disk, not on the volume. macOS "/dev/disk4s1" ->
/// "/dev/rdisk4", Linux "/dev/sdb1" -> "/dev/sdb", "/dev/mmcblk0p1" ->
/// "/dev/mmcblk0". Anything else comes back as it was.
std::string wholeDiskFor(const std::string& device);

/// Whether a card file at `rel` (card-relative, '/' separated) goes into the
/// .cpapcard beside the layout: not the system's own folders a computer adds
/// to a card (.Spotlight-V100, .fseventsd, .Trashes, System Volume
/// Information, $RECYCLE.BIN), nor any hidden file.
bool cardFileToKeep(const std::string& rel);

}  // namespace hms_cpap::sleepbridge
