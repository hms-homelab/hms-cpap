#pragma once

#include <QDialog>
#include <QFileSystemWatcher>
#include <QMenu>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QSystemTrayIcon>

#include <memory>
#include <vector>

#include <nlohmann/json.hpp>

#include "ChildProcess.h"
#include "HelperSetup.h"

class QAction;
class QButtonGroup;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QVBoxLayout;

namespace cpapdash::supervisor {

/**
 * SDD-081 D1, F9: the first window asks what this computer is for. hms-cpap
 * itself goes on to the usual wizard; "Read OSCAR for hms-cpap" to the
 * helper's window. One role per install.
 */
class RoleWindow : public QDialog {
    Q_OBJECT
public:
    explicit RoleWindow(QWidget* parent = nullptr);
    bool helperChosen() const;

private:
    QRadioButton* hms_ = nullptr;
    QRadioButton* helper_ = nullptr;
};

/**
 * SDD-081 D3, D7: the helper's window. It looks for hms-cpap once, when it
 * opens (`hms_cpap --oscar-helper --discover`, its own process: the window
 * stays live), and asks for the address only when nothing answered. Then the
 * three OSCAR fields, checked with `--oscar-helper --check`.
 */
class HelperSetupWindow : public QDialog {
    Q_OBJECT
public:
    HelperSetupWindow(QString service_exe, const nlohmann::json& config, QWidget* parent = nullptr);
    HelperInputs inputs() const;

    /// Shows what a look found, as if `--discover` had answered (also its
    /// own answer's path).
    void showFound(const std::vector<FoundHms>& found);
    /// Shows a check's answer (the `--check` JSON).
    void showCheck(const nlohmann::json& check);

private:
    void discover();
    void runCheck();
    void updateSave();

    QString service_exe_;
    Lang lang_;
    QLabel* hms_status_ = nullptr;
    QVBoxLayout* found_box_ = nullptr;
    QButtonGroup* found_group_ = nullptr;
    QRadioButton* other_radio_ = nullptr;
    QLineEdit* address_ = nullptr;
    QLabel* already_ = nullptr;
    QLineEdit* program_ = nullptr;
    QLineEdit* data_dir_ = nullptr;
    QLineEdit* profile_ = nullptr;
    QLabel* program_state_ = nullptr;
    QLabel* data_state_ = nullptr;
    QLabel* profile_state_ = nullptr;
    QPushButton* check_ = nullptr;
    QPushButton* save_ = nullptr;
    std::vector<FoundHms> found_;
    std::unique_ptr<QProcess> discover_proc_;
    std::unique_ptr<QProcess> check_proc_;
};

/**
 * SDD-081: the helper's tray. Its line is the daemon's state, read from
 * <data dir>/oscar-helper/status.json each time the daemon writes it (a file
 * watch: an event, never a poll). "Connect again" (D4) drops the token and
 * starts the helper again, so its hello carries none.
 */
class HelperTray : public QObject {
    Q_OBJECT
public:
    HelperTray(ChildProcess* child, QString service_exe, QString data_dir, QObject* parent = nullptr);
    bool available() const;
    void announcePresence();
    /// Reads status.json now (also the watch's slot).
    void refresh();
    /// Shows `status` as if the daemon had written it.
    void showStatus(const nlohmann::json& status);

private:
    void openSettings();
    void connectAgain();
    QString configPath() const;

    ChildProcess* child_;
    QString service_exe_;
    QString data_dir_;
    Lang lang_;
    std::unique_ptr<QSystemTrayIcon> tray_;
    std::unique_ptr<QMenu> menu_;
    QAction* status_action_ = nullptr;
    QAction* again_action_ = nullptr;
    QAction* login_action_ = nullptr;
    QFileSystemWatcher watch_;
};

}  // namespace cpapdash::supervisor
