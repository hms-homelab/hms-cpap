#pragma once

#include <string>

namespace cpapdash::supervisor {

/**
 * SDD-073: the Sleep Bridge setup steps' words in the five languages the app
 * ships (en, es, fr, hu, pt), as one table: run_tests checks every word has
 * all five. UTF-8; "%1" and "%2" are filled by the window (QString::arg).
 */
enum class Lang { En, Es, Fr, Hu, Pt };

/// The language for a locale name ("pt_BR", "hu-HU", "fr"); English for any
/// other.
Lang langForLocale(const std::string& name);

enum class Txt {
    WindowTitle, Skip, StepOf, Next, Back, Done, Close,
    Starting, ServiceDown,
    // D1
    CardTitle, CardBody, CardWaiting, CardFound, CardReadButton, CardReading, CardPermission, CardPhaseLayout, CardPhaseFiles,
    CardDone, CardDoneNote, CardFailed, CardFallback, CardFallbackButton, CardChosen, CardNotCard,
    // D2
    AssembleTitle, AssembleCard, AssembleExtender, AssemblePower, AssembleNotYet,
    // D3
    ConnectTitle, ConnectBody, ConnectCopy, ConnectCopied, ConnectNotThis, ConnectPort, ConnectMacFirewall,
    ConnectNoAddress,
    // D4
    WaitTitle, WaitWaiting, WaitFound, WaitSourceSet, WaitFormatUnknown, WaitRestarting, WaitSending, WaitSent,
    WaitSendFailed, WaitKeepOpen, WaitFirewallMac, WaitFirewallWin, WaitFirewallOther, WaitUseFailed,
    // D5
    MachineTitle, MachineSlot, MachinePower, MachineWaiting, MachineSeen, MachineMismatch,
    // D6
    OnlineTitle, OnlineBody, OnlineOpen, OnlineToken, OnlineSwitch, OnlineSave, OnlineSaved,
    StreamConnected, StreamConnecting, StreamNoToken, StreamUnreachable, StreamRefused, StreamRevoked,
    StreamOff, StreamOther,
    Count_
};

/// D6: what the last Save on the step set the switch to.
enum class Saved { NotYet, On, Off };

/**
 * D6: the line under the switch says where the person is, from the stream's
 * state (the watch socket) and their last Save: "Not sending yet" before a
 * token is saved (the switch's tick is a choice, not the state), connecting,
 * "Sending your nights", and "Sending is off." only once they turned it off
 * and saved. StreamOther takes the state as %1.
 */
Txt streamLine(const std::string& state, Saved saved);

/// The word in `lang`.
const char* text(Txt key, Lang lang);

}  // namespace cpapdash::supervisor
