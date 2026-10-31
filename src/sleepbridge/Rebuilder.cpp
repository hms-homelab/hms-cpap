#include "sleepbridge/Rebuilder.h"

namespace hms_cpap::sleepbridge {

std::string formatForMachine(const std::string& machine) {
    if (machine == "resmed") return "resmed";
    if (machine == "luna") return "bmc";
    return {};
}

}  // namespace hms_cpap::sleepbridge

// HMS_WITH_SLEEPBRIDGE is defined by CMake when BUILD_WITH_SLEEPBRIDGE fetched
// and verified the component; src/sleepbridge/library/ then provides these
// three. Without it, this build has no rebuild.
#ifndef HMS_WITH_SLEEPBRIDGE

namespace hms_cpap::sleepbridge {

std::unique_ptr<IRebuilder> makeRebuilder(const std::string&,
                                          std::function<void(const std::string&)>) {
    return nullptr;
}
bool rebuilderInBuild() { return false; }
std::string rebuilderVersion() { return {}; }

}  // namespace hms_cpap::sleepbridge

#endif
