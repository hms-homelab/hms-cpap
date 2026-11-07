#pragma once

// A synthetic DreamStation 2 for the SDD-064 tests: a fake ble::ITransport
// whose link answers the protocol from made-up records, and builders for the
// v3 blocks the Philips parser reads. No machine wrote any of these bytes.

#include "clients/ble/BleTransport.h"

#include <cpapdash/ds2ble/Frame.h>
#include <cpapdash/ds2ble/Protocol.h>
#include <cpapdash/ds2ble/Record.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace ds2test {

using Bytes = std::vector<uint8_t>;
namespace ds2 = cpapdash::ds2ble;
namespace ble = hms_cpap::ble;

struct Rec {
    uint8_t code;
    Bytes body;
};

inline Bytes le16(unsigned v) { return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)}; }
inline Bytes cat(Bytes a, const Bytes& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

/// A FileType 0, format 3, family version 6 block with its sums right.
inline Bytes v3Block(uint8_t ext, uint32_t session, uint32_t ts, const std::vector<Rec>& recs) {
    std::vector<std::pair<uint8_t, uint8_t>> table;
    Bytes data;
    for (const auto& r : recs) {
        auto it = std::find_if(table.begin(), table.end(), [&](const auto& p) { return p.first == r.code; });
        if (it == table.end()) table.emplace_back(r.code, static_cast<uint8_t>(r.body.size()));
        data.push_back(r.code);
        data.insert(data.end(), r.body.begin(), r.body.end());
    }
    Bytes b = {3, 0, 0, 0, 0, 6, ext};
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(session >> (8 * i)));
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(ts >> (8 * i)));
    b.push_back(static_cast<uint8_t>(table.size()));
    for (const auto& [code, size] : table) {
        b.push_back(code);
        b.push_back(size);
    }
    const size_t len = b.size() + 1 + data.size() + 4;
    b[1] = static_cast<uint8_t>(len);
    b[2] = static_cast<uint8_t>(len >> 8);
    uint8_t sum = 0;
    for (uint8_t c : b) sum = static_cast<uint8_t>(sum + c);
    b.push_back(sum);
    b.insert(b.end(), data.begin(), data.end());
    const uint32_t crc = ds2::blockCrc(data.data(), data.size());
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(crc >> (8 * i)));
    return b;
}

// Summary records.
inline Rec equipmentOn() { return {0x00, {0x00}}; }
inline Rec settingsAuto() { return {0x01, {0x00, 0x01, 0x02, 0x0d, 0x02, 40, 200}}; }
inline Rec maskOn(unsigned delta) { return {0x03, cat(le16(delta), {0x00, 0x00})}; }
inline Rec maskOff(unsigned delta) { return {0x04, le16(delta)}; }
inline Rec pressure() { return {0x05, {40, 80, 60, 50}}; }
inline Rec equipmentOff(unsigned delta) { return {0x02, cat(le16(delta), Bytes(7, 0))}; }
inline Rec stats(int oa, int ca, int h) {
    std::vector<uint16_t> w(15, 0);
    w[1] = static_cast<uint16_t>(oa);
    w[3] = static_cast<uint16_t>(ca);
    w[8] = static_cast<uint16_t>(h);
    Bytes b;
    for (auto v : w) b = cat(b, le16(v));
    b.push_back(0);
    return {0x08, b};
}
// Event records: u16 seconds since the previous record, then fields.
inline Rec ev(uint8_t code, unsigned delta, uint8_t byte) { return {code, cat(le16(delta), {byte})}; }

/// One night: a summary (mask on 1 s after `t`, for `secs`) and its events.
inline std::pair<Bytes, Bytes> night(uint32_t session, uint32_t t, unsigned secs, int oa = 1) {
    Bytes summary = v3Block(1, session, t, {equipmentOn(), settingsAuto(), maskOn(1), maskOff(secs), pressure(),
                                             stats(oa, 0, 1), equipmentOff(30)});
    std::vector<Rec> events;
    for (int i = 0; i < oa; ++i) events.push_back(ev(0x06, i == 0 ? 600 : 300, 10));
    events.push_back(ev(0x14, 300, 20));
    return {summary, v3Block(2, session, t, events)};
}

/// The machine: records per stream, bookmarks it makes up.
struct Machine {
    struct Item {
        uint32_t session;
        Bytes body;
    };
    std::map<uint8_t, std::vector<Item>> streams;
    std::string serial = "TEST0000000002";
    bool status06_at_end = true;
    std::optional<size_t> stop_after;   // stop answering after this many requests
    size_t requests = 0;
    int bare_opens = 0;
    uint8_t cur_stream = 0;
    long cur = -1;

    static Bytes bookmarkFor(uint8_t stream, size_t i) {
        Bytes b{static_cast<uint8_t>(0x10 + i), static_cast<uint8_t>(i >> 8), 0, 0, stream, 0x08,
                static_cast<uint8_t>(0xc0 + i), 1, 2, 3};
        return b;
    }

    void addNight(uint32_t session, uint32_t t, unsigned secs, int oa = 1) {
        auto [s, e] = night(session, t, secs, oa);
        streams[1].push_back({session, s});
        streams[2].push_back({session, e});
    }

    static Bytes reply(uint16_t cmd, uint8_t sub, uint8_t status, const Bytes& d = {}) {
        Bytes x{status};
        x.insert(x.end(), d.begin(), d.end());
        return ds2::encode(cmd, sub, x);
    }
    static Bytes be32(uint32_t v) {
        return {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8),
                static_cast<uint8_t>(v)};
    }

    /// The answer to one request, or nullopt when it stays silent.
    std::optional<Bytes> answer(const Bytes& req) {
        ++requests;
        if (stop_after && requests > *stop_after) return std::nullopt;
        const ds2::Frame f = ds2::decode(req);
        switch (f.cmd) {
            case ds2::kCmdSerial: {
                Bytes s(serial.begin(), serial.end());
                s.push_back(0);
                return reply(f.cmd, f.sub, 0, s);
            }
            case ds2::kCmdCommands: return reply(f.cmd, f.sub, 0, {0x00, 0x9a});
            case ds2::kCmdFirmware: return reply(f.cmd, f.sub, 0, {'V', '1', 0});
            case ds2::kCmdModel: return reply(f.cmd, f.sub, 0, {'5', '2', '0', 'X', '1', '1', '0', 'C', 0});
            case ds2::kCmdCounters: return reply(f.cmd, f.sub, 0, {0});
            case ds2::kCmdCursor: break;
            default: return reply(f.cmd, f.sub, 0x01);
        }
        if (f.sub == ds2::kCurNewest) {
            uint32_t n = 0;
            for (auto& [k, v] : streams)
                for (auto& it : v) n = std::max(n, it.session);
            return reply(f.cmd, f.sub, 0, be32(n));
        }
        if (f.sub == ds2::kCurOpen) {
            cur_stream = f.data[0];
            const Bytes bm(f.data.begin() + 1, f.data.end());
            auto& recs = streams[cur_stream];
            size_t next = 0;
            if (bm.empty()) {
                ++bare_opens;
            } else {
                for (size_t i = 0; i < recs.size(); ++i)
                    if (bookmarkFor(cur_stream, i) == bm) next = i + 1;
            }
            if (next >= recs.size()) {
                cur = -1;
                if (status06_at_end && !bm.empty()) return reply(f.cmd, f.sub, ds2::kStatusNoNext);
                return reply(f.cmd, f.sub, 0, Bytes(12, 0));
            }
            cur = static_cast<long>(next);
            const auto& r = recs[next];
            return reply(f.cmd, f.sub, 0,
                         cat(cat(be32(static_cast<uint32_t>((r.body.size() + 179) / 180)),
                                 be32(static_cast<uint32_t>(r.body.size()))),
                             be32(r.session)));
        }
        if (f.sub == ds2::kCurChunk) {
            uint32_t n = 0;
            for (uint8_t b : f.data) n = (n << 8) | b;
            const auto& body = streams[cur_stream][static_cast<size_t>(cur)].body;
            const size_t from = size_t(n) * 180, to = std::min(body.size(), from + 180);
            return reply(f.cmd, f.sub, 0, Bytes(body.begin() + static_cast<long>(from), body.begin() + static_cast<long>(to)));
        }
        if (f.sub == ds2::kCurBookmark) {
            if (cur_stream == 0 || cur < 0) return reply(f.cmd, f.sub, 0, bookmarkFor(cur_stream, 0));
            return reply(f.cmd, f.sub, 0, bookmarkFor(cur_stream, static_cast<size_t>(cur)));
        }
        return reply(f.cmd, f.sub, 0x01);
    }
};

/// A machine that answers from a captured exchange (one frame per line,
/// "<s> APP> <hex>" / "<s> <DEV <hex>"): every request/answer pair learned
/// with the record open when it was sent; a bare open answers with the
/// first open of that stream. For replays of a real capture, which is never
/// kept in this repository.
struct ReplayMachine {
    std::map<std::tuple<uint16_t, uint8_t, Bytes>, Bytes> fixed;
    std::map<Bytes, Bytes> opens, bookmarks;
    std::map<uint8_t, Bytes> oldest;
    std::map<std::pair<Bytes, Bytes>, Bytes> chunks;
    Bytes open;

    explicit ReplayMachine(const std::vector<std::pair<bool, Bytes>>& cap) {
        Bytes cur;
        for (size_t i = 0; i + 1 < cap.size();) {
            if (!cap[i].first || cap[i + 1].first) {
                ++i;
                continue;
            }
            const auto f = ds2::decode(cap[i].second);
            const Bytes& resp = cap[i + 1].second;
            if (f.cmd == ds2::kCmdCursor && f.sub == ds2::kCurOpen) {
                cur = f.data;
                opens[f.data] = resp;
                oldest.emplace(f.data[0], f.data);
            } else if (f.cmd == ds2::kCmdCursor && f.sub == ds2::kCurChunk) {
                chunks[{cur, f.data}] = resp;
            } else if (f.cmd == ds2::kCmdCursor && f.sub == ds2::kCurBookmark) {
                bookmarks[cur] = resp;
            } else {
                fixed[{f.cmd, f.sub, f.cmd == ds2::kCmdSetClock ? Bytes{} : f.data}] = resp;
            }
            i += 2;
        }
    }
    std::optional<Bytes> answer(const Bytes& req) {
        const auto f = ds2::decode(req);
        if (f.cmd == ds2::kCmdCursor && f.sub == ds2::kCurOpen) {
            Bytes key = f.data;
            if (key.size() == 1 && !opens.count(key) && oldest.count(key[0])) key = oldest[key[0]];
            open = key;
            auto it = opens.find(key);
            return it == opens.end() ? std::nullopt : std::optional<Bytes>(it->second);
        }
        if (f.cmd == ds2::kCmdCursor && f.sub == ds2::kCurChunk) {
            auto it = chunks.find({open, f.data});
            return it == chunks.end() ? std::nullopt : std::optional<Bytes>(it->second);
        }
        if (f.cmd == ds2::kCmdCursor && f.sub == ds2::kCurBookmark) {
            auto it = bookmarks.find(open);
            return it == bookmarks.end() ? std::nullopt : std::optional<Bytes>(it->second);
        }
        auto it = fixed.find({f.cmd, f.sub, f.cmd == ds2::kCmdSetClock ? Bytes{} : f.data});
        return it == fixed.end() ? std::nullopt : std::optional<Bytes>(it->second);
    }
};

/// A ble::ITransport around one Machine (or any answerer).
struct Transport : ble::ITransport {
    std::shared_ptr<Machine> machine = std::make_shared<Machine>();
    /// Who answers the link's writes; the Machine unless a test says otherwise.
    std::function<std::optional<Bytes>(const Bytes&)> answerer;
    ble::Device device{"dev-1", "aa:bb:cc:dd:ee:01", "PR BT 0001", {}, -50, true};
    bool advertising = true;
    bool os_dialog = false;            // behave like macOS
    std::string code = "042517";
    bool bonded_ = false;
    int pair_calls = 0, unpair_calls = 0, connects = 0;
    ble::PairOutcome machine_says = ble::PairOutcome::Paired;   // when the user said yes
    ble::LinkError connect_error = ble::LinkError::None;

    struct Link : ble::ILink {
        std::function<std::optional<Bytes>(const Bytes&)> answer;
        ble::LinkHandlers h;
        bool up = true;
        ble::WriteResult write(const std::vector<uint8_t>& piece, bool) override {
            ble::WriteResult r;
            r.ok = true;
            if (auto a = answer(piece))
                if (h.notify) h.notify(*a);
            return r;
        }
        uint16_t mtu() const override { return 247; }
        bool connected() const override { return up; }
        void close() override { up = false; }
    };

    const char* backend() const override { return "fake"; }
    bool available() const override { return true; }
    ble::RadioStatus radio(bool) override { return {ble::Radio::Ready, "", "fake0"}; }
    std::vector<ble::Device> known() override { return {}; }
    ble::DiscoverResult discover(const std::function<bool(const ble::Device&)>& match, int) override {
        ble::DiscoverResult r;
        if (advertising && match(device)) r.device = device;
        return r;
    }
    ble::ConnectResult connect(const ble::Device&, const ble::GattSpec&, ble::LinkHandlers h,
                               const ble::ConnectOptions&) override {
        ++connects;
        ble::ConnectResult r;
        if (connect_error != ble::LinkError::None) {
            r.error = connect_error;
            r.detail = "fake refusal";
            return r;
        }
        auto l = std::make_unique<Link>();
        if (answerer) {
            l->answer = answerer;
        } else {
            auto m = machine;
            l->answer = [m](const Bytes& b) { return m->answer(b); };
        }
        l->h = std::move(h);
        r.link = std::move(l);
        return r;
    }
    bool osShowsPairingCode() const override { return os_dialog; }
    ble::PairResult pair(const ble::Device&, const ble::GattSpec&, ble::PairHandlers h, int) override {
        ++pair_calls;
        if (os_dialog) {
            if (h.os_prompt) h.os_prompt();
            bonded_ = true;
            return {ble::PairOutcome::Paired, ""};
        }
        const bool yes = h.confirm && h.confirm(code);
        if (!yes) return {ble::PairOutcome::Rejected, "the user said no"};
        if (machine_says == ble::PairOutcome::Paired) bonded_ = true;
        return {machine_says, ""};
    }
    std::optional<bool> bonded(const ble::Device&) override { return bonded_; }
    bool unpair(const ble::Device&) override {
        ++unpair_calls;
        bonded_ = false;
        return true;
    }
};

}  // namespace ds2test
