#pragma once

#include <QNetworkAccessManager>
#include <QString>
#include <QWebSocket>
#include <QWizard>

#include <nlohmann/json.hpp>

#include "BridgeText.h"
#include "ChildProcess.h"

namespace cpapdash::supervisor {

/**
 * SDD-073: a Sleep Bridge set up entirely in the installer.
 *
 * Opened after the first-run wizard when the Sleep Bridge was chosen (and
 * from the tray's "Set up a Sleep Bridge..."), with hms_cpap running: one
 * step a screen, a picture of the hardware where there is hardware, and a
 * "Skip, I'll do it later" on each.
 *
 *   D1 Bring your card      the drive appearing (an OS event), the card read
 *                           with the computer's permission (CardReadJob),
 *                           or a .cpapcard chosen by hand
 *   D2 Put it together      pictures only
 *   D3 Connect it           this computer's address, Copy, its QR code
 *   D4 Waiting              the bridge's own hello (the watch socket), then
 *                           "Use this bridge as my source" and, once
 *                           hms_cpap is back, the card from D1 uploaded
 *   D5 Into the machine     the bridge's card statement (the watch socket)
 *   D6 Your nights online   the token, the switch, the stream's state
 *
 * Every wait is an event; nothing here runs on a clock. The service's events
 * come over ws://localhost:<web port>/api/sleepbridge/watch, opened when the
 * child is Running and again after it comes back from a restart.
 */
class BridgeSetupWindow : public QWizard {
    Q_OBJECT

public:
    BridgeSetupWindow(ChildProcess* child, QString service_exe, QString data_dir, int web_port, int bridge_port,
                      QWidget* parent = nullptr);

    QString t(Txt key) const;
    QString baseUrl() const;
    QString dataDir() const { return data_dir_; }
    QString serviceExe() const { return service_exe_; }
    int bridgePort() const { return bridge_port_; }
    bool serviceUp() const { return ws_open_; }
    QNetworkAccessManager* net() { return &net_; }

    /// The latest GET /api/sleepbridge body the socket brought.
    const nlohmann::json& bridges() const { return bridges_; }
    QString streamState() const { return stream_state_; }

    // What the steps share.
    QString   pending_card;        ///< D1's .cpapcard, sent after D4
    bool      card_sent = false;
    QString   sender;              ///< D4's bridge
    QString   format_set;          ///< what "Use this bridge" set
    bool      format_known = false;
    long long stated_before = 0;   ///< the bridge's card_stated_at at its hello
    long long opened_at = 0;       ///< epoch s, when this window opened

    /// D4/D5: POST /api/sleepbridge/<sender>/use, with the restart.
    void useBridge();
    /// D4: the card from D1, once the bridge's folder is the source.
    void sendCard();

signals:
    /// A sleepbridge frame: "snapshot", "hello", "card", "closed", "rebuilt".
    void bridgeEvent(const QString& event, const QString& sender, const nlohmann::json& frame);
    void streamChanged(const QString& state);
    void serviceChanged(bool up);
    void useAnswered(bool ok, bool restarting, const QString& error);
    void sendProgress(int percent);
    void sendAnswered(bool ok, const QString& error);

private:
    void connectWatch();
    void onFrame(const QString& text);

    ChildProcess* child_;
    QString       service_exe_;
    QString       data_dir_;
    int           web_port_;
    int           bridge_port_;
    Lang          lang_;
    QNetworkAccessManager net_;
    QWebSocket    ws_;
    bool          ws_open_ = false;
    bool          ws_connecting_ = false;
    bool          awaiting_restart_ = false;
    nlohmann::json bridges_ = nlohmann::json::object();
    QString       stream_state_;
};

}  // namespace cpapdash::supervisor
