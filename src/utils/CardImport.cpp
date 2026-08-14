#include "utils/CardImport.h"

#include "utils/CardResidue.h"

#include <algorithm>
#include <filesystem>
#include <regex>

namespace fs = std::filesystem;

namespace hms_cpap {

CardImportResult mirrorCardInto(const std::string& extracted_root,
                                const std::string& card_dir,
                                const std::string& archive_dir) {
    CardImportResult out;
    std::error_code ec;

    // The card root is the deepest directory holding DATALOG, so a zip that
    // wraps its contents in a folder resolves the same as one that does not.
    fs::path card_root = extracted_root;
    for (auto it = fs::recursive_directory_iterator(extracted_root, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_directory(ec) && it->path().filename() == "DATALOG") {
            card_root = it->path().parent_path();
            break;
        }
    }
    ec.clear();

    const std::regex datedir("^[0-9]{8}$");

    for (auto it = fs::recursive_directory_iterator(card_root, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;

        const std::string fname = it->path().filename().string();
        auto fsize = fs::file_size(it->path(), ec);
        if (ec) { ec.clear(); continue; }
        if (residualSkip(fname, static_cast<uint64_t>(fsize))) { out.skipped++; continue; }

        fs::path rel = fs::relative(it->path(), card_root, ec);
        if (ec) { ec.clear(); continue; }

        // A date directory anywhere in the path means the file belongs to that
        // night, and nights live under the archive's DATALOG regardless of how
        // deep the zip buried them.
        std::string date_dir;
        for (const auto& part : rel) {
            if (std::regex_match(part.string(), datedir)) {
                date_dir = part.string();
                break;
            }
        }

        fs::path dest = date_dir.empty() ? fs::path(card_dir) / rel
                                         : fs::path(archive_dir) / date_dir / fname;

        fs::create_directories(dest.parent_path(), ec);
        fs::copy_file(it->path(), dest, fs::copy_options::overwrite_existing, ec);
        if (ec) { ec.clear(); continue; }

        out.copied++;
        if (!date_dir.empty()) out.dates.insert(date_dir);

        std::string lower = fname;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower == "str.edf") out.saw_str = true;
    }

    return out;
}

}  // namespace hms_cpap
