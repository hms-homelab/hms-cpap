#pragma once

#include <QDialog>
#include <QString>
#include <QTimer>
#include <vector>

#include "ChildProcess.h"
#include "PreflightReport.h"

class QLabel;
class QPushButton;
class QNetworkAccessManager;
class QVBoxLayout;

namespace cpapdash::supervisor {

/**
 * SDD-016: starting up, where the user can watch it.
 *
 * Finishing the wizard used to hand control straight back to the menu bar, and
 * the service then took several seconds to check its configuration, create its
 * schema and start serving. During that gap the tray showed a stale menu and
 * nothing said what was happening -- so "did it work?" had no answer except
 * clicking around and guessing.
 *
 * This is the answer: the three things that actually happen, in order, with
 * their real outcomes, and a Done button that only means something once they
 * are finished.
 *
 *   1. Check the configuration     hms_cpap --preflight
 *   2. Start CpapDash              spawn the child
 *   3. Wait for it to respond      poll /health
 *
 * Every step reports what the SERVICE said, not a guess. A preflight failure
 * shows the failing check and its remedy rather than "something went wrong",
 * because preflight already writes for the person who has to fix it.
 */
class StartupWindow : public QDialog {
    Q_OBJECT

public:
    StartupWindow(ChildProcess* child, QString service_exe,
                  int web_port, QWidget* parent = nullptr);

    /// True when the service came up and answered. False means the user is
    /// looking at a failure and the tray should reflect a stopped service.
    bool succeeded() const { return succeeded_; }

signals:
    /// The user wants to go back and change something.
    void settingsRequested();

private:
    enum class StepState { Pending, Running, Ok, Failed };

    struct Step {
        QString  title;
        QLabel*  icon   = nullptr;
        QLabel*  label  = nullptr;
        QLabel*  detail = nullptr;
    };

    void addStep(QVBoxLayout* into, const QString& title);
    void setStep(int index, StepState state, const QString& detail = {});
    void runPreflight();
    PreflightReport runPreflight_impl();
    void startService();
    void pollHealth();
    void finish(bool ok, const QString& message);

    ChildProcess* child_;
    QString       service_exe_;
    int           web_port_ = 8893;

    std::vector<Step> steps_;
    QLabel*      summary_ = nullptr;
    QPushButton* done_    = nullptr;
    QPushButton* settings_= nullptr;

    QNetworkAccessManager* net_ = nullptr;
    QTimer  health_timer_;
    int     health_attempts_ = 0;
    bool    succeeded_ = false;

    /// The service has to create its schema and open its database before it
    /// answers, which on a cold PostgreSQL can take a few seconds. Forty
    /// half-second attempts is twenty seconds -- long enough not to give up on
    /// a slow first start, short enough that a genuinely dead service does not
    /// leave someone watching a spinner forever.
    static constexpr int kMaxHealthAttempts = 40;
    static constexpr int kHealthIntervalMs  = 500;
};

}  // namespace cpapdash::supervisor
