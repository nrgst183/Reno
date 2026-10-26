// Two small windows of the main window: Project Notes and the Keyboard Shortcuts list.
#include "mainwindow_internal.h"
#include "appstate.h"
#include "mainwindow.h"
#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <map>

namespace reno {
namespace app {

namespace {

// The Project Notes window (VEGAS 18's, Alt+6): notes kept with the project, each time-stamped, oldest first. "Add new
// note" makes one, the check mark
// resolves it (green), the cross deletes it, "Hide resolved notes" (cleared whenever a project opens) hides resolved
// ones. Each change is one undo step.
// Notes are kept in the project's notes text (a .veg's Project Notes, Reno's ProjectSettings::comments), one line a note:
// "[ ] 2026-10-16 14:05 | text", "[x] ..." when resolved; text in any other form (written by VEGAS or an older Reno) is one note.
class NotesPanel : public QWidget {
public:
    struct Note {
        bool resolved = false;
        QString stamp, text;
    };
    static std::vector<Note> parse(const std::string& all)
    {
        std::vector<Note> out;
        QStringList loose;
        static const QRegularExpression re("^\\[( |x|X)\\] (\\d{4}-\\d\\d-\\d\\d \\d\\d:\\d\\d) \\| (.*)$");
        for (const QString& line : QString::fromStdString(all).split('\n')) {
            const QRegularExpressionMatch m = re.match(line);
            if (m.hasMatch()) {
                Note n;
                n.resolved = m.captured(1) != " ";
                n.stamp = m.captured(2);
                n.text = m.captured(3);
                n.text.replace("\\n", "\n");
                out.push_back(n);
            } else if (!line.trimmed().isEmpty()) {
                loose << line;
            }
        }
        if (!loose.isEmpty()) {
            Note n;
            n.text = loose.join("\n");
            out.insert(out.begin(), n);
        }
        return out;
    }
    static std::string write(const std::vector<Note>& notes)
    {
        QStringList lines;
        for (const Note& n : notes) {
            QString t = n.text;
            t.replace("\n", "\\n");
            lines << QString("[%1] %2 | %3").arg(n.resolved ? "x" : " ", n.stamp.isEmpty() ? QString("0000-00-00 00:00") : n.stamp, t);
        }
        return lines.join("\n").toStdString();
    }

    explicit NotesPanel(QWidget* parent) : QWidget(parent)
    {
        setObjectName("projectNotes");
        QVBoxLayout* v = new QVBoxLayout(this);
        v->setContentsMargins(6, 6, 6, 6);
        QHBoxLayout* top = new QHBoxLayout();
        QPushButton* add = new QPushButton("Add new note", this);
        add->setObjectName("addNote");
        hide_ = new QCheckBox("Hide resolved notes", this);
        hide_->setObjectName("hideResolved");
        top->addWidget(add);
        top->addStretch(1);
        top->addWidget(hide_);
        v->addLayout(top);
        QScrollArea* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        rows_ = new QWidget(scroll);
        rowsLayout_ = new QVBoxLayout(rows_);
        rowsLayout_->setContentsMargins(0, 0, 0, 0);
        rowsLayout_->setSpacing(3);
        rowsLayout_->addStretch(1);
        scroll->setWidget(rows_);
        v->addWidget(scroll, 1);
        QObject::connect(add, &QPushButton::clicked, this, [this] {
            std::vector<Note> notes = parse(AppState::get().project().settings.comments);
            Note n;
            n.stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm");
            notes.push_back(n);
            store(notes, "Add Note");
            // the new note's text box takes the typing
            QTimer::singleShot(0, this, [this] {
                const QList<QLineEdit*> edits = rows_->findChildren<QLineEdit*>("noteText");
                if (!edits.isEmpty()) edits.last()->setFocus();
            });
        });
        QObject::connect(hide_, &QCheckBox::toggled, this, [this](bool) { load(true); });
        listener_ = AppState::get().addListener([this](int changes) {
            // a project opened: resolved notes show again
            if (changes & kFile) {
                const std::string p = AppState::get().path();
                if (p != path_) {
                    path_ = p;
                    QSignalBlocker b(hide_);
                    hide_->setChecked(false);
                    load(true);
                    return;
                }
            }
            if (changes & kProject) load(false);
        });
        path_ = AppState::get().path();
        load(true);
    }
    ~NotesPanel() override { AppState::get().removeListener(listener_); }

private:
    void store(const std::vector<Note>& notes, const char* what)
    {
        const std::string text = write(notes);
        if (text != AppState::get().project().settings.comments) AppState::get().edit(what, [&](Project& p) { p.settings.comments = text; });
        load(true);
    }
    void load(bool force)
    {
        const std::string all = AppState::get().project().settings.comments;
        if (!force && all == shown_) return;
        // not while a note is being typed: its box would go
        for (QLineEdit* e : rows_->findChildren<QLineEdit*>("noteText"))
            if (e->hasFocus() && !force) return;
        shown_ = all;
        while (rowsLayout_->count() > 1) {
            QLayoutItem* it = rowsLayout_->takeAt(0);
            if (it->widget()) it->widget()->deleteLater();
            delete it;
        }
        const std::vector<Note> notes = parse(all);
        for (size_t k = 0; k < notes.size(); ++k) {
            const Note& n = notes[k];
            if (n.resolved && hide_->isChecked()) continue;
            QWidget* row = new QWidget(rows_);
            row->setObjectName("noteRow");
            QHBoxLayout* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            QLabel* stamp = new QLabel(n.stamp, row);
            stamp->setObjectName("noteStamp");
            QLineEdit* text = new QLineEdit(n.text, row);
            text->setObjectName("noteText");
            text->setPlaceholderText("Type the note");
            QToolButton* done = new QToolButton(row);
            done->setObjectName("noteResolved");
            done->setText(QString::fromUtf8("\u2714"));
            done->setCheckable(true);
            done->setChecked(n.resolved);
            done->setToolTip(n.resolved ? "Resolved (click: not resolved)" : "Mark as resolved");
            done->setStyleSheet(n.resolved ? "QToolButton { color: #3cc864; }" : "QToolButton { color: #8a8a8a; }");
            QToolButton* del = new QToolButton(row);
            del->setObjectName("noteDelete");
            del->setText(QString::fromUtf8("\u2715"));
            del->setToolTip("Delete this note");
            h->addWidget(stamp);
            h->addWidget(text, 1);
            h->addWidget(done);
            h->addWidget(del);
            rowsLayout_->insertWidget(rowsLayout_->count() - 1, row);
            const size_t index = k;
            QObject::connect(text, &QLineEdit::editingFinished, this, [this, index, text] {
                std::vector<Note> ns = parse(AppState::get().project().settings.comments);
                if (index >= ns.size() || ns[index].text == text->text()) return;
                ns[index].text = text->text();
                QTimer::singleShot(0, this, [this, ns] { store(ns, "Edit Note"); });
            });
            QObject::connect(done, &QToolButton::clicked, this, [this, index](bool on) {
                std::vector<Note> ns = parse(AppState::get().project().settings.comments);
                if (index >= ns.size()) return;
                ns[index].resolved = on;
                store(ns, on ? "Resolve Note" : "Reopen Note");
            });
            QObject::connect(del, &QToolButton::clicked, this, [this, index] {
                std::vector<Note> ns = parse(AppState::get().project().settings.comments);
                if (index >= ns.size()) return;
                ns.erase(ns.begin() + (long)index);
                QTimer::singleShot(0, this, [this, ns] { store(ns, "Delete Note"); });
            });
        }
    }
    QCheckBox* hide_ = nullptr;
    QWidget* rows_ = nullptr;
    QVBoxLayout* rowsLayout_ = nullptr;
    std::string shown_ = "\x01", path_;
    int listener_ = 0;
};

// Help > Keyboard Shortcuts: every key, by menu, with a search box.
class ShortcutsDialog : public QDialog {
public:
    explicit ShortcutsDialog(QWidget* parent) : QDialog(parent)
    {
        setObjectName("shortcutsDialog");
        setWindowTitle("Keyboard Shortcuts");
        resize(760, 620);
        auto* root = new QVBoxLayout(this);
        auto* search = new QLineEdit(this);
        search->setObjectName("shortcutSearch");
        search->setPlaceholderText("Search commands and keys");
        search->setClearButtonEnabled(true);
        root->addWidget(search);
        tree_ = new QTreeWidget(this);
        tree_->setObjectName("shortcutList");
        tree_->setColumnCount(3);
        tree_->setHeaderLabels({"Command", "Keys", "Notes"});
        tree_->setRootIsDecorated(true);
        tree_->setUniformRowHeights(true);
        tree_->header()->setSectionResizeMode(0, QHeaderView::Interactive);
        tree_->setColumnWidth(0, 330);
        tree_->setColumnWidth(1, 190);
        root->addWidget(tree_, 1);
        auto* note = new QLabel("\"Timeline\" keys act when the timeline has the focus (click it, or press Alt+0). Greyed lines are the usual keys for things Reno cannot do yet. "
                                "These are VEGAS's default keys: Options > Customize Keyboard changes them (and shows the keys in use).", this);
        note->setWordWrap(true);
        root->addWidget(note);
        auto* close = new QPushButton("Close", this);
        close->setObjectName("close");
        auto* row = new QHBoxLayout();
        row->addStretch(1);
        row->addWidget(close);
        root->addLayout(row);
        QObject::connect(close, &QPushButton::clicked, this, [this] { hide(); });
        QObject::connect(search, &QLineEdit::textChanged, this, [this](const QString& t) { fill(t); });
        fill(QString());
    }

    void fill(const QString& filter)
    {
        tree_->clear();
        std::map<std::string, QTreeWidgetItem*> groups;
        std::vector<std::string> order;
        const QColor grey(130, 130, 130);
        for (const ShortcutInfo& s : shortcutList()) {
            const QString command = QString::fromUtf8(s.command), k = QString::fromUtf8(s.keys), note = QString::fromUtf8(s.note);
            QString notes = note;
            if (s.kind == ShortcutInfo::Timeline) notes = notes.isEmpty() ? QString("timeline") : QString("timeline; %1").arg(notes);
            if (s.kind == ShortcutInfo::Panel) {
                const QString where = QString("in the %1 window").arg(QString::fromUtf8(s.menu));
                notes = notes.isEmpty() ? where : QString("%1; %2").arg(where, notes);
            }
            if (s.kind == ShortcutInfo::NotAvailable) notes = QString("not available: %1").arg(note);
            if (!filter.isEmpty() && !command.contains(filter, Qt::CaseInsensitive) && !k.contains(filter, Qt::CaseInsensitive) &&
                !QString::fromUtf8(s.menu).contains(filter, Qt::CaseInsensitive) && !notes.contains(filter, Qt::CaseInsensitive))
                continue;
            QTreeWidgetItem*& g = groups[s.menu];
            if (!g) {
                g = new QTreeWidgetItem(tree_, QStringList() << QString::fromUtf8(s.menu));
                QFont f = g->font(0);
                f.setBold(true);
                g->setFont(0, f);
                g->setFirstColumnSpanned(true);
                order.push_back(s.menu);
            }
            auto* item = new QTreeWidgetItem(g, QStringList() << command << k << notes);
            if (s.kind == ShortcutInfo::NotAvailable)
                for (int c = 0; c < 3; ++c) item->setForeground(c, grey);
        }
        tree_->expandAll();
    }

private:
    QTreeWidget* tree_ = nullptr;
};

}  // namespace

QWidget* makeNotesPanel(QWidget* parent) { return new NotesPanel(parent); }
QDialog* makeShortcutsDialog(QWidget* parent) { return new ShortcutsDialog(parent); }

}  // namespace app
}  // namespace reno
