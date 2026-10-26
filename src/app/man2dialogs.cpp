// Small VEGAS dialogs (see man2dialogs.h).
#include "man2dialogs.h"
#include "timeline.h"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

namespace reno {
namespace app {

namespace {

const char* const kTemplates = "commandTemplates";

// VEGAS 18's commands (Insert > Command): what each is for, shown under the list
struct CommandKind { const char* name; const char* player; const char* what; };
const CommandKind kCommands[] = {
    {"URL", "Windows Media", "The page at the parameter's address is shown in the viewer's browser at this moment."},
    {"Text", "Windows Media", "The parameter's text is shown in the player's captioning area."},
    {"WMClosedCaption", "Windows Media", "The text is shown in the captioning window an HTML layout file defines."},
    {"WMTextBodyText", "Windows Media", "The text is shown in the text window an HTML layout file defines."},
    {"WMTextHeadline", "Windows Media", "The text is shown in the headline window an HTML layout file defines."},
    {"Scott EOM", "Scott Studios WAV file", "When the next queued clip starts playing in a Scott Studios system."},
    {"Scott Cue In", "Scott Studios WAV file", "The beginning of the file in a Scott Studios system (without destructive editing)."},
    {"608CC1", "", "Line 21 closed captions, channel 1 (primary language): the parameter is the caption - its control codes, or plain text, which becomes a pop-on caption ([BR]: a new line)."},
    {"608CC2", "", "Line 21 closed captions, channel 2: as 608CC1."},
    {"608CC3", "", "Line 21 closed captions, channel 3 (secondary language): as 608CC1."},
    {"608CC4", "", "Line 21 closed captions, channel 4: as 608CC1."},
};

QString encode(const CommandProps& c)
{
    QStringList parts;
    for (const std::string& s : {c.command, c.parameter, c.comment}) parts << QString::fromStdString(s).toUtf8().toBase64();
    parts << QString::number(c.position, 'g', 17);
    return parts.join('|');
}

bool decode(const QString& text, CommandProps& out)
{
    const QStringList parts = text.split('|');
    if (parts.size() != 4) return false;
    out.command = QString::fromUtf8(QByteArray::fromBase64(parts[0].toLatin1())).toStdString();
    out.parameter = QString::fromUtf8(QByteArray::fromBase64(parts[1].toLatin1())).toStdString();
    out.comment = QString::fromUtf8(QByteArray::fromBase64(parts[2].toLatin1())).toStdString();
    out.position = parts[3].toDouble();
    return true;
}

}  // namespace

std::vector<std::pair<std::string, CommandProps>> commandTemplates()
{
    std::vector<std::pair<std::string, CommandProps>> out;
    QSettings s;
    s.beginGroup(kTemplates);
    for (const QString& k : s.childKeys()) {
        CommandProps c;
        if (decode(s.value(k).toString(), c)) out.emplace_back(QString(QByteArray::fromHex(k.toLatin1())).toStdString(), c);
    }
    return out;
}

void saveCommandTemplate(const std::string& name, const CommandProps& props)
{
    QSettings s;
    s.beginGroup(kTemplates);
    // name as hexadecimal, so any name works as a key
    const QString key = QString::fromLatin1(QByteArray(name.c_str()).toHex());
    if (props.command.empty() && props.parameter.empty() && props.comment.empty()) s.remove(key);
    else s.setValue(key, encode(props));
}

bool commandPropertiesDialog(QWidget* parent, CommandProps& props, const QString& title)
{
    AppState& app = AppState::get();
    QDialog dlg(parent);
    dlg.setObjectName("commandMarkerDialog");
    dlg.setWindowTitle(title);
    auto* form = new QFormLayout(&dlg);
    // Template: the list (type to name a new one), Save and Delete
    QWidget* trow = new QWidget(&dlg);
    auto* tl = new QHBoxLayout(trow);
    tl->setContentsMargins(0, 0, 0, 0);
    auto* tmpl = new QComboBox(trow);
    tmpl->setObjectName("template");
    tmpl->setEditable(true);
    tmpl->addItem(QString());
    for (const auto& t : commandTemplates()) tmpl->addItem(QString::fromStdString(t.first));
    auto* save = new QToolButton(trow);
    save->setObjectName("saveTemplate");
    save->setText("Save");
    save->setToolTip("Save Template");
    auto* del = new QToolButton(trow);
    del->setObjectName("deleteTemplate");
    del->setText("Delete");
    del->setToolTip("Delete Template");
    tl->addWidget(tmpl, 1);
    tl->addWidget(save);
    tl->addWidget(del);
    form->addRow("Template:", trow);

    auto* command = new QComboBox(&dlg);
    command->setObjectName("command");
    command->setEditable(true);
    for (const CommandKind& k : kCommands) command->addItem(k.name);
    command->setCurrentText(QString::fromStdString(props.command));
    form->addRow("Command:", command);
    auto* about = new QLabel(&dlg);
    about->setObjectName("commandDescription");
    about->setWordWrap(true);
    about->setStyleSheet("color: gray;");
    form->addRow(QString(), about);
    auto describe = [about, command] {
        QString text = "A command of your own: the player receives it with its parameter.";
        for (const CommandKind& k : kCommands)
            if (command->currentText().compare(k.name, Qt::CaseInsensitive) == 0)
                text = (k.player[0] ? QString("%1: ").arg(k.player) : QString()) + k.what;
        about->setText(text);
    };
    describe();
    QObject::connect(command, &QComboBox::currentTextChanged, &dlg, [describe] { describe(); });
    auto* param = new QLineEdit(QString::fromStdString(props.parameter), &dlg);
    param->setObjectName("parameter");
    form->addRow("Parameter:", param);
    auto* comment = new QLineEdit(QString::fromStdString(props.comment), &dlg);
    comment->setObjectName("comment");
    form->addRow("Comment:", comment);
    auto* position = new QLineEdit(QString::fromStdString(rulerTime(app.project(), props.position)), &dlg);
    position->setObjectName("position");
    form->addRow("Position:", position);

    auto current = [&]() {
        CommandProps c;
        c.command = command->currentText().trimmed().toStdString();
        c.parameter = param->text().toStdString();
        c.comment = comment->text().toStdString();
        double t = props.position;
        if (parseRulerTime(app.project(), position->text(), &t)) c.position = std::max(0.0, t);
        else c.position = props.position;
        return c;
    };
    QObject::connect(tmpl, &QComboBox::activated, &dlg, [&](int) {
        const std::string name = tmpl->currentText().toStdString();
        for (const auto& t : commandTemplates())
            if (t.first == name) {
                command->setCurrentText(QString::fromStdString(t.second.command));
                param->setText(QString::fromStdString(t.second.parameter));
                comment->setText(QString::fromStdString(t.second.comment));
                position->setText(QString::fromStdString(rulerTime(app.project(), t.second.position)));
            }
    });
    QObject::connect(save, &QToolButton::clicked, &dlg, [&] {
        const std::string name = tmpl->currentText().trimmed().toStdString();
        if (name.empty()) {
            app.status("Type a name for the template in the Template box first.");
            return;
        }
        saveCommandTemplate(name, current());
        if (tmpl->findText(QString::fromStdString(name)) < 0) tmpl->addItem(QString::fromStdString(name));
    });
    QObject::connect(del, &QToolButton::clicked, &dlg, [&] {
        const QString name = tmpl->currentText().trimmed();
        if (name.isEmpty()) return;
        saveCommandTemplate(name.toStdString(), CommandProps{"", "", "", 0});
        const int i = tmpl->findText(name);
        if (i > 0) tmpl->removeItem(i);
        tmpl->setCurrentIndex(0);
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    dlg.resize(460, dlg.sizeHint().height());
    if (dlg.exec() != QDialog::Accepted) return false;
    props = current();
    return !props.command.empty();
}

}  // namespace app
}  // namespace reno
