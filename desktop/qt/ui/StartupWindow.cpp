#include "StartupWindow.h"

#include <QDialogButtonBox>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include "PreflightRunner.h"
#include "Theme.h"

namespace cpapdash::supervisor {

namespace {

/// Text markers rather than icon assets: they are legible at any size, need no
/// artwork, and carry meaning when read aloud by a screen reader.
const char* markerFor(bool ok, bool failed, bool running) {
    if (failed)  return "✕";   // ✕
    if (ok)      return "✓";   // ✓
    if (running) return "•";   // •
    return "○";                // ○
}

}  // namespace

StartupWindow::StartupWindow(ChildProcess* child, QString service_exe,
                             int web_port, QWidget* parent)
    : QDialog(parent),
      child_(child),
      service_exe_(std::move(service_exe)),
      web_port_(web_port) {
    setWindowTitle("Starting CpapDash");
    setMinimumWidth(520);
    setStyleSheet(theme::headerStyle() + theme::accentStyle());

    net_ = new QNetworkAccessManager(this);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* header = new QFrame(this);
    header->setObjectName("Header");
    header->setFixedHeight(72);
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(18, 0, 18, 0);
    hl->setSpacing(14);
    auto* mark = new QLabel(header);
    mark->setPixmap(QPixmap(":/icons/app-256.png")
                        .scaled(40, 40, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    hl->addWidget(mark);
    auto* title = new QLabel("Starting CpapDash", header);
    QFont tf = title->font();
    tf.setPointSizeF(tf.pointSizeF() * 1.25);
    tf.setBold(true);
    title->setFont(tf);
    hl->addWidget(title);
    hl->addStretch(1);
    outer->addWidget(header);

    auto* body = new QVBoxLayout();
    body->setContentsMargins(22, 20, 22, 16);
    body->setSpacing(14);

    addStep(body, "Check the configuration");
    addStep(body, "Start CpapDash");
    addStep(body, "Wait for it to respond");

    summary_ = new QLabel(this);
    summary_->setWordWrap(true);
    body->addSpacing(4);
    body->addWidget(summary_);
    body->addStretch(1);
    outer->addLayout(body);

    auto* buttons = new QDialogButtonBox(this);
    settings_ = buttons->addButton("Change Settings", QDialogButtonBox::ResetRole);
    done_     = buttons->addButton("Done", QDialogButtonBox::AcceptRole);
    done_->setProperty("accent", true);

    // Nothing to be done ABOUT yet, so neither offers itself until there is.
    done_->setEnabled(false);
    settings_->setVisible(false);

    connect(done_, &QPushButton::clicked, this, &QDialog::accept);
    connect(settings_, &QPushButton::clicked, this, [this] {
        emit settingsRequested();
        reject();
    });

    auto* bb = new QVBoxLayout();
    bb->setContentsMargins(22, 0, 22, 18);
    bb->addWidget(buttons);
    outer->addLayout(bb);

    connect(&health_timer_, &QTimer::timeout, this, &StartupWindow::pollHealth);

    // Let the window paint before the first step blocks on a process.
    QTimer::singleShot(60, this, &StartupWindow::runPreflight);
}

void StartupWindow::addStep(QVBoxLayout* into, const QString& title) {
    Step step;
    step.title = title;

    auto* row = new QWidget(this);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(10);

    step.icon = new QLabel(markerFor(false, false, false), row);
    step.icon->setFixedWidth(18);
    step.icon->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    h->addWidget(step.icon);

    auto* texts = new QVBoxLayout();
    texts->setSpacing(2);
    step.label = new QLabel(title, row);
    texts->addWidget(step.label);

    step.detail = new QLabel(row);
    step.detail->setWordWrap(true);
    QFont df = step.detail->font();
    df.setPointSizeF(df.pointSizeF() * 0.93);
    step.detail->setFont(df);
    step.detail->setVisible(false);
    texts->addWidget(step.detail);

    h->addLayout(texts, 1);
    into->addWidget(row);

    steps_.push_back(step);
}

void StartupWindow::setStep(int index, StepState state, const QString& detail) {
    if (index < 0 || index >= (int)steps_.size()) return;
    auto& s = steps_[index];

    s.icon->setText(markerFor(state == StepState::Ok,
                              state == StepState::Failed,
                              state == StepState::Running));
    if (state == StepState::Failed)
        s.icon->setStyleSheet("color: #c0392b; font-weight: bold;");
    else if (state == StepState::Ok)
        s.icon->setStyleSheet(QString("color: %1; font-weight: bold;").arg(theme::kSky));
    else
        s.icon->setStyleSheet("");

    QFont f = s.label->font();
    f.setBold(state == StepState::Running);
    s.label->setFont(f);

    if (!detail.isEmpty()) {
        s.detail->setText(detail);
        s.detail->setVisible(true);
    }
}

void StartupWindow::runPreflight() {
    setStep(0, StepState::Running);

    const auto report = runPreflight_impl();
    if (!report.ok()) {
        QString detail;
        if (report.verdict == Verdict::ConfigUnparseable || report.failures().empty()) {
            detail = QString::fromStdString(report.message);
        } else {
            // The failing check and its remedy verbatim. PreflightService
            // already writes these for the person who has to fix them, so
            // paraphrasing would only lose information.
            for (const auto& f : report.failures()) {
                detail += QString::fromStdString(f.detail);
                if (!f.remedy.empty()) detail += "\n" + QString::fromStdString(f.remedy);
            }
        }
        setStep(0, StepState::Failed, detail);
        finish(false, "CpapDash cannot start with these settings.");
        return;
    }

    QString ok_detail;
    if (!report.warnings().empty()) {
        // Warnings do not block a start, and saying so is the point: otherwise
        // a yellow note next to a green tick reads as a failure.
        for (const auto& w : report.warnings()) {
            ok_detail += QString::fromStdString(w.detail);
            if (!w.remedy.empty()) ok_detail += "\n" + QString::fromStdString(w.remedy);
        }
    }
    setStep(0, StepState::Ok, ok_detail);
    startService();
}

PreflightReport StartupWindow::runPreflight_impl() {
    return cpapdash::supervisor::runPreflight(service_exe_);
}

void StartupWindow::startService() {
    setStep(1, StepState::Running);

    child_->start();
    if (!child_->isRunning()) {
        setStep(1, StepState::Failed,
                child_->stderrTail().isEmpty()
                    ? QStringLiteral("CpapDash did not start.")
                    : child_->stderrTail().last());
        finish(false, "CpapDash could not be started.");
        return;
    }

    setStep(1, StepState::Ok);
    setStep(2, StepState::Running, "This can take a few seconds the first time, "
                                   "while the database is prepared.");
    health_attempts_ = 0;
    health_timer_.start(kHealthIntervalMs);
}

void StartupWindow::pollHealth() {
    // The child dying while we wait is a different failure from a slow start,
    // and worth saying so rather than timing out twenty seconds later.
    if (!child_->isRunning()) {
        health_timer_.stop();
        setStep(2, StepState::Failed,
                child_->stderrTail().isEmpty()
                    ? QStringLiteral("CpapDash stopped while starting up.")
                    : child_->stderrTail().last());
        finish(false, "CpapDash stopped while it was starting.");
        return;
    }

    if (++health_attempts_ > kMaxHealthAttempts) {
        health_timer_.stop();
        setStep(2, StepState::Failed,
                QStringLiteral("It is running but has not answered on port %1.")
                    .arg(web_port_));
        finish(false, "CpapDash started but is not responding yet.");
        return;
    }

    QNetworkRequest req{QUrl(QStringLiteral("http://localhost:%1/health").arg(web_port_))};
    req.setTransferTimeout(2000);
    auto* reply = net_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const bool ok = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        if (!ok) return;   // keep trying; the timer decides when to stop

        health_timer_.stop();
        setStep(2, StepState::Ok, QStringLiteral("Answering on port %1.").arg(web_port_));
        finish(true, "CpapDash is running and collecting.");
    });
}

void StartupWindow::finish(bool ok, const QString& message) {
    succeeded_ = ok;
    summary_->setText(message);
    summary_->setStyleSheet(ok ? "" : "color: #c0392b;");
    done_->setEnabled(true);
    done_->setText(ok ? "Done" : "Close");
    // Only offered when there is something to change.
    settings_->setVisible(!ok);
}

}  // namespace cpapdash::supervisor
