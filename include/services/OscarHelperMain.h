#pragma once
//
// SDD-081 D1: `hms_cpap --oscar-helper`, the helper role of this binary.
//
//   hms_cpap --oscar-helper                 the daemon (the supervisor's child on
//                                           macOS and Windows, the systemd unit's
//                                           ExecStart on Linux)
//   hms_cpap --oscar-helper --setup         D3 and D7 on the terminal, once (Linux)
//   hms_cpap --oscar-helper --discover      D3 as JSON on stdout (the installer)
//   hms_cpap --oscar-helper --probe <address>
//                                           one typed address probed, as JSON
//   hms_cpap --oscar-helper --check --program P --data-dir D --profile N
//                                           SDD-080 D7's check as JSON (the installer)
//   hms_cpap --oscar-helper --connect-again the token dropped: the next start
//                                           sends a hello with none (D4)
//
// The daemon starts no web server, no collector and no database. Its state
// goes to <data dir>/oscar-helper/status.json on every change, which the tray
// watches.
//
#include <string>
#include <vector>

namespace hms_cpap::oscar::helper {

/// argv after the program name. The exit code.
int runHelperMain(const std::vector<std::string>& args, const std::string& data_dir, const std::string& config_path,
                  const std::string& version);

/// Where the daemon writes its state.
std::string statusPath(const std::string& data_dir);

}  // namespace hms_cpap::oscar::helper
