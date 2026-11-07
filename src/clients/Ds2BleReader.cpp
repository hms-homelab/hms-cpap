#include "clients/Ds2BleReader.h"

#include <cpapdash/ds2ble/Protocol.h>
#include <cpapdash/ds2ble/SyncSession.h>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <iostream>

namespace ds2 = cpapdash::ds2ble;

namespace hms_cpap {

namespace {
constexpr const char* kTag = "DS2 Bluetooth: ";

long long nowEpoch() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

ble::GattSpec dataSpec() {
    const std::string u = ble::normalizeUuid(ds2::kDataCharUuid);
    return {u, u};
}

bool isDs2(const ble::Device& d) {
    if (d.name.rfind(ds2::kNamePrefix, 0) == 0) return true;
    const std::string svc = ble::normalizeUuid(ds2::kServiceUuid);
    for (const auto& u : d.uuids)
        if (u == svc) return true;
    return false;
}
}  // namespace

const char* Ds2BleReader::stateCode(PairState s) {
    switch (s) {
        case PairState::Idle: return "idle";
        case PairState::Scanning: return "scanning";
        case PairState::Pairing: return "pairing";
        case PairState::Confirm: return "confirm";
        case PairState::OsDialog: return "os_dialog";
        case PairState::Paired: return "paired";
        case PairState::Rejected: return "rejected";
        case PairState::TimedOut: return "timeout";
        case PairState::NotFound: return "not_found";
        case PairState::RadioOff: return "radio_off";
        case PairState::Unsupported: return "unsupported";
        case PairState::Failed: return "failed";
        case PairState::Cancelled: return "cancelled";
    }
    return "failed";
}

Ds2BleReader::Ds2BleReader(std::string data_dir, std::string address,
                           std::shared_ptr<ble::ITransport> transport, Timeouts t)
    : data_dir_(std::move(data_dir)), address_(ble::lower(std::move(address))),
      transport_(std::move(transport)), t_(t), store_(data_dir_) {}

Ds2BleReader::~Ds2BleReader() {
    pairCancel();
    if (pair_thread_.joinable()) pair_thread_.join();
}

std::shared_ptr<ble::ITransport> Ds2BleReader::transport() {
    if (!transport_) transport_ = ble::makeHostTransport(kTag);
    return transport_;
}

bool Ds2BleReader::osShowsCode() const {
    return transport_ ? transport_->osShowsPairingCode()
                      : std::string(ble::backendName()) == "corebluetooth";
}

bool Ds2BleReader::paired() const { return store_.load().paired.has_value(); }

std::optional<Ds2Store::Paired> Ds2BleReader::pairedMachine() const { return store_.load().paired; }

bool Ds2BleReader::radioReady(std::string* why) {
    auto t = transport();
    if (!t || !t->available()) {
        *why = "no Bluetooth adapter on this computer";
        return false;
    }
    const auto r = t->radio(true);
    if (r.state == ble::Radio::Ready) return true;
    *why = r.detail.empty() ? "this computer's Bluetooth is off" : r.detail;
    return false;
}

std::optional<ble::Device> Ds2BleReader::find(const std::optional<Ds2Store::Paired>& paired, int timeout_ms) {
    auto t = transport();
    const std::string want = !address_.empty() ? address_ : (paired ? ble::lower(paired->address) : "");
    auto match = [&](const ble::Device& d) {
        if (!want.empty()) return ble::lower(d.address) == want;
        return isDs2(d);
    };
    auto found = t->discover(match, timeout_ms);
    if (found.device) return found.device;
    if (!found.started) std::cerr << kTag << "the scan did not start: " << found.error << std::endl;
    return std::nullopt;
}

// ── pairing ────────────────────────────────────────────────────────────────

void Ds2BleReader::setPair(PairState s, const std::string& error) {
    std::lock_guard<std::mutex> lk(mtx_);
    pair_.state = s;
    pair_.error = error;
    if (s != PairState::Confirm) pair_.code.clear();
    cv_.notify_all();
}

bool Ds2BleReader::pairStart() {
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true)) return false;
    if (pair_thread_.joinable()) pair_thread_.join();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        pair_ = PairStatus{};
        pair_.state = PairState::Scanning;
        answered_ = false;
        answer_ = false;
        cancel_ = false;
    }
    pair_thread_ = std::thread([this] { pairThread(); });
    return true;
}

bool Ds2BleReader::pairAnswer(bool match) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (pair_.state != PairState::Confirm || answered_) return false;
    answered_ = true;
    answer_ = match;
    cv_.notify_all();
    return true;
}

void Ds2BleReader::pairCancel() {
    std::lock_guard<std::mutex> lk(mtx_);
    cancel_ = true;
    cv_.notify_all();
}

Ds2BleReader::PairStatus Ds2BleReader::pairStatus() const {
    std::lock_guard<std::mutex> lk(mtx_);
    PairStatus s = pair_;
    if (s.state == PairState::Confirm || s.state == PairState::OsDialog) {
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(deadline_ - std::chrono::steady_clock::now());
        s.seconds_left = static_cast<int>(std::max<long long>(0, left.count()));
    }
    return s;
}

void Ds2BleReader::pairThread() {
    struct Done {
        std::atomic<bool>& b;
        ~Done() { b = false; }
    } done{busy_};

    std::string why;
    if (!radioReady(&why)) {
        std::cerr << kTag << "pairing: " << why << std::endl;
        setPair(PairState::RadioOff, why);
        return;
    }
    auto t = transport();
    std::cout << kTag << "pairing: looking for a DreamStation 2 (blower off, Bluetooth on)" << std::endl;
    auto dev = find(std::nullopt, t_.scan_ms);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (cancel_) {
            pair_.state = PairState::Cancelled;
            return;
        }
    }
    if (!dev) {
        setPair(PairState::NotFound, "no DreamStation 2 was advertising");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        pair_.machine_name = dev->name;
        pair_.state = PairState::Pairing;
        deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(t_.pair_ms);
    }
    std::cout << kTag << "pairing with " << dev->name << " (" << dev->address << ")" << std::endl;

    ble::PairHandlers h;
    h.confirm = [this](const std::string& code) {
        std::unique_lock<std::mutex> lk(mtx_);
        pair_.state = PairState::Confirm;
        pair_.code = code;
        answered_ = false;
        deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(t_.confirm_ms);
        cv_.notify_all();
        cv_.wait_until(lk, deadline_, [this] { return answered_ || cancel_; });
        const bool yes = answered_ && answer_ && !cancel_;
        pair_.state = PairState::Pairing;
        pair_.code.clear();
        return yes;
    };
    h.os_prompt = [this] {
        std::lock_guard<std::mutex> lk(mtx_);
        if (pair_.state != PairState::OsDialog) {
            pair_.state = PairState::OsDialog;
            cv_.notify_all();
        }
    };

    // The machine drops the link while it shows its code; a pairing that
    // failed before any code was asked about is tried again inside the window.
    ble::PairResult r;
    const auto window_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(t_.pair_ms);
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            window_end - std::chrono::steady_clock::now()).count();
        if (left <= 1000) break;
        r = t->pair(*dev, dataSpec(), h, static_cast<int>(left));
        bool stop;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop = cancel_ || answered_;
        }
        if (r.outcome != ble::PairOutcome::Failed || stop) break;
        std::cerr << kTag << "pairing attempt " << (attempt + 1) << " failed (" << r.detail << "), trying again"
                  << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    bool cancelled, said_no;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        cancelled = cancel_;
        said_no = answered_ && !answer_;
    }
    switch (r.outcome) {
        case ble::PairOutcome::Paired: {
            auto st = store_.load();
            Ds2Store::Paired p;
            p.address = dev->address;
            p.name = dev->name;
            p.id = dev->id;
            p.paired_at = nowEpoch();
            // SDD-064 D9: nights from the day of pairing on, unless the user
            // picks another day; the same machine paired again keeps its day.
            p.import_from = st.paired && st.paired->address == p.address && !st.paired->import_from.empty()
                                ? st.paired->import_from
                                : Ds2Store::localDate(p.paired_at);
            p.serial = st.paired && st.paired->address == p.address ? st.paired->serial : "";
            st.paired = p;
            if (!store_.save(st)) {
                setPair(PairState::Failed, "paired, but the pairing could not be saved");
                return;
            }
            std::cout << kTag << "paired with " << dev->name << std::endl;
            setPair(PairState::Paired);
            return;
        }
        case ble::PairOutcome::Rejected:
            setPair(cancelled ? PairState::Cancelled : PairState::Rejected,
                    said_no ? "the codes did not match" : r.detail);
            break;
        case ble::PairOutcome::TimedOut:
            setPair(cancelled ? PairState::Cancelled : PairState::TimedOut, r.detail);
            break;
        case ble::PairOutcome::Unsupported:
            setPair(PairState::Unsupported, r.detail);
            break;
        case ble::PairOutcome::Failed:
            setPair(cancelled ? PairState::Cancelled : PairState::Failed, r.detail);
            break;
    }
    std::cerr << kTag << "pairing did not complete: " << r.detail << std::endl;
}

bool Ds2BleReader::forget() {
    auto st = store_.load();
    bool removed = false;
    if (st.paired) {
        if (auto t = transport()) {
            ble::Device d;
            d.id = st.paired->id;
            d.address = st.paired->address;
            d.name = st.paired->name;
            removed = t->unpair(d);
        }
        st.paired.reset();
        store_.save(st);
    }
    return removed;
}

// ── reading ────────────────────────────────────────────────────────────────

Ds2BleReader::ReadResult Ds2BleReader::read() {
    ReadResult res;
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true)) {
        res.code = "busy";
        res.error = "a pairing or a read is running";
        return res;
    }
    struct Done {
        std::atomic<bool>& b;
        ~Done() { b = false; }
    } done{busy_};

    auto fail = [&](const char* code, const std::string& why) {
        res.ok = false;
        res.code = code;
        res.error = why;
        std::cerr << kTag << "read: " << why << std::endl;
        return res;
    };

    Ds2Store::State st = store_.load();
    if (!st.paired) return fail("not_paired", "no DreamStation 2 is paired");
    std::string why;
    if (!radioReady(&why)) return fail("radio_off", why);
    auto t = transport();
    auto dev = find(st.paired, t_.scan_ms);
    if (!dev) return fail("not_found", st.paired->name + " was not advertising (out of range, or held by another reader)");

    // The paired machine's serial is known from its first read; its bookmarks
    // are where every later read resumes (SDD-064 D7). Before that first read
    // nothing is sent and each stream starts at the oldest record.
    const std::string last_serial = st.paired->serial;
    ds2::SyncOptions opt;
    if (!last_serial.empty() && st.machines.count(last_serial)) {
        for (const auto& [stream, bm] : st.machines[last_serial].bookmarks)
            if (stream != 0 && !bm.empty()) opt.bookmarks[stream] = bm;
    }

    // Notifications arrive on the backend's thread; the protocol runs here.
    std::mutex qm;
    std::condition_variable qcv;
    std::deque<std::vector<uint8_t>> queue;
    bool dropped = false;
    ble::LinkHandlers handlers;
    handlers.notify = [&](const std::vector<uint8_t>& v) {
        std::lock_guard<std::mutex> lk(qm);
        queue.push_back(v);
        qcv.notify_all();
    };
    handlers.disconnected = [&] {
        std::lock_guard<std::mutex> lk(qm);
        dropped = true;
        qcv.notify_all();
    };

    auto conn = t->connect(*dev, dataSpec(), handlers, ble::ConnectOptions{});
    if (!conn.link) {
        if (conn.error == ble::LinkError::Notify)
            return fail("bond_lost", "the machine refused the subscription: the pairing is gone, pair again ("
                                         + conn.detail + ")");
        return fail("connect", "could not connect to " + dev->name + ": " + conn.detail);
    }
    auto& link = *conn.link;
    const size_t piece = link.mtu() > 3 ? link.mtu() - 3u : 20u;

    std::string serial;
    bool storage_failed = false;
    std::string storage_error;
    ds2::SyncSession* session_ptr = nullptr;
    const long long cutoff = st.paired->import_from.empty()
                                 ? 0
                                 : Ds2Store::localMidnight(st.paired->import_from).value_or(0);
    opt.on_record = [&](const ds2::Record& r) {
        if (storage_failed) return;
        const auto& id = session_ptr->identity();
        if (serial.empty()) {
            serial = id.serial;
            store_.writeIdentity(id);
        }
        // SDD-064 D9: a session from before import_from (a used machine's
        // earlier owner, or nights the user did not want) is walked past:
        // its bookmark advances, its record is never kept.
        bool keep = true;
        if (cutoff > 0 && (r.stream == 1 || r.stream == 2)) {
            try {
                const auto blocks = ds2::verifyBody(r.body);
                keep = blocks.empty() || static_cast<long long>(blocks.front().timestamp) >= cutoff;
            } catch (const std::exception&) {
                // kept: the parser decides what a damaged record is worth
            }
        }
        if (!keep) {
            ++res.walked_past;
            if (!store_.saveBookmark(serial, r.stream, r.bookmark_out)) {
                storage_failed = true;
                storage_error = "could not save a bookmark";
            }
            return;
        }
        const std::string rel = store_.writeRecord(serial, r);
        if (rel.empty() || !store_.saveBookmark(serial, r.stream, r.bookmark_out)) {
            storage_failed = true;
            storage_error = "could not store " + ds2::cardFileName(r.stream, r.session);
            return;
        }
        ++res.records;
        if ((r.stream == 1 || r.stream == 2) &&
            std::find(res.sessions.begin(), res.sessions.end(), r.session) == res.sessions.end())
            res.sessions.push_back(r.session);
    };
    ds2::SyncSession s(opt);
    session_ptr = &s;

    auto send = [&](const std::vector<uint8_t>& bytes) {
        for (size_t k = 0; k < bytes.size(); k += piece) {
            const std::vector<uint8_t> part(bytes.begin() + static_cast<long>(k),
                                            bytes.begin() + static_cast<long>(std::min(bytes.size(), k + piece)));
            const auto w = link.write(part, false);
            if (!w.ok) {
                s.abort("write failed: " + w.error);
                return;
            }
        }
    };

    send(s.begin());
    while (!s.done() && !s.failed()) {
        std::vector<uint8_t> bytes;
        {
            std::unique_lock<std::mutex> lk(qm);
            const bool got = qcv.wait_for(lk, std::chrono::milliseconds(t_.answer_ms),
                                          [&] { return !queue.empty() || dropped; });
            if (!queue.empty()) {
                bytes = std::move(queue.front());
                queue.pop_front();
            } else if (dropped) {
                s.abort("the link dropped");
                break;
            } else if (!got) {
                char b[32];
                std::snprintf(b, sizeof b, "%04x/%02x", s.pendingCmd(), s.pendingSub());
                s.abort(std::string("no answer to ") + b + " in " + std::to_string(t_.answer_ms / 1000) + " s");
                break;
            }
        }
        const auto next = s.feed(bytes);
        if (storage_failed) {
            s.abort(storage_error);
            break;
        }
        if (!next.empty()) send(next);
    }
    link.close();

    res.identity = s.identity();
    if (serial.empty()) serial = s.identity().serial;

    // The bond answered with another serial than the one whose bookmarks were
    // sent (the machine was swapped under the same address). Its records went
    // under its own serial; its own bookmarks apply from the next read.
    if (!last_serial.empty() && !serial.empty() && serial != last_serial)
        std::cerr << kTag << "the paired address answered as another machine; its own bookmarks apply from now"
                  << std::endl;

    if (!serial.empty()) {
        st = store_.load();
        if (st.paired) st.paired->serial = serial;
        auto& m = st.machines[serial];
        m.serial = serial;
        if (!s.identity().model.empty()) m.model = s.identity().model;
        if (!s.identity().firmware.empty()) m.firmware = s.identity().firmware;
        if (s.identity().newest_session) m.newest_session = s.identity().newest_session;
        m.last_attempt_at = nowEpoch();
        if (s.done()) {
            m.last_read_at = m.last_attempt_at;
            m.last_records = res.records;
            m.last_error.clear();
            // Streams that brought nothing keep their bookmark; stream 0's is
            // whatever the machine answered (it is never sent back).
            for (const auto& [stream, bm] : s.bookmarks())
                if (!bm.empty()) m.bookmarks[stream] = bm;
        } else {
            m.last_error = s.error();
        }
        store_.save(st);
        if (!s.identity().serial.empty()) store_.writeIdentity(s.identity());
    }

    if (storage_failed) return fail("storage", storage_error);
    if (!s.done()) return fail("protocol", s.error());
    res.ok = true;
    res.code = "ok";
    std::cout << kTag << "read " << s.identity().model << ": " << res.records << " new record(s), "
              << res.walked_past << " before the import date walked past, "
              << res.sessions.size() << " session(s), newest session " << s.identity().newest_session << std::endl;
    return res;
}

}  // namespace hms_cpap
