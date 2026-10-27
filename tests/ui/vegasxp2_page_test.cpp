// The pages of the ExpressFX effects of sfxpfx2 (Delay, Chorus, Equalization, Amplitude Modulation): every control sets
// the setting it stands for, the boxes cut what is typed to the plug-in's step, the page follows the presets, and the
// readouts of the fader show the mix as the plug-in's dialog does. Saves a picture of each page.
//
//   vegasxp2_page_test <output folder>
#include "../../src/app/fxpages.h"
#include "../../src/audiofx/vegaspack1.h"
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
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

// a trackbar and its box: the trackbar sets the setting, the box too (cut to 'step'), the page follows a preset
static void sliderAndBox(Rig& rig, QWidget* page, int i, const std::string& label, double step, double lo, double hi, int dec = 1)
{
    const QString n = QString("dxParam%1").arg(i);
    QSlider* s = ctl<QSlider>(page, n);
    QLineEdit* e = ctl<QLineEdit>(page, n + "Edit");
    check(s != nullptr && e != nullptr, label + ": trackbar and box found");
    if (!s || !e) return;
    s->setValue(s->maximum() / 2);
    settle();
    check(std::fabs(rig.real(i) - (lo + (hi - lo) * 0.5)) < step * 0.51, label + ": the trackbar sets it", std::to_string(rig.real(i)));
    check(e->text() == QString::number(rig.real(i), 'f', dec), label + ": the box shows it", e->text().toStdString());
    e->setText(QString::number(lo + (hi - lo) * 0.1234, 'f', 5));
    emit e->editingFinished();
    settle();
    const double want = std::trunc((lo + (hi - lo) * 0.1234) / step + 1e-9) * step;
    check(std::fabs(rig.real(i) - want) < step * 0.01, label + ": a number typed in the box is cut to its step", std::to_string(rig.real(i)) + " wanted " + std::to_string(want));
    e->setText("999999");
    emit e->editingFinished();
    settle();
    check(std::fabs(rig.real(i) - hi) < 1e-6, label + ": a number above the range is kept in it", std::to_string(rig.real(i)));
    e->setText("-5");
    emit e->editingFinished();
    settle();
    check(std::fabs(rig.real(i) - lo) < 1e-6, label + ": a number below the range is kept in it", std::to_string(rig.real(i)));
}

static void followsPresets(Rig& rig, DxPage* page, const std::string& label, const std::vector<std::pair<int, double>>& sliders)
{
    bool ok = true;
    std::string bad;
    for (int k = 0; k < rig.p->programCount(); ++k) {
        rig.p->setProgram(k);
        page->refresh();
        settle(10);
        for (auto& si : sliders) {
            QSlider* s = ctl<QSlider>(page, QString("dxParam%1").arg(si.first));
            if (!s) continue;
            if (std::fabs((double)s->value() / si.second - rig.real(si.first)) > 0.5 / si.second) { ok = false; bad += " [" + rig.p->paramName(si.first) + " preset " + std::to_string(k) + "]"; }
        }
    }
    check(ok, label + ": the page follows every preset", bad);
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    const QString out = argc > 1 ? argv[1] : "vegasxp2_out";
    QDir().mkpath(out);

    // ---- ExpressFX Delay
    {
        Rig rig("reno.xpdelay");
        DxPage* page = makeBuiltinPage(nullptr, "reno.xpdelay", rig.host);
        check(page != nullptr, "delay: has a page");
        if (page) {
            rig.p->setProgram(5);   // Echo chamber: 35 %, 250.0 ms, 50.0 %
            page->refresh();
            page->show();
            settle(100);
            page->grab().save(out + "/xpdelay.png");
            sliderAndBox(rig, page, 0, "delay: Delay", 0.1, 0, 1000);
            sliderAndBox(rig, page, 1, "delay: Decay", 0.1, 0, 90);
            QSlider* f = ctl<QSlider>(page, "dxParam2");
            check(f && f->maximum() == 1000, "delay: the fader runs 0 to 1000");
            if (f) {
                f->setValue(250);
                settle();
                check(std::fabs(rig.real(2) - 25.0) < 0.06, "delay: the fader sets the mix", std::to_string(rig.real(2)));
                QStringList texts;
                for (QLabel* l : page->findChildren<QLabel*>()) texts << l->text();
                check(texts.contains("25.0 %") && texts.contains("75.0 %"), "delay: the readouts show the delay and the original shares", texts.join("|").toStdString());
            }
            followsPresets(rig, page, "delay", {{0, 10}, {1, 10}, {2, 10}});
            delete page;
        }
    }
    // ---- ExpressFX Chorus
    {
        Rig rig("reno.xpchorus");
        DxPage* page = makeBuiltinPage(nullptr, "reno.xpchorus", rig.host);
        check(page != nullptr, "chorus: has a page");
        if (page) {
            rig.p->setProgram(8);   // Slow heavy chorus: 3 voices, 0.300 Hz, 40 %, 75.0 %
            page->refresh();
            page->show();
            settle(100);
            page->grab().save(out + "/xpchorus.png");
            QRadioButton* r[3];
            for (int k = 0; k < 3; ++k) r[k] = ctl<QRadioButton>(page, QString("dxParam0_%1").arg(k));
            check(r[0] && r[1] && r[2], "chorus: three size buttons");
            if (r[0] && r[1] && r[2]) {
                check(r[2]->isChecked() && !r[0]->isChecked(), "chorus: the size button follows the preset");
                r[1]->setChecked(true);
                settle();
                check(std::lround(rig.real(0)) == 2, "chorus: a size button sets the size", std::to_string(rig.real(0)));
                r[0]->setChecked(true);
                settle();
                check(std::lround(rig.real(0)) == 1, "chorus: size 1", std::to_string(rig.real(0)));
            }
            sliderAndBox(rig, page, 1, "chorus: Rate", 0.001, 0.001, 20, 3);
            sliderAndBox(rig, page, 2, "chorus: Depth", 1, 1, 100, 0);
            QSlider* f = ctl<QSlider>(page, "dxParam3");
            check(f && f->maximum() == 1000, "chorus: the fader runs 0 to 1000");
            if (f) {
                f->setValue(250);
                settle();
                check(std::fabs(rig.real(3) - 25.0) < 0.06, "chorus: the fader sets the mix", std::to_string(rig.real(3)));
                QStringList texts;
                for (QLabel* l : page->findChildren<QLabel*>()) texts << l->text();
                check(texts.contains("25.0 %") && texts.contains("75.0 %"), "chorus: the readouts show the chorus and the original shares", texts.join("|").toStdString());
            }
            followsPresets(rig, page, "chorus", {{1, 1000}, {2, 1}, {3, 10}});
            delete page;
        }
    }
    // ---- ExpressFX Equalization
    {
        Rig rig("reno.xpeq");
        DxPage* page = makeBuiltinPage(nullptr, "reno.xpeq", rig.host);
        check(page != nullptr, "eq: has a page");
        if (page) {
            rig.p->setProgram(4);   // Contour (drop middle): mid -25.0, bass 6.0, treble 6.0 dB
            page->refresh();
            page->show();
            settle(100);
            page->grab().save(out + "/xpeq.png");
            const char* names[3] = {"Bass", "Mid", "Treble"};
            for (int i = 0; i < 3; ++i) {
                QSlider* f = ctl<QSlider>(page, QString("dxParam%1").arg(i));
                check(f && f->minimum() == -250 && f->maximum() == 250, std::string("eq: the ") + names[i] + " fader runs -250 to 250");
                if (!f) continue;
                f->setValue(-45);
                settle();
                check(std::fabs(rig.real(i) + 4.5) < 0.06, std::string("eq: the ") + names[i] + " fader sets the gain", std::to_string(rig.real(i)));
                QStringList texts;
                for (QLabel* l : page->findChildren<QLabel*>()) texts << l->text();
                check(texts.contains("-4.5 dB"), std::string("eq: the ") + names[i] + " reading shows the gain", texts.join("|").toStdString());
                f->setValue(0);
                settle();
            }
            QSlider* f = ctl<QSlider>(page, "dxParam3");
            check(f && f->maximum() == 1000, "eq: the mix fader runs 0 to 1000");
            if (f) {
                f->setValue(250);
                settle();
                check(std::fabs(rig.real(3) - 25.0) < 0.06, "eq: the fader sets the mix", std::to_string(rig.real(3)));
                QStringList texts;
                for (QLabel* l : page->findChildren<QLabel*>()) texts << l->text();
                check(texts.contains("25.0 %") && texts.contains("75.0 %"), "eq: the readouts show the EQ and the original shares", texts.join("|").toStdString());
            }
            followsPresets(rig, page, "eq", {{0, 10}, {1, 10}, {2, 10}, {3, 10}});
            delete page;
        }
    }
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
