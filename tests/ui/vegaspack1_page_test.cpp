// The pages of the sfppack1 effects (Simple Delay, Multi-Tap Delay, ...): every control sets the setting it stands for,
// the page follows presets and settings changed from elsewhere, enabling rules, the multi-tap graph. Saves a picture of
// each page.
//
//   vegaspack1_page_test <output folder>
#include "../../src/app/fxpages.h"
#include "../../src/audiofx/vegaspack1.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QSlider>
#include <QThread>
#include <cmath>
#include <cstdio>
#include <memory>

using namespace reno;
using namespace reno::app;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

static void settle(int ms = 30)
{
    QApplication::processEvents();
    QThread::msleep((unsigned long)ms);
    QApplication::processEvents();
}

struct Rig {
    std::unique_ptr<AudioPlugin> p;
    DxHost host;
    explicit Rig(const std::string& uid)
    {
        p = vegpack1::create(uid);
        p->activate(48000, 512);
        AudioPlugin* q = p.get();
        host.range = [q](int i, double* lo, double* hi) { return q->paramRange(i, lo, hi); };
        host.get = [q](int i) {
            double lo = 0, hi = 1;
            q->paramRange(i, &lo, &hi);
            return lo + (double)q->paramGet(i) * (hi - lo);
        };
        host.set = [q](int i, double v) {
            double lo = 0, hi = 1;
            q->paramRange(i, &lo, &hi);
            q->paramSet(i, (float)((v - lo) / (hi - lo)));
        };
        host.name = [q](int i) { return q->paramName(i); };
        host.begin = [](int) {};
        host.end = [] {};
        host.state = [q] { return q->saveState(); };
        host.setState = [q](const std::string& s, const std::string&) { q->loadState(s); };
        host.beginEdit = [](const std::string&) {};
    }
    double real(int i)
    {
        double lo = 0, hi = 1;
        p->paramRange(i, &lo, &hi);
        return lo + (double)p->paramGet(i) * (hi - lo);
    }
};

template <class T>
static T* ctl(QWidget* page, const QString& name)
{
    return page->findChild<T*>(name);
}

static bool skip(AudioPlugin* p, int i)
{
    const std::string n = p->paramName(i);
    return n == "Bypass" || n == "Initial Time" || n == "Initial Tempo" || n.compare(0, 4, "Tap ") == 0;
}

static void generic(const std::string& uid, const std::string& label, QWidget* page, Rig& rig)
{
    AudioPlugin* p = rig.p.get();
    int missing = 0;
    std::string which;
    for (int i = 0; i < p->paramCount(); ++i) {
        if (skip(p, i)) continue;
        const QString n = QString("dxParam%1").arg(i);
        if (!ctl<QSlider>(page, n) && !ctl<QCheckBox>(page, n) && !ctl<QComboBox>(page, n) && !ctl<QLineEdit>(page, n + "Edit")) { ++missing; which += " " + p->paramName(i); }
    }
    check(missing == 0, label + ": every setting has a control", which);
    for (int i = 0; i < p->paramCount(); ++i) {
        if (skip(p, i)) continue;
        const QString n = QString("dxParam%1").arg(i);
        double lo = 0, hi = 1;
        p->paramRange(i, &lo, &hi);
        const std::string nm = label + ": " + p->paramName(i);
        if (QSlider* s = ctl<QSlider>(page, n)) {
            if (!s->isEnabled()) continue;
            const int top = s->maximum();
            const double before = rig.real(i);
            s->setValue(top * 3 / 4 != s->value() ? top * 3 / 4 : top / 4);
            settle();
            const double t = (double)s->value() / top;
            const double lin = lo + t * (hi - lo), lg = lo > 0 ? lo * std::pow(hi / lo, t) : lin;
            const double v = rig.real(i);
            const double tol = (hi - lo) * 0.003 + 1e-6;
            check(v != before && (std::fabs(v - lin) <= tol || std::fabs(v - lg) <= std::max(tol, std::fabs(lg) * 0.003)), nm + ": its slider sets it", std::to_string(v));
            p->paramSet(i, 0.5f);
            static_cast<DxPage*>(page)->refresh();
            settle();
            const double t2 = (double)s->value() / s->maximum();
            const double v2 = rig.real(i);
            const double l2 = lo + t2 * (hi - lo), g2 = lo > 0 ? lo * std::pow(hi / lo, t2) : l2;
            const double tol2 = std::max(tol, (hi - lo) / s->maximum() * 0.6);
            check(std::fabs(v2 - l2) <= tol2 || std::fabs(v2 - g2) <= std::max(tol2, std::fabs(g2) * 0.003), nm + ": its slider follows a change from elsewhere");
        } else if (QCheckBox* c = ctl<QCheckBox>(page, n)) {
            if (!c->isEnabled()) continue;
            const bool was = c->isChecked();
            c->click();
            settle();
            check((rig.real(i) >= 0.5) == !was, nm + ": its check box sets it");
            c->click();
            settle();
        } else if (QComboBox* c = ctl<QComboBox>(page, n)) {
            if (!c->isEnabled()) continue;
            c->setCurrentIndex(c->count() - 1);
            settle();
            check(std::lround(rig.real(i)) == c->count() - 1, nm + ": its list sets it", std::to_string(rig.real(i)));
        } else if (QLineEdit* e = ctl<QLineEdit>(page, n + "Edit")) {
            if (!e->isEnabled()) continue;
            if (!e->property("shown").isValid()) {
                e->setText(QString::number(lo + (hi - lo) * 0.25, 'f', 3));
                emit e->editingFinished();
                settle();
                check(std::fabs(rig.real(i) - (lo + (hi - lo) * 0.25)) <= (hi - lo) * 0.002 + 1e-6, nm + ": its edit box sets it", std::to_string(rig.real(i)));
            }
            e->setText("-9999999");
            emit e->editingFinished();
            settle();
            check(std::fabs(rig.real(i) - lo) < 1e-6, nm + ": a number out of range is kept in range", std::to_string(rig.real(i)));
        }
    }
    // presets: the page follows
    bool ok = true;
    std::string bad;
    for (int k = 0; k < p->programCount(); ++k) {
        p->setProgram(k);
        static_cast<DxPage*>(page)->refresh();
        settle(10);
        for (int i = 0; i < p->paramCount(); ++i) {
            if (skip(p, i)) continue;
            const QString n = QString("dxParam%1").arg(i);
            double lo = 0, hi = 1;
            p->paramRange(i, &lo, &hi);
            const double v = rig.real(i);
            bool good = true;
            if (QSlider* s = ctl<QSlider>(page, n)) {
                const double t = (double)s->value() / s->maximum();
                const double lin = lo + t * (hi - lo), lg = lo > 0 ? lo * std::pow(hi / lo, t) : lin;
                good = std::fabs(v - lin) <= std::max((hi - lo) * 0.003, (hi - lo) / s->maximum() * 0.6) || std::fabs(v - lg) <= std::max((hi - lo) * 0.003, std::fabs(lg) * 0.003);
            } else if (QCheckBox* c = ctl<QCheckBox>(page, n)) good = c->isChecked() == (v >= 0.5);
            else if (QComboBox* c = ctl<QComboBox>(page, n)) good = c->currentIndex() == std::lround(v);
            else if (QLineEdit* e = ctl<QLineEdit>(page, n + "Edit")) good = std::fabs(e->text().remove(',').toDouble() - v * (e->property("shown").isValid() ? 0.001 : 1.0)) < 0.0015;
            if (!good) { ok = false; bad += " [" + p->paramName(i) + " preset " + std::to_string(k) + "]"; }
        }
    }
    check(ok, label + ": the page follows every preset", bad);
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    const QString out = argc > 1 ? argv[1] : "vegaspack1_out";
    QDir().mkpath(out);

    // ---- Simple Delay
    {
        Rig rig("reno.simpledelay");
        DxPage* page = makeBuiltinPage(nullptr, "reno.simpledelay", rig.host);
        check(page != nullptr, "simple delay: has a page");
        if (page) {
            page->show();
            settle(100);
            page->grab().save(out + "/simpledelay.png");
            generic("reno.simpledelay", "simple delay", page, rig);
            // enabling rules
            rig.p->setProgram(0);
            page->refresh();
            settle();
            QCheckBox* ts = ctl<QCheckBox>(page, "dxParam8");
            QCheckBox* multi = ctl<QCheckBox>(page, "dxParam2");
            QCheckBox* tsd = ctl<QCheckBox>(page, "dxParam11");
            check(ts && multi && tsd, "simple delay: tempo and multiple checks found");
            if (ts && multi && tsd) {
                check(ctl<QSlider>(page, "dxParam0")->isEnabled() && !ctl<QComboBox>(page, "dxParam6")->isEnabled(), "simple delay: delay time in seconds, tempo note off");
                check(!ctl<QSlider>(page, "dxParam1")->isEnabled() && !tsd->isEnabled(), "simple delay: decay time off without multiple delays");
                ts->click();
                multi->click();
                settle();
                check(!ctl<QSlider>(page, "dxParam0")->isEnabled() && ctl<QComboBox>(page, "dxParam6")->isEnabled(), "simple delay: tempo sync swaps the delay time for the note");
                check(ctl<QSlider>(page, "dxParam1")->isEnabled() && tsd->isEnabled(), "simple delay: multiple delays enables the decay time");
                tsd->click();
                settle();
                check(!ctl<QSlider>(page, "dxParam1")->isEnabled() && ctl<QComboBox>(page, "dxParam9")->isEnabled(), "simple delay: tempo sync decay swaps decay time for the note");
                page->grab().save(out + "/simpledelay_tempo.png");
            }
            // the delay time box shows seconds
            rig.p->setProgram(2);   // slap-back: 400 ms
            page->refresh();
            settle();
            QLineEdit* e = ctl<QLineEdit>(page, "dxParam0Edit");
            check(e && e->text() == "0.400", "simple delay: the delay time is shown in seconds", e ? e->text().toStdString() : "none");
            if (e) {
                e->setText("1.25");
                emit e->editingFinished();
                settle();
                check(std::fabs(rig.real(0) - 1250) < 1, "simple delay: seconds typed give ms", std::to_string(rig.real(0)));
            }
            delete page;
        }
    }

    // ---- Multi-Tap Delay
    {
        Rig rig("reno.multitapdelay");
        DxPage* page = makeBuiltinPage(nullptr, "reno.multitapdelay", rig.host);
        check(page != nullptr, "multi-tap: has a page");
        if (page) {
            rig.p->setProgram(2);   // Small room 1: 6 taps
            page->refresh();
            page->show();
            settle(100);
            page->grab().save(out + "/multitap.png");
            generic("reno.multitapdelay", "multi-tap", page, rig);
            rig.p->setProgram(2);   // Small hall 1: 8 taps
            page->refresh();
            settle();
            // the buttons follow the number of taps
            QSlider* nt = ctl<QSlider>(page, "dxParam8");
            nt->setValue(3 - 1);
            settle();
            check(std::lround(rig.real(8)) == 3, "multi-tap: number of taps slider", std::to_string(rig.real(8)));
            int enabled = 0;
            for (int k = 0; k < 8; ++k) enabled += ctl<QPushButton>(page, QString("dxTap%1").arg(k))->isEnabled();
            check(enabled == 3, "multi-tap: only the buttons of existing taps are on", std::to_string(enabled));
            // pick tap 3 and change its delay, gain and pan
            ctl<QPushButton>(page, "dxTap2")->click();
            settle();
            QSlider* ds = ctl<QSlider>(page, "dxTapDelay");
            QSlider* gs = ctl<QSlider>(page, "dxTapGain");
            QSlider* ps = ctl<QSlider>(page, "dxTapPan");
            check(ds && gs && ps, "multi-tap: tap controls found");
            if (ds && gs && ps) {
                const double d0 = rig.real(9 + 2);
                check(std::fabs(rig.real(9) - 34) < 1.5 && std::fabs(d0 - 113) < 2, "multi-tap: tap 3 shows its own delay", std::to_string(d0));
                ds->setValue(ds->maximum() / 2);
                gs->setValue(gs->maximum() / 4);
                ps->setValue(ps->maximum());
                settle();
                check(std::fabs(rig.real(9 + 2) - 1250) < 5, "multi-tap: the delay slider sets tap 3", std::to_string(rig.real(9 + 2)));
                check(std::fabs(rig.real(17 + 2) - (-50)) < 1.5, "multi-tap: the gain fader sets tap 3", std::to_string(rig.real(17 + 2)));
                check(std::lround(rig.real(25 + 2)) == 100, "multi-tap: the pan slider sets tap 3", std::to_string(rig.real(25 + 2)));
                check(std::lround(rig.real(9 + 1)) == 57 && std::lround(rig.real(9)) == 34, "multi-tap: the other taps are left alone");
            }
            // the graph: pick a tap by clicking it and pull it
            rig.p->setProgram(1);   // Small room 1: tap 2 at 40 ms
            page->refresh();
            settle();
            QWidget* g = ctl<QWidget>(page, "dxTapGraph");
            check(g != nullptr, "multi-tap: graph found");
            if (g) {
                // tap 2 (delay 40 ms of the 500 ms the graph spans)
                const double x = 40.0 / 500.0 * (g->width() - 1);
                const QPoint from((int)std::lround(x), g->height() / 2), to((int)std::lround(200.0 / 500.0 * (g->width() - 1)), g->height() / 4);
                auto send = [&](QEvent::Type t, const QPoint& at, Qt::MouseButton b, Qt::MouseButtons bs) {
                    QMouseEvent ev(t, QPointF(at), QPointF(g->mapToGlobal(at)), b, bs, Qt::NoModifier);
                    QApplication::sendEvent(g, &ev);
                };
                send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
                send(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
                send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
                settle();
                check(std::fabs(rig.real(9 + 1) - 200) <= 4, "multi-tap: pulling a tap in the graph sets its delay", std::to_string(rig.real(9 + 1)));
                check(rig.real(17 + 1) > 20, "multi-tap: and its gain", std::to_string(rig.real(17 + 1)));
                check(ctl<QPushButton>(page, "dxTap1")->isChecked(), "multi-tap: and picks it");
                g->parentWidget()->grab().save(out + "/multitap_graph.png");
            }
            // low-pass on/off enables its slider
            rig.p->setProgram(0);
            page->refresh();
            settle();
            check(!ctl<QSlider>(page, "dxParam7")->isEnabled(), "multi-tap: low-pass slider off while the low-pass is off");
            ctl<QCheckBox>(page, "dxParam6")->click();
            settle();
            check(ctl<QSlider>(page, "dxParam7")->isEnabled(), "multi-tap: low-pass slider on with the low-pass");
            delete page;
        }
    }
    // ---- Chorus and Reverb: every control, the presets, a picture
    for (const char* uid : {"reno.chorus", "reno.reverb"}) {
        Rig rig(uid);
        DxPage* page = makeBuiltinPage(nullptr, uid, rig.host);
        check(page != nullptr, std::string(uid) + ": has a page");
        if (!page) continue;
        page->show();
        settle(100);
        page->grab().save(out + "/" + (uid + 5) + ".png");
        if (QWidget* f = QApplication::focusWidget()) f->clearFocus();
        generic(uid, uid + 5, page, rig);
        page->grab().save(out + "/" + (uid + 5) + "_after.png");
        delete page;
    }
    // ---- Pitch Shift
    {
        Rig rig("reno.pitchshift");
        DxPage* page = makeBuiltinPage(nullptr, "reno.pitchshift", rig.host);
        check(page != nullptr, "pitch shift: has a page");
        if (page) {
            page->show();
            settle(100);
            page->grab().save(out + "/pitchshift.png");
            generic("reno.pitchshift", "pitch shift", page, rig);
            if (QWidget* f = QApplication::focusWidget()) f->clearFocus();   // a box being typed in keeps its text
            rig.p->setProgram(2);   // Fifth up
            page->refresh();
            settle();
            check(ctl<QLineEdit>(page, "dxParam0Edit") && ctl<QLineEdit>(page, "dxParam0Edit")->text() == "7", "pitch shift: the semitones box shows 7", ctl<QLineEdit>(page, "dxParam0Edit") ? ctl<QLineEdit>(page, "dxParam0Edit")->text().toStdString() : "none");
            page->grab().save(out + "/pitchshift_fifthup.png");
            QCheckBox* anti = ctl<QCheckBox>(page, "dxParam3");
            QCheckBox* keep = ctl<QCheckBox>(page, "dxParam4");
            check(anti && keep && anti->isEnabled() && keep->isEnabled(), "pitch shift: a fifth up allows the filter and Preserve duration");
            rig.p->setProgram(1);   // Fifth down
            page->refresh();
            settle();
            check(!anti->isEnabled() && keep->isEnabled(), "pitch shift: a pitch down has no anti-alias filter");
            rig.host.set(0, 20);
            page->refresh();
            settle();
            check(!keep->isEnabled() && anti->isEnabled(), "pitch shift: Preserve duration stops at 12 semitones");
            check(std::fabs(rig.real(0) - 20) < 1e-4, "pitch shift: 20 semitones kept", std::to_string(rig.real(0)));
            // the ratio follows the pitch
            rig.host.set(0, 12);
            rig.host.set(1, 0);
            page->refresh();
            settle();
            bool found = false;
            for (QLabel* l : page->findChildren<QLabel*>())
                if (l->text() == "2") found = true;
            check(found, "pitch shift: an octave up shows the ratio 2");
            // the initial time box takes hr:mn:sc.xxx
            QLineEdit* ini = ctl<QLineEdit>(page, "dxParam6Edit");
            check(ini != nullptr, "pitch shift: has the initial time box");
            if (ini) {
                ini->setText("00:00:02.000");
                emit ini->editingFinished();
                settle();
                check(std::fabs(rig.real(6) - 88200) < 1.0, "pitch shift: initial time of 2 s is 88200 samples", std::to_string(rig.real(6)));
            }
            delete page;
        }
    }
    // ---- Time Stretch
    {
        Rig rig("reno.timestretch");
        DxPage* page = makeBuiltinPage(nullptr, "reno.timestretch", rig.host);
        check(page != nullptr, "time stretch: has a page");
        if (page) {
            page->show();
            settle(100);
            page->grab().save(out + "/timestretch.png");
            generic("reno.timestretch", "time stretch", page, rig);
            if (QWidget* f = QApplication::focusWidget()) f->clearFocus();
            rig.p->setProgram(2);   // 110 % of original
            page->refresh();
            settle();
            QLineEdit* fin = ctl<QLineEdit>(page, "dxParam0Edit");
            QLineEdit* ini = ctl<QLineEdit>(page, "dxParam2Edit");
            QComboBox* fmt = ctl<QComboBox>(page, "dxParam4");
            check(fin && ini && fmt, "time stretch: boxes found");
            if (fin && ini && fmt) {
                check(fin->text() == "110.0" && !ini->isEnabled(), "time stretch: percentage shows 110.0 and no initial value", fin->text().toStdString());
                fmt->setCurrentIndex(1);   // tempo: 120 bpm stretched to 110 % is 109.09 bpm
                settle();
                check(ini->isEnabled() && fin->text() == "109.09", "time stretch: tempo format shows the final tempo", fin->text().toStdString());
                fin->setText("100");
                emit fin->editingFinished();
                settle();
                check(std::fabs(rig.real(0) - 120.0) < 1e-3, "time stretch: a final tempo of 100 bpm is 120 %", std::to_string(rig.real(0)));
                fmt->setCurrentIndex(2);   // time: 60 s initial
                settle();
                check(fin->text() == "00:01:12.000", "time stretch: time format shows the final time", fin->text().toStdString());
                ini->setText("00:00:10.000");
                emit ini->editingFinished();
                settle();
                check(std::fabs(rig.real(3) - 441000) < 1.0, "time stretch: initial time of 10 s is 441000 samples", std::to_string(rig.real(3)));
                fin->setText("00:00:20.000");
                emit fin->editingFinished();
                settle();
                check(std::fabs(rig.real(0) - 200.0) < 1e-3, "time stretch: a final time of 20 s is 200 %", std::to_string(rig.real(0)));
                page->grab().save(out + "/timestretch_time.png");
            }
            delete page;
        }
    }
    // ---- ExpressFX Time Stretch (five modes, a wider dialog)
    {
        Rig rig("reno.xptimestretch");
        DxPage* page = makeBuiltinPage(nullptr, "reno.xptimestretch", rig.host);
        check(page != nullptr, "express time stretch: has a page");
        if (page) {
            page->show();
            settle(100);
            page->grab().save(out + "/xptimestretch.png");
            generic("reno.xptimestretch", "express time stretch", page, rig);
            QComboBox* m = ctl<QComboBox>(page, "dxParam1");
            check(m && m->count() == 5, "express time stretch: five modes");
            delete page;
        }
    }
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
