#include "SettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QBrush>
#include <QListWidget>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "Theme.h"
#include "utils/CardLayout.h"

namespace cpapdash::supervisor {

namespace {

/// Marks a widget so one stylesheet rule can paint every invalid field, rather
/// than each editor carrying its own colour logic.
void setFieldState(QWidget* w, const char* state) {
    if (!w) return;
    w->setProperty("state", state);
    w->style()->unpolish(w);
    w->style()->polish(w);
}

const char* kStyleSheet = R"(
    QLineEdit[state="error"], QSpinBox[state="error"], QDoubleSpinBox[state="error"],
    QComboBox[state="error"] {
        border: 1px solid #c0392b;
    }
    QLineEdit[state="warning"], QSpinBox[state="warning"], QComboBox[state="warning"] {
        border: 1px solid #d99100;
    }
    QLabel[role="error"]   { color: #c0392b; }
    QLabel[role="warning"] { color: #a06800; }
    QLabel[role="help"]    { color: palette(placeholder-text); }
)";

}  // namespace

SettingsDialog::SettingsDialog(ConfigModel model, bool service_running, QWidget* parent)
    : QDialog(parent), model_(std::move(model)), service_running_(service_running) {
    setWindowTitle("CpapDash Settings");
    setMinimumSize(760, 620);
    setStyleSheet(kStyleSheet + theme::scrollBarStyle());
    buildUi();
    revalidate();
}

void SettingsDialog::buildUi() {
    building_ = true;

    auto* outer = new QVBoxLayout(this);

    banner_ = new QLabel(this);
    banner_->setWordWrap(true);
    banner_->setVisible(false);
    outer->addWidget(banner_);

    // A sidebar, not tabs. Fifteen tabs across the top compress to a few
    // characters each and become unreadable -- and they cannot grow, because
    // the config keeps gaining sections. A vertical list has room for the real
    // names, scrolls when it runs out, and is where every desktop settings
    // window has put them for a decade.
    auto* split = new QHBoxLayout();
    split->setContentsMargins(0, 0, 0, 0);
    split->setSpacing(0);

    nav_ = new QListWidget(this);
    nav_->setFixedWidth(196);
    nav_->setFrameShape(QFrame::NoFrame);
    nav_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // No background of its own. palette(alternate-base) is a LIGHT colour on
    // this theme, so tinting the sidebar with it put a grey slab beside a dark
    // window. The list inherits the window instead, and only the selected row
    // is painted.
    nav_->setStyleSheet(
        "QListWidget { background: transparent; border: none;"
        "              outline: none; padding: 6px 0; }"
        "QListWidget::item { padding: 8px 14px; border: none; }"
        "QListWidget::item:selected {"
        "   background: " + QString(theme::kNavyMid) + "; color: #FFFFFF; }"
        "QListWidget::item:hover:!selected { background: rgba(128,128,128,0.16); }");
    split->addWidget(nav_);

    stack_ = new QStackedWidget(this);
    split->addWidget(stack_, 1);

    for (const auto& group : settingsGroups()) {
        stack_->addWidget(buildGroupPage(group));
        auto* item = new QListWidgetItem(QString::fromStdString(group.title), nav_);
        item->setData(Qt::UserRole, QString::fromStdString(group.key));
    }
    connect(nav_, &QListWidget::currentRowChanged, stack_, &QStackedWidget::setCurrentIndex);
    nav_->setCurrentRow(0);

    // Every section on screen at once, no scrolling. Scrolling a list of
    // section names is a poor way to navigate: half the destinations are
    // hidden, so finding one means already knowing it is there.
    if (nav_->count() > 0) {
        const int row_h = nav_->sizeHintForRow(0);
        nav_->setMinimumHeight(row_h * nav_->count() + 14);
    }

    outer->addLayout(split, 1);

    auto* buttons = new QDialogButtonBox(this);
    save_button_ = buttons->addButton("Save", QDialogButtonBox::AcceptRole);
    buttons->addButton("Cancel", QDialogButtonBox::RejectRole);

    // Destructive, so it sits apart from Save and Cancel and asks again before
    // doing anything.
    auto* start_over = buttons->addButton("Start Over...", QDialogButtonBox::ResetRole);
    connect(start_over, &QPushButton::clicked, this, [this] {
        emit startOverRequested();
    });

    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        pullFromWidgets();
        // Checked again on the way out, independently of the live gating: a
        // rule can depend on a field in a tab that was never opened.
        if (!blockingIssues(model_).empty()) { revalidate(); return; }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(buttons);

    building_ = false;
}

QWidget* SettingsDialog::buildGroupPage(const Group& group) {
    auto* page   = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    if (!group.description.empty()) {
        auto* desc = new QLabel(QString::fromStdString(group.description), page);
        desc->setWordWrap(true);
        desc->setProperty("role", "help");
        layout->addWidget(desc);
    }

    auto* form_host = new QWidget(page);
    auto* form = new QFormLayout(form_host);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    for (const auto& spec : settingsFields()) {
        if (spec.group != group.key) continue;

        auto* editor = buildEditor(spec);

        auto* cell    = new QWidget(form_host);
        auto* v       = new QVBoxLayout(cell);
        v->setContentsMargins(0, 0, 0, 0);
        v->addWidget(editor);

        if (!spec.help.empty()) {
            auto* help = new QLabel(QString::fromStdString(spec.help), cell);
            help->setWordWrap(true);
            help->setProperty("role", "help");
            v->addWidget(help);
        }

        auto* error = new QLabel(cell);
        error->setWordWrap(true);
        error->setProperty("role", "error");
        error->setVisible(false);
        v->addWidget(error);

        // The restart marker is its own line, not a suffix on the caption.
        // Appending it to the label made it read as part of the setting's name
        // ("Use range requests (needs restart)"), which is not what it is: it
        // describes WHEN the change lands, not what the setting does.
        if (spec.restart_required) {
            auto* note = new QLabel("Takes effect after a restart.", cell);
            note->setProperty("role", "help");
            QFont nf = note->font();
            nf.setItalic(true);
            note->setFont(nf);
            v->addWidget(note);
        }

        form->addRow(QString::fromStdString(spec.label), cell);

        rows_.insert(QString::fromStdString(spec.path),
                     Row{spec, editor, error, cell, form});
    }

    auto* scroll = new QScrollArea(page);
    scroll->setWidget(form_host);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    layout->addWidget(scroll, 1);

    return page;
}

QWidget* SettingsDialog::buildEditor(const FieldSpec& spec) {
    const QString path = QString::fromStdString(spec.path);

    switch (spec.kind) {
        case FieldKind::Bool: {
            auto* box = new QCheckBox(this);
            box->setChecked(model_.getBool(spec.path));
            connect(box, &QCheckBox::toggled, this, [this] { revalidate(); });
            return box;
        }
        case FieldKind::Int: {
            auto* box = new QSpinBox(this);
            box->setRange(spec.min ? (int)*spec.min : 0,
                          spec.max ? (int)*spec.max : 2147483647);
            box->setValue(model_.getInt(spec.path));
            connect(box, &QSpinBox::valueChanged, this, [this] { revalidate(); });
            return box;
        }
        case FieldKind::Double: {
            auto* box = new QDoubleSpinBox(this);
            box->setRange(spec.min.value_or(0.0), spec.max.value_or(1e9));
            box->setSingleStep(0.1);
            const auto v = model_.get(spec.path);
            box->setValue(v.is_number() ? v.get<double>() : 0.0);
            connect(box, &QDoubleSpinBox::valueChanged, this, [this] { revalidate(); });
            return box;
        }
        case FieldKind::Choice: {
            auto* box = new QComboBox(this);
            for (const auto& c : spec.choices)
                box->addItem(QString::fromStdString(c.label), QString::fromStdString(c.value));
            const int idx = box->findData(QString::fromStdString(model_.getString(spec.path)));
            if (idx >= 0) box->setCurrentIndex(idx);
            connect(box, &QComboBox::currentIndexChanged, this, [this] {
                // A source or database change shows and hides other fields, so
                // the whole page is rebuilt in place rather than left with rows
                // that no longer apply.
                pullFromWidgets();
                revalidate();
            });
            return box;
        }
        case FieldKind::Secret: {
            auto* edit = new QLineEdit(this);
            edit->setEchoMode(QLineEdit::Password);

            // NEVER render the sentinel. If "********" were shown, selecting
            // all and typing one character would send a one-character password,
            // and clearing the box would send "" -- which the service reads as
            // a real value and ERASES the stored secret. An empty box with a
            // placeholder makes both accidents impossible.
            const auto mode = model_.secretMode(spec.path);
            edit->setPlaceholderText(mode == SecretMode::Kept ? "unchanged"
                                                              : "not set");
            connect(edit, &QLineEdit::textEdited, this, [this, spec](const QString& text) {
                if (text.isEmpty()) model_.keepSecret(spec.path);
                else                model_.set(spec.path, text.toStdString());
                revalidate();
            });
            return edit;
        }
        case FieldKind::Directory:
        case FieldKind::File: {
            auto* host = new QWidget(this);
            auto* h    = new QHBoxLayout(host);
            h->setContentsMargins(0, 0, 0, 0);

            auto* edit = new QLineEdit(host);
            const QString current = QString::fromStdString(model_.getString(spec.path));
            edit->setText(current);
            edit->setPlaceholderText(
                spec.placeholder.empty() ? QStringLiteral("No folder chosen")
                                         : QString::fromStdString(spec.placeholder));
            // Browse and Clear eat into the row, so a long path will not fit.
            // Show the TAIL rather than the head: "/Users/someone/Documents/..."
            // is the same for everything a person owns, and the folder name at
            // the end is what actually identifies it. Full path in the tooltip.
            edit->setToolTip(current);
            edit->setCursorPosition(current.length());
            // Read-only on purpose. A typed path is a support ticket waiting to
            // happen: a trailing space, a backslash on the wrong side, a folder
            // that does not exist yet. Browse is the only way in.
            edit->setReadOnly(true);
            edit->setObjectName(path + ".edit");
            h->addWidget(edit, 1);

            auto* browse = new QPushButton("Browse...", host);
            connect(browse, &QPushButton::clicked, this, [this, spec] { chooseDirectory(spec); });
            h->addWidget(browse);

            if (!model_.getString(spec.path).empty()) {
                auto* clear = new QPushButton("Clear", host);
                connect(clear, &QPushButton::clicked, this, [this, spec, edit] {
                    edit->clear();
                    model_.set(spec.path, "");
                    revalidate();
                });
                h->addWidget(clear);
            }
            return host;
        }
        case FieldKind::Text:
        default: {
            auto* edit = new QLineEdit(this);
            edit->setText(QString::fromStdString(model_.getString(spec.path)));
            edit->setPlaceholderText(QString::fromStdString(spec.placeholder));
            connect(edit, &QLineEdit::textEdited, this, [this] { revalidate(); });
            return edit;
        }
    }
}

void SettingsDialog::chooseDirectory(const FieldSpec& spec) {
    const QString current = QString::fromStdString(model_.getString(spec.path));

    QString chosen;
    if (spec.kind == FieldKind::Directory) {
        chosen = QFileDialog::getExistingDirectory(
            this, "Choose " + QString::fromStdString(spec.label), current);
    } else {
        chosen = QFileDialog::getSaveFileName(
            this, "Choose " + QString::fromStdString(spec.label), current);
    }
    if (chosen.isEmpty()) return;

    // The card folder is checked the moment it is picked, using the SAME
    // classifier the service uses (SDD-010). Telling someone at the picker that
    // they chose DATALOG instead of the card root is worth far more than
    // failing later with "no sessions found".
    if (spec.path == "local_dir") {
        const auto layout = hms_cpap::classifyLocalDir(chosen.toStdString());
        if (layout != hms_cpap::LocalDirLayout::Root) {
            QMessageBox box(this);
            box.setIcon(QMessageBox::Warning);
            box.setWindowTitle("That is not the card root");
            box.setText(QString::fromStdString(hms_cpap::localDirProblem(layout, chosen.toStdString())));
            box.setInformativeText(QString::fromStdString(hms_cpap::localDirRemedy(layout, chosen.toStdString())));
            box.setStandardButtons(QMessageBox::Retry | QMessageBox::Cancel);
            box.setDefaultButton(QMessageBox::Retry);
            if (box.exec() == QMessageBox::Retry) chooseDirectory(spec);
            return;
        }
    }

    model_.set(spec.path, chosen.toStdString());

    auto it = rows_.find(QString::fromStdString(spec.path));
    if (it != rows_.end() && it->editor) {
        if (auto* edit = it->editor->findChild<QLineEdit*>(
                QString::fromStdString(spec.path) + ".edit")) {
            edit->setText(chosen);
            edit->setToolTip(chosen);
            edit->setCursorPosition(chosen.length());
        }
    }
    revalidate();
}

void SettingsDialog::pullFromWidgets() {
    for (auto it = rows_.begin(); it != rows_.end(); ++it) {
        const auto& spec = it->spec;
        QWidget* w = it->editor;
        if (!w) continue;

        switch (spec.kind) {
            case FieldKind::Bool:
                if (auto* b = qobject_cast<QCheckBox*>(w)) model_.set(spec.path, b->isChecked());
                break;
            case FieldKind::Int:
                if (auto* b = qobject_cast<QSpinBox*>(w)) model_.set(spec.path, b->value());
                break;
            case FieldKind::Double:
                if (auto* b = qobject_cast<QDoubleSpinBox*>(w)) model_.set(spec.path, b->value());
                break;
            case FieldKind::Choice:
                if (auto* b = qobject_cast<QComboBox*>(w))
                    model_.set(spec.path, b->currentData().toString().toStdString());
                break;
            case FieldKind::Text:
                if (auto* e = qobject_cast<QLineEdit*>(w))
                    model_.set(spec.path, e->text().toStdString());
                break;
            case FieldKind::Secret:
            case FieldKind::Directory:
            case FieldKind::File:
                // Written as they change: secrets through their tri-state, and
                // paths only through the picker.
                break;
        }
    }
}

void SettingsDialog::revalidate() {
    if (building_) return;
    pullFromWidgets();

    // Show and hide rows that depend on other fields, so switching the source
    // or the database type does not leave stale inputs on screen.
    for (auto it = rows_.begin(); it != rows_.end(); ++it) {
        const bool visible = !it->spec.visible || it->spec.visible(model_);
        // setRowVisible takes the LABEL with the field. Hiding only the widget
        // leaves the caption behind pointing at nothing -- which is what put a
        // bare "SD Card Folder" on the Data Source page with no picker beside
        // it whenever the source was not a local folder.
        if (it->form && it->container) it->form->setRowVisible(it->container, visible);
        else if (it->container)        it->container->setVisible(visible);
    }

    applyIssues(validate(model_));
}

void SettingsDialog::applyIssues(const std::vector<Issue>& issues) {
    for (auto it = rows_.begin(); it != rows_.end(); ++it) {
        setFieldState(it->editor, "");
        if (it->error) it->error->setVisible(false);
    }

    int errors = 0, warnings = 0;
    for (const auto& issue : issues) {
        const bool is_error = issue.severity == Severity::Error;
        is_error ? ++errors : ++warnings;

        auto it = rows_.find(QString::fromStdString(issue.path));
        if (it == rows_.end() || !it->error) continue;

        QString text = QString::fromStdString(issue.message);
        if (!issue.hint.empty()) text += "\n" + QString::fromStdString(issue.hint);

        it->error->setText(text);
        it->error->setProperty("role", is_error ? "error" : "warning");
        it->error->style()->unpolish(it->error);
        it->error->style()->polish(it->error);
        it->error->setVisible(true);
        setFieldState(it->editor, is_error ? "error" : "warning");
    }

    if (save_button_) save_button_->setEnabled(errors == 0);

    // Mark the sections that hold the problems. With fifteen pages, a disabled
    // Save and no marker means hunting -- the user can see they are stuck and
    // cannot see where.
    if (nav_) {
        QMap<QString, int> per_group;
        for (const auto& issue : issues) {
            if (issue.severity != Severity::Error) continue;
            auto it = rows_.find(QString::fromStdString(issue.path));
            if (it != rows_.end())
                per_group[QString::fromStdString(it->spec.group)]++;
        }
        for (int i = 0; i < nav_->count(); ++i) {
            auto* item = nav_->item(i);
            const QString key = item->data(Qt::UserRole).toString();
            const QString base = QString::fromStdString(settingsGroups()[i].title);
            const int n = per_group.value(key, 0);
            item->setText(n > 0 ? QString("%1  (%2)").arg(base).arg(n) : base);
            item->setForeground(n > 0 ? QBrush(QColor("#c0392b")) : QBrush());
        }
    }

    if (errors > 0) {
        banner_->setText(QString("%1 setting%2 need%3 attention before this can be saved.")
                             .arg(errors).arg(errors == 1 ? "" : "s").arg(errors == 1 ? "s" : ""));
        banner_->setProperty("role", "error");
        banner_->setVisible(true);
    } else if (warnings > 0) {
        banner_->setText(QString("%1 warning%2. You can still save.")
                             .arg(warnings).arg(warnings == 1 ? "" : "s"));
        banner_->setProperty("role", "warning");
        banner_->setVisible(true);
    } else {
        banner_->setVisible(false);
    }
    banner_->style()->unpolish(banner_);
    banner_->style()->polish(banner_);
}

std::vector<std::string> SettingsDialog::restartReasons() const {
    return cpapdash::supervisor::restartReasons(model_);
}

}  // namespace cpapdash::supervisor
