#include "SingleInstance.h"

#include <QCryptographicHash>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>

namespace cpapdash::supervisor {

SingleInstance::SingleInstance(QObject* parent) : QObject(parent) {}

SingleInstance::~SingleInstance() {
    if (server_) {
        server_->close();
        // Belt and braces on Unix, where closing does not always unlink the
        // socket file. A leftover file is handled on the next launch anyway,
        // but leaving one behind is untidy for no reason.
        QLocalServer::removeServer(socketName());
    }
}

QString SingleInstance::socketName() {
    // Per USER, not per machine. Two people logged into the same computer each
    // run their own supervisor against their own config and their own port, and
    // one must not lock the other out.
    //
    // The name is hashed because it becomes a filesystem path on Unix and a
    // pipe name on Windows: a username with a space, a slash or a non-ASCII
    // character would otherwise produce a name one of them refuses.
    const QString user = qEnvironmentVariable("USER",
                            qEnvironmentVariable("USERNAME", "default"));
    const QByteArray digest = QCryptographicHash::hash(
        user.toUtf8(), QCryptographicHash::Sha1).toHex().left(12);
    return QStringLiteral("cpapdash-supervisor-") + QString::fromLatin1(digest);
}

bool SingleInstance::tryBecomePrimary() {
    const QString name = socketName();

    // ASK FIRST, then bind. The obvious order -- try to listen, and treat
    // failure as "someone else is here" -- does not work: QLocalServer::listen()
    // SUCCEEDS on an address a live server already holds, quietly taking the
    // name over instead of reporting it in use. Verified on macOS, where a
    // second launch became primary and started a rival service against the same
    // port.
    //
    // Connecting is unambiguous in a way binding is not: a live supervisor
    // accepts the connection, and a socket file left behind by one that crashed
    // refuses it. That difference is the whole question, and it is also why the
    // stale case cannot lock the application out -- the file alone never counts
    // as "running".
    {
        QLocalSocket probe;
        probe.connectToServer(name);
        if (probe.waitForConnected(400)) {
            probe.disconnectFromServer();
            return false;          // a real supervisor answered
        }
    }

    // Nobody answered. Clear anything a dead instance left behind and take the
    // name.
    QLocalServer::removeServer(name);

    server_ = std::make_unique<QLocalServer>(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);

    connect(server_.get(), &QLocalServer::newConnection, this, [this] {
        while (auto* conn = server_->nextPendingConnection()) {
            connect(conn, &QLocalSocket::readyRead, this, [this, conn] {
                const QString verb = QString::fromUtf8(conn->readAll()).trimmed();
                if (!verb.isEmpty()) emit messageReceived(verb);
                conn->disconnectFromServer();
                conn->deleteLater();
            });
        }
    });

    if (!server_->listen(name)) {
        // Could not claim it and nothing answered. Carry on WITHOUT the guard
        // rather than refusing to run: a supervisor with no mutual exclusion is
        // worse than one with it, but far better than an application that will
        // not start and cannot say why.
        qWarning("CpapDash: could not claim the single-instance socket (%s); "
                 "continuing without it", qPrintable(server_->errorString()));
        server_.reset();
    }
    return true;
}

bool SingleInstance::sendToPrimary(const QString& verb, int timeout_ms) {
    QLocalSocket sock;
    sock.connectToServer(socketName());
    if (!sock.waitForConnected(timeout_ms)) return false;
    sock.write(verb.toUtf8());
    sock.flush();
    sock.waitForBytesWritten(timeout_ms);
    sock.disconnectFromServer();
    return true;
}

}  // namespace cpapdash::supervisor
