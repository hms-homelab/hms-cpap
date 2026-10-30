#include "sleepbridge/BatchStore.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>

#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#else
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

Answer refusal(int status, const std::string& error, std::optional<int64_t> ack) {
    Answer a;
    a.status          = status;
    a.body["ok"]      = false;
    a.body["error"]   = error;
    if (ack) a.body["ack"] = static_cast<Json::Int64>(*ack);
    return a;
}

int currentPid() {
#ifdef _WIN32
    return _getpid();
#else
    return static_cast<int>(::getpid());
#endif
}

// The folder's entry for a renamed file is itself data: without this a power
// cut after the rename can come back with the old name. POSIX only; NTFS
// journals the rename.
void syncDir(const fs::path& dir) {
#ifndef _WIN32
    const int fd = ::open(dir.string().c_str(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#else
    (void)dir;
#endif
}

bool appendLine(const fs::path& path, const std::string& line) {
    std::ofstream f(path, std::ios::app | std::ios::binary);
    if (!f) return false;
    f << line << '\n';
    f.flush();
    return static_cast<bool>(f);
}

}  // namespace

bool durableWrite(const std::string& path, const void* data, size_t len, std::string* error) {
    auto fail = [&](const std::string& what) {
        if (error) *error = what;
        return false;
    };
    const fs::path final_path(path);
    const fs::path dir = final_path.parent_path();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return fail("mkdir " + dir.string() + ": " + ec.message());

    static std::atomic<unsigned long> counter{0};
    const fs::path tmp = dir / ("." + final_path.filename().string() + ".tmp-" +
                                std::to_string(currentPid()) + "-" +
                                std::to_string(counter.fetch_add(1)));
#ifdef _WIN32
    const int fd = ::_open(tmp.string().c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
                           _S_IREAD | _S_IWRITE);
#else
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0640);
#endif
    if (fd < 0) return fail("open " + tmp.string() + ": " + std::strerror(errno));

    const auto* p = static_cast<const char*>(data);
    size_t done   = 0;
    while (done < len) {
#ifdef _WIN32
        const int n = ::_write(fd, p + done, static_cast<unsigned>(len - done));
#else
        const ssize_t n = ::write(fd, p + done, len - done);
#endif
        if (n < 0) {
            if (errno == EINTR) continue;
            const std::string why = std::strerror(errno);
#ifdef _WIN32
            ::_close(fd);
#else
            ::close(fd);
#endif
            fs::remove(tmp, ec);
            return fail("write " + tmp.string() + ": " + why);
        }
        done += static_cast<size_t>(n);
    }
#ifdef _WIN32
    const bool synced = ::_commit(fd) == 0;
    ::_close(fd);
#else
    const bool synced = ::fsync(fd) == 0;
    ::close(fd);
#endif
    if (!synced) {
        const std::string why = std::strerror(errno);
        fs::remove(tmp, ec);
        return fail("fsync " + tmp.string() + ": " + why);
    }
    fs::rename(tmp, final_path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return fail("rename to " + final_path.string() + ": " + ec.message());
    }
    syncDir(dir);
    return true;
}

BatchStore::BatchStore(std::string root) : root_(std::move(root)) {}

std::string BatchStore::cardRoot(const std::string& sender) const {
    return (fs::path(root_) / sender).string();
}

std::string BatchStore::batchesDir(const std::string& sender) const {
    return (fs::path(root_) / sender / "batches").string();
}

std::vector<std::string> BatchStore::senders() const {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(root_, ec)) {
        if (!e.is_directory()) continue;
        const auto name = e.path().filename().string();
        if (validSender(name) && fs::exists(e.path() / "batches")) out.push_back(name);
    }
    return out;
}

std::optional<CursorState> BatchStore::cursorOf(const std::string& sender) {
    const auto it = cursors_.find(sender);
    if (it != cursors_.end()) return it->second;
    const fs::path p = fs::path(batchesDir(sender)) / "cursor.json";
    CursorState s;
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        cursors_[sender] = s;
        return s;
    }
    std::ifstream f(p, std::ios::binary);
    Json::Value v;
    Json::CharReaderBuilder rb;
    if (!f || !Json::parseFromStream(rb, f, &v, nullptr) || !v.isObject() ||
        !v["high_water"].isIntegral())
        return std::nullopt;
    s.exists     = true;
    s.high_water = v["high_water"].asInt64();
    cursors_[sender] = s;
    return s;
}

bool BatchStore::saveCursor(const std::string& sender, const CursorState& s, std::string* error) {
    if (fail_next_cursor_save_) {
        fail_next_cursor_save_ = false;
        if (error) *error = "cursor save failed (test)";
        return false;
    }
    Json::Value v(Json::objectValue);
    v["high_water"] = static_cast<Json::Int64>(s.high_water);
    const std::string text = compact(v);
    const std::string path = (fs::path(batchesDir(sender)) / "cursor.json").string();
    if (!durableWrite(path, text.data(), text.size(), error)) return false;
    cursors_[sender] = s;
    return true;
}

Answer BatchStore::handle(const std::string& sender, const std::string& machine,
                          const uint8_t* body, size_t len, Stored* stored) {
    if (!validSender(sender))
        return refusal(400, "X-Coyote-Sender must be 1-64 of [A-Za-z0-9._-], not starting with '.'",
                       int64_t{0});

    std::lock_guard<std::mutex> lk(mu_);
    const auto state = cursorOf(sender);
    if (!state) return refusal(503, "could not read the cursor", std::nullopt);
    const int64_t ack = state->high_water;

    if (!validMachine(machine))
        return refusal(400, "X-Coyote-Machine must be 1-64 of [A-Za-z0-9._-], not starting with '.'",
                       ack);

    // A CYS2 becomes the CYS1 it stands for before anything reads it. From
    // here on `p` is a CYS1 or CYR1: no file on disk is ever a CYS2.
    const uint8_t* p = body;
    size_t         n = len;
    std::string    expanded;
    if (isFillBatch(body, len)) {
        const auto xe = expandFillBatch(body, len, expanded);
        if (xe != BatchError::None) return refusal(400, batchErrorText(xe), ack);
        p = reinterpret_cast<const uint8_t*>(expanded.data());
        n = expanded.size();
    }

    Batch batch;
    const auto err = parseBatch(p, n, batch);
    if (err != BatchError::None) return refusal(400, batchErrorText(err), ack);

    if (batch.isCard()) {
        std::string why;
        const auto card = parseCardStatement(batch.card, &why);
        if (!card) return refusal(400, "bad card statement: " + why, ack);
        // Named by the first sequence it comes before: mark + 1, or 0 when
        // nothing is held yet, so the rebuild feeds it at its place.
        const int64_t next = state->exists ? ack + 1 : 0;
        const std::string name = std::to_string(next) + "-" +
            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch()).count()) +
            "-" + std::to_string(card_counter_++) + ".json";
        const std::string path = (fs::path(batchesDir(sender)) / "cards" / name).string();
        std::string werr;
        if (!durableWrite(path, batch.card.data(), batch.card.size(), &werr)) {
            std::cout << "[sleepbridge] " << sender << " card statement not stored: " << werr
                      << std::endl;
            return refusal(500, "could not store the card statement", ack);
        }
        if (stored) stored->card = true;
        Answer a;
        a.body["ok"]   = true;
        a.body["ack"]  = static_cast<Json::Int64>(ack);
        a.body["card"] = true;
        a.body["cid"]  = card->cid;
        return a;
    }

    if (batch.isHeartbeat()) {
        Answer a;
        a.body["ok"]        = true;
        a.body["ack"]       = static_cast<Json::Int64>(ack);
        a.body["heartbeat"] = true;
        return a;
    }

    const auto first = static_cast<int64_t>(batch.first_sequence);
    const auto last  = static_cast<int64_t>(batch.lastSequence());
    const auto d     = decideCursor(*state, first, last);

    if (d.duplicate) {
        Answer a;
        a.body["ok"]        = true;
        a.body["ack"]       = static_cast<Json::Int64>(ack);
        a.body["accepted"]  = 0;
        a.body["gap"]       = false;
        a.body["duplicate"] = true;
        return a;
    }

    const std::string path = (fs::path(batchesDir(sender)) /
                              (std::to_string(first) + "-" + std::to_string(last) + ".cys"))
                                 .string();
    std::string werr;
    if (!durableWrite(path, p, n, &werr)) {
        std::cout << "[sleepbridge] " << sender << " batch " << first << ".." << last
                  << " not stored: " << werr << std::endl;
        return refusal(500, "could not store the batch", ack);
    }

    if (d.gap) {
        Json::Value g(Json::objectValue);
        g["first"]   = static_cast<Json::Int64>(d.gap_first);
        g["last"]    = static_cast<Json::Int64>(d.gap_last);
        g["machine"] = machine;
        // Best effort: the hole is also visible from the batch names.
        appendLine(fs::path(batchesDir(sender)) / "gaps.jsonl", compact(g));
    }

    CursorState next;
    next.exists     = true;
    next.high_water = d.new_high_water;
    if (!saveCursor(sender, next, &werr)) {
        // The batch is on disk, the mark is not moved: the bridge sends it
        // again and it replaces its own file.
        std::cout << "[sleepbridge] " << sender << " cursor not saved: " << werr << std::endl;
        return refusal(500, "could not save the cursor", ack);
    }

    if (stored) {
        stored->with_records = true;
        stored->gap          = d.gap;
    }
    std::cout << "[sleepbridge] " << sender << " seq=" << first << ".." << last
              << " records=" << batch.records.size() << " accepted=" << d.accepted
              << (batch.reads ? " reads" : "")
              << (expanded.empty() ? std::string() : " cys2=" + std::to_string(len) + "/" +
                                                         std::to_string(n))
              << (d.gap ? " GAP " + std::to_string(d.gap_first) + ".." + std::to_string(d.gap_last)
                        : std::string())
              << std::endl;

    Answer a;
    a.body["ok"]        = true;
    a.body["ack"]       = static_cast<Json::Int64>(d.new_high_water);
    a.body["accepted"]  = static_cast<Json::Int64>(d.accepted);
    a.body["gap"]       = d.gap;
    a.body["duplicate"] = false;
    return a;
}

}  // namespace hms_cpap::sleepbridge
