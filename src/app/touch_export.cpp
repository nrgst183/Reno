// The touch interface's Export: a few choices (video or sound, size), then the render with its progress in the sheet.
// It renders with Render As's templates (rendertemplates.h), a frame size and rate of 0 meaning the project's, on a
// thread of its own as the Render As dialog does; Render As itself stays a tap away for every other format.
#include "touch_shell_internal.h"
#include "rendertemplates.h"
#include "../engine/renderas.h"
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>
#include <chrono>
#include <cmath>

namespace reno {
namespace app {

using namespace touch;

namespace {

// A bar that fills as the render goes.
class ProgressBar : public QWidget {
public:
    explicit ProgressBar(QWidget* parent) : QWidget(parent) { setFixedHeight(12); }
    void setValue(double v)
    {
        v_ = std::max(0.0, std::min(1.0, v));
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(color::line);
        p.drawRoundedRect(rect(), 6, 6);
        if (v_ > 0) {
            p.setBrush(color::accent);
            p.drawRoundedRect(QRectF(0, 0, std::max(12.0, width() * v_), height()), 6, 6);
        }
    }

private:
    double v_ = 0;
};

QLabel* note(const QString& text, QWidget* parent, int px = 14, const QColor& c = color::dim)
{
    QLabel* l = new QLabel(text, parent);
    l->setWordWrap(true);
    l->setFont(uiFont(px));
    QPalette pal = l->palette();
    pal.setColor(QPalette::WindowText, c);
    l->setPalette(pal);
    l->setContentsMargins(16, 4, 16, 4);
    return l;
}

QWidget* padded(QWidget* w, QWidget* parent)
{
    QWidget* box = new QWidget(parent);
    auto* h = new QHBoxLayout(box);
    h->setContentsMargins(16, 8, 16, 8);
    h->addWidget(w);
    return box;
}

// The size a picture of 'shortSide' pixels on its shorter side has in the project's shape (even numbers, as encoders
// want them); 0 keeps the project's own.
QSize sizeFor(const ProjectSettings& ps, int shortSide)
{
    const int w = std::max(2, ps.width), h = std::max(2, ps.height);
    if (shortSide <= 0) return QSize(w, h);
    const double k = (double)shortSide / std::min(w, h);
    auto even = [](double v) { return std::max(2, (int)std::lround(v / 2.0) * 2); };
    return QSize(even(w * k), even(h * k));
}

// Where exports go unless the user picks a place: the Movies folder (the app's own folder on Android and iOS, where
// it may always write).
QString exportFolder()
{
    const QString kept = QSettings().value("touch/exportDir").toString();
    if (!kept.isEmpty() && QFileInfo(kept).isDir()) return kept;
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/Exports";
#else
    QString dir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (dir.isEmpty()) dir = QDir::homePath();
#endif
    QDir().mkpath(dir);
    return dir;
}

QString freeName(const QString& folder, const QString& stem, const QString& ext)
{
    QString path = folder + "/" + stem + "." + ext;
    for (int n = 2; QFileInfo::exists(path) && n < 1000; ++n) path = folder + "/" + stem + QString(" (%1).").arg(n) + ext;
    return path;
}

}  // namespace

// The choices: what kind of file and how big. The first video template that this FFmpeg can make in MP4 is used
// (the AVC one normally; on phones FFmpeg's own H.264 encoder takes the place of x264).
void TouchShellImpl::exportSheet()
{
    if (exportJob) {
        exportProgressSheet();
        return;
    }
    Sheet* s = newSheet("Export");
    s->setObjectName("touchExportSheet");
    const ProjectSettings& ps = app.project().settings;
    const double len = app.project().length();
    s->body()->addWidget(note(QString("%1 x %2, %3 fps, %4 long").arg(ps.width).arg(ps.height).arg(ps.fps(), 0, 'f', 2)
                                  .arg(len >= 60 ? QString("%1 min %2 s").arg((int)(len / 60)).arg((int)std::fmod(len, 60)) : QString::number(len, 'f', 1) + " s"), s));
    // what: video or just the sound
    s->body()->addWidget(sectionLabel("File", s));
    QWidget* kinds = new QWidget(s);
    auto* kh = new QHBoxLayout(kinds);
    kh->setContentsMargins(12, 0, 12, 4);
    kh->setSpacing(6);
    Button* video = new Button(Button::Chip, Icon::Media, "Video (MP4)", kinds);
    video->setObjectName("touchExportVideo");
    Button* sound = new Button(Button::Chip, Icon::Music, "Sound only", kinds);
    sound->setObjectName("touchExportSound");
    kh->addWidget(video);
    kh->addWidget(sound);
    s->body()->addWidget(kinds);
    // how big (video): the project's size or a smaller one in its shape
    QWidget* sizesBox = new QWidget(s);
    auto* sv = new QVBoxLayout(sizesBox);
    sv->setContentsMargins(0, 0, 0, 0);
    sv->setSpacing(0);
    sv->addWidget(sectionLabel("Size", sizesBox));
    QWidget* sizes = new QWidget(sizesBox);
    auto* sh = new QHBoxLayout(sizes);
    sh->setContentsMargins(12, 0, 12, 4);
    sh->setSpacing(6);
    const int projectShort = std::min(ps.width, ps.height);
    std::vector<std::pair<int, Button*>> sizeChips;
    for (int shortSide : {0, 2160, 1080, 720, 480}) {
        if (shortSide && shortSide >= projectShort) continue;   // only smaller than the project's (never stretched up)
        const QSize sz = sizeFor(ps, shortSide);
        const QString label = shortSide == 0 ? QString("%1p").arg(std::min(sz.width(), sz.height())) : shortSide == 2160 ? QString("4K") : QString("%1p").arg(shortSide);
        Button* b = new Button(Button::Chip, Icon::Media, label, sizes);
        b->setObjectName(QString("touchExportSize%1").arg(shortSide));
        sh->addWidget(b);
        sizeChips.push_back({shortSide, b});
        if (sizeChips.size() >= 4) break;
    }
    sv->addWidget(sizes);
    s->body()->addWidget(sizesBox);
    QSettings st;
    int pickedSize = st.value("touch/exportSize", 0).toInt();
    bool pickedSound = st.value("touch/exportSound", false).toBool();
    bool found = false;
    for (auto& c : sizeChips) found = found || c.first == pickedSize;
    if (!found) pickedSize = 0;
    // the file it goes to
    const QString stem = QFileInfo(QString::fromStdString(app.path())).completeBaseName().isEmpty()
                             ? QString("Reno %1").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh.mm"))
                             : QFileInfo(QString::fromStdString(app.path())).completeBaseName();
    auto folder = std::make_shared<QString>(exportFolder());
    Button* where = new Button(Button::Row, Icon::Folder, "Save in", s);
    where->setObjectName("touchExportWhere");
    where->setSubtitle(QDir::toNativeSeparators(*folder));
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
    where->setEnabled(false);   // the app's own folder: elsewhere it may not write to a path
#endif
    QObject::connect(where, &QAbstractButton::clicked, q, [this, where, folder] {
        const QString dir = QFileDialog::getExistingDirectory(q, "Export To", *folder);
        if (dir.isEmpty()) return;
        *folder = dir;
        QSettings().setValue("touch/exportDir", dir);
        where->setSubtitle(QDir::toNativeSeparators(dir));
    });
    s->body()->addWidget(sectionLabel("Where", s));
    s->body()->addWidget(where);
    Button* go = new Button(Button::Primary, Icon::Export, "Export", s);
    go->setObjectName("touchExportGo");
    s->body()->addWidget(padded(go, s));
    Button* more = new Button(Button::Row, Icon::Settings, "Other formats and settings", s);
    more->setSubtitle("Render As, with every template");
    more->setChevron(true);
    QObject::connect(more, &QAbstractButton::clicked, q, [this] {
        q->closeSheet();
        QTimer::singleShot(180, q, [this] { runAction("render"); });
    });
    s->body()->addWidget(more);

    auto state = std::make_shared<std::pair<int, bool>>(pickedSize, pickedSound);
    auto show = [video, sound, sizesBox, sizeChips, state] {
        video->setChecked(!state->second);
        sound->setChecked(state->second);
        sizesBox->setVisible(!state->second);
        for (const auto& c : sizeChips) c.second->setChecked(c.first == state->first);
    };
    show();
    QObject::connect(video, &QAbstractButton::clicked, q, [state, show] { state->second = false; show(); });
    QObject::connect(sound, &QAbstractButton::clicked, q, [state, show] { state->second = true; show(); });
    for (const auto& c : sizeChips) {
        const int v = c.first;
        QObject::connect(c.second, &QAbstractButton::clicked, q, [state, show, v] { state->first = v; show(); });
    }
    QObject::connect(go, &QAbstractButton::clicked, q, [this, state, folder, stem] {
        QSettings st2;
        st2.setValue("touch/exportSize", state->first);
        st2.setValue("touch/exportSound", state->second);
        startExport(state->second, state->first, *folder, stem);
    });
    q->openSheet(s);
}

void TouchShellImpl::startExport(bool soundOnly, int shortSide, const QString& folder, const QString& stem)
{
    const std::vector<rt::Format> all = rt::loadFormats();
    // the first template that can be made here: MP4 video (AVC, else any MP4), or for sound MP3, M4A, else WAV
    const rt::Format* fmt = nullptr;
    const rt::Tmpl* tmpl = nullptr;
    auto pick = [&](const QString& key, bool wantVideo) {
        if (fmt) return;
        for (const rt::Format& f : all) {
            if (f.sequence || (!key.isEmpty() && f.key != key) || (key.isEmpty() && f.ext.compare("mp4", Qt::CaseInsensitive) != 0 && f.ext.compare(".mp4", Qt::CaseInsensitive) != 0)) continue;
            for (const rt::Tmpl& t : f.templates)
                if (t.video == wantVideo && rt::templateAvailable(f, t)) {
                    fmt = &f;
                    tmpl = &t;
                    return;
                }
        }
    };
    if (soundOnly) {
        for (const char* k : {"mp3", "m4a", "wav"}) pick(k, false);
    } else {
        pick("avc", true);
        pick("hevc", true);
        pick(QString(), true);
    }
    if (!fmt || !tmpl) {
        app.status("No format to export to was found (Render As lists what this build can make).");
        return;
    }
    rt::Tmpl t = *tmpl;
    const ProjectSettings& ps = app.project().settings;
    if (!soundOnly) {
        // the project's frame rate, the size chosen, and a bit rate that suits it (the template's is for its own size)
        const QSize sz = sizeFor(ps, shortSide);
        const double pixels = (double)sz.width() * sz.height();
        const double templatePixels = t.width > 0 && t.height > 0 ? (double)t.width * t.height : 1920.0 * 1080.0;
        t.width = sz.width();
        t.height = sz.height();
        t.fpsNum = 0;
        t.fpsDen = 1;
        t.field = "none";
        t.par = 1.0;
        const double k = std::max(0.15, std::min(4.0, pixels / templatePixels));
        t.kbps = std::max(1000, (int)std::lround(t.kbps * k));
        t.maxKbps = std::max(t.kbps, (int)std::lround(t.maxKbps * k));
        if (pixels > 1920.0 * 1088.0) t.level.clear();      // the level of a 1080p template would refuse 4K
    }
    t.sampleRate = 0;
    QString ext = fmt->ext;
    if (ext.startsWith('.')) ext = ext.mid(1);
    if (!t.ext.isEmpty()) ext = t.ext;
    const QString path = freeName(folder, stem, ext);
    rt::RenderChoices choices;
    choices.writerPath = path.toStdString();
    RenderSpec spec;
    QString err;
    if (!rt::makeRenderSpec(*fmt, t, choices, ps, &spec, &err)) {
        app.status(err.toStdString());
        return;
    }
    if (pb->playing()) togglePlay();
    auto job = std::make_shared<ExportJob>();
    job->path = path;
    // what it is, in the words of what was chosen (the template's own name is of the size it was made for)
    job->format = soundOnly ? fmt->name : QString("%1, %2 x %3 at %4 fps").arg(fmt->name).arg(t.width).arg(t.height).arg(ps.fps(), 0, 'f', ps.fps() == std::floor(ps.fps()) ? 0 : 2);
    job->clock.start();
    exportJob = job;
    Project copy = *app.snapshot();
    const double to = std::max(copy.length(), 1.0 / std::max(1.0, ps.fps()));
    if (exportThread.joinable()) exportThread.join();
    exportThread = std::thread([job, copy, spec, to]() mutable {
        std::string e, desc;
        bool ok = false;
        auto last = std::chrono::steady_clock::now();
        try {
            AppState& a = AppState::get();
            ok = renderProjectAs(copy, a.fx, spec, 0.0, to, [&](const ExportProgress& pr) {
                job->fraction.store(pr.fraction);
                const auto now = std::chrono::steady_clock::now();
                if (now - last > std::chrono::milliseconds(50)) {
                    last = now;
                    AppState::post([job] { if (job->onProgress) job->onProgress(); });
                }
                return !job->cancel.load();
            }, &e, a.mediaOpener, &desc);
        } catch (const std::exception& x) {
            e = x.what();
        } catch (...) {
            e = "an unexpected error";
        }
        job->ok = ok;
        job->error = e;
        job->done.store(true);
        AppState::post([job] { if (job->onDone) job->onDone(); });
    });
    exportProgressSheet();
}

// The render going on (or just finished): how far it has got, and Cancel; afterwards where the file is.
void TouchShellImpl::exportProgressSheet()
{
    std::shared_ptr<ExportJob> job = exportJob;
    if (!job) return;
    Sheet* s = newSheet(job->done.load() ? (job->ok ? "Exported" : job->cancel.load() ? "Export cancelled" : "Export failed") : "Exporting");
    s->setObjectName("touchExportProgress");
    QLabel* name = note(QFileInfo(job->path).fileName(), s, 15, color::text);
    s->body()->addWidget(name);
    s->body()->addWidget(note(job->format, s, 12));
    ProgressBar* bar = new ProgressBar(s);
    bar->setObjectName("touchExportBar");
    s->body()->addWidget(padded(bar, s));
    QLabel* status = note(QString(), s);
    status->setObjectName("touchExportStatus");
    s->body()->addWidget(status);
    Button* cancel = new Button(Button::Primary, Icon::Close, "Cancel", s);
    cancel->setObjectName("touchExportCancel");
    s->body()->addWidget(padded(cancel, s));
    Button* open = new Button(Button::Row, Icon::Folder, "Show the folder", s);
    open->setObjectName("touchExportShow");
    open->setChevron(true);
    s->body()->addWidget(open);
    const QString path = job->path;
    QObject::connect(open, &QAbstractButton::clicked, q, [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).path())); });
    QPointer<Sheet> sp = s;
    auto refresh = [this, job, sp, bar, status, cancel, open] {
        if (!sp) return;
        const double f = job->fraction.load();
        bar->setValue(job->done.load() && job->ok ? 1.0 : f);
        if (job->done.load()) {
            cancel->setText("Done");
            cancel->setIconKind(Icon::Check);
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
            open->setVisible(false);
#else
            open->setVisible(job->ok);
#endif
            if (job->ok) {
                const qint64 bytes = QFileInfo(job->path).size();
                status->setText(QString("Saved in %1 (%2 MB)").arg(QDir::toNativeSeparators(QFileInfo(job->path).path())).arg(bytes / 1048576.0, 0, 'f', 1));
                sp->setTitle("Exported");
                if (sheet == sp) placeSheet(sp, false);     // room for what was added
            } else {
                status->setText(job->cancel.load() ? QString("The export was stopped.") : "It could not be made: " + QString::fromStdString(job->error));
                sp->setTitle(job->cancel.load() ? "Export cancelled" : "Export failed");
            }
            return;
        }
        open->setVisible(false);
        const double secs = job->clock.elapsed() / 1000.0;
        QString text = QString("%1 %").arg((int)std::floor(f * 100));
        if (f > 0.02 && secs > 2) {
            const int left = (int)std::lround(secs / f - secs);
            text += left >= 60 ? QString(", about %1 min %2 s left").arg(left / 60).arg(left % 60) : QString(", about %1 s left").arg(left);
        }
        status->setText(text);
    };
    job->onProgress = refresh;
    job->onDone = [this, job, refresh] {
        refresh();
        if (exportThread.joinable()) exportThread.join();
        if (job->ok) app.status("Exported " + job->path.toStdString());
    };
    QObject::connect(cancel, &QAbstractButton::clicked, q, [this, job] {
        if (!job->done.load()) {
            job->cancel.store(true);
            return;
        }
        // finished: the next Export starts afresh
        if (exportJob == job) exportJob.reset();
        q->closeSheet();
    });
    refresh();
    q->openSheet(s);
}

void TouchShellImpl::stopExport()
{
    if (exportJob) exportJob->cancel.store(true);
    if (exportThread.joinable()) exportThread.join();
}

}  // namespace app
}  // namespace reno
