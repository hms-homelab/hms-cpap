#pragma once
//
// The Sleep Bridge local stream, as bytes (docs/SLEEP_BRIDGE_LOCAL_STREAM.md).
//
// A Sleep Bridge sits on the machine's SD bus and sends the sector writes it
// sees, in batches, over one WebSocket. This file is every rule about what
// those bytes mean, with no network, no disk and no Drogon in it, so the unit
// suite sees each one:
//
//   * the 12-byte header that starts every binary frame;
//   * a batch: CYS1 (writes), CYS2 (writes with one-byte fills folded),
//     CYR1 (sectors the machine READ, around a mount);
//   * the two count = 0 shapes: a heartbeat, and the card statement;
//   * the cursor: what a batch means against what is already held.
//
// All integers on the wire are little-endian.
//
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hms_cpap::sleepbridge {

// ── Binary frame header ─────────────────────────────────────────────────────

inline constexpr size_t   kFrameHeaderSize = 12;
inline constexpr uint16_t kKindLogText     = 4;   // up: a command's log text
inline constexpr uint16_t kKindRingFile    = 5;   // not taken in local mode
inline constexpr uint16_t kKindSectorBatch = 7;   // up: one batch
inline constexpr uint16_t kKindFirmware    = 8;   // down: a firmware image chunk (an update)
inline constexpr size_t   kFirmwareChunk   = 4096;   // image bytes per kind 8 frame

struct FrameHeader {
    uint32_t req_id{0};
    uint32_t seq{0};     // counts batches on one connection; echoed in the ack
    uint16_t flags{0};   // bit 0: last frame of a multi-frame item
    uint16_t kind{0};
};

/// nullopt when the frame is shorter than the header.
std::optional<FrameHeader> parseFrameHeader(const std::string& frame);
std::string encodeFrameHeader(const FrameHeader& h);

// ── Batches ─────────────────────────────────────────────────────────────────

// Fixed batch header: magic(4) count(2) first_sequence(8) geometry_epoch(4),
// then `count` records of lba(4) + 512 bytes.
inline constexpr size_t   kBatchHeaderSize = 18;
inline constexpr size_t   kSectorSize      = 512;
inline constexpr size_t   kRecordSize      = 4 + kSectorSize;
inline constexpr uint16_t kMaxRecords      = 512;
inline constexpr size_t   kMaxBatchSize    = kBatchHeaderSize + kMaxRecords * kRecordSize;

inline constexpr char kMagicWrites[4] = {'C', 'Y', 'S', '1'};
inline constexpr char kMagicReads[4]  = {'C', 'Y', 'R', '1'};
inline constexpr char kMagicFill[4]   = {'C', 'Y', 'S', '2'};
inline constexpr uint8_t kFillKindSector = 0;   // lba(4) kind(1) then 512 bytes
inline constexpr uint8_t kFillKindFill   = 1;   // lba(4) kind(1) then one byte, x512

struct Record {
    uint32_t       lba{0};
    const uint8_t* data{nullptr};   // kSectorSize bytes, inside the parsed body
};

/// A parsed batch. `records[i].data` points into the body it was parsed from,
/// so a Batch must not outlive that body.
struct Batch {
    uint64_t            first_sequence{0};
    uint32_t            geometry_epoch{0};
    bool                reads{false};   // CYR1
    std::vector<Record> records;
    std::string         card;           // the card statement's JSON, count = 0 only

    uint64_t lastSequence() const {
        return records.empty() ? first_sequence : first_sequence + records.size() - 1;
    }
    bool isHeartbeat() const { return records.empty() && card.empty(); }
    bool isCard() const { return records.empty() && !card.empty(); }
};

enum class BatchError {
    None,
    TooShort,
    BadMagic,
    TooManyRecords,
    LengthMismatch,
    SequenceTooLarge,
    BadKind,
};

const char* batchErrorText(BatchError e);

/// A CYS1 or CYR1 body. A CYS2 body is refused here: expand it first.
BatchError parseBatch(const uint8_t* body, size_t len, Batch& out);

/// The body starts with the CYS2 magic.
bool isFillBatch(const uint8_t* body, size_t len);

/// A CYS2 body as the CYS1 it stands for. Every record is walked and the
/// length proven before a byte is copied; `out` is untouched unless None.
BatchError expandFillBatch(const uint8_t* body, size_t len, std::string& out);

// ── Card statement ──────────────────────────────────────────────────────────

/// The card statement a bridge sends once per mount (count = 0, a JSON object
/// after the header). It names the card (cid, and csd or its layout) and,
/// from a bridge that counts boots, the boot it was seen in and the sequence
/// its next record takes.
struct CardStatement {
    std::string             cid;   // 32 lowercase hex digits
    std::string             csd;   // 32 lowercase hex digits, or empty
    uint32_t                sectors_per_cluster{0};   // 0: no layout given
    uint32_t                data_begin_lba{0};
    std::optional<uint32_t> mount;
    std::optional<uint64_t> seq;
};

/// nullopt with `why` set when the statement says nothing usable: cid is
/// required, and either csd or the layout (sectors_per_cluster a power of two
/// up to 128, data_begin_lba).
std::optional<CardStatement> parseCardStatement(const std::string& json, std::string* why);

// ── Names ───────────────────────────────────────────────────────────────────

/// X-Coyote-Sender: 1 to 64 of [A-Za-z0-9._-], not starting with '.'. It
/// becomes a directory name, so this shape is what keeps it inside the data
/// folder.
bool validSender(const std::string& s);
/// X-Coyote-Machine: empty, or the same shape.
bool validMachine(const std::string& s);

// ── Cursor ──────────────────────────────────────────────────────────────────
//
// Per bridge, the receiver holds a high-water mark: the last sequence stored.
// The ack is that mark, and the bridge drops what is at or below it and
// resends the rest. So:
//   * a batch wholly at or below the mark is a re-send: re-acked, not stored;
//   * a batch starting past mark + 1 is a gap: stored, and the hole recorded;
//   * a batch overlapping the mark is stored; its new part is what is counted;
//   * a bridge's first batch is never a gap, however high it starts.
// The mark never moves backwards.

struct CursorState {
    bool    exists{false};
    int64_t high_water{0};
};

struct CursorDecision {
    bool    duplicate{false};
    bool    gap{false};
    int64_t gap_first{0};
    int64_t gap_last{0};
    int64_t accepted{0};
    int64_t new_high_water{0};
};

CursorDecision decideCursor(const CursorState& s, int64_t seq_first, int64_t seq_last);

}  // namespace hms_cpap::sleepbridge
