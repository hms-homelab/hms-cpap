#pragma once
// A .cpapcard written the way CpapDash Desktop writes one (SDD-061): the
// card's files stored, then baseline.cbl1 deflated, in one zip.

#include "miniz.h"

#include <string>
#include <vector>

namespace cpapcardtest {

struct Entry {
    std::string name;
    std::string data;
    bool        deflate{false};
};

/// False when miniz could not write it.
inline bool writeZip(const std::string& path, const std::vector<Entry>& entries) {
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, path.c_str(), 0)) return false;
    bool ok = true;
    for (const auto& e : entries)
        ok = ok && mz_zip_writer_add_mem(&zip, e.name.c_str(), e.data.data(), e.data.size(),
                                         e.deflate ? MZ_DEFAULT_LEVEL : MZ_NO_COMPRESSION);
    ok = mz_zip_writer_finalize_archive(&zip) && ok;
    mz_zip_writer_end(&zip);
    return ok;
}

}  // namespace cpapcardtest
