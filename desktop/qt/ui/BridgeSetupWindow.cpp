#include "BridgeSetupWindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QEvent>
#include <QFrame>
#include <QPen>
#include <QPolygonF>
#include <QJsonArray>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QHttpMultiPart>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QPainter>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QUdpSocket>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QWizardPage>

#include "BridgeSetup.h"
#include "CardDrive.h"
#include "Theme.h"

namespace cpapdash::supervisor {

namespace {

constexpr int kPageCount = 6;

/// D6: where the hms-cpap token is made: CpapDash's Settings, Sharing tab
/// (#90; plain /settings opens the Account tab). Signed out, CpapDash sends
/// this through /login?returnUrl and comes back to it, query and all. The
/// web wizard's link goes to the same page.
constexpr const char* kTokenPage = "https://www.cpapdash.com/settings?tab=sharing";

/// The external-link mark: a square open at its top right, an arrow leaving
/// through it to the top right. Drawn, so it takes the link's colour and
/// stays sharp at any scale.
QPixmap externalLinkMark(const QColor& colour, int size) {
    const qreal dpr = qApp->devicePixelRatio();
    QPixmap p(QSize(size, size) * dpr);
    p.setDevicePixelRatio(dpr);
    p.fill(Qt::transparent);
    QPainter g(&p);
    g.setRenderHint(QPainter::Antialiasing);
    g.setPen(QPen(colour, std::max(1.2, size / 9.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const qreal s = size, m = s * 0.14, b = s - m;
    const qreal mid = s * 0.5;
    g.drawPolyline(QPolygonF({{mid, m + s * 0.12}, {m, m + s * 0.12}, {m, b}, {b - s * 0.12, b}, {b - s * 0.12, mid}}));
    g.drawLine(QPointF(mid - s * 0.04, mid + s * 0.04), QPointF(b, m));
    g.drawPolyline(QPolygonF({{b - s * 0.3, m}, {b, m}, {b, m + s * 0.3}}));
    return p;
}

QLabel* words(const QString& text, QWidget* parent, double scale = 1.0, bool bold = false) {
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    QFont f = l->font();
    f.setPointSizeF(f.pointSizeF() * scale);
    f.setBold(bold);
    l->setFont(f);
    return l;
}

/// One of SDD-073 section 5's pictures (:/bridge/), scaled to `width` in its
/// own shape: the renders are 16:9, the rest 4:3. Always the PNG still: the
/// web wizard's animated WebP needs Qt's WebP plugin (qtimageformats), which
/// the Windows build does not install, so the desktop shows the still
/// everywhere rather than animating on one platform only.
QLabel* picture(const QString& name, int width, QWidget* parent) {
    auto* l = new QLabel(parent);
    const qreal dpr = qApp->devicePixelRatio();
    QPixmap p(":/bridge/" + name);
    const int height = p.isNull() ? width * 3 / 4 : width * p.height() / p.width();
    p = p.scaled(QSize(width, height) * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    p.setDevicePixelRatio(dpr);
    l->setPixmap(p);
    l->setMinimumSize(width, height);   // never cropped by a tight layout
    l->setAlignment(Qt::AlignCenter);
    return l;
}

/// A picture with its caption under it.
QWidget* captioned(const QString& name, int width, const QString& caption, QWidget* parent) {
    auto* w = new QWidget(parent);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(picture(name, width, w));
    auto* c = words(caption, w);
    c->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    c->setFixedWidth(width);
    // A wrapped label's size hint does not know its width, so a two-line
    // caption was given one line's height and drawn over the picture.
    c->setMinimumHeight(c->heightForWidth(width));
    v->addWidget(c);
    return w;
}

QJsonObject jsonOf(QNetworkReply* r) { return QJsonDocument::fromJson(r->readAll()).object(); }

std::vector<std::string> interfaceAddresses() {
    std::vector<std::string> ipv4;
    for (const auto& iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) ||
            (flags & QNetworkInterface::IsLoopBack))
            continue;
        for (const auto& e : iface.addressEntries())
            if (e.ip().protocol() == QAbstractSocket::IPv4Protocol) ipv4.push_back(e.ip().toString().toStdString());
    }
    return ipv4;
}

/// The source address the OS would use for the internet, which is the
/// address on the interface with the default route. A UDP "connect" picks
/// the route and sends nothing.
std::string defaultRouteAddress() {
    QUdpSocket s;
    s.connectToHost(QHostAddress("192.0.2.1"), 9);   // TEST-NET-1: never answered, never sent to
    if (s.state() != QAbstractSocket::ConnectedState) return {};
    return s.localAddress().toString().toStdString();
}

/// The common foot of every step: "Skip, I'll do it later".
class Step : public QWizardPage {
public:
    Step(BridgeSetupWindow* w, int number, Txt title) : w_(w) {
        setTitle(w->t(title));
        setSubTitle(w->t(Txt::StepOf).arg(number).arg(kPageCount));
        body_ = new QVBoxLayout;
        body_->setSpacing(10);
        auto* outer = new QVBoxLayout(this);
        outer->addLayout(body_, 1);
        auto* skip = new QLabel("<a href=\"#\">" + w->t(Txt::Skip).toHtmlEscaped() + "</a>", this);
        skip->setAlignment(Qt::AlignRight);
        outer->addWidget(skip);
        connect(skip, &QLabel::linkActivated, this, [this] {
            skipping_ = true;
            if (wizard()->nextId() == -1) wizard()->accept();
            else wizard()->next();
            skipping_ = false;
        });
        down_ = words(w->t(Txt::ServiceDown), this);
        down_->setStyleSheet("color: #a06800;");
        outer->insertWidget(0, down_);
        down_->setVisible(!w->serviceUp());
        connect(w, &BridgeSetupWindow::serviceChanged, this, [this](bool up) { down_->setVisible(!up); });
    }

protected:
    QVBoxLayout*       body_;
    BridgeSetupWindow* w_;
    bool               skipping_ = false;
    QLabel*            down_;
};

// ── D1: Bring your card ───────────────────────────────────────────────────

class CardStep : public Step {
public:
    explicit CardStep(BridgeSetupWindow* w) : Step(w, 1, Txt::CardTitle), job_(w->serviceExe(), w->dataDir() + "/setup") {
        body_->addWidget(words(w->t(Txt::CardBody), this, 1.15, true));
        body_->addWidget(picture("d1-card-into-computer.png", 400, this));
        state_ = words(w->t(Txt::CardWaiting), this);
        body_->addWidget(state_);
        permission_ = words(w->t(Txt::CardPermission), this);
        permission_->setVisible(false);
        body_->addWidget(permission_);
        // #89: the read needs the computer's permission (raw sectors), so it
        // starts only when the user presses this, never on its own the moment
        // a card appears: the prompt then answers the user's own click.
        readBtn_ = new QPushButton(w->t(Txt::CardReadButton), this);
        readBtn_->setVisible(false);
        body_->addWidget(readBtn_, 0, Qt::AlignLeft);
        bar_ = new QProgressBar(this);
        bar_->setVisible(false);
        body_->addWidget(bar_);
        body_->addStretch(1);
        auto* row = new QHBoxLayout;
        row->addWidget(words(w->t(Txt::CardFallback), this), 1);
        auto* choose = new QPushButton(w->t(Txt::CardFallbackButton), this);
        row->addWidget(choose);
        body_->addLayout(row);

        connect(&drives_, &DriveWatch::changed, this, [this] { look(); });
        connect(readBtn_, &QPushButton::clicked, this, [this] { read(); });
        connect(choose, &QPushButton::clicked, this, [this] {
            // Documents, where CpapDash Desktop's Export for HMS CPAP saves
            // the .cpapcard (#93). An empty folder meant the working folder,
            // which for the installed app is the program folder.
            const QString f = QFileDialog::getOpenFileName(this, w_->t(Txt::CardFallbackButton),
                                                           QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
                                                           "CpapDash card (*.cpapcard)");
            if (f.isEmpty()) return;
            w_->pending_card = f;
            state_->setText(w_->t(Txt::CardChosen).arg(QFileInfo(f).fileName()) + "\n" + w_->t(Txt::CardDoneNote));
            emit completeChanged();
        });
        connect(&job_, &CardReadJob::progress, this, [this](const QString& phase, qint64 done, qint64 total) {
            bar_->setFormat(w_->t(phase == "files" ? Txt::CardPhaseFiles : Txt::CardPhaseLayout) + "  %p%");
            bar_->setRange(0, static_cast<int>(std::max<qint64>(1, total)));
            bar_->setValue(static_cast<int>(std::min(done, total)));
        });
        connect(&job_, &CardReadJob::finished, this,
                [this](bool ok, bool declined, const QString& error, const QJsonObject&) {
                    bar_->setVisible(false);
                    permission_->setVisible(false);
                    readBtn_->setEnabled(true);
                    if (ok) {
                        readBtn_->setVisible(false);
                        w_->pending_card = job_.outPath();
                        read_ = true;
                        state_->setText(w_->t(Txt::CardDone) + "\n" + w_->t(Txt::CardDoneNote));
                        state_->setStyleSheet("color: #2d7a2d;");
                    } else {
                        state_->setText(w_->t(Txt::CardFailed).arg(declined || error.isEmpty() ? w_->t(Txt::CardPermission) : error));
                        state_->setStyleSheet("color: #c0392b;");
                    }
                    emit completeChanged();
                });
    }

    void initializePage() override { look(true); }
    bool isComplete() const override { return !w_->pending_card.isEmpty(); }
    bool validatePage() override { return true; }

private:
    /// A volume came or went: is a card there now? Finding one only offers
    /// the read (#89); the permission prompt waits for the user's press.
    /// `opening`: the step is being shown; otherwise only while it is shown.
    void look(bool opening = false) {
        if (job_.running() || read_ || (!opening && wizard()->currentPage() != this)) return;
        card_ = findCardVolume();
        if (card_.root.isEmpty()) {
            readBtn_->setVisible(false);
            state_->setText(w_->t(Txt::CardWaiting));
            state_->setStyleSheet(QString());
            return;
        }
        state_->setText(w_->t(Txt::CardFound).arg(card_.name));
        state_->setStyleSheet(QString());
        permission_->setVisible(true);
        readBtn_->setVisible(true);
    }

    /// The user pressed Read card: now ask for the permission and read.
    void read() {
        if (job_.running() || read_ || card_.root.isEmpty()) return;
        readBtn_->setEnabled(false);
        state_->setText(w_->t(Txt::CardFound).arg(card_.name) + "  " + w_->t(Txt::CardReading));
        state_->setStyleSheet(QString());
        bar_->setRange(0, 0);
        bar_->setVisible(true);
        job_.start(card_, w_->t(Txt::CardPermission));
    }

    DriveWatch    drives_;
    CardReadJob   job_;
    CardVolume    card_;
    QLabel*       state_;
    QLabel*       permission_;
    QPushButton*  readBtn_;
    QProgressBar* bar_;
    bool          read_ = false;
};

// ── D2: Put the bridge together ───────────────────────────────────────────

class AssembleStep : public Step {
public:
    explicit AssembleStep(BridgeSetupWindow* w) : Step(w, 2, Txt::AssembleTitle) {
        auto* row = new QHBoxLayout;
        row->addWidget(captioned("d2a-card-into-bridge.png", 220, w->t(Txt::AssembleCard), this));
        row->addWidget(captioned("d2b-bridge-onto-extender.png", 220, w->t(Txt::AssembleExtender), this));
        row->addWidget(captioned("d2c-power.png", 220, w->t(Txt::AssemblePower), this));
        body_->addLayout(row);
        auto* warn = words(w->t(Txt::AssembleNotYet), this, 1.3, true);
        warn->setAlignment(Qt::AlignCenter);
        warn->setStyleSheet("color: #c0392b;");
        body_->addWidget(warn);
        body_->addStretch(1);
    }
};

// ── D3: Connect the bridge to this computer ───────────────────────────────

class ConnectStep : public Step {
public:
    explicit ConnectStep(BridgeSetupWindow* w) : Step(w, 3, Txt::ConnectTitle) {
        body_->addWidget(words(w->t(Txt::ConnectBody), this, 1.05));
        auto* row = new QHBoxLayout;
        row->addWidget(picture("d3-join-bridge-wifi.png", 320, this), 0, Qt::AlignTop);

        auto* right = new QVBoxLayout;
        address_ = words(QString(), this, 2.6, true);
        address_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        right->addWidget(address_);
        auto* copy = new QPushButton(w->t(Txt::ConnectCopy), this);
        copy->setFixedWidth(120);
        right->addWidget(copy);
        port_ = words(QString(), this);
        right->addWidget(port_);
        qr_ = new QLabel(this);
        right->addWidget(qr_);
        others_ = new QLabel(this);
        others_->setWordWrap(true);
        right->addWidget(others_);
        right->addStretch(1);
        row->addLayout(right, 1);
        body_->addLayout(row);

#ifdef Q_OS_MACOS
        // An info notice, no picture (SDD-073 D7).
        auto* fw = words(w->t(Txt::ConnectMacFirewall), this);
        fw->setStyleSheet("background: #13263d; border: 1px solid #2f6aa8; color: #cfe3ff;"
                          " border-radius: 6px; padding: 8px 10px;");
        body_->addWidget(fw);
#endif
        body_->addStretch(1);

        connect(copy, &QPushButton::clicked, this, [this, copy] {
            QGuiApplication::clipboard()->setText(current_);
            copy->setText(w_->t(Txt::ConnectCopied));
        });
        connect(others_, &QLabel::linkActivated, this, [this](const QString& ip) { show(ip); });
        connect(w, &BridgeSetupWindow::serviceChanged, this, [this](bool up) {
            if (up && qr_->pixmap().isNull()) show(current_);
        });
    }

    void initializePage() override {
        const auto c = chooseBridgeAddress(interfaceAddresses(), defaultRouteAddress());
        choice_ = c;
        if (c.primary.empty()) {
            address_->setText(w_->t(Txt::ConnectNoAddress));
            return;
        }
        show(QString::fromStdString(c.primary));
    }

private:
    void show(const QString& ip) {
        if (ip.isEmpty()) return;
        current_ = ip;
        address_->setText(ip);
        port_->setText(mentionPort(w_->bridgePort()) ? w_->t(Txt::ConnectPort).arg(w_->bridgePort()) : QString());
        port_->setVisible(mentionPort(w_->bridgePort()));
        QString links;
        for (const auto& o : choice_.others) {
            const QString q = QString::fromStdString(o);
            if (q == ip) continue;
            links += " <a href=\"" + q + "\">" + q + "</a>";
        }
        if (ip != QString::fromStdString(choice_.primary) && !choice_.primary.empty())
            links += " <a href=\"" + QString::fromStdString(choice_.primary) + "\">" +
                     QString::fromStdString(choice_.primary) + "</a>";
        others_->setText(links.isEmpty() ? QString() : w_->t(Txt::ConnectNotThis) + links);
        // The code, from the service (one encoder for both setups).
        QUrl url(w_->baseUrl() + "/api/sleepbridge/qr");
        QUrlQuery q;
        q.addQueryItem("text", ip);
        url.setQuery(q);
        auto* r = w_->net()->get(QNetworkRequest(url));
        connect(r, &QNetworkReply::finished, this, [this, r, ip] {
            r->deleteLater();
            if (r->error() != QNetworkReply::NoError || ip != current_) return;
            const auto o = jsonOf(r);
            const auto rows = o.value("rows").toArray();
            const int n = o.value("size").toInt();
            if (n <= 0) return;
            const int m = 5, quiet = 4, px = (n + 2 * quiet) * m;
            QPixmap p(px, px);
            p.fill(Qt::white);
            QPainter g(&p);
            for (int y = 0; y < n; y++) {
                const QString row = rows.at(y).toString();
                for (int x = 0; x < n && x < row.size(); x++)
                    if (row[x] == '1') g.fillRect((x + quiet) * m, (y + quiet) * m, m, m, Qt::black);
            }
            g.end();
            qr_->setPixmap(p);
        });
    }

    AddressChoice choice_;
    QString       current_;
    QLabel*       address_;
    QLabel*       port_;
    QLabel*       qr_;
    QLabel*       others_;
};

// ── D4: Waiting for your bridge ───────────────────────────────────────────

class WaitStep : public Step {
public:
    explicit WaitStep(BridgeSetupWindow* w) : Step(w, 4, Txt::WaitTitle) {
        state_ = words(w->t(Txt::WaitWaiting), this, 1.3, true);
        body_->addWidget(state_);
        source_ = words(QString(), this);
        body_->addWidget(source_);
        send_ = words(QString(), this);
        body_->addWidget(send_);
        bar_ = new QProgressBar(this);
        bar_->setRange(0, 100);
        bar_->setVisible(false);
        body_->addWidget(bar_);
        body_->addStretch(1);
#if defined(Q_OS_MACOS)
        auto* hint = words(w->t(Txt::WaitFirewallMac), this, 0.95);
#elif defined(Q_OS_WIN)
        auto* hint = words(w->t(Txt::WaitFirewallWin), this, 0.95);
#else
        auto* hint = words(w->t(Txt::WaitFirewallOther).arg(w->bridgePort()), this, 0.95);
#endif
        body_->addWidget(hint);

        connect(w, &BridgeSetupWindow::bridgeEvent, this,
                [this](const QString&, const QString&, const nlohmann::json& frame) {
                    if (!w_->sender.isEmpty()) return;
                    const std::string s = arrivedBridge(frame, w_->opened_at);
                    if (s.empty()) return;
                    w_->sender = QString::fromStdString(s);
                    const auto b = bridgeEntry(w_->bridges(), s);
                    w_->stated_before = b.is_object() && b.contains("card_stated_at") ? b["card_stated_at"].get<long long>() : 0;
                    state_->setText("✔  " + w_->t(Txt::WaitFound).arg(w_->sender));
                    state_->setStyleSheet("color: #2d7a2d;");
                    w_->useBridge();
                });
        connect(w, &BridgeSetupWindow::useAnswered, this, [this](bool ok, bool restarting, const QString& error) {
            if (!ok) {
                source_->setText(w_->t(Txt::WaitUseFailed).arg(error));
                source_->setStyleSheet("color: #c0392b;");
                return;
            }
            source_->setStyleSheet(QString());
            source_->setText(w_->t(Txt::WaitSourceSet).arg(w_->format_set == "bmc" ? "BMC / Luna" : "ResMed") +
                             (restarting ? " " + w_->t(Txt::WaitRestarting) : QString()) +
                             (w_->format_known ? QString() : "\n" + w_->t(Txt::WaitFormatUnknown)));
            emit completeChanged();
        });
        connect(w, &BridgeSetupWindow::sendProgress, this, [this](int pct) {
            bar_->setVisible(true);
            bar_->setValue(pct);
            send_->setText(w_->t(Txt::WaitSending).arg(pct) + "\n" + w_->t(Txt::WaitKeepOpen));
        });
        connect(w, &BridgeSetupWindow::sendAnswered, this, [this](bool ok, const QString& error) {
            bar_->setVisible(false);
            send_->setText(ok ? w_->t(Txt::WaitSent) : w_->t(Txt::WaitSendFailed).arg(error));
            send_->setStyleSheet(ok ? "color: #2d7a2d;" : "color: #c0392b;");
        });
    }

    void initializePage() override {
        // A bridge that already said hello while the earlier steps were open
        // counts: the snapshot the window holds says so.
        if (w_->sender.isEmpty()) {
            nlohmann::json snap = {{"t", "sleepbridge"}, {"event", "snapshot"}, {"status", w_->bridges()}};
            emit w_->bridgeEvent("snapshot", QString(), snap);
        }
    }
    bool isComplete() const override { return !w_->format_set.isEmpty(); }
    bool validatePage() override { return true; }

private:
    QLabel*       state_;
    QLabel*       source_;
    QLabel*       send_;
    QProgressBar* bar_;
};

// ── D5: Into the machine ──────────────────────────────────────────────────

class MachineStep : public Step {
public:
    explicit MachineStep(BridgeSetupWindow* w) : Step(w, 5, Txt::MachineTitle) {
        auto* row = new QHBoxLayout;
        row->addWidget(captioned("d5a-into-machine.png", 320, w->t(Txt::MachineSlot), this));
        row->addWidget(captioned("d5b-power-cycle.png", 320, w->t(Txt::MachinePower), this));
        body_->addLayout(row);
        state_ = words(w->t(Txt::MachineWaiting), this, 1.3, true);
        body_->addWidget(state_);
        body_->addStretch(1);

        connect(w, &BridgeSetupWindow::bridgeEvent, this, [this](const QString&, const QString&, const nlohmann::json&) {
            check();
        });
    }

    void initializePage() override { check(); }
    bool isComplete() const override { return seen_; }
    bool validatePage() override { return true; }

private:
    void check() {
        if (w_->sender.isEmpty()) return;
        const std::string s = w_->sender.toStdString();
        // The card is recognised as another machine's than the one set.
        const std::string fix = formatToFix(w_->bridges(), s, w_->format_set.toStdString());
        if (!fix.empty()) {
            w_->format_set = QString::fromStdString(fix);   // asked once, not per frame
            w_->useBridge();
        }
        switch (cardSeen(w_->bridges(), s, w_->stated_before)) {
            case CardSeen::Waiting: return;
            case CardSeen::Seen:
                state_->setText("✔  " + w_->t(Txt::MachineSeen));
                state_->setStyleSheet("color: #2d7a2d;");
                break;
            case CardSeen::Mismatch:
                state_->setText(w_->t(Txt::MachineMismatch));
                state_->setStyleSheet("color: #a06800;");
                break;
        }
        seen_ = true;
        emit completeChanged();
    }

    QLabel* state_;
    bool    seen_ = false;
};

// ── D6: Your nights online ────────────────────────────────────────────────

class OnlineStep : public Step {
public:
    explicit OnlineStep(BridgeSetupWindow* w) : Step(w, 6, Txt::OnlineTitle) {
        body_->addWidget(words(w->t(Txt::OnlineBody), this, 1.3, true));
        // A link, not a button (SDD-073 D6), with the external-link
        // mark after the words: it leaves for the browser.
        auto* open = new QLabel("<a href=\"" + QString(kTokenPage) + "\">" + w->t(Txt::OnlineOpen).toHtmlEscaped() +
                                    "</a>",
                                this);
        open->setTextFormat(Qt::RichText);
        open->setOpenExternalLinks(true);
        open->setCursor(Qt::PointingHandCursor);
        auto* mark = new QLabel(this);
        mark->setPixmap(externalLinkMark(open->palette().color(QPalette::Link), open->fontMetrics().height()));
        mark->setCursor(Qt::PointingHandCursor);
        mark->installEventFilter(this);
        mark_ = mark;
        auto* link_row = new QHBoxLayout;
        link_row->setSpacing(4);
        link_row->addWidget(open);
        link_row->addWidget(mark);
        link_row->addStretch(1);
        body_->addLayout(link_row);
        body_->addWidget(words(w->t(Txt::OnlineToken), this));
        token_ = new QLineEdit(this);
        token_->setEchoMode(QLineEdit::Password);
        body_->addWidget(token_);
        send_ = new QCheckBox(w->t(Txt::OnlineSwitch), this);
        send_->setChecked(true);
        body_->addWidget(send_);
        auto* save = new QPushButton(w->t(Txt::OnlineSave), this);
        theme::primary(save);
        save->setFixedWidth(160);
        body_->addWidget(save);
        state_ = words(QString(), this, 1.15, true);
        body_->addWidget(state_);
        body_->addStretch(1);

        connect(save, &QPushButton::clicked, this, [this] {
            QJsonObject cd{{"stream", send_->isChecked()}};
            if (!token_->text().trimmed().isEmpty()) cd["token"] = token_->text().trimmed();
            QNetworkRequest req(QUrl(w_->baseUrl() + "/api/config"));
            req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
            auto* r = w_->net()->put(req, QJsonDocument(QJsonObject{{"cpapdash", cd}}).toJson());
            const bool on = send_->isChecked();
            connect(r, &QNetworkReply::finished, this, [this, r, on] {
                r->deleteLater();
                if (r->error() != QNetworkReply::NoError) return;
                saved_ = on ? Saved::On : Saved::Off;
                show(w_->streamState());   // the stream's own event follows
            });
        });
        connect(w, &BridgeSetupWindow::streamChanged, this, [this](const QString& s) { show(s); });
    }

    void initializePage() override { show(w_->streamState()); }

protected:
    /// The mark opens the page too, as part of the link.
    bool eventFilter(QObject* o, QEvent* e) override {
        if (o == mark_ && e->type() == QEvent::MouseButtonRelease) {
            QDesktopServices::openUrl(QUrl(kTokenPage));
            return true;
        }
        return Step::eventFilter(o, e);
    }

private:
    QLabel* mark_ = nullptr;
    Saved   saved_ = Saved::NotYet;
    void show(const QString& s) {
        const Txt k = streamLine(s.toStdString(), saved_);
        state_->setText(k == Txt::StreamOther ? w_->t(k).arg(s) : w_->t(k));
        state_->setStyleSheet(k == Txt::StreamConnected ? "color: #2d7a2d;" : QString());
    }

    QLineEdit* token_;
    QCheckBox* send_;
    QLabel*    state_;
};

}  // namespace

// ── The window ─────────────────────────────────────────────────────────────

BridgeSetupWindow::BridgeSetupWindow(ChildProcess* child, QString service_exe, QString data_dir, int web_port,
                                     int bridge_port, QWidget* parent)
    : QWizard(parent),
      child_(child),
      service_exe_(std::move(service_exe)),
      data_dir_(std::move(data_dir)),
      web_port_(web_port),
      bridge_port_(bridge_port),
      lang_(langForLocale(QLocale::system().name().toStdString())) {
    opened_at = QDateTime::currentSecsSinceEpoch();
    setWindowTitle(t(Txt::WindowTitle));
    setWizardStyle(QWizard::ModernStyle);
    setMinimumSize(820, 660);
    setStyleSheet(theme::headerStyle() + theme::buttonStyle());
    theme::primary(button(QWizard::NextButton));
    theme::primary(button(QWizard::FinishButton));
    // Links (Skip, the token page) in the same blue as the web app's.
    QPalette links = palette();
    links.setColor(QPalette::Link, QColor(theme::kButtonBlueText));
    setPalette(links);
    setButtonText(QWizard::NextButton, t(Txt::Next));
    setButtonText(QWizard::BackButton, t(Txt::Back));
    setButtonText(QWizard::FinishButton, t(Txt::Done));
    setButtonText(QWizard::CancelButton, t(Txt::Close));
    setAttribute(Qt::WA_DeleteOnClose);

    addPage(new CardStep(this));
    addPage(new AssembleStep(this));
    addPage(new ConnectStep(this));
    addPage(new WaitStep(this));
    addPage(new MachineStep(this));
    addPage(new OnlineStep(this));

    connect(&ws_, &QWebSocket::connected, this, [this] {
        ws_connecting_ = false;
        ws_open_       = true;
        emit serviceChanged(true);
        // Back from the restart "Use this bridge" asked for: the card goes in.
        if (awaiting_restart_) {
            awaiting_restart_ = false;
            sendCard();
        }
    });
    connect(&ws_, &QWebSocket::disconnected, this, [this] {
        const bool was_open = ws_open_;
        ws_connecting_ = false;
        ws_open_       = false;
        emit serviceChanged(false);
        // Once, on the close itself, while the child still runs; after that
        // the child's next Running is the event.
        if (was_open && child_->state() == ChildProcess::State::Running) connectWatch();
    });
    connect(&ws_, &QWebSocket::textMessageReceived, this, &BridgeSetupWindow::onFrame);
    connect(child_, &ChildProcess::stateChanged, this, [this](ChildProcess::State s) {
        if (s == ChildProcess::State::Running) connectWatch();
    });
    if (child_->state() == ChildProcess::State::Running) connectWatch();
}

QString BridgeSetupWindow::t(Txt key) const { return QString::fromUtf8(text(key, lang_)); }

QString BridgeSetupWindow::baseUrl() const { return QStringLiteral("http://localhost:%1").arg(web_port_); }

void BridgeSetupWindow::connectWatch() {
    if (ws_open_ || ws_connecting_) return;
    ws_connecting_ = true;
    ws_.open(QUrl(QStringLiteral("ws://localhost:%1/api/sleepbridge/watch").arg(web_port_)));
}

void BridgeSetupWindow::onFrame(const QString& text) {
    nlohmann::json f;
    try {
        f = nlohmann::json::parse(text.toStdString());
    } catch (const std::exception&) {
        return;
    }
    if (!f.is_object()) return;
    const std::string kind = f.value("t", "");
    if (kind == "sleepbridge") {
        if (f.contains("status") && f["status"].is_object()) bridges_ = f["status"];
        emit bridgeEvent(QString::fromStdString(f.value("event", "")), QString::fromStdString(f.value("sender", "")), f);
    } else if (kind == "cpapdash" && f.contains("stream") && f["stream"].is_object()) {
        stream_state_ = QString::fromStdString(f["stream"].value("state", ""));
        emit streamChanged(stream_state_);
    }
}

void BridgeSetupWindow::useBridge() {
    if (sender.isEmpty()) return;
    QNetworkRequest req(QUrl(baseUrl() + "/api/sleepbridge/" + QUrl::toPercentEncoding(sender) + "/use"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    auto* r = net_.post(req, QJsonDocument(QJsonObject{{"restart", true}}).toJson());
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        const auto o = jsonOf(r);
        if (!o.value("ok").toBool()) {
            emit useAnswered(false, false, o.value("error").toString(r->errorString()));
            return;
        }
        format_set   = o.value("format").toString();
        format_known = o.value("format_known").toBool();
        const bool restarting = o.value("restarting").toBool();
        // The card waits for hms_cpap to come back on the bridge's folder.
        if (restarting) awaiting_restart_ = true;
        else sendCard();
        emit useAnswered(true, restarting, QString());
    });
}

void BridgeSetupWindow::sendCard() {
    if (pending_card.isEmpty() || card_sent || !ws_open_) return;
    auto* file = new QFile(pending_card);
    if (!file->open(QIODevice::ReadOnly)) {
        delete file;
        emit sendAnswered(false, pending_card);
        return;
    }
    card_sent = true;
    auto* multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart part;
    part.setHeader(QNetworkRequest::ContentTypeHeader, "application/zip");
    part.setHeader(QNetworkRequest::ContentDispositionHeader,
                   QStringLiteral("form-data; name=\"file\"; filename=\"%1\"").arg(QFileInfo(pending_card).fileName()));
    part.setBodyDevice(file);
    file->setParent(multi);
    multi->append(part);
    auto* r = net_.post(QNetworkRequest(QUrl(baseUrl() + "/api/upload/cpap")), multi);
    multi->setParent(r);
    connect(r, &QNetworkReply::uploadProgress, this, [this](qint64 sent, qint64 total) {
        if (total > 0) emit sendProgress(static_cast<int>(sent * 100 / total));
    });
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        const auto o = jsonOf(r);
        if (r->error() != QNetworkReply::NoError) {
            card_sent = false;
            emit sendAnswered(false, o.value("error").toString(r->errorString()));
            return;
        }
        emit sendAnswered(true, QString());
    });
}

}  // namespace cpapdash::supervisor
