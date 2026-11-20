#pragma once
//
// SDD-081: a DreamStation 2 card is read by an OSCAR helper on a home PC.
//
// The helper connects out to hms-cpap on the Sleep Bridge's port:
//
//   ws://<hms-cpap>:8894/oscar/v1/helper
//
//   upgrade      X-Hms-Helper: <PC name>, and Authorization: Bearer <token>
//                once it holds one (SDD-057's shape). X-Hms-Probe: 1 marks a
//                probe (D3): answered, never a session.
//   helper       {"t":"hello","proto":1,"version","name","files":[{path,size,mtime}]}
//   hms-cpap     {"t":"paired","token"} (the first helper, D4), then
//                {"t":"ack","hms_cpap":{"name","install_id","version","helper"}}
//                or {"t":"error","error":"proto"|"has_helper","helper"} and a close
//   files (D5.1) binary kind 3 chunks, then {"t":"file_ready","req_id","path",
//                "chunks","size","md5","mtime"}; the helper answers
//                {"t":"file_ack","req_id","ok"}
//   import       {"t":"cmd","cmd":"import","req_id"} -> {"t":"result","req_id",
//                "outcome","version","schema","message","heads":[{key,start_ms,end_ms}]}
//   want         {"t":"cmd","cmd":"want","req_id","keys":[...]} -> kind 3 chunks,
//                then {"t":"slice_ready","req_id","chunks","size","md5"};
//                hms-cpap answers file_ack
//
// The binary frames are the CpapDash stream's (platform SDD-059): a 12-byte
// little-endian header (req_id, seq, flags bit 0 = last, kind) then the bytes.
// The card files cross exactly as the machine wrote them, encrypted: nothing
// here reads, decodes or decrypts them.
//
#include <json/json.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hms_cpap::oscar::helper {

inline constexpr const char* kRoute        = "/oscar/v1/helper";
inline constexpr int         kProto        = 1;
inline constexpr const char* kHeaderName   = "X-Hms-Helper";
inline constexpr const char* kHeaderProbe  = "X-Hms-Probe";
inline constexpr int         kDefaultPort  = 8894;
inline constexpr uint16_t    kKindBytes    = 3;          // platform SDD-059's file bytes
inline constexpr std::size_t kChunkBytes   = 32 * 1024;
inline constexpr const char* kMdnsService  = "_hms-cpap._tcp.local";
inline constexpr const char* kMdnsHa       = "_home-assistant._tcp.local";

/// One file of a card copy, as the hello lists it. mtime: whole seconds since
/// the epoch (a FAT card keeps two-second times; nothing finer is compared).
struct FileStamp {
    std::string path;    ///< card-relative, '/' separated, under P-SERIES/
    uint64_t    size{0};
    long long   mtime{0};
    bool operator==(const FileStamp& o) const { return path == o.path && size == o.size && mtime == o.mtime; }
};

/// D5.1: a path the helper takes: "P-SERIES/..." relative, '/' separated, no
/// "..", ".", empty or absolute segment, no backslash, no drive, no NUL.
bool safeCardPath(const std::string& rel);

/// Every regular file under <root>/P-SERIES/ (links are not followed and not
/// listed), sorted by path.
std::vector<FileStamp> listCard(const std::string& root);

/// The files of <root>/P-SERIES/ whose size or mtime differs from `held` (or
/// that `held` lacks): mtime AND size, never size alone.
std::vector<FileStamp> changedFiles(const std::string& root, const std::vector<FileStamp>& held);

Json::Value toJson(const std::vector<FileStamp>& files);
/// Only entries with a safe path are kept.
std::vector<FileStamp> filesFromJson(const Json::Value& v);

/// A file's mtime in whole seconds since the epoch; 0 when it cannot be read.
long long mtimeSeconds(const std::string& path);
/// Sets it. False when the OS refused.
bool setMtimeSeconds(const std::string& path, long long t);

/// 32 random bytes, hex (D4's token).
std::string newToken();
std::string sha256Hex(const std::string& bytes);
std::string md5Hex(const std::string& bytes);

/// One text frame, compact.
std::string frame(const Json::Value& v);
/// nullopt unless an object with a string "t".
std::optional<Json::Value> parse(const std::string& text);

/// A binary frame's header, and its body after the header.
struct Chunk {
    uint32_t    req_id{0};
    uint32_t    seq{0};
    bool        last{false};
    uint16_t    kind{0};
    std::string body;
};
std::optional<Chunk> parseChunk(const std::string& bytes);
/// `bytes` as kind 3 frames under `req_id`.
std::vector<std::string> chunks(uint32_t req_id, const std::string& bytes, std::size_t chunk_bytes = kChunkBytes);

/// The bytes of one req_id, gathered from its chunks in order.
class Assembly {
public:
    /// False when a chunk came out of order (the transfer is then broken).
    bool add(const Chunk& c);
    std::string take(uint32_t req_id);
    uint32_t count(uint32_t req_id) const;
    void clear() { parts_.clear(); }

private:
    struct Part {
        uint32_t    next{0};
        std::string bytes;
    };
    std::map<uint32_t, Part> parts_;
};

/// A session head as it crosses: OscarDb::sessions reduced.
struct Head {
    std::string key;       ///< "<serial>/<session>"
    long long   start_ms{0};
    long long   end_ms{0};
};
Json::Value headsJson(const std::vector<Head>& h);
std::vector<Head> headsFromJson(const Json::Value& v);

/// This computer's name (the PC name a helper gives, the hms-cpap's own).
std::string hostName();

}  // namespace hms_cpap::oscar::helper
