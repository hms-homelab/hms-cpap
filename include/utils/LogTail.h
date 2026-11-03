#pragma once
//
// SDD-059: the log file read by byte cursor, so the log page follows the end
// and loads the past on demand instead of refetching the whole tail.
//
// Every answer is complete lines only, with the byte offsets around them:
// `start` before the first line sent, `end` after the last. The page asks for
// what is new with after(end) and for the page before what it shows with
// before(start); neither reads the file from its beginning. A line still being
// written (no newline yet) is never sent, so it arrives whole next time.
//
// The file rotates. Its identity (`file_id`) rides every answer; after() with
// another file's identity, or with an offset past the file's end, answers the
// tail instead and says `rotated`.
//
#include <cstdint>
#include <string>
#include <vector>

namespace hms_cpap::logtail {

/// At most this many lines in one after() answer; `more` says ask again.
inline constexpr std::size_t kAfterCap = 2000;

struct Chunk {
    std::vector<std::string> lines;
    std::uint64_t start = 0;     ///< byte offset of the first line sent
    std::uint64_t end   = 0;     ///< byte offset just after the last complete line sent
    std::uint64_t size  = 0;     ///< the file's size when it was read
    std::string   file_id;       ///< the file's identity, "" when it cannot be read
    bool readable  = false;      ///< false: nothing could be read (no file, no access)
    bool more      = false;      ///< after(): complete lines remain past `end`
    bool rotated   = false;      ///< after(): the cursor was another file's, so this is the tail
};

/// The file's identity: device and inode where the platform has them, else
/// its creation or write time. "" when it cannot be read.
std::string fileId(const std::string& path);

/// The last `n` complete lines.
Chunk tail(const std::string& path, std::size_t n);

/// The complete lines written after byte `offset`, at most kAfterCap of them.
/// `id` is the file_id the offset was taken from; another file, or an offset
/// past the end, answers tail(path, rotated_n) with `rotated` set.
Chunk after(const std::string& path, std::uint64_t offset, const std::string& id,
            std::size_t rotated_n);

/// The `n` complete lines that end at byte `offset` (an offset some answer
/// gave as `start`). `start` 0 means nothing older is left in this file.
Chunk before(const std::string& path, std::uint64_t offset, std::size_t n);

} // namespace hms_cpap::logtail
