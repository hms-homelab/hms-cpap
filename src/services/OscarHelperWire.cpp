#include "services/OscarHelperWire.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>

#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <sys/utime.h>
#else
#include <unistd.h>
#include <utime.h>
#endif

namespace hms_cpap::oscar::helper {

namespace fs = std::filesystem;

namespace {

std::string hexOf(const unsigned char* d, unsigned int n) {
    std::ostringstream o;
    for (unsigned int i = 0; i < n; ++i) o << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(d[i]);
    return o.str();
}

std::string digest(const EVP_MD* md, const std::string& bytes) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int  len = 0;
    EVP_MD_CTX*   ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, md, nullptr);
    EVP_DigestUpdate(ctx, bytes.data(), bytes.size());
    EVP_DigestFinal_ex(ctx, out, &len);
    EVP_MD_CTX_free(ctx);
    return hexOf(out, len);
}

uint32_t le32(const std::string& b, std::size_t at) {
    return static_cast<uint32_t>(static_cast<unsigned char>(b[at])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(b[at + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(b[at + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(b[at + 3])) << 24);
}

uint16_t le16(const std::string& b, std::size_t at) {
    return static_cast<uint16_t>(static_cast<unsigned char>(b[at]) |
                                 (static_cast<unsigned char>(b[at + 1]) << 8));
}

void put32(std::string& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

void put16(std::string& b, uint16_t v) {
    b.push_back(static_cast<char>(v & 0xff));
    b.push_back(static_cast<char>((v >> 8) & 0xff));
}

}  // namespace

bool safeCardPath(const std::string& rel) {
    if (rel.empty() || rel.size() > 1024) return false;
    if (rel.find('\\') != std::string::npos || rel.find(':') != std::string::npos ||
        rel.find('\0') != std::string::npos)
        return false;
    if (rel.front() == '/') return false;
    std::vector<std::string> parts;
    std::size_t from = 0;
    for (;;) {
        const auto slash = rel.find('/', from);
        parts.push_back(rel.substr(from, slash == std::string::npos ? std::string::npos : slash - from));
        if (slash == std::string::npos) break;
        from = slash + 1;
    }
    if (parts.size() < 2) return false;
    std::string top = parts[0];
    std::transform(top.begin(), top.end(), top.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (top != "P-SERIES") return false;
    for (const auto& p : parts)
        if (p.empty() || p == "." || p == "..") return false;
    return true;
}

long long mtimeSeconds(const std::string& path) {
#ifdef _WIN32
    struct _stat64 st;
    if (_wstat64(fs::path(path).wstring().c_str(), &st) != 0) return 0;
    return static_cast<long long>(st.st_mtime);
#else
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return 0;
    return static_cast<long long>(st.st_mtime);
#endif
}

bool setMtimeSeconds(const std::string& path, long long t) {
#ifdef _WIN32
    struct __utimbuf64 u;
    u.actime = t;
    u.modtime = t;
    return _wutime64(fs::path(path).wstring().c_str(), &u) == 0;
#else
    struct utimbuf u;
    u.actime = static_cast<time_t>(t);
    u.modtime = static_cast<time_t>(t);
    return ::utime(path.c_str(), &u) == 0;
#endif
}

std::vector<FileStamp> listCard(const std::string& root) {
    std::vector<FileStamp> out;
    if (root.empty()) return out;
    std::error_code ec;
    const fs::path base(root);
    for (fs::recursive_directory_iterator it(base / "P-SERIES", fs::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_symlink(ec)) continue;
        if (!it->is_regular_file(ec)) continue;
        const auto rel = it->path().lexically_relative(base).generic_string();
        if (!safeCardPath(rel)) continue;
        FileStamp f;
        f.path = rel;
        f.size = it->file_size(ec);
        if (ec) {
            ec.clear();
            continue;
        }
        f.mtime = mtimeSeconds(it->path().string());
        out.push_back(std::move(f));
    }
    std::sort(out.begin(), out.end(), [](const FileStamp& a, const FileStamp& b) { return a.path < b.path; });
    return out;
}

std::vector<FileStamp> changedFiles(const std::string& root, const std::vector<FileStamp>& held) {
    std::map<std::string, const FileStamp*> by;
    for (const auto& h : held) by[h.path] = &h;
    std::vector<FileStamp> out;
    for (auto& f : listCard(root)) {
        const auto it = by.find(f.path);
        if (it != by.end() && it->second->size == f.size && it->second->mtime == f.mtime) continue;
        out.push_back(std::move(f));
    }
    return out;
}

Json::Value toJson(const std::vector<FileStamp>& files) {
    Json::Value a(Json::arrayValue);
    for (const auto& f : files) {
        Json::Value v(Json::objectValue);
        v["path"]  = f.path;
        v["size"]  = static_cast<Json::UInt64>(f.size);
        v["mtime"] = static_cast<Json::Int64>(f.mtime);
        a.append(v);
    }
    return a;
}

std::vector<FileStamp> filesFromJson(const Json::Value& v) {
    std::vector<FileStamp> out;
    if (!v.isArray()) return out;
    for (const auto& e : v) {
        if (!e.isObject() || !e["path"].isString()) continue;
        FileStamp f;
        f.path = e["path"].asString();
        if (!safeCardPath(f.path)) continue;
        f.size  = e["size"].isNumeric() ? e["size"].asUInt64() : 0;
        f.mtime = e["mtime"].isNumeric() ? e["mtime"].asInt64() : 0;
        out.push_back(std::move(f));
    }
    return out;
}

std::string newToken() {
    unsigned char b[32];
    if (RAND_bytes(b, sizeof(b)) != 1) return "";
    return hexOf(b, sizeof(b));
}

std::string sha256Hex(const std::string& bytes) { return digest(EVP_sha256(), bytes); }
std::string md5Hex(const std::string& bytes) { return digest(EVP_md5(), bytes); }

std::string frame(const Json::Value& v) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, v);
}

std::optional<Json::Value> parse(const std::string& text) {
    Json::CharReaderBuilder b;
    Json::Value v;
    std::string err;
    std::istringstream in(text);
    if (!Json::parseFromStream(b, in, &v, &err)) return std::nullopt;
    if (!v.isObject() || !v["t"].isString()) return std::nullopt;
    return v;
}

std::optional<Chunk> parseChunk(const std::string& bytes) {
    if (bytes.size() < 12) return std::nullopt;
    Chunk c;
    c.req_id = le32(bytes, 0);
    c.seq    = le32(bytes, 4);
    c.last   = (le16(bytes, 8) & 1) != 0;
    c.kind   = le16(bytes, 10);
    c.body   = bytes.substr(12);
    return c;
}

std::vector<std::string> chunks(uint32_t req_id, const std::string& bytes, std::size_t chunk_bytes) {
    std::vector<std::string> out;
    const std::size_t step = std::max<std::size_t>(chunk_bytes, 1);
    const std::size_t n = std::max<std::size_t>((bytes.size() + step - 1) / step, 1);   // an empty file: one frame
    for (std::size_t i = 0; i < n; ++i) {
        std::string f;
        put32(f, req_id);
        put32(f, static_cast<uint32_t>(i));
        put16(f, i + 1 == n ? 1 : 0);
        put16(f, kKindBytes);
        const std::size_t at = i * step;
        if (at < bytes.size()) f.append(bytes, at, std::min(step, bytes.size() - at));
        out.push_back(std::move(f));
    }
    return out;
}

bool Assembly::add(const Chunk& c) {
    auto& p = parts_[c.req_id];
    if (c.seq != p.next) {
        parts_.erase(c.req_id);
        return false;
    }
    p.bytes += c.body;
    ++p.next;
    return true;
}

std::string Assembly::take(uint32_t req_id) {
    const auto it = parts_.find(req_id);
    if (it == parts_.end()) return "";
    std::string b = std::move(it->second.bytes);
    parts_.erase(it);
    return b;
}

uint32_t Assembly::count(uint32_t req_id) const {
    const auto it = parts_.find(req_id);
    return it == parts_.end() ? 0 : it->second.next;
}

Json::Value headsJson(const std::vector<Head>& h) {
    Json::Value a(Json::arrayValue);
    for (const auto& x : h) {
        Json::Value v(Json::objectValue);
        v["key"]      = x.key;
        v["start_ms"] = static_cast<Json::Int64>(x.start_ms);
        v["end_ms"]   = static_cast<Json::Int64>(x.end_ms);
        a.append(v);
    }
    return a;
}

std::vector<Head> headsFromJson(const Json::Value& v) {
    std::vector<Head> out;
    if (!v.isArray()) return out;
    for (const auto& e : v) {
        if (!e.isObject() || !e["key"].isString()) continue;
        Head h;
        h.key      = e["key"].asString();
        h.start_ms = e["start_ms"].isNumeric() ? e["start_ms"].asInt64() : 0;
        h.end_ms   = e["end_ms"].isNumeric() ? e["end_ms"].asInt64() : 0;
        out.push_back(std::move(h));
    }
    return out;
}

std::string hostName() {
#ifdef _WIN32
    if (const char* n = std::getenv("COMPUTERNAME"); n && *n) return n;
    return "PC";
#else
    char buf[256] = {0};
    if (::gethostname(buf, sizeof(buf) - 1) != 0 || !buf[0]) return "PC";
    std::string n(buf);
    // "home-pc.local" -> "home-pc": the name a person gave the PC.
    if (const auto dot = n.find('.'); dot != std::string::npos && dot > 0) n.resize(dot);
    return n;
#endif
}

}  // namespace hms_cpap::oscar::helper
