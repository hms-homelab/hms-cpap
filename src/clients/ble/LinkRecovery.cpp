#include "clients/ble/LinkRecovery.h"

#include <algorithm>
#include <mutex>

namespace hms_cpap::ble {

bool countsTowardRecovery(LinkError e) {
    return e == LinkError::Connect || e == LinkError::Resolve || e == LinkError::NoCharacteristics;
}

const char* recoveryStepText(RecoveryStep s) {
    switch (s) {
    case RecoveryStep::None:          return "none";
    case RecoveryStep::Forget:        return "forget";
    case RecoveryStep::ResetAdapter:  return "reset_adapter";
    case RecoveryStep::NotRecovering: return "not_recovering";
    }
    return "none";
}

RecoveryStep LinkRecovery::onFailure() {
    failures_++;
    if (!forgot_) {
        if (failures_ < kForgetAfter) return RecoveryStep::None;
        forgot_ = true;
        return RecoveryStep::Forget;
    }
    if (!reset_) {
        reset_ = true;
        return RecoveryStep::ResetAdapter;
    }
    if (!stuck_) {
        stuck_ = true;
        return RecoveryStep::NotRecovering;
    }
    return RecoveryStep::None;
}

void LinkRecovery::onSuccess() { *this = LinkRecovery{}; }

void LinkRecovery::resetDeferred() { reset_ = false; }

void LinkRecovery::restart() { *this = LinkRecovery{}; }

namespace {
std::mutex& inUseMutex() {
    static std::mutex m;
    return m;
}
std::vector<std::string>& inUseList() {
    static std::vector<std::string> v;
    return v;
}
} // namespace

RadioInUse::RadioInUse(std::string what) : what_(std::move(what)) {
    std::lock_guard<std::mutex> lk(inUseMutex());
    inUseList().push_back(what_);
}

RadioInUse::~RadioInUse() {
    std::lock_guard<std::mutex> lk(inUseMutex());
    auto& v = inUseList();
    auto it = std::find(v.begin(), v.end(), what_);
    if (it != v.end()) v.erase(it);
}

std::vector<std::string> radioInUse() {
    std::lock_guard<std::mutex> lk(inUseMutex());
    return inUseList();
}

} // namespace hms_cpap::ble
