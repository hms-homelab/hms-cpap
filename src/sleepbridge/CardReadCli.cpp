#include "sleepbridge/CardReadCli.h"

#include "services/CardUpload.h"
#include "services/CpapCard.h"
#include "sleepbridge/CardRead.h"
#include "sleepbridge/LearnedCard.h"
#include "sleepbridge/Wire.h"
#include "utils/CardLayout.h"
#include "utils/MachineIdentity.h"

#include "miniz.h"

#include <json/json.h>

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace hms_cpap::sleepbridge {

namespace fs = std::filesystem;

namespace {

std::string compact(const Json::Value& v) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, v);
}

std::string g_progress;   // --progress, for the last word too

void writeFile(const std::string& path, const std::string& text) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << text;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
}

/// The answer on stdout, and as the progress file's last state ("done" or
/// "refused"): an elevated run on Windows has no stdout its caller can read.
int answer(bool ok, Json::Value v, int code) {
    v["ok"] = ok;
    std::cout << compact(v) << std::endl;
    if (!g_progress.empty()) {
        v["phase"] = ok ? "done" : "refused";
        writeFile(g_progress, compact(v));
    }
    return code;
}

int refuse(const std::string& why) {
    Json::Value v(Json::objectValue);
    v["error"] = why;
    return answer(false, v, 2);
}

void writeProgress(const std::string& path, const char* phase, size_t done, size_t total) {
    if (path.empty()) return;
    Json::Value v(Json::objectValue);
    v["phase"] = phase;
    v["done"]  = static_cast<Json::UInt64>(done);
    v["total"] = static_cast<Json::UInt64>(total);
    writeFile(path, compact(v));
}

/// The raw device, read-only.
class Device {
public:
    ~Device() { close(); }

    bool open(const std::string& device, std::string* why) {
#ifdef _WIN32
        // "E:" or "E:\": the volume names its disk; the disk is what is read,
        // since the card's LBA 0 is the disk's, not the volume's.
        std::string letter = device.substr(0, 2);
        const std::wstring vol = L"\\\\.\\" + std::wstring(letter.begin(), letter.end());
        HANDLE v = CreateFileW(vol.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (v == INVALID_HANDLE_VALUE) {
            *why = "The card's drive could not be opened.";
            return false;
        }
        STORAGE_DEVICE_NUMBER num{};
        DWORD got = 0;
        const BOOL ok = DeviceIoControl(v, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &num, sizeof num, &got, nullptr);
        CloseHandle(v);
        if (!ok) {
            *why = "The card's drive does not name its disk.";
            return false;
        }
        const std::wstring disk = L"\\\\.\\PhysicalDrive" + std::to_wstring(num.DeviceNumber);
        h_ = CreateFileW(disk.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) {
            *why = GetLastError() == ERROR_ACCESS_DENIED
                       ? "Reading the card's layout needs this computer's permission, and it was not given."
                       : "The card's disk could not be opened.";
            return false;
        }
        buf_ = static_cast<char*>(VirtualAlloc(nullptr, kBufBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        return buf_ != nullptr;
#else
        const std::string disk = wholeDiskFor(device);
        fd_ = ::open(disk.c_str(), O_RDONLY);
        if (fd_ < 0) {
            *why = errno == EACCES || errno == EPERM
                       ? "Reading the card's layout needs this computer's permission, and it was not given."
                       : "The card's disk " + disk + " could not be opened.";
            return false;
        }
        return true;
#endif
    }

    bool read(uint32_t lba, uint32_t count, std::string& out) {
        out.assign(static_cast<size_t>(count) * kSectorSize, '\0');
        size_t done = 0;
        while (done < out.size()) {
            const size_t want = std::min(out.size() - done, kBufBytes);
            const uint64_t at = static_cast<uint64_t>(lba) * kSectorSize + done;
#ifdef _WIN32
            LARGE_INTEGER pos;
            pos.QuadPart = static_cast<LONGLONG>(at);
            DWORD got    = 0;
            if (!SetFilePointerEx(h_, pos, nullptr, FILE_BEGIN) ||
                !ReadFile(h_, buf_, static_cast<DWORD>(want), &got, nullptr) || got != want)
                return false;
            std::copy(buf_, buf_ + want, out.begin() + static_cast<long>(done));
#else
            const ssize_t got = ::pread(fd_, out.data() + done, want, static_cast<off_t>(at));
            if (got != static_cast<ssize_t>(want)) return false;
#endif
            done += want;
        }
        return true;
    }

    void close() {
#ifdef _WIN32
        if (buf_) VirtualFree(buf_, 0, MEM_RELEASE);
        buf_ = nullptr;
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
#else
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
#endif
    }

private:
    static constexpr size_t kBufBytes = 128 * kSectorSize;   // sector-aligned, page-aligned on Windows
#ifdef _WIN32
    HANDLE h_{INVALID_HANDLE_VALUE};
    char*  buf_{nullptr};
#else
    int fd_{-1};
#endif
};

}  // namespace

int runReadCard(const std::vector<std::string>& args) {
    std::string device, root, out, progress;
    for (size_t i = 0; i < args.size(); i++) {
        auto value = [&](std::string& into) {
            if (i + 1 < args.size()) into = args[++i];
        };
        if (args[i] == "--read-card") value(device);
        else if (args[i] == "--card-root") value(root);
        else if (args[i] == "--out") value(out);
        else if (args[i] == "--progress") value(progress);
    }
    if (device.empty() || root.empty() || out.empty()) {
        std::cerr << "usage: --read-card <device> --card-root <folder> --out <file.cpapcard> [--progress <file>]"
                  << std::endl;
        return 1;
    }
    g_progress = progress;

    // A CPAP card by its files first, before the raw read: nothing that is
    // not one is opened at all.
    const auto layout = classifyLocalDir(root);
    const bool resmed = layout == LocalDirLayout::Root;
    const auto kind   = classifyUploadedCard(root);
    if (!resmed && kind == UploadedCard::Unknown)
        return refuse("This is not a CPAP machine's card: it has no STR.edf and DATALOG, and no other machine's files.");

    Json::Value labels(Json::objectValue);
    labels["brand"] = resmed ? "resmed" : uploadedCardName(kind);
    const MachineIdentity id = readIdentification(root);
    labels["machine_serial"] = id.serial;
    labels["reader"]         = "hms-cpap installer";

    Device dev;
    std::string why;
    if (!dev.open(device, &why)) return refuse(why);
    const auto base = readCardBaseline(
        [&dev](uint32_t lba, uint32_t count, std::string& s) { return dev.read(lba, count, s); }, &why, labels,
        [&progress](size_t done, size_t total) { writeProgress(progress, "layout", done, total); });
    dev.close();
    if (!base) return refuse(why);
    const std::string cbl1 = encodeCbl1(base->statement, base->sectors);
    if (!decodeLearnedCard(cbl1, &why)) return refuse(why);   // what the upload would refuse

    // The card's files, stored, then the layout.
    std::vector<std::pair<fs::path, std::string>> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const std::string rel = fs::relative(it->path(), root, ec).generic_string();
        if (!cardFileToKeep(rel)) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec)) files.emplace_back(it->path(), rel);
    }

    const std::string tmp = out + ".part";
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file_v2(&zip, tmp.c_str(), 0, MZ_ZIP_FLAG_WRITE_ZIP64))
        return refuse("The card's copy could not be written to " + out + ".");
    size_t n = 0;
    for (const auto& [path, rel] : files) {
        if (!mz_zip_writer_add_file(&zip, rel.c_str(), path.string().c_str(), nullptr, 0, MZ_NO_COMPRESSION)) {
            mz_zip_writer_end(&zip);
            fs::remove(tmp, ec);
            return refuse("The card's file " + rel + " could not be copied.");
        }
        writeProgress(progress, "files", ++n, files.size());
    }
    const bool laid = mz_zip_writer_add_mem(&zip, kCardLayoutEntry, cbl1.data(), cbl1.size(), MZ_BEST_COMPRESSION);
    const bool done = laid && mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    if (!done) {
        fs::remove(tmp, ec);
        return refuse("The card's copy could not be written to " + out + ".");
    }
    fs::rename(tmp, out, ec);
    if (ec) return refuse("The card's copy could not be written to " + out + ".");

    Json::Value v(Json::objectValue);
    v["out"]            = out;
    v["sectors"]        = static_cast<Json::UInt64>(base->sectors.size());
    v["files"]          = static_cast<Json::UInt64>(files.size());
    v["nights"]         = base->statement.get("nights", 0);
    v["fat"]            = base->statement.get("fat", "");
    v["brand"]          = labels["brand"];
    v["machine_serial"] = id.serial;
    return answer(true, v, 0);
}

}  // namespace hms_cpap::sleepbridge
