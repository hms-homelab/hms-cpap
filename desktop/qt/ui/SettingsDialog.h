#pragma once

#include <QDialog>
#include <QMap>
#include <QString>
#include <QWidget>
#include <memory>
#include <vector>

#include "ConfigModel.h"
#include "FieldSpec.h"

class QVBoxLayout;
class QLabel;
class QFormLayout;
class QPushButton;
class QListWidget;
class QStackedWidget;

namespace cpapdash::supervisor {

class ServiceClient;

/**
 * SDD-016: every setting, natively, so nobody has to edit JSON.
 *
 * Built from the FieldSpec table rather than by hand, so a new config key is
 * one row and appears here automatically, already validated and already
 * restart-flagged.
 *
 * Where it writes depends on whether the service is alive, and the rule is
 * about LIVENESS rather than setup state:
 *
 *   running  -> PUT /api/config, then the supervisor restarts the child if the
 *               change needs it. The service stays the only writer of the file.
 *   stopped  -> write config.json directly. There is no API to call and no web
 *               UI to reach, so nothing else can be writing it either.
 *
 * The obvious alternative -- "the supervisor may only write during first run"
 * -- builds a trap: set the port to one that is taken, the service will not
 * start, the web UI is unreachable BECAUSE it will not start, and a read-only
 * settings dialog has nothing to offer. That state is reachable by ordinary
 * use, and getting out of it is the reason this program exists.
 */
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    SettingsDialog(ConfigModel model, bool service_running, QWidget* parent = nullptr);

    /// The edited document. Read after exec() returns Accepted.
    const ConfigModel& model() const { return model_; }

    /// Fields that changed and only take effect after a restart.
    std::vector<std::string> restartReasons() const;

signals:
    /// The user asked to wipe their configuration and start again.
    void startOverRequested();

private:
    void buildUi();
    QWidget* buildGroupPage(const Group& group);
    QWidget* buildEditor(const FieldSpec& spec);
    void revalidate();
    void applyIssues(const std::vector<Issue>& issues);
    void chooseDirectory(const FieldSpec& spec);
    void pullFromWidgets();

    ConfigModel model_;
    bool service_running_ = false;

    QListWidget*    nav_   = nullptr;
    QStackedWidget* stack_ = nullptr;
    QPushButton* save_button_= nullptr;
    QLabel*      banner_     = nullptr;

    struct Row {
        FieldSpec spec;
        QWidget*  editor = nullptr;
        QLabel*   error  = nullptr;
        QWidget*  container = nullptr;
        QFormLayout* form = nullptr;   ///< so the label hides with the field
    };
    QMap<QString, Row> rows_;

    bool building_ = false;   ///< suppresses revalidate() while widgets are populated
};

}  // namespace cpapdash::supervisor
