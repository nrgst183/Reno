// Dumps what every built-in audio effect's page (src/app/fxpages.cpp) is made of - every widget with its class, name, text and place,
// the settings with their ranges, the presets - as text lines, for tools/vegas/dlgcheck.py to compare control by control with the
// dialog resource of the VEGAS plug-in it reproduces.
//
//   fxdlg_dump <output file> [<folder for a picture of every page>]
#include "../../src/app/fxwindow.h"
#include "../../src/app/mainwindow.h"
#include "../../src/app/playback.h"
#include "../../src/engine/catalog.h"
#include "uitestlib.h"
#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QSlider>
#include <QSpinBox>
#include <QTextDocument>
#include <QTextStream>

using namespace reno;
using namespace reno::app;

namespace {

QString plain(QString s)
{
    // rich text (<u>F</u>requency) back to the dialog's form (&Frequency)
    s.replace("<u>", "&").replace("</u>", "");
    if (s.contains('<')) {
        QTextDocument d;
        d.setHtml(s);
        s = d.toPlainText();
    }
    s.replace('\n', '|');
    return s;
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTest");
    QApplication::setApplicationName("fxdlg_dump");
    applyTheme();
    const QStringList args = QApplication::arguments();
    QFile out(args.size() > 1 ? args[1] : QString("fxdlg.txt"));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) return 2;
    QTextStream o(&out);
    o.setEncoding(QStringConverter::Utf8);
    uitest::setup({}, {});

    AppState& app = AppState::get();
    app.newProject();
    std::string err;
    const uint64_t media = app.importMedia("synth:1", &err);
    uint64_t a1 = 0, ae = 0;
    app.edit("tracks", [&](Project& p) {
        Track tr;
        tr.id = p.newId();
        tr.type = TrackType::Audio;
        tr.name = "A";
        p.tracks.push_back(tr);
        a1 = tr.id;
        Event e;
        e.id = p.newId();
        e.mediaId = media;
        e.start = 1;
        e.length = 3;
        e.name = "E";
        p.findTrack(a1)->events.push_back(e);
        ae = e.id;
    });
    FxTarget at;
    at.kind = FxTarget::Event;
    at.id = ae;
    app.setCursor(1.0);
    Playback playback;
    FxWindow* w = new FxWindow(&playback, nullptr);
    w->resize(900, 800);
    w->show();

    for (const AudioFxEntry& e : Catalog::get().audio()) {
        if (e.desc.format != "BUILTIN") continue;
        const uint64_t id = app.addAudioFx(at, e.desc);
        w->showTarget(at, id);
        uitest::settle(200);
        QWidget* page = nullptr;
        for (QWidget* c : w->findChildren<QWidget*>("fxDxPage"))
            if (c->isVisibleTo(w)) page = c;
        AudioPlugin* p = app.fx.audio(id);
        o << "PAGE\t" << QString::fromStdString(e.desc.uid) << "\t" << QString::fromStdString(e.desc.name) << "\t" << (page ? page->width() : -1) << "\t"
          << (page ? page->height() : -1) << "\n";
        if (p) {
            for (int i = 0; i < p->paramCount(); ++i) {
                double lo = 0, hi = 1;
                const bool has = p->paramRange(i, &lo, &hi);
                o << "PARAM\t" << i << "\t" << QString::fromStdString(p->paramName(i)) << "\t" << (has ? lo : 0) << "\t" << (has ? hi : 1) << "\t" << p->paramGet(i)
                  << "\t" << (has ? 1 : 0) << "\n";
            }
            for (int i = 0; i < p->programCount(); ++i) o << "PRESET\t" << i << "\t" << QString::fromStdString(p->programName(i)) << "\n";
        }
        if (page && args.size() > 2) page->grab().save(args[2] + "/" + QString::fromStdString(e.desc.uid) + ".png");
        if (page) {
            for (QWidget* c : page->findChildren<QWidget*>()) {
                if (!c->isVisibleTo(page)) continue;
                const QPoint tl = c->mapTo(page, QPoint(0, 0));
                QString cls = c->metaObject()->className(), text, extra;
                if (QLabel* l = qobject_cast<QLabel*>(c)) text = plain(l->text());
                else if (QGroupBox* g = qobject_cast<QGroupBox*>(c)) text = plain(g->title());
                else if (QAbstractButton* b = qobject_cast<QAbstractButton*>(c)) text = plain(b->text());
                else if (QSlider* s = qobject_cast<QSlider*>(c)) extra = QString("%1..%2 step %3 tick %4").arg(s->minimum()).arg(s->maximum()).arg(s->singleStep()).arg(s->tickInterval());
                else if (QLineEdit* le = qobject_cast<QLineEdit*>(c)) text = le->text();
                else if (QComboBox* cb = qobject_cast<QComboBox*>(c)) {
                    QStringList items;
                    for (int i = 0; i < cb->count(); ++i) items << cb->itemText(i);
                    extra = items.join("|");
                } else if (QDoubleSpinBox* ds = qobject_cast<QDoubleSpinBox*>(c)) extra = QString("%1..%2 dec %3 step %4 suffix '%5'").arg(ds->minimum()).arg(ds->maximum()).arg(ds->decimals()).arg(ds->singleStep()).arg(ds->suffix());
                else if (QSpinBox* sb = qobject_cast<QSpinBox*>(c)) extra = QString("%1..%2 step %3 suffix '%4'").arg(sb->minimum()).arg(sb->maximum()).arg(sb->singleStep()).arg(sb->suffix());
                o << "W\t" << cls << "\t" << c->objectName() << "\t" << text << "\t" << tl.x() << "\t" << tl.y() << "\t" << c->width() << "\t" << c->height() << "\t" << extra << "\n";
            }
        }
        app.removeFx(at, id);
    }
    delete w;
    return 0;
}
