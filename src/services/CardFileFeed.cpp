#include "services/CardFileFeed.h"

#include <iostream>
#include <mutex>

namespace hms_cpap::card_feed {

namespace {
std::mutex& mu() {
    static std::mutex m;
    return m;
}
FileSink& fileSink() {
    static FileSink s;
    return s;
}
RootSink& rootSink() {
    static RootSink s;
    return s;
}
}  // namespace

void setSinks(FileSink file_sink, RootSink root_sink) {
    std::lock_guard<std::mutex> lk(mu());
    fileSink() = std::move(file_sink);
    rootSink() = std::move(root_sink);
}

void fileWritten(const std::string& root, const std::string& rel_path, const std::string& machine) {
    FileSink s;
    {
        std::lock_guard<std::mutex> lk(mu());
        s = fileSink();
    }
    if (!s || root.empty() || rel_path.empty()) return;
    // A collector must never fail because the uploader did.
    try {
        s(root, rel_path, machine);
    } catch (const std::exception& e) {
        std::cerr << "[cpapdash-stream] note for " << rel_path << " dropped: " << e.what() << std::endl;
    } catch (...) {
    }
}

void rootWritten(const std::string& root, const std::string& machine) {
    RootSink s;
    {
        std::lock_guard<std::mutex> lk(mu());
        s = rootSink();
    }
    if (!s || root.empty()) return;
    try {
        s(root, machine);
    } catch (const std::exception& e) {
        std::cerr << "[cpapdash-stream] note for " << root << " dropped: " << e.what() << std::endl;
    } catch (...) {
    }
}

}  // namespace hms_cpap::card_feed
