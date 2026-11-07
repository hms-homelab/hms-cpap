#pragma once

#include <string>
#include <vector>

#include "ConfigModel.h"

namespace cpapdash::supervisor {

/**
 * The first-run wizard's source step as data: what each choice writes to
 * config.json, and where a Sleep Bridge is told to stream.
 *
 * Pure, like the rest of core/pure: the window only reads its widgets into
 * SourceInputs and calls applySourceChoice, so what the wizard writes is tested
 * in run_tests with no display.
 */

enum class SourceChoice {
    Mule,         ///< a CpapDash Mule or Miner, or an ezShare card (ezshare_url)
    FlashAir,     ///< a FlashAir card on the home WiFi (flashair_url)
    Local,        ///< a folder on this computer (local_dir)
    SleepBridge,  ///< a Sleep Bridge streaming here: no source setting at all
};

/// What the user filled in beside the choice.
struct SourceInputs {
    std::string ezshare_url;
    std::string flashair_url;
    std::string local_dir;
    std::string archive_dir;
};

/// The Sleep Bridge receiver's port when the config does not set one.
constexpr int kSleepBridgeDefaultPort = 8894;

/**
 * Write the choice into the model.
 *
 * SleepBridge writes NOTHING, the same as the web wizard's skip: the Sleep
 * Bridge receiver (sleep_bridge.enabled) is on by default and listens on its
 * own port, so it needs no source, and its nights land under the bridge's own
 * folder in the data folder.
 */
void applySourceChoice(ConfigModel& model, SourceChoice choice, const SourceInputs& in);

/**
 * The choice selected when the source page opens: the Sleep Bridge, the top
 * choice. A saved local folder or FlashAir is selected instead, so a wizard
 * reopened over a folder that moved lands on the field to fix. A saved
 * `ezshare` is not, because the service writes it as its default on first
 * start, and so it says nothing about what the user has; the Sleep Bridge
 * writes nothing, so leaving it selected keeps that setting as it is.
 */
SourceChoice initialSourceChoice(const ConfigModel& model);

/// The receiver's port: sleep_bridge.port, or 8894 when unset or out of range.
int sleepBridgePort(const ConfigModel& model);

/**
 * The address a Sleep Bridge on the network reaches this computer at, from
 * the IPv4 addresses of its interfaces: the first private one (10/8,
 * 172.16/12, 192.168/16), else the first that is not loopback or link-local.
 * Empty when there is none.
 */
std::string lanAddress(const std::vector<std::string>& ipv4);

/// The note under the Sleep Bridge choice: nothing to set, where to point the
/// bridge (address:port, or this computer's IP and the port when the address
/// is unknown), and where its nights land.
std::string sleepBridgeNote(const std::string& address, int port);

}  // namespace cpapdash::supervisor
