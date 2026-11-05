#include "services/CpapCard.h"

#include "sleepbridge/LearnedCard.h"

#include "miniz.h"

#include <cstdlib>
#include <filesystem>
#include <vector>

namespace hms_cpap {

namespace fs = std::filesystem;

namespace {

// "baseline.cbl1" or "<folder>/baseline.cbl1", '/' or '\' separated.
bool isLayoutEntry(const std::string& name) {
    const std::string entry = kCardLayoutEntry;
    if (name == entry) return true;
    if (name.size() <= entry.size() + 1) return false;
    if (name.compare(name.size() - entry.size(), entry.size(), entry) != 0) return false;
    const char sep = name[name.size() - entry.size() - 1];
    if (sep != '/' && sep != '\\') return false;
    const std::string folder = name.substr(0, name.size() - entry.size() - 1);
    return !folder.empty() && folder.find('/') == std::string::npos &&
           folder.find('\\') == std::string::npos;
}

}  // namespace

std::optional<std::string> readCardLayout(const std::string& zip_path, std::string* why) {
    if (why) why->clear();
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zip_path.c_str(), 0)) return std::nullopt;
    std::optional<std::string> out;
    const auto n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory) continue;
        if (!isLayoutEntry(st.m_filename)) continue;
        // Judged by the size it says before a byte is inflated.
        if (st.m_uncomp_size > sleepbridge::kLearnedCardMax) {
            if (why) *why = "The card's layout is larger than 32 MB.";
            break;
        }
        size_t size = 0;
        void*  heap = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (!heap) {
            if (why) *why = "The card's layout in the file could not be unpacked.";
            break;
        }
        out.emplace(static_cast<const char*>(heap), size);
        mz_free(heap);
        break;
    }
    mz_zip_reader_end(&zip);
    return out;
}

int removeCardLayout(const std::string& extracted_root) {
    std::vector<fs::path> found;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(extracted_root, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().filename() == kCardLayoutEntry) found.push_back(it->path());
    }
    int removed = 0;
    for (const auto& p : found)
        if (fs::remove(p, ec)) ++removed;
    return removed;
}

}  // namespace hms_cpap
