// SDD-067: the rules of moving the add-on's data directory. See DataDirMove.h.

#include "utils/DataDirMove.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#else
#include <io.h>
#endif

namespace fs = std::filesystem;

namespace hms_cpap::datamove {

namespace {

struct Item {
    fs::path rel;
    std::uint64_t size = 0;
    bool is_symlink = false;
    bool home_config = false;  // <home>/config.json: copied, never moved (D8)
    int rank = 0;              // the SQLite database goes last
};

// The SQLite files go last, the database itself after its journal files, so a
// move that fails part way (a full disk) is most likely to leave the database
// where the fallback run opens it.
int rankOf(const fs::path& rel) {
    if (rel.has_parent_path() && rel.parent_path() != fs::path(".")) return 0;
    const std::string n = rel.filename().string();
    if (n == "cpap.db") return 2;
    if (n.rfind("cpap.db-", 0) == 0) return 1;
    return 0;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

fs::path normal(const fs::path& p) {
    std::error_code ec;
    fs::path w = fs::weakly_canonical(p, ec);
    if (ec) w = fs::absolute(p, ec).lexically_normal();
    // weakly_canonical keeps a trailing separator as an empty last element.
    std::string s = w.string();
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return fs::path(s);
}

// Is `inner` the same as, or underneath, `outer`?
bool within(const fs::path& inner, const fs::path& outer) {
    const fs::path rel = inner.lexically_relative(outer);
    if (rel.empty()) return false;
    const auto first = *rel.begin();
    return first != "..";
}

std::string errnoText(int e) {
    return std::strerror(e);
}

// Write `text` to `path` through a temporary name and a rename, so a reader
// never sees half of it.
bool writeAtomically(const fs::path& path, const std::string& text, std::string& err) {
    const fs::path tmp = path.string() + kTempSuffix;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { err = "cannot write " + tmp.string(); return false; }
        f << text;
        f.flush();
        if (!f) { err = "cannot write " + tmp.string(); return false; }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        err = "cannot write " + path.string();
        return false;
    }
    return true;
}

class Mover {
public:
    explicit Mover(const Options& o) : o_(o) {}

    Result run() {
        Result r;
        p_.from = o_.from.string();
        p_.to = o_.to.string();

        const fs::path from = normal(o_.from);
        const fs::path to = normal(o_.to);
        const fs::path home = normal(o_.home);
        std::error_code ec;

        if (!fs::is_directory(from, ec)) return finish(r, "the source is not a directory: " + from.string());
        if (from == to) return finish(r, "the source and the destination are the same directory");
        if (within(to, from) || within(from, to))
            return finish(r, "one directory is inside the other: " + from.string() + ", " + to.string());

        fs::create_directories(to, ec);
        if (ec || !fs::is_directory(to)) return finish(r, "cannot create " + to.string() + ": " + ec.message());

        std::vector<Item> items;
        if (!collect(from, home, items, r)) return finish(r, r.error);
        p_.files_total = items.size();
        for (const auto& i : items) p_.bytes_total += i.size;

        // Nothing to move is a complete move, and no marker is written for it:
        // the marker records a move, and none happened.
        if (items.empty()) { r.ok = true; return finish(r, ""); }

        // Before the first file: a move cut off half way must read as "this
        // install moved there" on the next start (DataDirMove.h).
        {
            std::string err;
            fs::create_directories(home, ec);
            if (!writeAtomically(home / kMarkerName, to.string() + "\n", err)) return finish(r, err);
        }

        if (!emit()) return finish(r, "interrupted");

        for (const auto& item : items) {
            p_.current = item.rel.generic_string();
            std::string err;
            if (!moveOne(from, to, item, r, err)) return finish(r, err);
            ++p_.files_done;
            if (!emit()) return finish(r, "interrupted");
        }

        removeEmptyDirs(from);

        // D8, D15: moving from one chosen directory to another refreshes the
        // copy in home. From home itself, config.json was copied, so the copy
        // is already there; in a catch-up it existed at the destination and
        // stays as it is.
        if (from != home && fs::exists(to / "config.json", ec)) {
            std::string err;
            std::ifstream f(to / "config.json", std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            if (!writeAtomically(home / "config.json", ss.str(), err)) return finish(r, err);
        }

        r.ok = true;
        return finish(r, "");
    }

private:
    bool collect(const fs::path& from, const fs::path& home, std::vector<Item>& items, Result& r) {
        std::error_code ec;
        const bool from_is_home = (from == home);
        fs::recursive_directory_iterator it(from, fs::directory_options::skip_permission_denied, ec);
        if (ec) { r.error = "cannot read " + from.string() + ": " + ec.message(); return false; }
        for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { r.error = "cannot read " + from.string() + ": " + ec.message(); return false; }
            const fs::path path = it->path();
            const fs::path rel = path.lexically_relative(from);
            const std::string name = path.filename().string();
            const bool top = !rel.has_parent_path() || rel.parent_path().empty();
            const auto st = fs::symlink_status(path, ec);
            if (ec) continue;
            if (fs::is_directory(st)) continue;
            if (top && (name == kOptionsName || name == kMarkerName)) continue;
            if (endsWith(name, kTempSuffix)) continue;  // our own leftovers
            Item item;
            item.rel = rel;
            if (fs::is_symlink(st)) {
                item.is_symlink = true;
            } else if (fs::is_regular_file(st)) {
                item.size = fs::file_size(path, ec);
                if (ec) item.size = 0;
            } else {
                continue;  // sockets, fifos: not data
            }
            item.home_config = from_is_home && top && name == "config.json";
            item.rank = rankOf(rel);
            items.push_back(std::move(item));
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            if (a.rank != b.rank) return a.rank < b.rank;
            return a.rel.generic_string() < b.rel.generic_string();
        });
        return true;
    }

    bool moveOne(const fs::path& from, const fs::path& to, const Item& item, Result& r, std::string& err) {
        std::error_code ec;
        const fs::path src = from / item.rel;
        const fs::path dst = to / item.rel;

        // Never overwrite (D9). The one exception is a file this mover already
        // put there, which a move interrupted between the rename and the
        // removal leaves on both sides: same size, same modification time.
        const auto dst_st = fs::symlink_status(dst, ec);
        if (fs::exists(dst_st)) {
            bool ours = false;
            if (!item.home_config && !item.is_symlink && fs::is_regular_file(dst_st)) {
                std::error_code e1, e2;
                ours = fs::file_size(dst, e1) == item.size &&
                       fs::last_write_time(dst, e1) == fs::last_write_time(src, e2) && !e1 && !e2;
            }
            if (ours) {
                fs::remove(src, ec);
                ++r.moved;
            } else {
                ++r.left;
            }
            addBytes(item.size);
            return true;
        }

        fs::create_directories(dst.parent_path(), ec);
        if (ec) { err = "cannot create " + dst.parent_path().string() + ": " + ec.message(); return false; }

        if (!item.home_config && !o_.force_copy) {
            fs::rename(src, dst, ec);
            if (!ec) {
                ++r.moved;
                r.bytes += item.size;
                addBytes(item.size);
                return true;
            }
            // EXDEV, /data to /share: copy instead. Any other reason surfaces
            // from the copy with its own error.
        }

        if (item.is_symlink) {
            const fs::path target = fs::read_symlink(src, ec);
            if (!ec) fs::create_symlink(target, dst, ec);
            if (ec) { err = "cannot move " + item.rel.generic_string() + ": " + ec.message(); return false; }
            fs::remove(src, ec);
            ++r.moved;
            return true;
        }

        if (!copyAcross(src, dst, item, err)) return false;
        r.bytes += item.size;
        if (!item.home_config) {
            fs::remove(src, ec);
            if (ec) { err = "copied but cannot remove " + src.string() + ": " + ec.message(); return false; }
            ++r.moved;
        }
        return true;
    }

    bool copyAcross(const fs::path& src, const fs::path& dst, const Item& item, std::string& err) {
        const fs::path tmp = dst.string() + kTempSuffix;
        const std::string rel = item.rel.generic_string();
        std::FILE* in = std::fopen(src.string().c_str(), "rb");
        if (!in) { err = "cannot read " + rel + ": " + errnoText(errno); return false; }
        std::FILE* out = std::fopen(tmp.string().c_str(), "wb");
        if (!out) {
            err = "cannot write " + tmp.string() + ": " + errnoText(errno);
            std::fclose(in);
            return false;
        }

        auto fail = [&](const std::string& why) {
            std::fclose(in);
            std::fclose(out);
            std::error_code ec;
            fs::remove(tmp, ec);
            err = "cannot copy " + rel + ": " + why;
            return false;
        };

        std::vector<char> buf(1 << 20);
        for (;;) {
            const size_t n = std::fread(buf.data(), 1, buf.size(), in);
            if (n == 0) {
                if (std::ferror(in)) return fail(errnoText(errno));
                break;
            }
            if (o_.fail_after_bytes > 0 && written_ + n > o_.fail_after_bytes)
                return fail(errnoText(ENOSPC));
            if (std::fwrite(buf.data(), 1, n, out) != n) return fail(errnoText(errno));
            written_ += n;
            addBytes(n);
        }
        if (std::fflush(out) != 0) return fail(errnoText(errno));
#ifndef _WIN32
        if (::fsync(::fileno(out)) != 0) return fail(errnoText(errno));
#else
        if (::_commit(::_fileno(out)) != 0) return fail(errnoText(errno));
#endif
        std::fclose(in);
        if (std::fclose(out) != 0) {
            std::error_code ec;
            fs::remove(tmp, ec);
            err = "cannot copy " + rel + ": " + errnoText(errno);
            return false;
        }

        std::error_code ec;
        fs::permissions(tmp, fs::status(src, ec).permissions(), ec);
        // The source's modification time, which is also what tells a rerun
        // that a file on both sides is this mover's finished copy.
        const auto mtime = fs::last_write_time(src, ec);
        if (!ec) fs::last_write_time(tmp, mtime, ec);
        fs::rename(tmp, dst, ec);
        if (ec) {
            std::error_code e2;
            fs::remove(tmp, e2);
            err = "cannot move " + rel + " into place: " + ec.message();
            return false;
        }
        return true;
    }

    void removeEmptyDirs(const fs::path& from) {
        std::error_code ec;
        std::vector<fs::path> dirs;
        for (fs::recursive_directory_iterator it(from, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (it->is_directory(ec) && !it->is_symlink(ec)) dirs.push_back(it->path());
        }
        std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
            return a.string().size() > b.string().size();
        });
        for (const auto& d : dirs) {
            if (fs::is_empty(d, ec)) fs::remove(d, ec);
        }
    }

    // Bytes advanced: an event when the whole percent changes.
    void addBytes(std::uint64_t n) {
        p_.bytes_done += n;
        const int pct = percentOf(p_);
        if (pct != last_pct_) {
            last_pct_ = pct;
            emit();
        }
    }

    bool emit() {
        if (!o_.on_progress) return true;
        return o_.on_progress(p_);
    }

    Result& finish(Result& r, const std::string& error) {
        if (!error.empty()) { r.ok = false; r.error = error; }
        p_.finished = true;
        p_.ok = r.ok;
        p_.error = r.error;
        p_.current.clear();
        emit();
        return r;
    }

    const Options& o_;
    Progress p_;
    int last_pct_ = -1;
    std::uint64_t written_ = 0;
};

std::string human(std::uint64_t b) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(b);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    char out[32];
    if (u == 0) std::snprintf(out, sizeof out, "%llu B", static_cast<unsigned long long>(b));
    else std::snprintf(out, sizeof out, "%.1f %s", v, units[u]);
    return out;
}

}  // namespace

Result moveData(const Options& opts) {
    try {
        Mover m(opts);
        return m.run();
    } catch (const std::exception& e) {
        Result r;
        r.error = e.what();
        return r;
    }
}

std::string readMarker(const fs::path& home) {
    std::ifstream f(home / kMarkerName);
    std::string line;
    if (!f || !std::getline(f, line)) return "";
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}

int percentOf(const Progress& p) {
    if (p.bytes_total == 0) return p.files_total == 0 || p.files_done == p.files_total ? 100 : 0;
    return static_cast<int>((p.bytes_done * 100) / p.bytes_total);
}

std::string progressLine(const Progress& p) {
    std::ostringstream s;
    s << "[data-move] ";
    if (p.finished) {
        if (p.ok) s << "done: " << p.files_done << " of " << p.files_total << " files, "
                    << human(p.bytes_done) << ", from " << p.from << " to " << p.to;
        else s << "FAILED after " << p.files_done << " of " << p.files_total << " files: " << p.error;
        return s.str();
    }
    s << p.files_done << "/" << p.files_total << " files, " << human(p.bytes_done) << " of "
      << human(p.bytes_total) << " (" << percentOf(p) << "%)";
    if (!p.current.empty()) s << ": " << p.current;
    return s.str();
}

std::string progressJson(const Progress& p, std::uint64_t seq) {
    nlohmann::json j;
    j["seq"] = seq;
    j["from"] = p.from;
    j["to"] = p.to;
    j["files_total"] = p.files_total;
    j["files_done"] = p.files_done;
    j["bytes_total"] = p.bytes_total;
    j["bytes_done"] = p.bytes_done;
    j["percent"] = percentOf(p);
    j["current"] = p.current;
    j["finished"] = p.finished;
    j["ok"] = p.ok;
    j["error"] = p.error;
    return j.dump();
}

std::optional<Fallback> fallbackFromEnv(const std::string& in_use) {
    const char* v = std::getenv("HMS_CPAP_DATA_DIR_FALLBACK");
    if (!v || !*v) return std::nullopt;
    return Fallback{v, in_use};
}

}  // namespace hms_cpap::datamove
