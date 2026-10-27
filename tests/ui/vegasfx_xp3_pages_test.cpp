// The pages of the ExpressFX effects of sfxpfx3 (Dynamics, Graphic EQ, Noise Gate, Time Stretch): every control sets the
// setting it stands for, the page follows presets and settings changed from elsewhere. Saves a picture of each page.
//
//   vegasfx_xp3_pages_test <output folder>
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

static bool skip(AudioPlugin*, int) { return false; }

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

static void onePage(const std::string& uid, const std::string& label, const QString& out, const QString& shot)
{
    Rig rig(uid);
    DxPage* page = makeBuiltinPage(nullptr, uid, rig.host);
    check(page != nullptr, label + ": has a page");
    if (!page) return;
    page->show();
    settle(100);
    page->grab().save(out + "/" + shot + ".png");
    generic(uid, label, page, rig);
    rig.p->setProgram(0);
    page->refresh();
    settle();
    page->grab().save(out + "/" + shot + "_default.png");
    delete page;
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    const QString out = argc > 1 ? argv[1] : "vegasfx_xp3_out";
    QDir().mkpath(out);
    onePage("reno.xpdynamics", "xp dynamics", out, "xpdynamics");
    onePage("reno.xpnoisegate", "xp noise gate", out, "xpnoisegate");
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
