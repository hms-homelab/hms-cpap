#include "clients/BlueZAdapter.h"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace hms_cpap::bluez {

namespace {

std::string readTrimmed(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::string s;
    if (!in) return s;
    std::getline(in, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

} // namespace

std::string rfkillAdvice() {
    return "Bluetooth is switched off by rf-kill on this computer: run `rfkill unblock bluetooth` "
           "(or switch Bluetooth on in the system settings) and try again";
}

Outcome ensurePowered(const AdapterOps& ops) {
    Outcome out;
    const auto on = ops.powered ? ops.powered() : std::nullopt;
    if (!on) {
        out.detail = "the adapter's Powered state could not be read";
        return out;
    }
    if (*on) {
        out.power = Power::Ready;
        return out;
    }
    if (!ops.power_on) {
        out.detail = "the adapter cannot be powered from here";
        return out;
    }
    std::string err = ops.power_on();
    if (err.empty()) {
        out.power = Power::Ready;
        out.powered_on = true;
        return out;
    }
    // BlueZ refuses to power an rf-killed adapter with org.bluez.Error.Failed,
    // which says nothing about the switch: the sysfs rfkill entries do.
    const auto blocked = ops.soft_blocked ? ops.soft_blocked() : std::vector<std::string>{};
    if (blocked.empty()) {
        out.detail = "the adapter would not power on: " + err;
        return out;
    }
    bool lifted = false;
    for (const auto& soft : blocked)
        if (ops.unblock && ops.unblock(soft)) lifted = true;
    if (lifted) {
        err = ops.power_on();
        if (err.empty()) {
            out.power = Power::Ready;
            out.powered_on = true;
            out.rfkill_lifted = true;
            return out;
        }
    }
    out.power = Power::RfKilled;
    out.detail = rfkillAdvice();
    return out;
}

std::vector<std::string> softBlockedBluetooth(const std::string& sysfs_root) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(sysfs_root, ec)) {
        if (ec) break;
        const auto dir = entry.path();
        if (dir.filename().string().rfind("rfkill", 0) != 0) continue;
        if (readTrimmed(dir / "type") != "bluetooth") continue;
        if (readTrimmed(dir / "soft") != "1") continue;
        out.push_back((dir / "soft").string());
    }
    return out;
}

bool liftSoftBlock(const std::string& soft_file) {
    std::ofstream out(soft_file);
    if (!out) return false;
    out << "0\n";
    out.flush();
    return static_cast<bool>(out);
}

#ifdef WITH_BLE

namespace {
const sdbus::ServiceName kBlueZ{"org.bluez"};
constexpr const char* kAdapter = "org.bluez.Adapter1";
constexpr const char* kProps = "org.freedesktop.DBus.Properties";
} // namespace

bool timedOut(const sdbus::Error& e) {
    const std::string name = e.getName();
    return name == "org.freedesktop.DBus.Error.Timeout" || name == "org.freedesktop.DBus.Error.NoReply" ||
           name == "org.freedesktop.DBus.Error.TimedOut";
}

bool forgetDevice(sdbus::IConnection& conn, const std::string& adapter_path, const std::string& device_path) {
    if (adapter_path.empty() || device_path.empty()) return false;
    try {
        auto proxy = sdbus::createProxy(conn, kBlueZ, sdbus::ObjectPath{adapter_path});
        proxy->callMethod("RemoveDevice")
            .onInterface(kAdapter)
            .withTimeout(kCallTimeout)
            .withArguments(sdbus::ObjectPath{device_path});
        return true;
    } catch (...) {
        return false;
    }
}

AdapterOps hostOps(sdbus::IConnection& conn, const std::string& adapter_path) {
    AdapterOps ops;
    // Properties through the Get/Set methods rather than the property
    // helpers: a method call takes an explicit timeout.
    ops.powered = [&conn, adapter_path]() -> std::optional<bool> {
        try {
            auto proxy = sdbus::createProxy(conn, kBlueZ, sdbus::ObjectPath{adapter_path});
            sdbus::Variant v;
            proxy->callMethod("Get")
                .onInterface(kProps)
                .withTimeout(kCallTimeout)
                .withArguments(std::string(kAdapter), std::string("Powered"))
                .storeResultsTo(v);
            return v.get<bool>();
        } catch (...) {
            return std::nullopt;
        }
    };
    ops.power_on = [&conn, adapter_path]() -> std::string {
        try {
            auto proxy = sdbus::createProxy(conn, kBlueZ, sdbus::ObjectPath{adapter_path});
            proxy->callMethod("Set")
                .onInterface(kProps)
                .withTimeout(kCallTimeout)
                .withArguments(std::string(kAdapter), std::string("Powered"), sdbus::Variant{true});
            return {};
        } catch (const sdbus::Error& e) {
            return e.getName() + " " + e.getMessage();
        } catch (const std::exception& e) {
            return e.what();
        }
    };
    ops.soft_blocked = []() { return softBlockedBluetooth(); };
    ops.unblock = [](const std::string& soft) { return liftSoftBlock(soft); };
    return ops;
}

Outcome ensurePowered(sdbus::IConnection& conn, const std::string& adapter_path, const char* tag) {
    const Outcome out = ensurePowered(hostOps(conn, adapter_path));
    if (out.power == Power::Ready) {
        if (out.rfkill_lifted) std::cout << tag << "lifted the rf-kill block and powered the adapter on" << std::endl;
        else if (out.powered_on) std::cout << tag << "adapter was off: powered it on" << std::endl;
    } else {
        std::cerr << tag << out.detail << std::endl;
    }
    return out;
}

#endif // WITH_BLE

} // namespace hms_cpap::bluez
