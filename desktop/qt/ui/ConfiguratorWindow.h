#pragma once

#include <QWizard>
#include <QString>
#include <memory>

#include "ConfigGate.h"
#include "ConfigModel.h"

class QLabel;
class QLineEdit;
class QComboBox;
class QRadioButton;
class QPushButton;

namespace cpapdash::supervisor {

/**
 * SDD-016: the first-run wizard, and the way back from a broken configuration.
 *
 * This is what the supervisor opens when the gate says the child cannot run.
 * It is a native window, not a browser page, and that is the point: the web
 * wizard needs the service running to serve it, which is exactly what is not
 * happening when there is nothing configured. Telling a first-time user to
 * visit a URL that will not answer is not a setup flow.
 *
 * It writes config.json DIRECTLY, which is safe here by construction: the
 * service is not running, so there is no second writer and no API to race.
 *
 * Each step validates through the same FieldSpec rules the settings dialog
 * uses, and Next stays disabled until the step is valid -- so it is impossible
 * to walk out of the wizard with a configuration the supervisor would refuse to
 * start, which is the whole failure this replaces.
 */
class ConfiguratorWindow : public QWizard {
    Q_OBJECT

public:
    ConfiguratorWindow(ConfigModel model, GateResult gate,
                       QString service_exe, QWidget* parent = nullptr);

    /// The finished configuration. Read after exec() returns Accepted.
    const ConfigModel& model() const { return model_; }

private:
    ConfigModel model_;
    GateResult  gate_;
    QString     service_exe_;
    QLabel*     step_label_ = nullptr;

    friend class WelcomePage;
    friend class SourcePage;
    friend class DatabasePage;
    friend class ReadyPage;
};

}  // namespace cpapdash::supervisor
