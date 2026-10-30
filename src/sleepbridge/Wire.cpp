#include "sleepbridge/Wire.h"

#include <json/json.h>

#include <cstring>
#include <limits>
#include <sstream>

namespace hms_cpap::sleepbridge {

namespace {

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t rd64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

void wr16(std::string& s, uint16_t v) {
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>(v >> 8));
}

void wr32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; i++) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

bool hasMagic(const uint8_t* body, size_t len, const char (&magic)[4]) {
    return body && len >= 4 && std::memcmp(body, magic, 4) == 0;
}

bool isHex32(const std::string& s) {
    if (s.size() != 32) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

std::string lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::optional<uint32_t> u32Of(const Json::Value& v) {
    if (!v.isIntegral()) return std::nullopt;
    const auto n = v.asLargestInt();
    if (n < 0 || n > static_cast<Json::LargestInt>(std::numeric_limits<uint32_t>::max()))
        return std::nullopt;
    return static_cast<uint32_t>(n);
}

}  // namespace

std::optional<FrameHeader> parseFrameHeader(const std::string& frame) {
    if (frame.size() < kFrameHeaderSize) return std::nullopt;
    const auto* p = reinterpret_cast<const uint8_t*>(frame.data());
    FrameHeader h;
    h.req_id = rd32(p);
    h.seq    = rd32(p + 4);
    h.flags  = rd16(p + 8);
    h.kind   = rd16(p + 10);
    return h;
}

std::string encodeFrameHeader(const FrameHeader& h) {
    std::string s;
    s.reserve(kFrameHeaderSize);
    wr32(s, h.req_id);
    wr32(s, h.seq);
    wr16(s, h.flags);
    wr16(s, h.kind);
    return s;
}

const char* batchErrorText(BatchError e) {
    switch (e) {
        case BatchError::None:             return "ok";
        case BatchError::TooShort:         return "body shorter than the batch header";
        case BatchError::BadMagic:         return "bad magic, not a CYS1, CYS2 or CYR1 batch";
        case BatchError::TooManyRecords:   return "record count above the cap";
        case BatchError::LengthMismatch:   return "record count disagrees with body length";
        case BatchError::SequenceTooLarge: return "sequence beyond 2^63-1";
        case BatchError::BadKind:          return "CYS2 record of an unknown kind";
    }
    return "unknown";
}

bool isFillBatch(const uint8_t* body, size_t len) { return hasMagic(body, len, kMagicFill); }

BatchError expandFillBatch(const uint8_t* body, size_t len, std::string& out) {
    if (!body || len < kBatchHeaderSize) return BatchError::TooShort;
    if (!isFillBatch(body, len)) return BatchError::BadMagic;
    const uint16_t count = rd16(body + 4);
    if (count > kMaxRecords) return BatchError::TooManyRecords;

    size_t at = kBatchHeaderSize;
    for (uint16_t i = 0; i < count; i++) {
        if (len < at + 5) return BatchError::LengthMismatch;
        const uint8_t kind = body[at + 4];
        if (kind == kFillKindSector)    at += 5 + kSectorSize;
        else if (kind == kFillKindFill) at += 5 + 1;
        else                            return BatchError::BadKind;
        if (at > len) return BatchError::LengthMismatch;
    }
    if (at != len) return BatchError::LengthMismatch;

    std::string x;
    x.reserve(kBatchHeaderSize + static_cast<size_t>(count) * kRecordSize);
    x.append(kMagicWrites, 4);
    x.append(reinterpret_cast<const char*>(body + 4), kBatchHeaderSize - 4);
    at = kBatchHeaderSize;
    for (uint16_t i = 0; i < count; i++) {
        x.append(reinterpret_cast<const char*>(body + at), 4);
        if (body[at + 4] == kFillKindSector) {
            x.append(reinterpret_cast<const char*>(body + at + 5), kSectorSize);
            at += 5 + kSectorSize;
        } else {
            x.append(kSectorSize, static_cast<char>(body[at + 5]));
            at += 6;
        }
    }
    out.swap(x);
    return BatchError::None;
}

BatchError parseBatch(const uint8_t* body, size_t len, Batch& out) {
    if (!body || len < kBatchHeaderSize) return BatchError::TooShort;
    const bool reads = hasMagic(body, len, kMagicReads);
    if (!reads && !hasMagic(body, len, kMagicWrites)) return BatchError::BadMagic;

    const uint16_t count = rd16(body + 4);
    if (count > kMaxRecords) return BatchError::TooManyRecords;

    // count = 0 followed by a JSON object is the card statement. Only a body
    // whose next byte is '{' reads that way, so a truncated batch cannot pass
    // for one. A CYR1 carries none.
    const bool card = !reads && count == 0 && len > kBatchHeaderSize && body[kBatchHeaderSize] == '{';
    if (!card && len != kBatchHeaderSize + static_cast<size_t>(count) * kRecordSize)
        return BatchError::LengthMismatch;

    const uint64_t first = rd64(body + 6);
    out.reads          = reads;
    out.first_sequence = first;
    out.geometry_epoch = rd32(body + 14);
    out.records.clear();
    out.card.clear();

    if (count == 0) {
        if (card)
            out.card.assign(reinterpret_cast<const char*>(body + kBatchHeaderSize),
                            len - kBatchHeaderSize);
        return BatchError::None;
    }

    constexpr uint64_t kCeiling = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if (first > kCeiling - (count - 1u)) return BatchError::SequenceTooLarge;

    out.records.reserve(count);
    const uint8_t* p = body + kBatchHeaderSize;
    for (uint16_t i = 0; i < count; i++) {
        out.records.push_back(Record{rd32(p), p + 4});
        p += kRecordSize;
    }
    return BatchError::None;
}

std::optional<CardStatement> parseCardStatement(const std::string& json, std::string* why) {
    auto fail = [&](const std::string& w) -> std::optional<CardStatement> {
        if (why) *why = w;
        return std::nullopt;
    };
    Json::Value v;
    Json::CharReaderBuilder rb;
    std::istringstream ss(json);
    if (!Json::parseFromStream(rb, ss, &v, nullptr) || !v.isObject())
        return fail("not a JSON object");

    CardStatement c;
    if (!v["cid"].isString() || !isHex32(lower(v["cid"].asString())))
        return fail("cid must be 32 hex digits");
    c.cid = lower(v["cid"].asString());

    if (v.isMember("csd")) {
        if (!v["csd"].isString() || !isHex32(lower(v["csd"].asString())))
            return fail("csd must be 32 hex digits");
        c.csd = lower(v["csd"].asString());
    }
    if (v.isMember("sectors_per_cluster")) {
        const auto spc = u32Of(v["sectors_per_cluster"]);
        if (!spc || *spc == 0 || *spc > 128 || (*spc & (*spc - 1)) != 0)
            return fail("sectors_per_cluster must be a power of two up to 128");
        const auto dbl = u32Of(v["data_begin_lba"]);
        if (!dbl) return fail("a layout needs data_begin_lba");
        c.sectors_per_cluster = *spc;
        c.data_begin_lba      = *dbl;
    }
    if (c.csd.empty() && c.sectors_per_cluster == 0)
        return fail("neither a csd nor a layout");
    if (v.isMember("mount")) {
        const auto m = u32Of(v["mount"]);
        if (!m) return fail("mount must be a u32");
        c.mount = m;
    }
    if (v.isMember("seq")) {
        if (!v["seq"].isUInt64()) return fail("seq must be a non-negative integer");
        c.seq = static_cast<uint64_t>(v["seq"].asUInt64());
    }
    return c;
}

bool validSender(const std::string& s) {
    if (s.empty() || s.size() > 64 || s.front() == '.') return false;
    for (const char c : s) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool validMachine(const std::string& s) { return s.empty() || validSender(s); }

CursorDecision decideCursor(const CursorState& s, int64_t seq_first, int64_t seq_last) {
    CursorDecision d;
    if (!s.exists) {
        d.accepted       = seq_last - seq_first + 1;
        d.new_high_water = seq_last;
        return d;
    }
    if (seq_last <= s.high_water) {
        d.duplicate      = true;
        d.new_high_water = s.high_water;
        return d;
    }
    if (seq_first > s.high_water + 1) {
        d.gap       = true;
        d.gap_first = s.high_water + 1;
        d.gap_last  = seq_first - 1;
    }
    const int64_t from = seq_first > s.high_water ? seq_first : s.high_water + 1;
    d.accepted       = seq_last - from + 1;
    d.new_high_water = seq_last;
    return d;
}

}  // namespace hms_cpap::sleepbridge
