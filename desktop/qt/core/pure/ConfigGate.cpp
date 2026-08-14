#include "ConfigGate.h"

namespace cpapdash::supervisor {

GateResult evaluateConfig(const std::optional<nlohmann::json>& config,
                          bool parse_failed) {
    GateResult r;

    if (parse_failed) {
        r.readiness = Readiness::NeedsSetup;
        r.reason    = SetupReason::Unparseable;
        r.summary   = "Your configuration file could not be read. Nothing has been "
                      "changed, and you can either fix it yourself or start again.";
        return r;
    }

    if (!config.has_value() || !config->is_object()) {
        r.readiness = Readiness::NeedsSetup;
        r.reason    = SetupReason::NoConfigFile;
        r.summary   = "Let's get CpapDash set up. This takes a minute.";
        return r;
    }

    // setup_complete is the service's own record of having been through setup.
    // Honouring it here keeps one answer to "has this been configured" rather
    // than inventing a second.
    const bool complete = config->value("setup_complete", false);
    if (!complete) {
        r.readiness = Readiness::NeedsSetup;
        r.reason    = SetupReason::SetupIncomplete;
        r.summary   = "Setup was never finished. Let's pick up where it left off.";
        return r;
    }

    // The same rules the settings dialog enforces. A config that would fail
    // validation there cannot start a service here either, and finding that out
    // now -- with a window to fix it in -- beats starting a child that exits.
    ConfigModel model;
    model.loadFromDisk(*config);

    auto blocking = blockingIssues(model);
    if (!blocking.empty()) {
        r.readiness = Readiness::NeedsSetup;
        r.reason    = SetupReason::Invalid;
        r.issues    = std::move(blocking);
        r.summary   = r.issues.size() == 1
            ? "One setting needs attention before CpapDash can start."
            : std::to_string(r.issues.size()) +
              " settings need attention before CpapDash can start.";
        return r;
    }

    r.readiness = Readiness::Ready;
    r.reason    = SetupReason::None;
    r.summary   = "Ready.";
    return r;
}

}  // namespace cpapdash::supervisor
