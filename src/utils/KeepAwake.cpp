#include "utils/KeepAwake.h"

#include <chrono>
#include <iostream>

// The OS calls are made only by the hms_cpap executable (HMS_KEEP_AWAKE_OS);
// the test binary gets the hold that does nothing.
#if defined(HMS_KEEP_AWAKE_OS) && defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#elif defined(HMS_KEEP_AWAKE_OS) && defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(HMS_KEEP_AWAKE_OS) && defined(HMS_BLE_BLUEZ)
#include <sdbus-c++/sdbus-c++.h>
#endif

namespace hms_cpap {

KeepAwake::Ops KeepAwake::hostOps() {
    Ops ops;
#if defined(HMS_KEEP_AWAKE_OS) && defined(__APPLE__)
    auto id = std::make_shared<IOPMAssertionID>(kIOPMNullAssertionID);
    ops.name = "IOKit power assertion";
    ops.take = [id](const std::string& why, std::string* error) {
        CFStringRef reason = CFStringCreateWithCString(kCFAllocatorDefault, why.c_str(), kCFStringEncodingUTF8);
        const IOReturn r = IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleSystemSleep,
                                                       kIOPMAssertionLevelOn, reason, id.get());
        if (reason) CFRelease(reason);
        if (r == kIOReturnSuccess) return true;
        if (error) *error = "IOPMAssertionCreateWithName returned " + std::to_string(r);
        return false;
    };
    ops.release = [id] {
        if (*id != kIOPMNullAssertionID) IOPMAssertionRelease(*id);
        *id = kIOPMNullAssertionID;
    };
#elif defined(HMS_KEEP_AWAKE_OS) && defined(_WIN32)
    ops.name = "SetThreadExecutionState";
    ops.take = [](const std::string&, std::string* error) {
        if (SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED) != 0) return true;
        if (error) *error = "SetThreadExecutionState failed (" + std::to_string(GetLastError()) + ")";
        return false;
    };
    ops.release = [] { SetThreadExecutionState(ES_CONTINUOUS); };
#elif defined(HMS_KEEP_AWAKE_OS) && defined(HMS_BLE_BLUEZ)
    // A logind "sleep" inhibitor: held while its file descriptor is open.
    auto fd = std::make_shared<sdbus::UnixFd>();
    ops.name = "logind sleep inhibitor";
    ops.take = [fd](const std::string& why, std::string* error) {
        try {
            auto conn = sdbus::createSystemBusConnection();
            auto proxy = sdbus::createProxy(*conn, sdbus::ServiceName{"org.freedesktop.login1"},
                                            sdbus::ObjectPath{"/org/freedesktop/login1"});
            proxy->callMethod("Inhibit")
                .onInterface("org.freedesktop.login1.Manager")
                .withTimeout(std::chrono::seconds(5))
                .withArguments(std::string("sleep"), std::string("hms-cpap"), why, std::string("block"))
                .storeResultsTo(*fd);
            return true;
        } catch (const std::exception& e) {
            if (error) *error = e.what();
            return false;
        }
    };
    ops.release = [fd] { *fd = sdbus::UnixFd{}; };
#endif
    return ops;
}

KeepAwake::KeepAwake(std::string why, Ops ops) : why_(std::move(why)), ops_(std::move(ops)) {
    if (!ops_.take) return;   // nothing on this OS
    std::string error;
    held_ = ops_.take(why_, &error);
    if (held_)
        std::cout << "Sleep: keeping the computer awake while " << why_ << " (" << ops_.name << ")" << std::endl;
    else
        std::cerr << "Sleep: could not keep the computer awake while " << why_ << ": " << error << std::endl;
}

KeepAwake::~KeepAwake() {
    if (!held_) return;
    if (ops_.release) ops_.release();
    held_ = false;
    std::cout << "Sleep: released (" << why_ << " is over)" << std::endl;
}

} // namespace hms_cpap
