#pragma once
//
// SDD-065: what a Mule & Miner hands back for one ds2_read, a ".ds2r" file
// (stream kind 7 on the local socket). The miner walks the DreamStation 2
// itself and writes, in order (push-c3 SDD-043 D4, format version 1):
//
//   "DS2R" 0x01                              magic and version
//   then items:  kind u8 | len u32 LE | payload[len]
//     0x01 ANSWER      cmd u16 BE | sub u8 | status u8 | data       one per identity request
//     0x02 RECORD      stream u8 | session u32 BE | bookmark[10] | body
//     0x03 STREAM_END  stream u8 | how u8 | records u16 LE | bookmark[10]
//     0x04 END         result u8 | more u8 | status u8
//
// A RECORD is written only once its body is whole and its bookmark is in
// hand, so every RECORD in a file is complete: a reader stores the record and
// then advances that stream's bookmark. A file cut short (the stream broke)
// still holds every RECORD before the cut; the cut item is dropped.
//
#include <cpapdash/ds2ble/Record.h>
#include <cpapdash/ds2ble/SyncSession.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hms_cpap::mule::ds2r {

inline constexpr uint8_t kItemAnswer    = 0x01;
inline constexpr uint8_t kItemRecord    = 0x02;
inline constexpr uint8_t kItemStreamEnd = 0x03;
inline constexpr uint8_t kItemEnd       = 0x04;

/// STREAM_END's `how`.
inline constexpr uint8_t kHowNone    = 0x00;   // the open answered all zero
inline constexpr uint8_t kHowNoNext  = 0x06;   // the open answered status 06
inline constexpr uint8_t kHowStopped = 0xFF;   // the cap, the time budget, or a failure

/// One answer to an identity request, as the machine sent it.
struct Answer {
    uint16_t cmd{0};
    uint8_t  sub{0};
    uint8_t  status{0};
    std::vector<uint8_t> data;   // after the status byte
};

struct StreamEnd {
    uint8_t  stream{0};
    uint8_t  how{0};
    uint16_t records{0};
    cpapdash::ds2ble::Bookmark bookmark;   // ten zero bytes: none yet
};

struct End {
    uint8_t result{0};   // push-c3 SDD-043 §4's number; 0 = ok
    bool    more{false}; // the cap or the time budget ended the walk with records left
    uint8_t status{0};   // the machine's status byte when one ended it, else 0
};

struct File {
    std::vector<Answer> answers;
    /// In the order read. bookmark_in is left empty: the file does not carry it.
    std::vector<cpapdash::ds2ble::Record> records;
    std::vector<StreamEnd> stream_ends;
    std::optional<End> end;
    /// The bytes stopped inside an item (that item is dropped), or no END came.
    bool cut{false};
};

/// Reads a .ds2r. nullopt only when it is not one (wrong magic or version);
/// `why` says what. Items of an unknown kind are skipped by their length.
std::optional<File> parse(const std::string& bytes, std::string* why = nullptr);

/// What the identity answers say: serial, firmware, model, newest session
/// and the supported commands, as the library's own read decodes them.
/// A field whose answer is missing or failed stays empty.
cpapdash::ds2ble::Identity identity(const File& f);

/// SDD-043 §4's token for a result number ("" for 0); a protocol failure
/// with a machine status byte is "status_<hex>".
std::string resultToken(uint8_t result, uint8_t status = 0);

/// Ten zero bytes, or empty: no bookmark.
bool noBookmark(const cpapdash::ds2ble::Bookmark& b);

/// "0a1b..." (20 lower-case hex digits) and back; fromHex is nullopt for
/// anything but 20 hex digits.
std::string toHex(const cpapdash::ds2ble::Bookmark& b);
std::optional<cpapdash::ds2ble::Bookmark> fromHex(const std::string& s);

}  // namespace hms_cpap::mule::ds2r
