#pragma once

#include <QObject>
#include <QString>
#include <memory>

class QLocalServer;

namespace cpapdash::supervisor {

/**
 * SDD-016: exactly one supervisor per user.
 *
 * Two of these running at once is not a cosmetic problem. Each one OWNS an
 * hms_cpap child, so a second supervisor spawns a second service, both try to
 * bind the same web port, and the loser dies with "port already in use" --
 * against a port its own sibling is holding. The user sees a service that
 * refuses to start for no visible reason, and the C# tray hit exactly this,
 * which is why it guarded with a mutex from the beginning.
 *
 * Implemented with QLocalServer rather than QSharedMemory, for two reasons:
 *
 *   - Shared memory SURVIVES A CRASH on Unix. A supervisor killed with SIGKILL
 *     leaves its segment behind and every later launch believes another copy is
 *     running -- the application locks itself out permanently and the only cure
 *     is an ipcrm nobody will ever run.
 *   - A socket carries a message. The second launch is a user asking for the
 *     application, so instead of dying silently it tells the first instance to
 *     show itself. Clicking an app and having nothing happen is indistinguishable
 *     from it being broken.
 *
 * The name is per-user: on a shared machine two people each get their own
 * supervisor, their own config and their own port, and must not collide.
 */
class SingleInstance : public QObject {
    Q_OBJECT

public:
    explicit SingleInstance(QObject* parent = nullptr);
    ~SingleInstance() override;

    /// Try to become THE supervisor. False means another one already is.
    bool tryBecomePrimary();

    /// Hand a verb to the running instance. Used by the launch that lost.
    static bool sendToPrimary(const QString& verb, int timeout_ms = 1500);

signals:
    /// Another launch asked for something: "show" or "settings".
    void messageReceived(const QString& verb);

private:
    static QString socketName();

    std::unique_ptr<QLocalServer> server_;
};

}  // namespace cpapdash::supervisor
