#pragma once

#include <QString>

namespace cpapdash::supervisor::autostart {

/**
 * SDD-016: start CpapDash when the user logs in.
 *
 * The OS starts the SUPERVISOR and nothing else. That is the whole point of
 * the design: what used to be three service backends -- a Run key, a
 * LaunchAgent and a systemd unit each launching hms_cpap directly -- becomes
 * three tiny login entries that launch one application, which then owns the
 * service.
 *
 * So this is deliberately the smallest possible surface. It does not start,
 * stop or supervise anything; it only answers whether we are in the user's
 * login items and lets that be changed.
 *
 * Per-user everywhere, never system-wide. Nothing here needs elevation, and a
 * therapy collector that reads one person's SD card has no business running
 * before anyone has logged in -- the installer is `PrivilegesRequired=lowest`
 * for the same reason.
 */

/// Is CpapDash set to start at login?
bool isEnabled();

/// Turn it on or off. Returns false and fills `error` when the platform
/// refused -- a locked registry, an unwritable LaunchAgents directory -- so
/// the caller can say what happened instead of silently mis-reporting state.
bool setEnabled(bool enabled, QString* error = nullptr);

/// False when this build cannot manage login items at all, so the UI can hide
/// the control rather than offer something that will not work.
bool isSupported();

}  // namespace cpapdash::supervisor::autostart
