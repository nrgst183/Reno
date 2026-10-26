// Project Properties, the plug-in scan dialog and About.
#include "panels_internal.h"
#include "vprefs.h"
#include "audiotools.h"
#include "../engine/colormgmt.h"
#include "../engine/captions.h"
#include "previewextras.h"
#include "../engine/timefmt.h"
#include "../engine/fxnames.h"
#include "logo.h"
#include "fxlibrary.h"
#include "fxthumbs.h"
#include "theme.h"
#include "playback.h"
#include "timeline.h"
#include "../media/media.h"
#include "lazyrefresh.h"
#include "strconv.h"
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QWheelEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QGuiApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QListWidget>
#include <QInputDialog>
#include <QDesktopServices>
#include <QProcess>
#include <QTabWidget>
#include <QPlainTextEdit>
#include <QLocale>
#include <QStyle>
#include <QTreeWidgetItemIterator>
#include <QDateTime>
#include <QContextMenuEvent>
#include <QCursor>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QDir>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeView>
#include <QListView>
#include <QDrag>
#include <QActionGroup>
#include <QSplitter>
#include <QFileSystemModel>
#include <QUrl>
#include <QVBoxLayout>

namespace reno {
namespace app {

namespace {

struct RateChoice {
    const char* label;
    int num, den;
};
const RateChoice kRates[] = {{"23.976 fps (film, NTSC)", 24000, 1001}, {"24 fps (film)", 24, 1},
                             {"25 fps (PAL)", 25, 1},                  {"29.97 fps (NTSC)", 30000, 1001},
                             {"30 fps", 30, 1},                        {"50 fps", 50, 1},
                             {"59.94 fps", 60000, 1001},               {"60 fps", 60, 1}};
const int kRateCount = (int)(sizeof(kRates) / sizeof(kRates[0]));

struct SizeTemplate {
    const char* label;
    int w, h;
};
const SizeTemplate kTemplates[] = {{"1920 x 1080 (HD)", 1920, 1080},          {"1280 x 720 (HD ready)", 1280, 720},
                                   {"3840 x 2160 (4K UHD)", 3840, 2160},      {"1080 x 1920 (vertical)", 1080, 1920},
                                   {"640 x 480 (standard definition)", 640, 480}};
const int kTemplateCount = (int)(sizeof(kTemplates) / sizeof(kTemplates[0]));

class PropertiesDialog : public QDialog {
public:
    explicit PropertiesDialog(QWidget* parent) : QDialog(parent)
    {
        setObjectName("propertiesDialog");
        setWindowTitle("Project Properties");
        const ProjectSettings& s = AppState::get().project().settings;
        auto* root = new QVBoxLayout(this);
        auto* form = new QFormLayout();

        template_ = new QComboBox(this);
        template_->setObjectName("template");
        template_->addItem("Custom");
        for (const auto& t : kTemplates) template_->addItem(t.label);
        form->addRow("Template:", template_);

        auto* sizeRow = new QHBoxLayout();
        width_ = new QSpinBox(this);
        width_->setObjectName("width");
        width_->setRange(16, 8192);
        width_->setSingleStep(2);
        width_->setSuffix(" px");
        width_->setValue(s.width);
        height_ = new QSpinBox(this);
        height_->setObjectName("height");
        height_->setRange(16, 8192);
        height_->setSingleStep(2);
        height_->setSuffix(" px");
        height_->setValue(s.height);
        sizeRow->addWidget(width_);
        sizeRow->addWidget(new QLabel("x", this));
        sizeRow->addWidget(height_);
        sizeRow->addStretch(1);
        form->addRow("Size:", sizeRow);

        rate_ = new QComboBox(this);
        rate_->setObjectName("frameRate");
        for (const auto& r : kRates) rate_->addItem(r.label);
        rate_->addItem("Custom...");
        form->addRow("Frame rate:", rate_);
        auto* fracRow = new QHBoxLayout();
        num_ = new QSpinBox(this);
        num_->setObjectName("fpsNum");
        num_->setRange(1, 1000000);
        den_ = new QSpinBox(this);
        den_->setObjectName("fpsDen");
        den_->setRange(1, 1000000);
        fracRow->addWidget(num_);
        fracRow->addWidget(new QLabel("/", this));
        fracRow->addWidget(den_);
        fracRow->addWidget(new QLabel("frames per second", this));
        fracRow->addStretch(1);
        form->addRow("Custom rate:", fracRow);

        sampleRate_ = new QComboBox(this);
        sampleRate_->setObjectName("sampleRate");
        for (int hz : {44100, 48000, 96000}) sampleRate_->addItem(QString("%1 Hz").arg(hz), hz);
        int sr = sampleRate_->findData(s.sampleRate);
        if (sr < 0) {
            sampleRate_->addItem(QString("%1 Hz").arg(s.sampleRate), s.sampleRate);
            sr = sampleRate_->count() - 1;
        }
        sampleRate_->setCurrentIndex(sr);
        form->addRow("Sample rate:", sampleRate_);
        root->addLayout(form);

        auto* note = new QLabel("Effects already on the timeline keep the picture size they were made at until the project is reopened. "
                                "Sizes are made even, as video formats need.",
                                this);
        note->setObjectName("note");
        note->setWordWrap(true);
        note->setPalette([note] {
            QPalette p = note->palette();
            p.setColor(QPalette::WindowText, QColor(170, 170, 175));
            return p;
        }());
        root->addWidget(note);

        auto* row = new QHBoxLayout();
        row->addStretch(1);
        auto* ok = new QPushButton("OK", this);
        ok->setObjectName("ok");
        ok->setDefault(true);
        auto* cancel = new QPushButton("Cancel", this);
        cancel->setObjectName("cancel");
        row->addWidget(ok);
        row->addWidget(cancel);
        root->addLayout(row);

        int ri = kRateCount;
        for (int k = 0; k < kRateCount; ++k)
            if ((long long)kRates[k].num * s.fpsDen == (long long)s.fpsNum * kRates[k].den) ri = k;
        rate_->setCurrentIndex(ri);
        num_->setValue(s.fpsNum);
        den_->setValue(s.fpsDen);
        syncRate();
        syncTemplate();

        QObject::connect(template_, &QComboBox::activated, [this](int index) {
            if (index <= 0) return;
            width_->setValue(kTemplates[index - 1].w);
            height_->setValue(kTemplates[index - 1].h);
        });
        QObject::connect(width_, &QSpinBox::valueChanged, [this](int) { syncTemplate(); });
        QObject::connect(height_, &QSpinBox::valueChanged, [this](int) { syncTemplate(); });
        QObject::connect(rate_, &QComboBox::currentIndexChanged, [this](int) { syncRate(); });
        QObject::connect(ok, &QPushButton::clicked, [this] { apply(); accept(); });
        QObject::connect(cancel, &QPushButton::clicked, [this] { reject(); });
    }

private:
    void syncTemplate()
    {
        int found = 0;
        for (int k = 0; k < kTemplateCount; ++k)
            if (kTemplates[k].w == width_->value() && kTemplates[k].h == height_->value()) found = k + 1;
        QSignalBlocker b(template_);
        template_->setCurrentIndex(found);
    }

    void syncRate()
    {
        const int i = rate_->currentIndex();
        const bool custom = i >= kRateCount;
        if (!custom) {
            num_->setValue(kRates[i].num);
            den_->setValue(kRates[i].den);
        }
        num_->setEnabled(custom);
        den_->setEnabled(custom);
    }

    void apply()
    {
        const int w = width_->value() & ~1, h = height_->value() & ~1;
        const int num = num_->value(), den = den_->value();
        const int hz = sampleRate_->currentData().toInt();
        const ProjectSettings& cur = AppState::get().project().settings;
        if (cur.width == w && cur.height == h && cur.fpsNum == num && cur.fpsDen == den && cur.sampleRate == hz) return;
        AppState::get().edit("Project Properties", [=](Project& p) {
            p.settings.width = w;
            p.settings.height = h;
            p.settings.fpsNum = num;
            p.settings.fpsDen = den;
            p.settings.sampleRate = hz;
        });
    }

    QComboBox *template_ = nullptr, *rate_ = nullptr, *sampleRate_ = nullptr;
    QSpinBox *width_ = nullptr, *height_ = nullptr, *num_ = nullptr, *den_ = nullptr;
};

}  // namespace

void runProjectProperties(QWidget* parent)
{
    PropertiesDialog dlg(parent);
    dlg.exec();
}

namespace {

// The scan itself is ScanTask's (catalog.h), one at a time on its own thread; the dialog polls it on a timer.
// If one is already running (the splash screen's, sent to the background with Skip) the dialog shows that one.
class ScanDialog : public QDialog {
public:
    ScanDialog(QWidget* parent, bool full, const std::string& exe) : QDialog(parent)
    {
        setObjectName("scanDialog");
        setWindowTitle("Looking for Plug-Ins");
        setMinimumWidth(460);
        auto* root = new QVBoxLayout(this);
        const bool joined = !ScanTask::get().start(exe, full, false);
        title_ = new QLabel(full && !joined ? "Trying every plug-in again..." : "Looking for new plug-ins...", this);
        title_->setObjectName("scanTitle");
        QFont f = title_->font();
        f.setBold(true);
        title_->setFont(f);
        what_ = new QLabel("Starting...", this);
        what_->setObjectName("scanWhat");
        what_->setTextFormat(Qt::PlainText);
        what_->setWordWrap(false);
        bar_ = new QProgressBar(this);
        bar_->setObjectName("scanBar");
        bar_->setRange(0, 0);  // busy until the file count is known
        count_ = new QLabel(this);
        count_->setObjectName("scanCount");
        summary_ = new QLabel(this);
        summary_->setObjectName("scanSummary");
        summary_->setWordWrap(true);
        root->addWidget(title_);
        root->addWidget(what_);
        root->addWidget(bar_);
        root->addWidget(count_);
        root->addWidget(summary_);
        auto* row = new QHBoxLayout();
        background_ = new QPushButton("Run in &Background", this);
        background_->setObjectName("scanBackground");
        background_->setToolTip("Close this window and go on working: the status bar shows the search, and the lists are "
                                "brought up to date when it is done");
        row->addWidget(background_);
        row->addStretch(1);
        cancel_ = new QPushButton("Cancel", this);
        cancel_->setObjectName("scanCancel");
        close_ = new QPushButton("Close", this);
        close_->setObjectName("scanClose");
        close_->hide();
        row->addWidget(cancel_);
        row->addWidget(close_);
        root->addLayout(row);

        QObject::connect(cancel_, &QPushButton::clicked, [this] {
            ScanTask::get().cancel();
            cancel_->setEnabled(false);
            background_->setEnabled(false);
            what_->setText("Stopping...");
        });
        QObject::connect(close_, &QPushButton::clicked, [this] { accept(); });
        QObject::connect(background_, &QPushButton::clicked, [this] {
            inBackground_ = true;
            if (QWidget* w = parentWidget() ? parentWidget()->window() : nullptr) watchBackgroundScan(w);
            accept();
        });
        serial_ = ScanTask::get().state().serial;
        poll_ = new QTimer(this);
        poll_->setInterval(40);
        QObject::connect(poll_, &QTimer::timeout, [this] {
            const ScanTask::State st = ScanTask::get().state();
            if (st.serial != serial_ || !st.running) {
                poll_->stop();
                ScanTask::get().join();
                finished(st.serial == serial_ ? st.failure : std::string(), st.cancelled);
                return;
            }
            progress(st.what, st.done, st.total);
        });
        poll_->start();
    }

    ~ScanDialog() override { stop(); }
    void reject() override
    {
        stop();
        QDialog::reject();
    }

    void progress(const std::string& what, int done, int total)
    {
        if (finished_) return;
        if (total > 0) {
            bar_->setRange(0, total);
            bar_->setValue(done);
            count_->setText(QString("%1 of %2 done").arg(done).arg(total));
        }
        if (!what.empty() && cancel_->isEnabled()) what_->setText(QString("Trying %1").arg(qs(what)));
    }

    void finished(const std::string& failure, bool cancelled)
    {
        finished_ = true;
        AppState::get().notify(kCatalog);
        int vTotal = 0, vBad = 0, aTotal = 0, aBad = 0;
        for (const auto& e : Catalog::get().video()) { ++vTotal; if (!e.ok) ++vBad; }
        for (const auto& e : Catalog::get().audio()) { ++aTotal; if (!e.ok) ++aBad; }
        title_->setText(!failure.empty() ? "Looking for plug-ins failed" : cancelled ? "Stopped" : "Finished");
        what_->setText("");
        if (bar_->maximum() == 0) bar_->setRange(0, 1);
        if (!cancelled && failure.empty()) bar_->setValue(bar_->maximum());
        count_->setText("");
        QString text;
        if (!failure.empty()) text = QString("The search stopped because of an error: %1\n\n").arg(qs(failure));
        else if (cancelled) text = "The search was stopped; plug-ins not reached yet keep what was known about them.\n\n";
        text += QString("%1 video %2 found, %3.\n%4 audio %5 found, %6.")
                    .arg(vTotal).arg(vTotal == 1 ? "effect" : "effects").arg(vBad == 0 ? QString("all work") : QString("%1 do not work").arg(vBad))
                    .arg(aTotal).arg(aTotal == 1 ? "effect" : "effects").arg(aBad == 0 ? QString("all work") : QString("%1 do not work").arg(aBad));
        if (vBad + aBad > 0) text += "\nPoint at a greyed entry in the lists to see why.";
        summary_->setText(text);
        cancel_->hide();
        background_->hide();
        close_->show();
        close_->setDefault(true);
    }

private:
    // closed early (Esc, window X): stop the scan like Cancel, unless it was sent to the background
    void stop()
    {
        if (finished_ || inBackground_) return;
        poll_->stop();
        if (ScanTask::get().state().serial == serial_) {
            ScanTask::get().cancel();
            ScanTask::get().join();
            AppState::get().notify(kCatalog);
        }
        finished_ = true;
    }
    QLabel *title_ = nullptr, *what_ = nullptr, *count_ = nullptr, *summary_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QPushButton *cancel_ = nullptr, *close_ = nullptr, *background_ = nullptr;
    QTimer* poll_ = nullptr;
    int serial_ = 0;
    bool finished_ = false, inBackground_ = false;
};

}  // namespace

void watchBackgroundScan(QWidget* owner)
{
    auto* timer = new QTimer(owner);
    timer->setObjectName("backgroundScanWatch");
    timer->setInterval(250);
    const int serial = ScanTask::get().state().serial;
    QObject::connect(timer, &QTimer::timeout, timer, [timer, serial] {
        const ScanTask::State s = ScanTask::get().state();
        if (s.serial != serial || !s.running) {
            timer->stop();
            timer->deleteLater();
            if (!ScanTask::get().state().running) ScanTask::get().join();
            AppState::get().notify(kCatalog);
            if (s.serial == serial)
                AppState::get().status(!s.failure.empty() ? "Looking for plug-ins stopped: " + s.failure
                                       : s.cancelled          ? std::string("Looking for plug-ins was stopped")
                                                              : std::string("Finished looking for new plug-ins"));
            return;
        }
        if (s.total > 0)
            AppState::get().status(strfmt("Looking for new plug-ins in the background: %d of %d%s%s", s.done, s.total,
                                          s.what.empty() ? "" : " - ", s.what.c_str()));
    });
    timer->start();
}

void runPluginScan(QWidget* parent, bool full)
{
    const std::string exe = defaultProbeExe();
    if (!fileExists(exe)) {
        QMessageBox box(QMessageBox::Warning, "Looking for Plug-Ins",
                        QString("Plug-ins cannot be looked for because the helper program is missing.\n\nExpected file:\n%1\n\n"
                                "It is installed next to Reno; reinstalling Reno puts it back.")
                            .arg(QDir::toNativeSeparators(qs(exe))),
                        QMessageBox::Ok, parent);
        box.setObjectName("probeMissing");
        box.exec();
        return;
    }
    ScanDialog dlg(parent, full, exe);
    dlg.exec();
}

void showAbout(QWidget* parent)
{
    QDialog dlg(parent);
    dlg.setObjectName("aboutDialog");
    dlg.setWindowTitle("About Reno");
    dlg.setMinimumWidth(480);
    auto* root = new QVBoxLayout(&dlg);
    auto* logo = new QLabel(&dlg);
    logo->setObjectName("aboutLogo");
    logo->setPixmap(QPixmap::fromImage(renoLogo(96)));
    root->addWidget(logo);
    auto* name = new QLabel(QString("Reno %1").arg(renoVersion()), &dlg);
    QFont nf = name->font();
    nf.setPixelSize(30);
    nf.setBold(true);
    name->setFont(nf);
    root->addWidget(name);
    auto* what = new QLabel("A video editor with a timeline, OpenFX video effects and VST sound effects.", &dlg);
    what->setWordWrap(true);
    root->addWidget(what);

    QString sound = gPlayback ? qs(gPlayback->audioDeviceName()) : QString();
    if (sound.isEmpty()) sound = "not opened yet (it is opened when you first play)";
    auto* facts = new QLabel(QString("<table cellspacing='4'>"
                                     "<tr><td>FFmpeg:</td><td>%1</td></tr>"
                                     "<tr><td>Qt:</td><td>%2</td></tr>"
                                     "<tr><td>Sound device:</td><td>%3</td></tr></table>")
                                 .arg(qs(mediaBackendVersion()).toHtmlEscaped(), QString::fromLatin1(qVersion()), sound.toHtmlEscaped()),
                             &dlg);
    facts->setObjectName("aboutFacts");
    facts->setTextFormat(Qt::RichText);
    root->addWidget(facts);

    auto* notices = new QTextBrowser(&dlg);
    notices->setObjectName("aboutNotices");
    notices->setOpenExternalLinks(true);
    notices->setMinimumHeight(180);
    notices->setHtml("<b>Third-party software</b>"
                     "<p><b>OpenFX headers</b> - BSD 3-Clause licence. Copyright The Open Effects Association Ltd. Used to host video effect plug-ins.</p>"
                     "<p><b>VST 2 interface header</b> - BSD 3-Clause licence. Used to host VST 2 sound effects.</p>"
                     "<p><b>FFmpeg</b> - GNU Lesser General Public Licence (LGPL) version 2.1 or later. Used to read and write media files; "
                     "its libraries are loaded as separate shared libraries.</p>"
                     "<p><b>Qt</b> - GNU Lesser General Public Licence (LGPL) version 3. Used for the interface.</p>");
    root->addWidget(notices, 1);

    auto* row = new QHBoxLayout();
    row->addStretch(1);
    auto* ok = new QPushButton("OK", &dlg);
    ok->setDefault(true);
    row->addWidget(ok);
    root->addLayout(row);
    QObject::connect(ok, &QPushButton::clicked, [&dlg] { dlg.accept(); });
    dlg.exec();
}

}  // namespace app
}  // namespace reno
