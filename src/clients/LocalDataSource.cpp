#include "clients/LocalDataSource.h"

#include "utils/CardLayout.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace hms_cpap {

namespace fs = std::filesystem;

namespace {

/// The listings this interface returns are in KB, because that is what an ez
/// Share's HTML gives (EzShareClient reads the listing's own integer) and what
/// every size comparison in the collector was written against.
///
/// Truncating division, so a file under 1 KB reads as 0. The collector compares
/// a night's sizes against the sizes IT stored, so the arithmetic only has to
/// agree with itself; a card that rounds its HTML differently would at worst
/// cost one re-read of a night whose transport changed.
int sizeKb(const fs::path& p) {
    std::error_code ec;
    const auto bytes = fs::file_size(p, ec);
    if (ec) return 0;
    return static_cast<int>(bytes / 1024);
}

/// Fill the listing's date fields from the file's own modification time, in the
/// local zone, which is how EzShareClient's HTML listing reports them.
void setModTime(EzShareFileEntry& e, const fs::path& p) {
    std::error_code ec;
    const auto ftime = fs::last_write_time(p, ec);
    if (ec) return;
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    const std::time_t t = std::chrono::system_clock::to_time_t(sys);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    e.year = tm.tm_year + 1900;
    e.month = tm.tm_mon + 1;
    e.day = tm.tm_mday;
    e.hour = tm.tm_hour;
    e.minute = tm.tm_min;
    e.second = tm.tm_sec;
}

bool isDateFolderName(const std::string& name) {
    return name.size() == 8 &&
           std::all_of(name.begin(), name.end(),
                       [](unsigned char c) { return std::isdigit(c); });
}

/// A card-relative path as this interface spells it: backslash separated, ""
/// for the root (SDD-002). Turned into a real path under the card root.
fs::path underRoot(const std::string& card_root, const std::string& card_path) {
    fs::path p(card_root);
    std::string part;
    for (char c : card_path) {
        if (c == '\\' || c == '/') {
            if (!part.empty()) p /= part;
            part.clear();
        } else {
            part += c;
        }
    }
    if (!part.empty()) p /= part;
    return p;
}

/// Copy [from] to [to] unless they are the same file. The cycle skips staging
/// for this source (filesAreInPlace), but a caller that asks anyway must not
/// truncate the user's own card by copying a file onto itself.
///
/// DELIBERATE DIVERGENCE from EzShareClient::downloadFile(), which deletes the
/// destination and returns false when the result is 0 bytes: over HTTP an empty
/// body means the fetch failed, and on a disk an empty file means the card holds
/// an empty file. Refusing it here would turn a real (if useless) card file into
/// a download error that repeats every cycle.
bool copyOut(const fs::path& from, const std::string& to) {
    std::error_code ec;
    if (!fs::exists(from, ec)) return false;
    if (fs::equivalent(from, fs::path(to), ec)) return true;   // already there
    ec.clear();
    fs::create_directories(fs::path(to).parent_path(), ec);
    ec.clear();
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

}  // namespace

LocalDataSource::LocalDataSource(std::string card_root)
    : card_root_(std::move(card_root)) {}

std::string LocalDataSource::datalogDir() const {
    return datalogDirFor(card_root_);
}

std::vector<std::string> LocalDataSource::listDateFolders() {
    std::vector<std::string> folders;
    std::error_code ec;
    for (fs::directory_iterator it(datalogDir(), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code de;
        if (!it->is_directory(de)) continue;
        const std::string name = it->path().filename().string();
        if (isDateFolderName(name)) folders.push_back(name);
    }
    std::sort(folders.begin(), folders.end());
    return folders;
}

std::vector<EzShareFileEntry> LocalDataSource::listFiles(const std::string& date_folder) {
    std::vector<EzShareFileEntry> files;
    std::error_code ec;
    const fs::path dir = fs::path(datalogDir()) / date_folder;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fe;
        if (!it->is_regular_file(fe)) continue;
        EzShareFileEntry e;
        e.name = it->path().filename().string();
        e.size_kb = sizeKb(it->path());
        e.is_dir = false;
        // The listing's timestamp, which grouping READS: estimateCheckpointEnd()
        // widens a checkpoint's end to its modification time, so leaving these
        // zero would group a local folder differently from the same folder on a
        // card. The local grouping path takes the same value from the
        // filesystem; this is how one cycle can produce one answer.
        setModTime(e, it->path());
        files.push_back(std::move(e));
    }
    std::sort(files.begin(), files.end(),
              [](const EzShareFileEntry& a, const EzShareFileEntry& b) { return a.name < b.name; });
    return files;
}

std::vector<EzShareFileEntry> LocalDataSource::listDir(const std::string& card_path) {
    std::vector<EzShareFileEntry> entries;
    std::error_code ec;
    for (fs::directory_iterator it(underRoot(card_root_, card_path), ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code de;
        EzShareFileEntry e;
        e.name = it->path().filename().string();
        e.is_dir = it->is_directory(de);
        e.size_kb = e.is_dir ? 0 : sizeKb(it->path());
        entries.push_back(std::move(e));
    }
    std::sort(entries.begin(), entries.end(),
              [](const EzShareFileEntry& a, const EzShareFileEntry& b) { return a.name < b.name; });
    return entries;
}

bool LocalDataSource::downloadByPath(const std::string& card_rel_path,
                                     const std::string& local_path) {
    return copyOut(underRoot(card_root_, card_rel_path), local_path);
}

bool LocalDataSource::downloadFile(const std::string& date_folder,
                                   const std::string& filename,
                                   const std::string& local_path) {
    return copyOut(fs::path(datalogDir()) / date_folder / filename, local_path);
}

bool LocalDataSource::downloadFileRange(const std::string& date_folder,
                                        const std::string& filename,
                                        const std::string& local_path,
                                        size_t start_byte,
                                        size_t& bytes_downloaded) {
    bytes_downloaded = 0;
    const fs::path src = fs::path(datalogDir()) / date_folder / filename;
    std::error_code ec;
    const auto size = fs::file_size(src, ec);
    if (ec) return false;

    // Already have all of it: true, nothing appended, which the collector reads
    // as "unchanged" (BurstCollectorService's smartDownload logs exactly that
    // for bytes_downloaded == 0) and is what settles a night.
    //
    // DELIBERATE DIVERGENCE: an ez Share cannot answer this way. A range at or
    // past EOF comes back outside 200/206, so EzShareClient returns false and
    // the caller falls back to a full download -- and per the sidecar rule in
    // the collector, asking it at all can wedge the card. A file on disk has a
    // knowable end, so the honest answer is available here and worth giving.
    if (start_byte >= size) return true;

    std::ifstream in(src, std::ios::binary);
    if (!in) return false;
    in.seekg(static_cast<std::streamoff>(start_byte));
    if (!in) return false;

    // The same open modes EzShareClient uses, checked against it rather than
    // assumed: from byte 0 this REPLACES the local copy, from anywhere else it
    // appends. Always appending would silently double a file whose caller asked
    // for the whole thing.
    std::ios_base::openmode mode = std::ios::binary;
    mode |= (start_byte == 0) ? std::ios::trunc : std::ios::app;
    std::ofstream out(local_path, mode);
    if (!out) return false;

    constexpr size_t kChunk = 64 * 1024;
    std::vector<char> buf(kChunk);
    size_t remaining = static_cast<size_t>(size) - start_byte;
    while (remaining > 0 && in) {
        const size_t want = std::min(kChunk, remaining);
        in.read(buf.data(), static_cast<std::streamsize>(want));
        const auto got = in.gcount();
        if (got <= 0) break;
        out.write(buf.data(), got);
        if (!out) return false;
        bytes_downloaded += static_cast<size_t>(got);
        remaining -= static_cast<size_t>(got);
    }
    return true;
}

bool LocalDataSource::downloadRootFile(const std::string& filename,
                                       const std::string& local_path) {
    return copyOut(fs::path(card_root_) / filename, local_path);
}

}  // namespace hms_cpap
