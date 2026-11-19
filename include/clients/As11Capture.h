#pragma once

// SDD-071: an AirCurve 11 night recorded as the machine sends it, so an ASV's
// fields beyond the 13 hms-cpap understands can be read later beside the
// card. One JSON object per line:
//   {"t":"2026-11-18T23:01:02.345Z","dir":"in","msg":{...}}   a message received
//   {"t":...,"dir":"out","msg":{...}}                         a request sent
//   {"t":...,"dir":"note","note":"..."}                       hms-cpap's own word
// "msg" is the message as parsed JSON, or the text when it does not parse.
// What goes in is only what As11Session's capture tap hands over: never a
// pairing or session-opening message, so never a key or the pairing code.

#include <atomic>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>

namespace hms_cpap::as11 {

class As11Capture {
public:
    /// <data_dir>/as11/<serial>/capture/<YYYYMMDD-HHMMSS>.jsonl, `when` in UTC.
    static std::string pathFor(const std::string& data_dir, const std::string& serial,
                               std::time_t when);

    /// Starts a new file at `path` (its folders made). False when it cannot.
    bool open(const std::string& path);
    /// One line for a message or a request (dir "in" or "out").
    void record(const char* dir, const std::string& text);
    /// One line in hms-cpap's own words (a refusal, a retry).
    void note(const std::string& what);
    /// Ends the file. `keep` false removes it (a connect that never streamed).
    void close(bool keep = true);

    bool isOpen() const;
    /// The file being written, or the last one kept; empty before any.
    std::string path() const;
    /// Its size so far, or the last kept file's.
    long long bytes() const;

private:
    void line(const std::string& s);   // m_ held
    mutable std::mutex m_;
    std::ofstream out_;
    std::string path_, kept_path_;
    std::atomic<long long> bytes_{0};
    long long kept_bytes_ = 0;
};

}  // namespace hms_cpap::as11
