#include "utils/ArchiveRecordCount.h"

#include <cpapdash/parser/EdfRecordCount.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace hms_cpap {

namespace fs = std::filesystem;

namespace {

/// A header larger than this is not a ResMed signal file's (they carry a
/// handful of signals, 256 bytes each); refusing it bounds the read.
constexpr std::size_t kMaxHeaderBytes = 256 * 64;

bool isDateFolder(const std::string& name) {
    return name.size() == 8 &&
           std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isdigit(c); });
}

}  // namespace

bool repairSignalEdfFile(const std::string& path) {
    const fs::path p(path);
    if (!cpapdash::parser::isResmedSignalEdf(p.filename().string())) return false;
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    if (ec) return false;

    std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
    if (!f) return false;

    std::vector<char> head(256);
    if (!f.read(head.data(), static_cast<std::streamsize>(head.size()))) return false;
    const std::size_t header_bytes = cpapdash::parser::edfHeaderBytes(head.data(), head.size());
    if (header_bytes == 0 || header_bytes > kMaxHeaderBytes || header_bytes > size) return false;
    head.resize(header_bytes);
    if (!f.read(head.data() + 256, static_cast<std::streamsize>(header_bytes - 256))) return false;

    if (!cpapdash::parser::repairEdfDataRecords(head.data(), head.size(), size)) return false;

    f.seekp(236);
    f.write(head.data() + 236, 8);
    f.flush();
    return static_cast<bool>(f);
}

SignalEdfRepair repairSignalEdfsIn(const std::string& dir) {
    SignalEdfRepair r;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fe;
        if (!it->is_regular_file(fe)) continue;
        if (!cpapdash::parser::isResmedSignalEdf(it->path().filename().string())) continue;
        ++r.checked;
        if (repairSignalEdfFile(it->path().string())) ++r.repaired;
    }
    return r;
}

SignalEdfRepair sweepArchiveSignalEdfs(const std::string& archive_root) {
    SignalEdfRepair total;
    if (archive_root.empty()) return total;
    std::error_code ec;
    const fs::path datalog = fs::path(archive_root) / "DATALOG";
    for (fs::directory_iterator it(datalog, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code de;
        if (!it->is_directory(de) || !isDateFolder(it->path().filename().string())) continue;
        const auto r = repairSignalEdfsIn(it->path().string());
        total.checked += r.checked;
        total.repaired += r.repaired;
    }
    return total;
}

}  // namespace hms_cpap
