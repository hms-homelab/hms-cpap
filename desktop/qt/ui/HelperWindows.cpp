#include "HelperWindows.h"

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QRadioButton>
#include <QStyle>
#include <QVBoxLayout>

#include "Autostart.h"
#include "Theme.h"

namespace cpapdash::supervisor {

namespace {

Lang uiLang() { return langForLocale(QLocale().name().toStdString()); }

QString w(HTxt k, Lang l) { return QString::fromUtf8(htext(k, l)); }

QLabel* hint(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    QFont f = l->font();
    f.setPointSizeF(f.pointSizeF() * 0.92);
    l->setFont(f);
    return l;
}

QPushButton* primary(const QString& text, QWidget* parent) {
    auto* b = new QPushButton(text, parent);
    b->setProperty("accent", true);   // .btn-primary: solid dashboard blue
    return b;
}

std::optional<nlohmann::json> readJson(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    try {
        return nlohmann::json::parse(f.readAll().toStdString());
    } catch (...) {
        return std::nullopt;
    }
}

bool writeJson(const QString& path, const nlohmann::json& j) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QByteArray::fromStdString(j.dump(2)));
    return true;
}

std::string str(const nlohmann::json& j, const char* k) {
    return j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string();
}

QIcon trayIcon(const char* state) {
#ifdef Q_OS_MACOS
    QIcon mono(QStringLiteral(":/icons/mono-%1.png").arg(state));
    mono.addFile(QStringLiteral(":/icons/mono-%1@2x.png").arg(state));
    if (!mono.isNull()) {
        mono.setIsMask(true);
        return mono;
    }
#endif
    QIcon icon(QStringLiteral(":/icons/tray-%1.png").arg(state));
    icon.addFile(QStringLiteral(":/icons/tray-%1@2x.png").arg(state));
    if (!icon.isNull()) return icon;
    return QApplication::style()->standardIcon(QStyle::SP_ComputerIcon);
}

}  // namespace

// ── the role ────────────────────────────────────────────────────────────────

RoleWindow::RoleWindow(QWidget* parent) : QDialog(parent) {
    const Lang l = uiLang();
    setWindowTitle(w(HTxt::RoleTitle, l));
    setStyleSheet(theme::buttonStyle());
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(24, 20, 24, 20);
    v->setSpacing(10);
    auto* q = new QLabel(w(HTxt::RoleQuestion, l), this);
    QFont f = q->font();
    f.setPointSizeF(f.pointSizeF() * 1.3);
    f.setBold(true);
    q->setFont(f);
    v->addWidget(q);

    hms_ = new QRadioButton(w(HTxt::RoleHms, l), this);
    hms_->setChecked(true);
    v->addWidget(hms_);
    auto* h1 = hint(w(HTxt::RoleHmsHint, l), this);
    h1->setContentsMargins(24, 0, 0, 6);
    v->addWidget(h1);
    helper_ = new QRadioButton(w(HTxt::RoleHelper, l), this);
    v->addWidget(helper_);
    auto* h2 = hint(w(HTxt::RoleHelperHint, l), this);
    h2->setContentsMargins(24, 0, 0, 6);
    v->addWidget(h2);
    v->addStretch(1);

    auto* row = new QHBoxLayout;
    row->addStretch(1);
    auto* go = primary(w(HTxt::Continue, l), this);
    go->setDefault(true);
    connect(go, &QPushButton::clicked, this, &QDialog::accept);
    row->addWidget(go);
    v->addLayout(row);
    resize(520, 300);
}

bool RoleWindow::helperChosen() const { return helper_->isChecked(); }

// ── the helper's setup ──────────────────────────────────────────────────────

HelperSetupWindow::HelperSetupWindow(QString service_exe, const nlohmann::json& config, QWidget* parent)
    : QDialog(parent), service_exe_(std::move(service_exe)), lang_(uiLang()) {
    setWindowTitle(w(HTxt::WindowTitle, lang_));
    setStyleSheet(theme::buttonStyle());
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(22, 18, 22, 18);
    v->setSpacing(12);

    // D3: hms-cpap.
    auto* hms = new QGroupBox(w(HTxt::HmsHeading, lang_), this);
    auto* hv = new QVBoxLayout(hms);
    hms_status_ = new QLabel(w(HTxt::Looking, lang_), hms);
    hms_status_->setWordWrap(true);
    hv->addWidget(hms_status_);
    found_group_ = new QButtonGroup(hms);
    found_box_ = new QVBoxLayout;
    hv->addLayout(found_box_);
    auto* addr_row = new QHBoxLayout;
    other_radio_ = new QRadioButton(w(HTxt::AnotherAddress, lang_), hms);
    other_radio_->setVisible(false);
    found_group_->addButton(other_radio_);
    addr_row->addWidget(other_radio_);
    address_ = new QLineEdit(hms);
    address_->setPlaceholderText("192.168.1.20");
    address_->setVisible(false);
    addr_row->addWidget(address_, 1);
    hv->addLayout(addr_row);
    already_ = new QLabel(hms);
    already_->setWordWrap(true);
    already_->setStyleSheet("color: #c98a00;");
    already_->setVisible(false);
    hv->addWidget(already_);
    v->addWidget(hms);

    // D7: the OSCAR on this computer.
    auto* osc = new QGroupBox(w(HTxt::OscarHeading, lang_), this);
    auto* ov = new QVBoxLayout(osc);
    ov->addWidget(hint(w(HTxt::OscarHint, lang_), osc));
    auto* form = new QFormLayout;
    // The paths are long: the fields take the width on every platform (macOS
    // keeps them at their hint otherwise).
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft);
    const auto field = [&](QLineEdit*& edit, QLabel*& state, const QString& value, QWidget* extra = nullptr) {
        auto* col = new QVBoxLayout;
        auto* row = new QHBoxLayout;
        edit = new QLineEdit(value, osc);
        row->addWidget(edit, 1);
        if (extra) row->addWidget(extra);
        col->addLayout(row);
        state = new QLabel(osc);
        state->setWordWrap(true);
        state->setVisible(false);
        col->addWidget(state);
        return col;
    };
    const auto& o = config.is_object() && config.contains("oscar") ? config["oscar"] : nlohmann::json::object();
    form->addRow(w(HTxt::Program, lang_), field(program_, program_state_, QString::fromStdString(str(o, "program"))));
    auto* browse = new QPushButton(w(HTxt::Browse, lang_), osc);   // .btn-secondary: blue outline
    form->addRow(w(HTxt::DataDir, lang_),
                 field(data_dir_, data_state_, QString::fromStdString(str(o, "data_dir")), browse));
    const std::string prof = str(o, "profile");
    form->addRow(w(HTxt::Profile, lang_),
                 field(profile_, profile_state_, QString::fromStdString(prof.empty() ? "hms-cpap" : prof)));
    ov->addLayout(form);
    ov->addWidget(hint(w(HTxt::ProfileHint, lang_), osc));
    v->addWidget(osc);
    v->addStretch(1);

    auto* row = new QHBoxLayout;
    check_ = new QPushButton(w(HTxt::Check, lang_), this);
    row->addWidget(check_);
    row->addStretch(1);
    auto* cancel = new QPushButton(w(HTxt::Cancel, lang_), this);
    row->addWidget(cancel);
    save_ = primary(w(HTxt::Save, lang_), this);
    row->addWidget(save_);
    v->addLayout(row);

    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, w(HTxt::DataDir, lang_), data_dir_->text());
        if (!d.isEmpty()) data_dir_->setText(QDir::toNativeSeparators(d));
    });
    connect(check_, &QPushButton::clicked, this, &HelperSetupWindow::runCheck);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(save_, &QPushButton::clicked, this, &QDialog::accept);
    connect(address_, &QLineEdit::textChanged, this, &HelperSetupWindow::updateSave);
    connect(data_dir_, &QLineEdit::textChanged, this, &HelperSetupWindow::updateSave);
    connect(found_group_, &QButtonGroup::buttonToggled, this, [this](QAbstractButton*, bool) {
        address_->setEnabled(other_radio_->isChecked() || found_.empty());
        updateSave();
    });

    // A saved address is offered as the typed one.
    const auto& h = config.is_object() && config.contains("oscar_helper") ? config["oscar_helper"]
                                                                          : nlohmann::json::object();
    address_->setText(QString::fromStdString(str(h, "host")));
    updateSave();
    resize(600, 560);
    if (!service_exe_.isEmpty()) {
        discover();
        // The program field filled from where OSCAR's installer puts it.
        if (program_->text().isEmpty()) runCheck();
    }
}

void HelperSetupWindow::discover() {
    discover_proc_ = std::make_unique<QProcess>();
    connect(discover_proc_.get(), &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        showFound(parseDiscover(discover_proc_->readAllStandardOutput().toStdString()));
    });
    discover_proc_->start(service_exe_, {"--oscar-helper", "--discover"});
}

void HelperSetupWindow::showFound(const std::vector<FoundHms>& found) {
    found_ = found;
    for (auto* b : found_group_->buttons())
        if (b != other_radio_) {
            found_group_->removeButton(b);
            b->deleteLater();
        }
    QStringList others;
    if (found.empty()) {
        hms_status_->setText(w(HTxt::FoundNone, lang_) + " " + w(HTxt::TypeAddress, lang_) + ":");
        other_radio_->setVisible(false);
        address_->setVisible(true);
        address_->setEnabled(true);
    } else {
        hms_status_->setText(found.size() == 1
                                 ? w(HTxt::FoundOne, lang_).arg(QString::fromStdString(found[0].address))
                                 : w(HTxt::FoundMany, lang_));
        for (size_t i = 0; i < found.size(); ++i) {
            const auto& f = found[i];
            const QString label = QString::fromStdString(f.name.empty() ? f.address : f.name + "  (" + f.address + ")");
            auto* r = new QRadioButton(label, this);
            found_group_->addButton(r, static_cast<int>(i));
            found_box_->addWidget(r);
            if (i == 0) r->setChecked(true);
            if (!f.helper.empty()) others << QString::fromStdString(f.helper);
        }
        other_radio_->setVisible(true);
        address_->setVisible(true);
        address_->setEnabled(false);
    }
    // D4: an hms-cpap that already reads OSCAR on another PC says so.
    already_->setVisible(!others.isEmpty());
    if (!others.isEmpty()) already_->setText(w(HTxt::AlreadyReads, lang_).arg(others.join(", ")));
    updateSave();
}

void HelperSetupWindow::runCheck() {
    check_->setEnabled(false);
    check_->setText(w(HTxt::Checking, lang_));
    check_proc_ = std::make_unique<QProcess>();
    connect(check_proc_.get(), &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        check_->setEnabled(true);
        check_->setText(w(HTxt::Check, lang_));
        try {
            showCheck(nlohmann::json::parse(check_proc_->readAllStandardOutput().toStdString()));
        } catch (...) {
        }
    });
    check_proc_->start(service_exe_, {"--oscar-helper", "--check", "--program", program_->text(), "--data-dir",
                                      data_dir_->text(), "--profile", profile_->text()});
}

void HelperSetupWindow::showCheck(const nlohmann::json& check) {
    if (!check.is_object()) return;
    const auto show = [this](QLabel* l, const nlohmann::json& part, bool silent_empty) {
        const std::string st = str(part, "state");
        if (st.empty() || (silent_empty && st == "empty")) {
            l->setVisible(false);
            return;
        }
        l->setText(w(checkWord(st), lang_));
        l->setStyleSheet(st == "ok" ? "color: #2e9d55;" : "color: #c98a00;");
        l->setVisible(true);
    };
    const auto& prog = check.contains("program") ? check["program"] : nlohmann::json::object();
    if (program_->text().isEmpty() && !str(prog, "found").empty())
        program_->setText(QString::fromStdString(str(prog, "found")));
    const bool first = program_state_->isHidden() && data_state_->isHidden();
    show(program_state_, prog, first);
    show(data_state_, check.contains("data_dir") ? check["data_dir"] : nlohmann::json::object(), first);
    show(profile_state_, check.contains("profile") ? check["profile"] : nlohmann::json::object(), first);
}

void HelperSetupWindow::updateSave() {
    save_->setEnabled(!inputs().host.empty() && !data_dir_->text().trimmed().isEmpty());
}

HelperInputs HelperSetupWindow::inputs() const {
    HelperInputs in;
    const int id = found_group_->checkedId();
    if (!found_.empty() && id >= 0 && static_cast<size_t>(id) < found_.size() && !other_radio_->isChecked()) {
        in.host = found_[static_cast<size_t>(id)].address;
        in.port = found_[static_cast<size_t>(id)].port;
    } else {
        in.host = address_->text().trimmed().toStdString();
    }
    in.program = program_->text().trimmed().toStdString();
    in.data_dir = data_dir_->text().trimmed().toStdString();
    in.profile = profile_->text().trimmed().toStdString();
    return in;
}

// ── the tray ────────────────────────────────────────────────────────────────

HelperTray::HelperTray(ChildProcess* child, QString service_exe, QString data_dir, QObject* parent)
    : QObject(parent), child_(child), service_exe_(std::move(service_exe)), data_dir_(std::move(data_dir)),
      lang_(uiLang()) {
    tray_ = std::make_unique<QSystemTrayIcon>(this);
    menu_ = std::make_unique<QMenu>();
    status_action_ = menu_->addAction(w(HTxt::TrayConnecting, lang_).arg(QString()));
    status_action_->setEnabled(false);
    again_action_ = menu_->addAction(w(HTxt::ConnectAgain, lang_));
    connect(again_action_, &QAction::triggered, this, &HelperTray::connectAgain);
    again_action_->setVisible(false);
    menu_->addSeparator();
    connect(menu_->addAction(w(HTxt::ChangeSettings, lang_)), &QAction::triggered, this, &HelperTray::openSettings);
    if (autostart::isSupported()) {
        login_action_ = menu_->addAction(w(HTxt::StartAtLogin, lang_));
        login_action_->setCheckable(true);
        login_action_->setChecked(autostart::isEnabled());
        connect(login_action_, &QAction::toggled, this, [](bool on) { autostart::setEnabled(on); });
    }
    menu_->addSeparator();
    connect(menu_->addAction(w(HTxt::Quit, lang_)), &QAction::triggered, this, [this] {
        child_->stop();
        QApplication::quit();
    });
    tray_->setContextMenu(menu_.get());
    tray_->setToolTip(w(HTxt::WindowTitle, lang_));
    tray_->setIcon(trayIcon("starting"));
    tray_->show();

    // The daemon replaces status.json (write, then rename): the folder is
    // watched too, so a replaced file is seen.
    const QString dir = data_dir_ + "/oscar-helper";
    QDir().mkpath(dir);
    watch_.addPath(dir);
    if (QFile::exists(dir + "/status.json")) watch_.addPath(dir + "/status.json");
    connect(&watch_, &QFileSystemWatcher::directoryChanged, this, [this] { refresh(); });
    connect(&watch_, &QFileSystemWatcher::fileChanged, this, [this] { refresh(); });
    connect(child_, &ChildProcess::stateChanged, this, [this](ChildProcess::State s) {
        if (s == ChildProcess::State::Failed || s == ChildProcess::State::Stopped) {
            status_action_->setText(w(HTxt::TrayStopped, lang_));
            tray_->setIcon(trayIcon(s == ChildProcess::State::Failed ? "problem" : "stopped"));
        }
    });
    refresh();
}

bool HelperTray::available() const { return QSystemTrayIcon::isSystemTrayAvailable(); }

void HelperTray::announcePresence() {
    tray_->showMessage(w(HTxt::WindowTitle, lang_), status_action_->text());
}

QString HelperTray::configPath() const { return data_dir_ + "/config.json"; }

void HelperTray::refresh() {
    const QString file = data_dir_ + "/oscar-helper/status.json";
    if (QFile::exists(file) && !watch_.files().contains(file)) watch_.addPath(file);
    const auto j = readJson(file);
    if (!j) return;
    // The daemon writing its state is the child alive and answering.
    child_->setHealthy(true);
    showStatus(*j);
}

void HelperTray::showStatus(const nlohmann::json& status) {
    QString last;
    if (status.contains("last_import_at") && status["last_import_at"].is_number_integer())
        last = QDateTime::fromSecsSinceEpoch(status["last_import_at"].get<long long>()).toString("HH:mm");
    const TrayLine t = trayLine(status, last.toStdString());
    QString line = w(t.key, lang_);
    if (line.contains("%1")) line = line.arg(QString::fromStdString(t.arg1));
    if (line.contains("%2")) line = line.arg(QString::fromStdString(t.arg2));
    status_action_->setText(line);
    again_action_->setVisible(t.connect_again);
    tray_->setIcon(trayIcon(t.problem ? "problem" : (str(status, "state") == "connected" ? "running" : "starting")));
    tray_->setToolTip(status_action_->text());
}

void HelperTray::connectAgain() {
    // D4: a hello with no token; a forgotten PC never re-pairs by itself.
    auto j = readJson(configPath()).value_or(nlohmann::json::object());
    dropHelperToken(j);
    writeJson(configPath(), j);
    child_->restart();
}

void HelperTray::openSettings() {
    auto j = readJson(configPath()).value_or(nlohmann::json::object());
    HelperSetupWindow win(service_exe_, j);
    if (win.exec() != QDialog::Accepted) return;
    applyHelperInputs(j, win.inputs());
    writeJson(configPath(), j);
    child_->restart();
}

}  // namespace cpapdash::supervisor
