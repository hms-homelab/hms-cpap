#include "utils/LogTail.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <fstream>

namespace hms_cpap::logtail {

namespace {

constexpr std::size_t kBlock = 64 * 1024;

std::uint64_t sizeOf(std::ifstream& in) {
    in.clear();
    in.seekg(0, std::ios::end);
    const auto pos = in.tellg();
    return pos < 0 ? 0 : static_cast<std::uint64_t>(pos);
}

/// The offset just after the k-th '\n' found scanning back from `from`
/// (positions before it), or 0 when fewer than k are there.
std::uint64_t findBack(std::ifstream& in, std::uint64_t from, std::size_t k) {
    std::string buf;
    std::size_t found = 0;
    std::uint64_t hi = from;
    while (hi > 0) {
        const std::uint64_t lo = hi > kBlock ? hi - kBlock : 0;
        buf.resize(static_cast<std::size_t>(hi - lo));
        in.clear();
        in.seekg(static_cast<std::streamoff>(lo));
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        for (std::size_t i = buf.size(); i-- > 0;) {
            if (buf[i] == '\n' && ++found == k) return lo + i + 1;
        }
        hi = lo;
    }
    return 0;
}

/// The lines in [a, b), which holds complete lines only.
std::vector<std::string> readLines(std::ifstream& in, std::uint64_t a, std::uint64_t b) {
    std::vector<std::string> out;
    if (b <= a) return out;
    std::string buf(static_cast<std::size_t>(b - a), '\0');
    in.clear();
    in.seekg(static_cast<std::streamoff>(a));
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    std::size_t s = 0;
    for (std::size_t i = 0; i < buf.size(); ++i) {
        if (buf[i] != '\n') continue;
        std::size_t e = i;
        if (e > s && buf[e - 1] == '\r') --e;
        out.emplace_back(buf, s, e - s);
        s = i + 1;
    }
    return out;
}

} // namespace

std::string fileId(const std::string& path) {
#ifdef _WIN32
    // No inode: the creation time tells a new file from the one it replaced.
    struct _stat64 st;
    if (_stat64(path.c_str(), &st) != 0) return "";
    return "c" + std::to_string(static_cast<long long>(st.st_ctime));
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return "";
    return std::to_string(static_cast<unsigned long long>(st.st_dev)) + ":" +
           std::to_string(static_cast<unsigned long long>(st.st_ino));
#endif
}

Chunk tail(const std::string& path, std::size_t n) {
    Chunk c;
    std::ifstream in(path, std::ios::binary);
    if (!in) return c;
    c.readable = true;
    c.file_id = fileId(path);
    c.size = sizeOf(in);
    c.end = findBack(in, c.size, 1);               // after the last complete line
    if (n == 0 || c.end == 0) { c.start = c.end; return c; }
    c.start = findBack(in, c.end, n + 1);          // the newline before the n-th line from the end
    c.lines = readLines(in, c.start, c.end);
    return c;
}

Chunk after(const std::string& path, std::uint64_t offset, const std::string& id,
            std::size_t rotated_n) {
    const std::string now = fileId(path);
    {
        std::ifstream probe(path, std::ios::binary);
        if (!probe) return Chunk{};
        if (now != id || offset > sizeOf(probe)) {
            Chunk c = tail(path, rotated_n);
            c.rotated = true;
            return c;
        }
    }

    Chunk c;
    std::ifstream in(path, std::ios::binary);
    if (!in) return c;
    c.readable = true;
    c.file_id = now;
    c.size = sizeOf(in);
    c.start = offset;
    c.end = offset;

    // Forward from the cursor, a block at a time, keeping only complete lines
    // and stopping at the cap.
    std::string buf, carry;
    std::uint64_t pos = offset;
    while (pos < c.size && c.lines.size() < kAfterCap) {
        const std::size_t len = static_cast<std::size_t>(std::min<std::uint64_t>(kBlock, c.size - pos));
        buf.resize(len);
        in.clear();
        in.seekg(static_cast<std::streamoff>(pos));
        in.read(buf.data(), static_cast<std::streamsize>(len));
        std::size_t s = 0;
        for (std::size_t i = 0; i < len && c.lines.size() < kAfterCap; ++i) {
            if (buf[i] != '\n') continue;
            std::string line = carry + buf.substr(s, i - s);
            carry.clear();
            if (!line.empty() && line.back() == '\r') line.pop_back();
            c.lines.push_back(std::move(line));
            s = i + 1;
            c.end = pos + i + 1;
        }
        if (c.lines.size() >= kAfterCap) break;
        carry += buf.substr(s);
        pos += len;
    }
    // More when a complete line is left past `end`.
    c.more = c.lines.size() >= kAfterCap && findBack(in, c.size, 1) > c.end;
    return c;
}

Chunk before(const std::string& path, std::uint64_t offset, std::size_t n) {
    Chunk c;
    std::ifstream in(path, std::ios::binary);
    if (!in) return c;
    c.readable = true;
    c.file_id = fileId(path);
    c.size = sizeOf(in);
    c.end = std::min(offset, findBack(in, c.size, 1));
    if (n == 0 || c.end == 0) { c.start = c.end; return c; }
    c.start = findBack(in, c.end, n + 1);
    c.lines = readLines(in, c.start, c.end);
    return c;
}

} // namespace hms_cpap::logtail
