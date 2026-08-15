#include "ConfiguratorWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QFont>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QFrame>
#include <QPixmap>
#include <QVBoxLayout>
#include <QWizardPage>

#include <nlohmann/json.hpp>

#include "Theme.h"
#include "utils/CardLayout.h"

namespace cpapdash::supervisor {

namespace {

/// The unit advertises itself over mDNS as _cpapdash._tcp.local, and AppConfig
/// already documents http://cpapdash.local as the Mule address. Defaulting to a
/// NAME rather than an address also survives the unit getting a new IP.
constexpr const char* kMuleDefault   = "http://cpapdash.local";


QLabel* explain(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    // No colour override: palette(mid) is a border colour and is DARK on dark
    // themes, so secondary text written in it is invisible. The theme's own
    // text colour is readable in both appearances by definition.
    QFont f = l->font();
    f.setPointSizeF(f.pointSizeF() * 0.95);
    l->setFont(f);
    return l;
}

}  // namespace

// ── Welcome ────────────────────────────────────────────────────────────────
//
// Says WHY it is here. A wizard that opens with a generic greeting when the
// user has been running fine for months is disorienting; naming the reason
// ("the folder you chose no longer exists") makes it obvious.

class WelcomePage : public QWizardPage {
public:
    explicit WelcomePage(ConfiguratorWindow* wiz) : wiz_(wiz) {
        setTitle("CpapDash Setup");

        auto* layout = new QVBoxLayout(this);
        layout->addWidget(explain(QString::fromStdString(wiz_->gate_.summary), this));

        if (!wiz_->gate_.issues.empty()) {
            auto* box = new QGroupBox("What needs fixing", this);
            auto* v = new QVBoxLayout(box);
            for (const auto& issue : wiz_->gate_.issues) {
                auto* l = new QLabel("- " + QString::fromStdString(issue.message), box);
                l->setWordWrap(true);
                v->addWidget(l);
            }
            layout->addWidget(box);
        }

        if (wiz_->gate_.reason == SetupReason::Unparseable) {
            auto* warn = new QLabel(
                "Your existing config.json will be replaced when you finish. "
                "If you would rather repair it by hand, close this window now.", this);
            warn->setWordWrap(true);
            warn->setStyleSheet("color: #a06800;");
            layout->addWidget(warn);
        }

        layout->addStretch(1);
    }

private:
    ConfiguratorWindow* wiz_;
};

// ── Where does the data come from ──────────────────────────────────────────

class SourcePage : public QWizardPage {
public:
    explicit SourcePage(ConfiguratorWindow* wiz) : wiz_(wiz) {
        setTitle("Where is your CPAP data?");

        auto* layout = new QVBoxLayout(this);
        layout->setSpacing(10);

        // The CHOICE first, as one block. Previously the detail fields sat
        // directly under the second radio, so the whole form read as belonging
        // to that option -- the page looked like "folder on this computer" had
        // a unit address.
        ezshare_ = new QRadioButton("A CpapDash Mule or Miner on my network", this);
        local_   = new QRadioButton("A folder on this computer", this);

        auto* choices = new QWidget(this);
        auto* cv = new QVBoxLayout(choices);
        cv->setContentsMargins(0, 0, 0, 0);
        cv->setSpacing(3);

        cv->addWidget(ezshare_);
        auto* ez_help = explain(
            "The unit sits next to your machine, reads the SD card, and CpapDash "
            "collects from it over your normal network.", choices);
        ez_help->setContentsMargins(24, 0, 0, 8);   // indented under its radio
        cv->addWidget(ez_help);

        cv->addWidget(local_);
        auto* local_help = explain(
            "Point at the card ROOT: the folder holding both STR.edf and DATALOG.",
            choices);
        local_help->setContentsMargins(24, 0, 0, 2);
        cv->addWidget(local_help);

        layout->addWidget(choices);

        // A clean break between "which one" and "the details for it".
        //
        // A painted 1px frame, NOT QFrame::HLine. HLine draws through the style
        // engine, which ignores a stylesheet `color` on macOS and renders an
        // almost invisible hairline -- and palette(mid) is the same trap that
        // made the body text disappear on a dark theme. A mid grey at partial
        // alpha is legible against both light and dark backgrounds without
        // guessing which one is in use.
        auto* divider = new QFrame(this);
        divider->setFrameShape(QFrame::NoFrame);
        divider->setFixedHeight(1);
        divider->setStyleSheet("background-color: rgba(150, 150, 150, 0.55);");
        layout->addSpacing(18);
        layout->addWidget(divider);
        layout->addSpacing(18);

        form_ = new QFormLayout();
        auto* form = form_;
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        // Labels align to the TOP of their field, not the middle. A row whose
        // field is a stack -- an address with its own checkbox under it -- would
        // otherwise centre the caption against the whole stack and leave it
        // floating beside the wrong line.
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignTop);
        form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
        form->setHorizontalSpacing(14);
        form->setVerticalSpacing(18);

        url_ = new QLineEdit(this);
        url_->setMinimumWidth(320);
        // One field, because there is one setting. ezshare_url is a base URL
        // handed straight to EzShareClient and NOTHING in the service branches
        // on what is in it -- a Mule, a Miner and the card's own access point
        // are the same string in different clothes. Offering a choice between
        // them would be a UI distinction with no code behind it.
        url_->setToolTip("The CpapDash unit, or the card's own address "
                         "if this computer is on the card's WiFi.");
        url_->setText(QString::fromStdString(wiz_->model_.getString("ezshare_url")));
        if (url_->text().isEmpty()) url_->setText(kMuleDefault);
        form->addRow("Unit address", url_);



        // Folders are CHOSEN, never typed. A typed path is a trailing space, a
        // backslash on the wrong side, or a folder that does not exist yet.
        folder_row_ = new QWidget(this);
        auto* folder_row = folder_row_;
        auto* fh = new QHBoxLayout(folder_row);
        fh->setContentsMargins(0, 0, 0, 0);
        folder_ = new QLineEdit(folder_row);
        folder_->setReadOnly(true);
        folder_->setPlaceholderText("No folder chosen");
        folder_->setText(QString::fromStdString(wiz_->model_.getString("local_dir")));
        fh->addWidget(folder_, 1);
        auto* browse = new QPushButton("Choose...", folder_row);
        fh->addWidget(browse);
        form->addRow("Card folder", folder_row);

        archive_row_ = new QWidget(this);
        auto* ah = new QHBoxLayout(archive_row_);
        ah->setContentsMargins(0, 0, 0, 0);
        archive_ = new QLineEdit(archive_row_);
        archive_->setReadOnly(true);
        archive_->setPlaceholderText("No folder chosen");
        archive_->setText(QString::fromStdString(wiz_->model_.getString("archive_dir")));
        ah->addWidget(archive_, 1);
        auto* browse_archive = new QPushButton("Choose...", archive_row_);
        ah->addWidget(browse_archive);
        form->addRow("Copy the card to", archive_row_);

        layout->addLayout(form);

        todo_ = explain("", this);
        todo_->setContentsMargins(0, 6, 0, 0);
        layout->addWidget(todo_);

        error_ = new QLabel(this);
        error_->setWordWrap(true);
        error_->setStyleSheet("color: #c0392b;");
        error_->setVisible(false);
        layout->addWidget(error_);

        layout->addStretch(1);

        connect(browse, &QPushButton::clicked, this, &SourcePage::chooseCardFolder);
        connect(browse_archive, &QPushButton::clicked, this, [this] {
            const QString dir = QFileDialog::getExistingDirectory(
                this, "Choose a folder to copy the card into", archive_->text());
            if (dir.isEmpty()) return;
            archive_->setText(dir);
            emit completeChanged();
        });
        connect(ezshare_, &QRadioButton::toggled, this, [this] { sync(); });
        connect(local_,   &QRadioButton::toggled, this, [this] { sync(); });
        connect(url_, &QLineEdit::textEdited, this, [this] { emit completeChanged(); });

        const auto src = wiz_->model_.getString("source");
        (src == "local" ? local_ : ezshare_)->setChecked(true);
        sync();
    }

    bool isComplete() const override {
        return missingPiece().isEmpty();
    }

    /// What still has to be filled in, or empty when the page is done.
    ///
    /// A disabled Next with nothing explaining it is the same complaint as a
    /// greyed-out field: the user can see they are stuck and cannot see why.
    QString missingPiece() const {
        if (local_->isChecked()) {
            if (folder_->text().trimmed().isEmpty())
                return "Choose the folder that holds your SD card data.";
            return {};
        }
        if (ezshare_->isChecked()) {
            if (url_->text().trimmed().isEmpty())
                return "Enter the address of your CpapDash unit.";
            // The card is DOWNLOADED over the network, so without somewhere to
            // put it nothing reaches disk at all.
            if (archive_->text().trimmed().isEmpty())
                return "Choose where to copy the card to. Downloaded files have "
                       "to be written somewhere.";
            return {};
        }
        return "Choose where your CPAP data comes from.";
    }

    bool validatePage() override {
        if (local_->isChecked()) {
            wiz_->model_.set("source", "local");
            wiz_->model_.set("local_dir", folder_->text().toStdString());
        } else {
            wiz_->model_.set("source", "ezshare");
            wiz_->model_.set("ezshare_url", url_->text().trimmed().toStdString());
        }
        wiz_->model_.set("archive_dir", archive_->text().toStdString());
        return true;
    }

private:
    void sync() {
        const bool is_local = local_->isChecked();

        // HIDDEN, not disabled. A greyed-out field still occupies the page and
        // still asks the user to read it and work out why it does not apply to
        // them -- which is worse than it simply not being there. setRowVisible
        // takes the LABEL with the field; hiding only the widget would leave an
        // orphaned caption pointing at nothing.
        form_->setRowVisible(url_,          !is_local);
        form_->setRowVisible(folder_row_,     is_local);

        // An ezShare card is DOWNLOADED, so it must be written somewhere or
        // nothing reaches disk. A local folder is already on disk and is read
        // in place, so asking where to copy it to would be meaningless.
        form_->setRowVisible(archive_row_,  !is_local);

        const QString missing = missingPiece();
        todo_->setText(missing);
        todo_->setVisible(!missing.isEmpty() && !error_->isVisible());

        emit completeChanged();
    }

    void chooseCardFolder() {
        const QString dir = QFileDialog::getExistingDirectory(
            this, "Choose your SD card folder", folder_->text());
        if (dir.isEmpty()) return;

        // The same classifier the service uses (SDD-010). Catching "you picked
        // DATALOG" here, with the real remedy, beats importing nothing and
        // leaving the user to work out why STR.edf was never found.
        const auto layout = hms_cpap::classifyLocalDir(dir.toStdString());
        if (layout != hms_cpap::LocalDirLayout::Root) {
            error_->setText(
                QString::fromStdString(hms_cpap::localDirProblem(layout, dir.toStdString())) +
                "\n\n" +
                QString::fromStdString(hms_cpap::localDirRemedy(layout, dir.toStdString())));
            error_->setVisible(true);
            return;
        }

        error_->setVisible(false);
        folder_->setText(dir);
        emit completeChanged();
    }

    ConfiguratorWindow* wiz_;
    QRadioButton* ezshare_ = nullptr;
    QRadioButton* local_   = nullptr;
    QLineEdit*    url_     = nullptr;
    QLineEdit*    folder_  = nullptr;
    QLineEdit*    archive_ = nullptr;
    QWidget*      archive_row_ = nullptr;
    QWidget*      folder_row_  = nullptr;
    QFormLayout*  form_        = nullptr;
    QLabel*       error_   = nullptr;
    QLabel*       todo_    = nullptr;
};

// ── Where do the results go ────────────────────────────────────────────────

class DatabasePage : public QWizardPage {
public:
    explicit DatabasePage(ConfiguratorWindow* wiz) : wiz_(wiz) {
        setTitle("Where should CpapDash keep your nights?");

        auto* layout = new QVBoxLayout(this);

        builtin_ = new QRadioButton("Built in (recommended)", this);
        server_  = new QRadioButton("A database server I already run", this);
        layout->addWidget(builtin_);
        layout->addWidget(explain("A single file in your CpapDash folder. Nothing to install.", this));
        layout->addWidget(server_);

        server_box_ = new QGroupBox(this);
        auto* form = new QFormLayout(server_box_);

        engine_ = new QComboBox(server_box_);
        engine_->addItem("PostgreSQL", "postgresql");
        engine_->addItem("MySQL", "mysql");
        form->addRow("Engine", engine_);

        host_ = new QLineEdit(server_box_); host_->setText("localhost");
        port_ = new QSpinBox(server_box_);  port_->setRange(1, 65535); port_->setValue(5432);
        name_ = new QLineEdit(server_box_); name_->setText("cpap");
        user_ = new QLineEdit(server_box_); user_->setText("cpap_user");
        pass_ = new QLineEdit(server_box_); pass_->setEchoMode(QLineEdit::Password);
        form->addRow("Host", host_);
        form->addRow("Port", port_);
        form->addRow("Database", name_);
        form->addRow("User", user_);
        form->addRow("Password", pass_);

        test_button_ = new QPushButton("Test connection", server_box_);
        form->addRow("", test_button_);

        result_ = new QLabel(server_box_);
        result_->setWordWrap(true);
        form->addRow("", result_);

        layout->addWidget(server_box_);
        layout->addStretch(1);

        connect(builtin_, &QRadioButton::toggled, this, [this] { sync(); });
        connect(server_,  &QRadioButton::toggled, this, [this] { sync(); });
        connect(engine_, &QComboBox::currentIndexChanged, this, [this] {
            port_->setValue(engine_->currentData().toString() == "mysql" ? 3306 : 5432);
            invalidate();
        });
        for (auto* e : {host_, name_, user_, pass_})
            connect(e, &QLineEdit::textEdited, this, [this] { invalidate(); });
        connect(test_button_, &QPushButton::clicked, this, &DatabasePage::testConnection);

        const bool is_server = wiz_->model_.getString("database.type") != "sqlite"
                            && !wiz_->model_.getString("database.type").empty();
        (is_server ? server_ : builtin_)->setChecked(true);
        sync();
    }

    // A server database cannot be accepted on trust. The wizard exists to stop
    // someone finishing setup with credentials that do not work, so Next waits
    // for a real connection.
    bool isComplete() const override {
        return builtin_->isChecked() || verified_;
    }

    bool validatePage() override {
        if (builtin_->isChecked()) {
            wiz_->model_.set("database.type", "sqlite");
            return true;
        }
        wiz_->model_.set("database.type", engine_->currentData().toString().toStdString());
        wiz_->model_.set("database.host", host_->text().trimmed().toStdString());
        wiz_->model_.set("database.port", port_->value());
        wiz_->model_.set("database.name", name_->text().trimmed().toStdString());
        wiz_->model_.set("database.user", user_->text().trimmed().toStdString());
        wiz_->model_.set("database.password", pass_->text().toStdString());
        return true;
    }

private:
    void sync() {
        server_box_->setVisible(server_->isChecked());
        emit completeChanged();
    }

    void invalidate() {
        // Changing a field after a successful test must un-verify it. The web
        // wizard does not do this: verify, then change the host, and Next stays
        // enabled on a connection nobody ever made.
        verified_ = false;
        result_->clear();
        emit completeChanged();
    }

    void testConnection() {
        // Asked of the SERVICE BINARY, not of a running service -- there is no
        // service yet. `hms_cpap --test-db` reads a request on stdin and answers
        // on stdout, so one implementation of "reachable" serves both the wizard
        // and the settings dialog.
        nlohmann::json req;
        req["type"]     = engine_->currentData().toString().toStdString();
        req["host"]     = host_->text().trimmed().toStdString();
        req["port"]     = port_->value();
        req["name"]     = name_->text().trimmed().toStdString();
        req["user"]     = user_->text().trimmed().toStdString();
        req["password"] = pass_->text().toStdString();

        QProcess p;
        p.setProgram(wiz_->service_exe_);
        p.setArguments({"--test-db"});
        p.start();
        if (!p.waitForStarted(5000)) {
            result_->setText("Could not run the database check.");
            result_->setStyleSheet("color: #c0392b;");
            return;
        }
        p.write(QByteArray::fromStdString(req.dump()));
        p.closeWriteChannel();
        p.waitForFinished(20000);

        try {
            const auto out = nlohmann::json::parse(
                QString::fromUtf8(p.readAllStandardOutput()).toStdString());
            verified_ = out.value("ok", false);
            if (verified_) {
                const long long count = out.value("session_count", 0LL);
                result_->setText(count > 0
                    ? QString("Connected. This database already holds %1 night(s); "
                              "they will be reused.").arg(count)
                    : "Connected. The tables will be created when CpapDash starts.");
                result_->setStyleSheet("color: #2d7a2d;");
            } else {
                result_->setText(QString::fromStdString(out.value("error", "Could not connect.")));
                result_->setStyleSheet("color: #c0392b;");
            }
        } catch (const std::exception&) {
            verified_ = false;
            result_->setText("The database check did not answer.");
            result_->setStyleSheet("color: #c0392b;");
        }
        emit completeChanged();
    }

    ConfiguratorWindow* wiz_;
    QRadioButton* builtin_ = nullptr;
    QRadioButton* server_  = nullptr;
    QGroupBox*    server_box_ = nullptr;
    QComboBox*    engine_ = nullptr;
    QLineEdit*    host_ = nullptr;
    QSpinBox*     port_ = nullptr;
    QLineEdit*    name_ = nullptr;
    QLineEdit*    user_ = nullptr;
    QLineEdit*    pass_ = nullptr;
    QPushButton*  test_button_ = nullptr;
    QLabel*       result_ = nullptr;
    bool          verified_ = false;
};

// ── Done ───────────────────────────────────────────────────────────────────

class ReadyPage : public QWizardPage {
public:
    explicit ReadyPage(ConfiguratorWindow* wiz) : wiz_(wiz) {
        setTitle("Ready to go");
        auto* layout = new QVBoxLayout(this);
        summary_ = new QLabel(this);
        summary_->setWordWrap(true);
        layout->addWidget(summary_);
        layout->addStretch(1);
    }

    void initializePage() override {
        // Marks the configuration as finished, which is what the gate reads on
        // the next launch.
        wiz_->model_.set("setup_complete", true);

        const auto src = wiz_->model_.getString("source");
        QString text = "CpapDash will read from ";
        text += (src == "local")
            ? "the folder you chose"
            : "your ezShare card at " + QString::fromStdString(wiz_->model_.getString("ezshare_url"));
        text += ", and store your nights in ";
        text += (wiz_->model_.getString("database.type") == "sqlite")
            ? "its own database file."
            : "your " + QString::fromStdString(wiz_->model_.getString("database.type")) + " server.";
        text += "\n\nFinish to save this and start collecting. "
                "Everything else can be changed later from Settings.";
        summary_->setText(text);
    }

private:
    ConfiguratorWindow* wiz_;
    QLabel* summary_ = nullptr;
};

// ── The wizard ─────────────────────────────────────────────────────────────

ConfiguratorWindow::ConfiguratorWindow(ConfigModel model, GateResult gate,
                                       QString service_exe, QWidget* parent)
    : QWizard(parent),
      model_(std::move(model)),
      gate_(std::move(gate)),
      service_exe_(std::move(service_exe)) {
    setWindowTitle("CpapDash Setup");

    // ModernStyle on every platform, deliberately. MacStyle has no side widget
    // and no banner slot, so the brand panel below simply would not render
    // there -- and a setup window that looks like the product on two platforms
    // out of three is worse than one that looks the same everywhere.
    setWizardStyle(QWizard::ModernStyle);
    setMinimumSize(720, 540);
    setOption(QWizard::NoBackButtonOnStartPage, true);
    setStyleSheet(theme::headerStyle() + theme::accentStyle());

    // The brand, as the side panel QWizard reserves for exactly this. Building
    // it as a widget rather than a pixmap means it scales with the window and
    // stays sharp on any display, instead of being a bitmap stretched to fit.
    auto* side = new QFrame(this);
    side->setObjectName("Header");
    side->setFixedWidth(190);
    auto* sl = new QVBoxLayout(side);
    sl->setContentsMargins(20, 26, 20, 20);
    sl->setSpacing(12);

    auto* mark = new QLabel(side);
    mark->setPixmap(QPixmap(":/icons/app-256.png")
                        .scaled(84, 84, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    mark->setAlignment(Qt::AlignHCenter);
    sl->addWidget(mark);

    auto* name = new QLabel("CpapDash", side);
    QFont nf = name->font();
    nf.setPointSizeF(nf.pointSizeF() * 1.45);
    nf.setBold(true);
    name->setFont(nf);
    name->setAlignment(Qt::AlignHCenter);
    sl->addWidget(name);

    // Deliberately no tagline. Marketing copy is not the setup window's to
    // invent, and a slogan nobody signed off on ends up shipped.
    auto* step = new QLabel(side);
    step->setObjectName("Sub");
    step->setAlignment(Qt::AlignHCenter);
    step->setWordWrap(true);
    sl->addWidget(step);
    step_label_ = step;

    sl->addStretch(1);
    setSideWidget(side);

    // The side panel tracks progress instead: "Step 2 of 4" is information the
    // user can act on, which a slogan is not.
    connect(this, &QWizard::currentIdChanged, this, [this](int) {
        if (!step_label_) return;
        const auto ids = pageIds();
        const int pos = ids.indexOf(currentId()) + 1;
        step_label_->setText(pos > 0
            ? QString("Step %1 of %2").arg(pos).arg(ids.size())
            : QString());
    });

    addPage(new WelcomePage(this));
    addPage(new SourcePage(this));
    addPage(new DatabasePage(this));
    addPage(new ReadyPage(this));
}

}  // namespace cpapdash::supervisor
