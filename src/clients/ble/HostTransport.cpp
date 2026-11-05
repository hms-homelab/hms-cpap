#include "clients/ble/BleTransport.h"

#include "clients/As11Session.h"
#include "clients/O2RingBleClient.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>

namespace hms_cpap::ble {

#if defined(HMS_BLE_BLUEZ)
std::shared_ptr<ITransport> makeBlueZTransport(const std::string& tag);
#elif defined(HMS_BLE_COREBLUETOOTH)
std::shared_ptr<ITransport> makeCoreBluetoothTransport(const std::string& tag);
#elif defined(HMS_BLE_WINRT)
std::shared_ptr<ITransport> makeWinRtTransport(const std::string& tag);
#endif

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string normalizeUuid(const std::string& uuid) {
    std::string s;
    for (char c : uuid)
        if (c != '{' && c != '}' && !std::isspace(static_cast<unsigned char>(c))) s += c;
    s = lower(s);
    if (s.rfind("0x", 0) == 0) s = s.substr(2);
    const bool hex = !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
        return std::isxdigit(static_cast<unsigned char>(c)) != 0;
    });
    // 16- and 32-bit forms sit in the Bluetooth base UUID.
    if (hex && s.size() == 4) return "0000" + s + "-0000-1000-8000-00805f9b34fb";
    if (hex && s.size() == 8) return s + "-0000-1000-8000-00805f9b34fb";
    return s;
}

bool compiled() {
#ifdef WITH_BLE
    return true;
#else
    return false;
#endif
}

const char* backendName() {
#if defined(HMS_BLE_BLUEZ)
    return "bluez";
#elif defined(HMS_BLE_COREBLUETOOTH)
    return "corebluetooth";
#elif defined(HMS_BLE_WINRT)
    return "winrt";
#else
    return "";
#endif
}

std::shared_ptr<ITransport> makeHostTransport(const std::string& tag) {
#if defined(HMS_BLE_BLUEZ)
    return makeBlueZTransport(tag);
#elif defined(HMS_BLE_COREBLUETOOTH)
    return makeCoreBluetoothTransport(tag);
#elif defined(HMS_BLE_WINRT)
    return makeWinRtTransport(tag);
#else
    (void)tag;
    return nullptr;
#endif
}

Probe probe() {
    Probe p;
    p.compiled = compiled();
    p.backend = backendName();
    if (!p.compiled) {
        p.radio.state = Radio::NoAdapter;
        p.radio.detail = "this build has no Bluetooth support";
        return p;
    }
    try {
        auto t = makeHostTransport("Bluetooth check: ");
        if (!t) {
            p.radio.detail = "no Bluetooth transport";
            return p;
        }
        p.radio = t->radio(false);
    } catch (const std::exception& e) {
        p.radio.state = Radio::Failed;
        p.radio.detail = e.what();
    }
    return p;
}

namespace {

const char* radioText(Radio r) {
    switch (r) {
    case Radio::Ready:        return "ready";
    case Radio::Off:          return "off";
    case Radio::RfKilled:     return "rf-killed";
    case Radio::Unauthorized: return "not allowed";
    case Radio::NoAdapter:    return "no adapter";
    case Radio::Failed:       return "failed";
    }
    return "failed";
}

std::string looksLike(const Device& d) {
    if (as11::advertIsAirSense(d.name, d.uuids)) return "AirSense 11";
    bool by_t8520 = false;
    if (auto proto = O2RingBleClient::classifyDevice(d.name, d.uuids, &by_t8520))
        return *proto == O2RingBleClient::Proto::OxyII ? "O2Ring-S" : "O2 Ring family";
    return "";
}

} // namespace

int runScan(int seconds) {
    if (!compiled()) {
        std::cerr << "This build has no Bluetooth support." << std::endl;
        return 2;
    }
    auto t = makeHostTransport("BLE scan: ");
    if (!t || !t->available()) {
        std::cout << "### BLE_BACKEND=" << backendName() << std::endl;
        std::cout << "### BLE_RADIO=no adapter" << std::endl;
        return 3;
    }
    const RadioStatus r = t->radio(false);
    std::cout << "### BLE_BACKEND=" << t->backend() << std::endl;
    std::cout << "### BLE_RADIO=" << radioText(r.state)
              << (r.adapter.empty() ? "" : " (" + r.adapter + ")")
              << (r.detail.empty() ? "" : ": " + r.detail) << std::endl;
    if (r.state != Radio::Ready) return 4;

    // Collected, not printed as they come: BlueZ offers its cache on every
    // poll, and a name can arrive in a later advert than the first.
    std::mutex m;
    std::map<std::string, Device> seen;
    const auto res = t->discover(
        [&](const Device& d) {
            if (!d.advertising) return false;
            std::lock_guard<std::mutex> lk(m);
            auto& slot = seen[d.id];
            const std::string name = d.name.empty() ? slot.name : d.name;
            auto uuids = slot.uuids;
            for (const auto& u : d.uuids)
                if (std::find(uuids.begin(), uuids.end(), u) == uuids.end()) uuids.push_back(u);
            slot = d;
            slot.name = name;
            slot.uuids = uuids;
            return false;   // never stop early, never connect
        },
        seconds * 1000);
    if (!res.started) {
        std::cout << "### BLE_SCAN=failed: " << res.error << std::endl;
        return 5;
    }
    std::vector<Device> list;
    {
        std::lock_guard<std::mutex> lk(m);
        for (auto& [id, d] : seen) list.push_back(d);
    }
    std::sort(list.begin(), list.end(),
              [](const Device& a, const Device& b) { return a.rssi.value_or(-200) > b.rssi.value_or(-200); });
    int ours = 0;
    for (const auto& d : list) {
        const std::string kind = looksLike(d);
        if (!kind.empty()) ours++;
        std::cout << "  " << std::setw(4) << (d.rssi ? std::to_string(*d.rssi) : "?") << " dBm  "
                  << std::left << std::setw(38) << d.address << std::right << "  "
                  << (d.name.empty() ? "<no name>" : d.name);
        if (!kind.empty()) std::cout << "  [" << kind << "]";
        if (!d.uuids.empty()) {
            std::cout << "  services:";
            for (const auto& u : d.uuids) std::cout << " " << u;
        }
        std::cout << std::endl;
    }
    std::cout << "### BLE_DEVICES=" << list.size() << std::endl;
    std::cout << "### BLE_OURS=" << ours << std::endl;
    return 0;
}

} // namespace hms_cpap::ble
