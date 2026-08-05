#include "utils/CardLayout.h"

#include <cctype>
#include <filesystem>

namespace hms_cpap {

namespace {

/// A DATALOG date folder is exactly eight digits. Anything else in there is not
/// a session folder, and treating it as one is how a stray directory turns a
/// working card root into a hard failure.
bool isDateFolderName(const std::string& name) {
    if (name.size() != 8) return false;
    for (unsigned char ch : name) {
        if (!std::isdigit(ch)) return false;
    }
    return true;
}

/// ResMed writes DATALOG uppercase, but a card copied by hand onto a
/// case-sensitive filesystem can arrive lowercase. Both name the same place, so
/// accepting either is tolerance about spelling, not about the contract.
const char* kDatalogNames[] = {"DATALOG", "datalog", "Datalog"};

}  // namespace

std::string datalogDirFor(const std::string& root) {
    std::error_code ec;
    for (const char* name : kDatalogNames) {
        const auto candidate = std::filesystem::path(root) / name;
        if (std::filesystem::is_directory(candidate, ec)) return candidate.string();
    }
    return (std::filesystem::path(root) / "DATALOG").string();
}

LocalDirLayout classifyLocalDir(const std::string& path) {
    if (path.empty()) return LocalDirLayout::Unusable;

    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec)) return LocalDirLayout::Unusable;

    // DATALOG present means this is the root, and nothing else needs asking. A
    // root that also holds a stray eight-digit folder is still a root.
    for (const char* name : kDatalogNames) {
        if (std::filesystem::is_directory(std::filesystem::path(path) / name, ec))
            return LocalDirLayout::Root;
    }

    // No DATALOG. If date folders sit here directly, the user pointed at DATALOG
    // itself: sessions would be discovered and STR never would, which is the
    // precise failure SDD-010 exists to stop being silent.
    std::filesystem::directory_iterator it(path, ec);
    if (ec) return LocalDirLayout::Unusable;
    for (const auto& entry : it) {
        std::error_code entry_ec;
        if (!entry.is_directory(entry_ec)) continue;
        if (isDateFolderName(entry.path().filename().string()))
            return LocalDirLayout::IsDatalog;
    }

    return LocalDirLayout::Unusable;
}

const char* localDirLayoutString(LocalDirLayout layout) {
    switch (layout) {
        case LocalDirLayout::Root:      return "root";
        case LocalDirLayout::IsDatalog: return "is_datalog";
        case LocalDirLayout::Unusable:  return "unusable";
    }
    return "unusable";
}

std::string localDirProblem(LocalDirLayout layout, const std::string& path) {
    switch (layout) {
        case LocalDirLayout::Root:
            return {};
        case LocalDirLayout::IsDatalog:
            return "the local folder points at DATALOG itself (" + path +
                   "), not at the card root. Sessions would import, but STR.edf "
                   "sits one level up and would never be found";
        case LocalDirLayout::Unusable:
            return path.empty()
                ? std::string("no local folder is configured")
                : "no DATALOG folder was found in " + path +
                      " (wrong path, or the share is not mounted)";
    }
    return {};
}

std::string localDirRemedy(LocalDirLayout layout, const std::string& path) {
    switch (layout) {
        case LocalDirLayout::Root:
            return {};
        case LocalDirLayout::IsDatalog: {
            const auto parent = std::filesystem::path(path).parent_path().string();
            return "Point the local folder at " +
                   (parent.empty() ? std::string("the folder ABOVE it") : parent) +
                   " instead. That is the folder holding both STR.edf and DATALOG.";
        }
        case LocalDirLayout::Unusable:
            return "Point the local folder at the SD card root, the folder "
                   "holding both STR.edf and DATALOG. If it is a network share, "
                   "check that it is mounted.";
    }
    return {};
}

}  // namespace hms_cpap
