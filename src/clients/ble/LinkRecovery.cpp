#include "clients/ble/LinkRecovery.h"

#include <algorithm>
#include <mutex>
#include <utility>

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
// (code, what), one per RadioInUse alive.
std::vector<std::pair<std::string, std::string>>& inUseList() {
    static std::vector<std::pair<std::string, std::string>> v;
    return v;
}
} // namespace

RadioInUse::RadioInUse(std::string code, std::string what) : code_(std::move(code)), what_(std::move(what)) {
    std::lock_guard<std::mutex> lk(inUseMutex());
    inUseList().emplace_back(code_, what_);
}

RadioInUse::~RadioInUse() {
    std::lock_guard<std::mutex> lk(inUseMutex());
    auto& v = inUseList();
    auto it = std::find(v.begin(), v.end(), std::make_pair(code_, what_));
    if (it != v.end()) v.erase(it);
}

std::vector<std::string> radioInUse() {
    std::lock_guard<std::mutex> lk(inUseMutex());
    std::vector<std::string> out;
    for (const auto& e : inUseList()) out.push_back(e.second);
    return out;
}

std::vector<std::string> radioInUseCodes() {
    std::lock_guard<std::mutex> lk(inUseMutex());
    std::vector<std::string> out;
    for (const auto& e : inUseList()) out.push_back(e.first);
    return out;
}

} // namespace hms_cpap::ble
