// File > Render As and Tools > Render to New Track, as in VEGAS Pro 18 (see renderdialog.h).
//
// VEGAS's formats are its file-format plug-ins (MAGIX AVC/AAC MP4, MAGIX HEVC/AAC MP4, Windows Media, Video for Windows,
// QuickTime, Sony MXF / XAVC, Wave, MP3, FLAC, OggVorbis, AAC, Image Sequence), each with templates; names, sizes and
// rates.
//
// No moc (the Windows build doesn't run it): lambdas on existing signals, as everywhere in src/app.
#include "logbook.h"
#include "vprefs.h"
#include "renderdialog.h"
#include "../engine/colormgmt.h"
#include "../engine/captions.h"
#include "appstate.h"
#include "panels.h"
#include "rendertemplates.h"
#include "../engine/renderas.h"
#include "strconv.h"

#include <QApplication>
#include <ctime>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStorageInfo>
#include <QThread>
#include <QDateTime>
#include <QUrl>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <thread>

namespace reno {
namespace app {

namespace {

double processCpuSeconds()
{
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    auto secs = [](const FILETIME& f) { return (((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) / 1.0e7; };
    return secs(k) + secs(u);
#else
    return (double)std::clock() / CLOCKS_PER_SEC;
#endif
}

qint64 processMemory()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return (qint64)pmc.WorkingSetSize;
    return 0;
#else
    QFile f("/proc/self/statm");
    if (!f.open(QIODevice::ReadOnly)) return 0;
    const QList<QByteArray> parts = f.readAll().split(' ');
    return parts.size() > 1 ? parts[1].toLongLong() * 4096 : 0;
#endif
}

// VEGAS 18's way of writing sizes and times in Render As and its progress: the disk's free space in whole GB ("285 GB"),
// the estimated file size in MB ("44 MB"), the sizes while rendering with two decimals ("25.04 MB"), clocks as 00:00:33
QString diskText(qint64 bytes)
{
    if (bytes >= 1024LL * 1024 * 1024) return QString("%1 GB").arg((qint64)(bytes / (1024.0 * 1024.0 * 1024.0) + 0.5));
    return QString("%1 MB").arg((qint64)(bytes / (1024.0 * 1024.0) + 0.5));
}

QString estimateText(qint64 bytes)
{
    if (bytes >= 10LL * 1024 * 1024 * 1024) return QString("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
    if (bytes >= 1024LL * 1024) return QString("%1 MB").arg((qint64)(bytes / (1024.0 * 1024.0) + 0.5));
    return QString("%1 KB").arg((qint64)(bytes / 1024.0 + 0.5));
}

QString megaText(qint64 bytes)
{
    if (bytes >= 1024LL * 1024 * 1024) return QString("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
    return QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 2);
}

QString hmsText(double seconds)
{
    const int s = (int)std::max(0.0, seconds + 0.5);
    return QString("%1:%2:%3").arg(s / 3600, 2, 10, QChar('0')).arg(s / 60 % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
}

// A label that shortens its text in the middle (C:\Users\USER\Desktop\Master F...\Montage Video 2021.mp4) to fit its width.
class ElidedLabel : public QLabel {
public:
    explicit ElidedLabel(QWidget* parent) : QLabel(parent)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setMinimumWidth(60);
    }
    QSize sizeHint() const override { return QSize(300, fontMetrics().height() + 4); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::WindowText));
        const QRect r = contentsRect();
        p.drawText(r, Qt::AlignLeft | Qt::AlignVCenter, fontMetrics().elidedText(text(), Qt::ElideMiddle, r.width()));
    }
};

QIcon magnifierIcon(const QWidget* like)
{
    QPixmap px(16, 16);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(like->palette().color(QPalette::Disabled, QPalette::Text));
    pen.setWidthF(1.6);
    p.setPen(pen);
    p.drawEllipse(QRectF(2, 2, 8, 8));
    p.drawLine(QPointF(9, 9), QPointF(14, 14));
    return QIcon(px);
}

using namespace rt;

struct Choice {
    const char* label;
    int a, b;
};
const Choice kSizes[] = {{"(Use project settings)", 0, 0},      {"UHD 2160 (3840x2160)", 3840, 2160}, {"HD 1080 (1920x1080)", 1920, 1080},
                         {"HDV 1080 (1440x1080)", 1440, 1080}, {"HD 720 (1280x720)", 1280, 720},    {"540p (960x540)", 960, 540},
                         {"Multimedia wide (854x480)", 854, 480}, {"Multimedia wide (640x360)", 640, 360}, {"NTSC DV (720x480)", 720, 480},
                         {"PAL DV (720x576)", 720, 576},       {"Portrait (1080x1920)", 1080, 1920}, {"Portrait Multimedia (720x1280)", 720, 1280}};
const Choice kRates[] = {{"(Use project settings)", 0, 1},     {"23.976 (IVTC Film)", 24000, 1001}, {"24.000 (Film)", 24, 1},
                         {"25.000 (PAL)", 25, 1},              {"29.970 (NTSC)", 30000, 1001},      {"30.000", 30, 1},
                         {"50.000", 50, 1},                    {"59.940", 60000, 1001},             {"60.000", 60, 1}};

class CustomizeDialog : public QDialog {
public:
    CustomizeDialog(QWidget* parent, std::vector<Format>& formats, int formatIndex, const Tmpl& start)
        : QDialog(parent), formats_(formats), fi_(formatIndex)
    {
        const Format& f = formats_[fi_];
        setObjectName("customizeTemplate");
        setWindowTitle(QString("Custom Settings - %1").arg(f.name));
        setMinimumWidth(520);
        auto* root = new QVBoxLayout(this);

        auto* top = new QGridLayout();
        name_ = new QComboBox(this);
        name_->setObjectName("templateName");
        name_->setEditable(true);
        name_->setInsertPolicy(QComboBox::NoInsert);
        for (const Tmpl& t : f.templates) name_->addItem(t.name);
        name_->setCurrentText(start.name);
        auto* save = new QToolButton(this);
        save->setObjectName("saveTemplate");
        save->setText("Save");
        save->setToolTip("Save the settings as a template with this name");
        auto* del = new QToolButton(this);
        del->setObjectName("deleteTemplate");
        del->setText("Delete");
        del->setToolTip("Delete this template (only the ones you saved)");
        top->addWidget(new QLabel("Template:", this), 0, 0);
        top->addWidget(name_, 0, 1);
        top->addWidget(save, 0, 2);
        top->addWidget(del, 0, 3);
        description_ = new QLineEdit(start.description, this);
        description_->setObjectName("templateDescription");
        top->addWidget(new QLabel("Description:", this), 1, 0);
        top->addWidget(description_, 1, 1, 1, 3);
        top->setColumnStretch(1, 1);
        root->addLayout(top);

        tabs_ = new QTabWidget(this);
        tabs_->setObjectName("customTabs");
        root->addWidget(tabs_, 1);
        if (f.hasVideo) buildVideo(f);
        if (f.hasAudio) buildAudio(f);
        buildProject();

        auto* buttons = new QHBoxLayout();
        buttons->addStretch(1);
        auto* ok = new QPushButton("OK", this);
        ok->setObjectName("ok");
        ok->setDefault(true);
        auto* cancel = new QPushButton("Cancel", this);
        cancel->setObjectName("cancel");
        buttons->addWidget(ok);
        buttons->addWidget(cancel);
        root->addLayout(buttons);

        load(start);
        QObject::connect(name_, &QComboBox::activated, this, [this](int i) {
            const Format& fm = formats_[fi_];
            if (i >= 0 && i < (int)fm.templates.size()) load(fm.templates[(size_t)i]);
        });
        QObject::connect(save, &QToolButton::clicked, this, [this] { saveTemplate(); });
        QObject::connect(del, &QToolButton::clicked, this, [this] { deleteTemplate(); });
        QObject::connect(ok, &QPushButton::clicked, this, [this] { accept(); });
        QObject::connect(cancel, &QPushButton::clicked, this, [this] { reject(); });
    }

    Tmpl result() const
    {
        const Format& f = formats_[fi_];
        Tmpl t = base_;
        t.name = name_->currentText().trimmed();
        t.description = description_->text();
        if (f.hasVideo) {
            t.video = includeVideo_->isChecked();
            t.width = size_->currentIndex() == 0 ? 0 : width_->value();
            t.height = size_->currentIndex() == 0 ? 0 : height_->value();
            const int r = rate_->currentIndex();
            t.fpsNum = r >= 0 && r < (int)(sizeof(kRates) / sizeof(kRates[0])) ? kRates[r].a : 0;
            t.fpsDen = r >= 0 && r < (int)(sizeof(kRates) / sizeof(kRates[0])) ? kRates[r].b : 1;
            t.field = field_->currentData().toString();
            t.par = par_->currentData().toDouble();
            if (profile_) t.profile = profile_->currentData().toString();
            if (level_) t.level = level_->currentData().toString();
            if (f.bitRate) {
                t.rc = cbr_->isChecked() ? "cbr" : quality_->isChecked() ? "quality" : "vbr";
                t.kbps = cbr_->isChecked() ? cbrKbps_->value() : avgKbps_->value();
                t.maxKbps = maxKbps_->value();
                t.quality = qualityValue_->value();
                t.twoPass = twoPass_->isChecked() && vbr_->isChecked();
            }
            if (mode_) t.mode = mode_->currentData().toString();
            t.keySeconds = key_->value();
            if (alpha_) t.alpha = alpha_->isChecked();
            if (ffOptions_) t.ffOptions = ffOptions_->text().simplified();
        }
        if (f.hasAudio) {
            t.audio = includeAudio_->isChecked();
            t.sampleRate = sampleRate_->currentData().toInt();
            if (audioKbps_) t.audioKbps = audioKbps_->currentData().toInt();
            if (bits_) t.bits = bits_->currentData().toInt();
            t.channels = channels_->currentData().toInt();
        }
        t.renderQuality = renderQuality_->currentData().toString();
        t.motionBlur = motionBlur_->currentData().toString();
        if (colorSpace_) t.colorSpace = colorSpace_->currentData().toString();
        return t;
    }
    QTabWidget* tabs() const { return tabs_; }

private:
    static void pick(QComboBox* c, const QVariant& v)
    {
        const int i = c->findData(v);
        if (i >= 0) c->setCurrentIndex(i);
    }
    static QComboBox* combo(QWidget* parent, const char* name, const std::vector<std::pair<QString, QVariant>>& items)
    {
        auto* c = new QComboBox(parent);
        c->setObjectName(name);
        for (const auto& it : items) c->addItem(it.first, it.second);
        return c;
    }

    void buildVideo(const Format& f)
    {
        auto* page = new QWidget(tabs_);
        auto* form = new QFormLayout(page);
        includeVideo_ = new QCheckBox("Include video", page);
        includeVideo_->setObjectName("includeVideo");
        form->addRow(includeVideo_);
        size_ = new QComboBox(page);
        size_->setObjectName("frameSize");
        for (const Choice& c : kSizes) size_->addItem(c.label);
        size_->addItem("(Custom frame size)");
        form->addRow("Frame size:", size_);
        auto* wh = new QHBoxLayout();
        width_ = new QSpinBox(page);
        width_->setObjectName("width");
        width_->setRange(16, 8192);
        width_->setSingleStep(2);
        height_ = new QSpinBox(page);
        height_->setObjectName("height");
        height_->setRange(16, 8192);
        height_->setSingleStep(2);
        wh->addWidget(new QLabel("Width:", page));
        wh->addWidget(width_);
        wh->addSpacing(12);
        wh->addWidget(new QLabel("Height:", page));
        wh->addWidget(height_);
        wh->addStretch(1);
        form->addRow(QString(), wh);
        if (!f.profiles.isEmpty()) {
            profile_ = new QComboBox(page);
            profile_->setObjectName("profile");
            for (const QString& p : f.profiles) profile_->addItem(p.section('|', 1), p.section('|', 0, 0));
            form->addRow("Profile:", profile_);
        }
        if (f.videoFamily == "h264" || f.videoFamily == "hevc") {
            level_ = combo(page, "level", {{"Automatic", ""}, {"3.1", "3.1"}, {"4.0", "4.0"}, {"4.1", "4.1"}, {"4.2", "4.2"}, {"5.0", "5.0"}, {"5.1", "5.1"}, {"5.2", "5.2"}});
            form->addRow("Level:", level_);
        }
        rate_ = new QComboBox(page);
        rate_->setObjectName("frameRate");
        for (const Choice& c : kRates) rate_->addItem(c.label);
        form->addRow("Frame rate:", rate_);
        field_ = combo(page, "fieldOrder", {{"(Use project settings)", ""}, {"None (progressive scan)", "none"}, {"Upper field first", "upper"}, {"Lower field first", "lower"}});
        form->addRow("Field order:", field_);
        par_ = combo(page, "pixelAspect", {{"(Use project settings)", 0.0}, {"1.0000 (Square)", 1.0}, {"0.9091 (NTSC DV)", 0.9091}, {"1.2121 (NTSC DV Widescreen)", 1.2121},
                                           {"1.0926 (PAL DV)", 1.0926}, {"1.4568 (PAL DV Widescreen)", 1.4568}, {"1.3333 (HDV 1080)", 1.3333}});
        form->addRow("Pixel aspect ratio:", par_);
        if (f.bitRate) {
            auto* box = new QGroupBox("Bit rate", page);
            auto* g = new QGridLayout(box);
            cbr_ = new QRadioButton("Constant bit rate (Kbps):", box);
            cbr_->setObjectName("cbr");
            cbrKbps_ = new QSpinBox(box);
            cbrKbps_->setObjectName("cbrKbps");
            cbrKbps_->setRange(64, 800000);
            cbrKbps_->setSingleStep(500);
            vbr_ = new QRadioButton("Variable bit rate", box);
            vbr_->setObjectName("vbr");
            twoPass_ = new QCheckBox("Two-pass", box);
            twoPass_->setObjectName("twoPass");
            twoPass_->setToolTip("Look at the whole render once before encoding it, to share the bits out better (x264 and x265; NVENC does it in one go)");
            maxKbps_ = new QSpinBox(box);
            maxKbps_->setObjectName("maxKbps");
            maxKbps_->setRange(0, 800000);
            maxKbps_->setSingleStep(500);
            avgKbps_ = new QSpinBox(box);
            avgKbps_->setObjectName("avgKbps");
            avgKbps_->setRange(64, 800000);
            avgKbps_->setSingleStep(500);
            quality_ = new QRadioButton("Constant quality:", box);
            quality_->setObjectName("constantQuality");
            qualityValue_ = new QSpinBox(box);
            qualityValue_->setObjectName("qualityValue");
            qualityValue_->setRange(0, 51);
            qualityValue_->setToolTip("0 is the best, 51 the smallest file; 18 to 23 look good");
            g->addWidget(cbr_, 0, 0);
            g->addWidget(cbrKbps_, 0, 1);
            g->addWidget(vbr_, 1, 0);
            g->addWidget(twoPass_, 1, 1);
            g->addWidget(new QLabel("    Maximum (Kbps):", box), 2, 0);
            g->addWidget(maxKbps_, 2, 1);
            g->addWidget(new QLabel("    Average (Kbps):", box), 3, 0);
            g->addWidget(avgKbps_, 3, 1);
            g->addWidget(quality_, 4, 0);
            g->addWidget(qualityValue_, 4, 1);
            form->addRow(box);
            for (QRadioButton* b : {cbr_, vbr_, quality_}) QObject::connect(b, &QRadioButton::toggled, this, [this](bool) { syncRate(); });
        }
        key_ = new QDoubleSpinBox(page);
        key_->setObjectName("keyframeInterval");
        key_->setRange(0, 60);
        key_->setDecimals(1);
        key_->setSingleStep(0.5);
        key_->setSuffix(" s");
        key_->setSpecialValueText("Encoder's choice");
        form->addRow("Key frame every:", key_);
        if (f.videoFamily == "h264" || f.videoFamily == "hevc") {
            const std::string fam = f.videoFamily;
            mode_ = new QComboBox(page);
            mode_->setObjectName("encodeMode");
            mode_->addItem("Automatic (recommended)", "auto");
            mode_->addItem("Render using CPU only", "cpu");
            const char* modes[][2] = {{"nvenc", "NV Encoder (NVIDIA NVENC)"}, {"qsv", "Intel QSV"}, {"amf", "AMD VCE (AMF)"}, {"mf", "Windows Media Foundation"}};
            QStringList have;
            for (auto& m : modes) {
                const bool works = hardware().works(fam, m[0]), present = hardware().present(fam, m[0]);
                QString label = m[1];
                if (!present) label += " (not in this FFmpeg)";
                else if (!works) label += " (not available on this computer)";
                else have << m[1];
                mode_->addItem(label, m[0]);
                if (!works) {
                    if (auto* model = qobject_cast<QStandardItemModel*>(mode_->model()))
                        if (QStandardItem* it = model->item(mode_->count() - 1)) it->setEnabled(false);
                }
            }
            form->addRow("Encode mode:", mode_);
            auto* gpu = new QLabel(have.isEmpty() ? QString("No graphics card encoder is available: the CPU encodes.") : QString("Available: %1.").arg(have.join(", ")), page);
            gpu->setObjectName("gpuStatus");
            gpu->setWordWrap(true);
            form->addRow(QString(), gpu);
        }
        if (f.alphaPossible) {
            alpha_ = new QCheckBox("Keep transparency (alpha channel)", page);
            alpha_->setObjectName("alpha");
            form->addRow(QString(), alpha_);
        }
        if (f.key == "ffmpeg") {
            ffOptions_ = new QLineEdit(page);
            ffOptions_->setObjectName("encoderOptions");
            ffOptions_->setPlaceholderText("preset=slow tune=film x265-params=aq-mode=3");
            ffOptions_->setToolTip("Options given to the picture encoder, as key=value words separated by spaces (what FFmpeg's -preset, -tune, -x265-params ... say). "
                                   "The x264-params / x265-params words are added to the ones Reno sets. Options the encoder does not have are ignored.");
            form->addRow("Encoder options:", ffOptions_);
        }
        tabs_->addTab(page, "Video");
        QObject::connect(size_, &QComboBox::activated, this, [this](int i) {
            if (i > 0 && i < (int)(sizeof(kSizes) / sizeof(kSizes[0]))) {
                width_->setValue(kSizes[i].a);
                height_->setValue(kSizes[i].b);
            }
            syncSize();
        });
        for (QSpinBox* b : {width_, height_})
            QObject::connect(b, &QSpinBox::valueChanged, this, [this](int) {
                if (size_->currentIndex() == 0) return;
                int found = (int)(sizeof(kSizes) / sizeof(kSizes[0]));
                for (int k = 1; k < (int)(sizeof(kSizes) / sizeof(kSizes[0])); ++k)
                    if (kSizes[k].a == width_->value() && kSizes[k].b == height_->value()) found = k;
                QSignalBlocker block(size_);
                size_->setCurrentIndex(found);
            });
    }

    void buildAudio(const Format& f)
    {
        auto* page = new QWidget(tabs_);
        auto* form = new QFormLayout(page);
        includeAudio_ = new QCheckBox("Include audio", page);
        includeAudio_->setObjectName("includeAudio");
        form->addRow(includeAudio_);
        sampleRate_ = combo(page, "sampleRate", {{"(Use project settings)", 0}, {"22,050", 22050}, {"32,000", 32000}, {"44,100", 44100}, {"48,000", 48000}, {"96,000", 96000}});
        form->addRow("Sample rate (Hz):", sampleRate_);
        if (f.audioRate) {
            audioKbps_ = combo(page, "audioKbps", {{"64,000", 64}, {"96,000", 96}, {"128,000", 128}, {"160,000", 160}, {"192,000", 192}, {"256,000", 256}, {"320,000", 320}, {"350,000", 350}, {"384,000", 384},
                                                     {"448,000", 448}, {"512,000", 512}, {"640,000", 640}});
            form->addRow("Bit rate (bps):", audioKbps_);
        } else {
            bits_ = combo(page, "bitDepth", {{"16", 16}, {"24", 24}});
            if (f.audioFamily == "pcm" && f.key != "mxf" && f.key != "avi") bits_->addItem("32 (IEEE float)", 32);
            form->addRow("Bit depth:", bits_);
        }
        channels_ = combo(page, "channels", {{"Stereo", 2}, {"Mono", 1}});
        // 5.1 (L R C LFE Ls Rs) where the kind of sound can hold it: a 5.1 project's six channels (a stereo project's
        // left and right with the rest silent); Stereo folds a 5.1 project down
        if (f.audioFamily == "aac" || f.audioFamily == "pcm" || f.audioFamily == "flac" || f.audioFamily == "ac3" || f.audioFamily == "vorbis" ||
            f.audioFamily == "opus")
            channels_->addItem("5.1 Surround", 6);
        form->addRow("Channels:", channels_);
        tabs_->addTab(page, "Audio");
    }

    void buildProject()
    {
        auto* page = new QWidget(tabs_);
        auto* form = new QFormLayout(page);
        renderQuality_ = combo(page, "renderQuality", {{"Use project settings", ""}, {"Draft", "draft"}, {"Preview", "preview"}, {"Good", "good"}, {"Best", "best"}});
        form->addRow("Video rendering quality:", renderQuality_);
        motionBlur_ = combo(page, "motionBlur", {{"Use project settings", ""}, {"Gaussian", "gaussian"}, {"Pyramid", "pyramid"}, {"Box", "box"},
                                                  {"Asymmetric Gaussian", "asymgaussian"}, {"Asymmetric Pyramid", "asympyramid"}, {"Asymmetric Box", "asymbox"}});
        form->addRow("Motion blur type:", motionBlur_);
        // VEGAS's stereoscopic 3D settings: Reno has no stereoscopic 3D projects, so they stay as the project has them
        auto* stereo = combo(page, "stereoMode", {{"Use project settings", ""}});
        stereo->setEnabled(false);
        stereo->setToolTip("Reno has no stereoscopic 3D projects");
        form->addRow("Stereoscopic 3D mode:", stereo);
        auto* swap = new QCheckBox("Swap Left/Right", page);
        swap->setObjectName("swapLeftRight");
        swap->setEnabled(false);
        form->addRow(QString(), swap);
        auto* crosstalk = new QSlider(Qt::Horizontal, page);
        crosstalk->setObjectName("crosstalk");
        crosstalk->setEnabled(false);
        form->addRow("Crosstalk cancellation:", crosstalk);
        colorSpace_ = new QComboBox(page);
        colorSpace_->setObjectName("colorSpace");
        for (const ColorSpaceChoice& c : renderColorSpaces()) colorSpace_->addItem(QString::fromStdString(c.label), QString::fromStdString(c.id));
        colorSpace_->setToolTip("Used only when Project Properties' pixel format is 32-bit floating point (full range): Default makes the file through the "
                                "project's View transform");
        form->addRow("Color space:", colorSpace_);
        auto* note = new QLabel("These take the place of Project Properties' Full-resolution rendering quality and Motion blur type for this render.", page);
        note->setWordWrap(true);
        form->addRow(note);
        tabs_->addTab(page, "Project");
    }

    void load(const Tmpl& t)
    {
        base_ = t;
        const Format& f = formats_[fi_];
        description_->setText(t.description);
        if (f.hasVideo) {
            includeVideo_->setChecked(t.video);
            {
                QSignalBlocker b1(width_), b2(height_);
                const ProjectSettings& ps = AppState::get().project().settings;
                width_->setValue(t.width > 0 ? t.width : ps.width);
                height_->setValue(t.height > 0 ? t.height : ps.height);
            }
            int si = (int)(sizeof(kSizes) / sizeof(kSizes[0]));
            if (t.width == 0) si = 0;
            for (int k = 1; k < (int)(sizeof(kSizes) / sizeof(kSizes[0])); ++k)
                if (kSizes[k].a == t.width && kSizes[k].b == t.height) si = k;
            size_->setCurrentIndex(si);
            int ri = 0;
            for (int k = 1; k < (int)(sizeof(kRates) / sizeof(kRates[0])); ++k)
                if (t.fpsNum > 0 && (long long)kRates[k].a * t.fpsDen == (long long)t.fpsNum * kRates[k].b) ri = k;
            rate_->setCurrentIndex(ri);
            pick(field_, t.field);
            int pi = 0;
            for (int k = 0; k < par_->count(); ++k)
                if (std::fabs(par_->itemData(k).toDouble() - t.par) < 5e-4) pi = k;
            par_->setCurrentIndex(pi);
            if (profile_) pick(profile_, t.profile);
            if (level_) pick(level_, t.level);
            if (cbr_) {
                cbr_->setChecked(t.rc == "cbr");
                vbr_->setChecked(t.rc == "vbr");
                quality_->setChecked(t.rc == "quality");
                cbrKbps_->setValue(t.kbps);
                avgKbps_->setValue(t.kbps);
                maxKbps_->setValue(t.maxKbps);
                qualityValue_->setValue(t.quality);
                twoPass_->setChecked(t.twoPass);
                syncRate();
            }
            if (mode_) pick(mode_, t.mode);
            key_->setValue(t.keySeconds);
            if (alpha_) alpha_->setChecked(t.alpha);
            if (ffOptions_) ffOptions_->setText(t.ffOptions);
            syncSize();
        }
        if (f.hasAudio) {
            includeAudio_->setChecked(t.audio);
            pick(sampleRate_, t.sampleRate);
            if (audioKbps_) {
                if (audioKbps_->findData(t.audioKbps) < 0) audioKbps_->addItem(thousands(t.audioKbps * 1000), t.audioKbps);
                pick(audioKbps_, t.audioKbps);
            }
            if (bits_) pick(bits_, t.bits);
            pick(channels_, t.channels);
        }
        pick(renderQuality_, t.renderQuality);
        pick(motionBlur_, t.motionBlur);
        if (colorSpace_) {
            if (colorSpace_->findData(t.colorSpace) < 0) colorSpace_->addItem(QString::fromStdString(colorSpaceLabel(t.colorSpace.toStdString())), t.colorSpace);
            pick(colorSpace_, t.colorSpace);
        }
    }

    void syncSize()
    {
        const bool custom = size_->currentIndex() != 0;
        width_->setEnabled(custom);
        height_->setEnabled(custom);
    }
    void syncRate()
    {
        cbrKbps_->setEnabled(cbr_->isChecked());
        for (QWidget* w : std::initializer_list<QWidget*>{twoPass_, maxKbps_, avgKbps_}) w->setEnabled(vbr_->isChecked());
        qualityValue_->setEnabled(quality_->isChecked());
    }

    void saveTemplate()
    {
        Format& f = formats_[fi_];
        Tmpl t = result();
        if (t.name.isEmpty() || t.name == "(Untitled)") {
            QMessageBox::information(this, "Save Template", "Type a name for the template first.");
            return;
        }
        for (Tmpl& o : f.templates)
            if (o.name == t.name) {
                if (!o.custom) {
                    QMessageBox::information(this, "Save Template", QString("\"%1\" is one of the built-in templates. Choose another name.").arg(t.name));
                    return;
                }
                t.custom = true;
                o = t;
                saveCustomTemplates(formats_);
                saved_ = t.name;
                return;
            }
        t.custom = true;
        f.templates.insert(f.templates.begin(), t);
        name_->insertItem(0, t.name);
        name_->setCurrentIndex(0);
        saveCustomTemplates(formats_);
        saved_ = t.name;
    }
    void deleteTemplate()
    {
        Format& f = formats_[fi_];
        const QString n = name_->currentText().trimmed();
        for (size_t k = 0; k < f.templates.size(); ++k)
            if (f.templates[k].name == n) {
                if (!f.templates[k].custom) {
                    QMessageBox::information(this, "Delete Template", "Built-in templates cannot be deleted.");
                    return;
                }
                f.templates.erase(f.templates.begin() + (std::ptrdiff_t)k);
                name_->removeItem((int)k);
                saveCustomTemplates(formats_);
                if (!f.templates.empty()) load(f.templates.front());
                return;
            }
    }

public:
    QString saved_;

private:
    std::vector<Format>& formats_;
    int fi_;
    Tmpl base_;
    QComboBox* name_ = nullptr;
    QLineEdit* description_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QCheckBox *includeVideo_ = nullptr, *includeAudio_ = nullptr, *twoPass_ = nullptr, *alpha_ = nullptr;
    QLineEdit* ffOptions_ = nullptr;
    QComboBox *size_ = nullptr, *profile_ = nullptr, *level_ = nullptr, *rate_ = nullptr, *field_ = nullptr, *par_ = nullptr, *mode_ = nullptr;
    QSpinBox *width_ = nullptr, *height_ = nullptr, *cbrKbps_ = nullptr, *maxKbps_ = nullptr, *avgKbps_ = nullptr, *qualityValue_ = nullptr;
    QDoubleSpinBox* key_ = nullptr;
    QRadioButton *cbr_ = nullptr, *vbr_ = nullptr, *quality_ = nullptr;
    QComboBox *sampleRate_ = nullptr, *audioKbps_ = nullptr, *bits_ = nullptr, *channels_ = nullptr;
    QComboBox *renderQuality_ = nullptr, *motionBlur_ = nullptr;
    QComboBox* colorSpace_ = nullptr;
};

// What a render thread and its dialog share. The thread only touches this; the dialog reads it on the interface thread.
struct RenderJob {
    std::atomic<bool> alive{true};
    std::atomic<bool> cancel{false};
};

const int kRoleFormat = Qt::UserRole;
const int kRoleTemplate = Qt::UserRole + 1;

class Section : public QWidget {
public:
    Section(QWidget* parent, const QString& title, const char* name, QWidget* body, bool open) : QWidget(parent), body_(body)
    {
        setObjectName(name);
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(2);
        toggle_ = new QToolButton(this);
        toggle_->setObjectName(QString(name) + "Toggle");
        toggle_->setText(title);
        open_ = open;
        toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toggle_->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        toggle_->setAutoRaise(true);
        QFont f = toggle_->font();
        f.setBold(true);
        toggle_->setFont(f);
        v->addWidget(toggle_);
        body_->setParent(this);
        v->addWidget(body_);
        body_->setVisible(open);
        QObject::connect(toggle_, &QToolButton::clicked, this, [this] {
            open_ = !open_;
            toggle_->setArrowType(open_ ? Qt::DownArrow : Qt::RightArrow);
            body_->setVisible(open_);
        });
    }

private:
    bool open_ = true;
    QToolButton* toggle_;
    QWidget* body_;
};

// VEGAS's Channel Mapping dialog (Render As > Enable multichannel mapping > Channels...): which outputs go into the file
// and in which order: the master (a 5.1 project's six channels each alone) and every bus, each with its (Mono downmix).
bool editChannelMapping(QWidget* parent, std::vector<RenderSpec::ChannelSource>& map)
{
    const Project& p = AppState::get().project();
    struct Item { RenderSpec::ChannelSource src; QString name; bool on = false; };
    std::vector<Item> items;
    if (p.settings.surround()) {
        const char* names[6] = {"Surround Master: Left", "Surround Master: Right", "Surround Master: Center", "Surround Master: LFE",
                                "Surround Master: Left Surround", "Surround Master: Right Surround"};
        for (int c = 0; c < 6; ++c) {
            Item it;
            it.src.channel = c;
            it.name = names[c];
            items.push_back(it);
        }
    } else {
        Item it;
        it.name = "Master";
        items.push_back(it);
    }
    for (const Bus& b : p.buses) {
        Item it;
        it.src.bus = (int64_t)b.id;
        it.name = QString::fromStdString(b.name.empty() ? std::string("Bus") : b.name);
        items.push_back(it);
    }
    std::vector<Item> ordered;
    for (const RenderSpec::ChannelSource& m : map)
        for (Item& it : items)
            if (!it.on && it.src.bus == m.bus && it.src.channel == m.channel) {
                it.on = true;
                it.src.mono = m.mono;
                ordered.push_back(it);
            }
    for (const Item& it : items)
        if (!it.on) ordered.push_back(it);
    QDialog dlg(parent);
    dlg.setObjectName("channelMappingDialog");
    dlg.setWindowTitle("Channel Mapping");
    auto* v = new QVBoxLayout(&dlg);
    v->addWidget(new QLabel("Tick the outputs to render, top to bottom: a stereo output takes two channels of the file, a mono downmix one.", &dlg));
    auto* h = new QHBoxLayout();
    auto* list = new QTreeWidget(&dlg);
    list->setObjectName("channelList");
    list->setColumnCount(2);
    list->setHeaderLabels({"Output", "(Mono downmix)"});
    list->setRootIsDecorated(false);
    for (const Item& it : ordered) {
        auto* row = new QTreeWidgetItem(list, QStringList() << it.name << QString());
        row->setCheckState(0, it.on ? Qt::Checked : Qt::Unchecked);
        if (it.src.channel < 0) row->setCheckState(1, it.src.mono ? Qt::Checked : Qt::Unchecked);
        row->setData(0, Qt::UserRole, QVariant::fromValue((qlonglong)it.src.bus));
        row->setData(1, Qt::UserRole, it.src.channel);
    }
    list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    h->addWidget(list, 1);
    auto* side = new QVBoxLayout();
    auto* up = new QPushButton("Move Up", &dlg);
    up->setObjectName("moveUp");
    auto* down = new QPushButton("Move Down", &dlg);
    down->setObjectName("moveDown");
    side->addWidget(up);
    side->addWidget(down);
    side->addStretch(1);
    h->addLayout(side);
    v->addLayout(h, 1);
    auto move = [list](int by) {
        QTreeWidgetItem* it = list->currentItem();
        if (!it) return;
        const int at = list->indexOfTopLevelItem(it), to = at + by;
        if (to < 0 || to >= list->topLevelItemCount()) return;
        list->takeTopLevelItem(at);
        list->insertTopLevelItem(to, it);
        list->setCurrentItem(it);
    };
    QObject::connect(up, &QPushButton::clicked, &dlg, [move] { move(-1); });
    QObject::connect(down, &QPushButton::clicked, &dlg, [move] { move(1); });
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    v->addWidget(bb);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    dlg.resize(460, 320);
    if (dlg.exec() != QDialog::Accepted) return false;
    map.clear();
    for (int r = 0; r < list->topLevelItemCount(); ++r) {
        QTreeWidgetItem* it = list->topLevelItem(r);
        if (it->checkState(0) != Qt::Checked) continue;
        RenderSpec::ChannelSource c;
        c.bus = it->data(0, Qt::UserRole).toLongLong();
        c.channel = it->data(1, Qt::UserRole).toInt();
        c.mono = c.channel < 0 && it->checkState(1) == Qt::Checked;
        map.push_back(c);
    }
    return true;
}

class RenderAsDialog : public QDialog {
public:
    enum Mode { RenderAs, NewTrack };

    RenderAsDialog(QWidget* parent, Mode mode, double from = 0, double to = 0, const QString& suggested = QString())
        : QDialog(parent), mode_(mode), from_(from), to_(to)
    {
        setObjectName("renderDialog");
        setWindowTitle(mode == NewTrack ? "Render to New Track" : "Render As");
        setMinimumSize(660, 600);
        resize(760, 720);
        const AppState& app = AppState::get();
        QSettings settings;
        hardwareQueryHook() = [](bool busy) {
            if (busy) QApplication::setOverrideCursor(Qt::WaitCursor);
            else QApplication::restoreOverrideCursor();
        };
        formats_ = loadFormats();

        stack_ = new QStackedWidget(this);
        auto* outer = new QVBoxLayout(this);
        outer->addWidget(stack_);

        // page 1: the choices, laid out as in VEGAS 18's Render As
        auto* setup = new QWidget(stack_);
        auto* sv = new QGridLayout(setup);
        sv->setHorizontalSpacing(12);
        sv->setVerticalSpacing(6);
        sv->setColumnStretch(0, 1);
        sv->setColumnStretch(1, 1);

        search_ = new QLineEdit(setup);
        search_->setObjectName("templateSearch");
        search_->setPlaceholderText("Search render templates");
        search_->setClearButtonEnabled(true);
        search_->addAction(magnifierIcon(search_), QLineEdit::TrailingPosition);
        filter_ = new QToolButton(setup);
        filter_->setObjectName("templateFilter");
        filter_->setText("Filters Off");
        filter_->setPopupMode(QToolButton::InstantPopup);
        filter_->setToolButtonStyle(Qt::ToolButtonTextOnly);
        auto* filters = new QMenu(filter_);
        favoritesOnly_ = filters->addAction("Show favorites only");
        matchProject_ = filters->addAction("Match project settings");
        filters->addSeparator();
        videoOnly_ = filters->addAction("Video");
        audioOnly_ = filters->addAction("Audio only");
        for (QAction* a : {favoritesOnly_, matchProject_, videoOnly_, audioOnly_}) a->setCheckable(true);
        favoritesOnly_->setObjectName("filterFavorites");
        matchProject_->setObjectName("filterMatchProject");
        favoritesOnly_->setChecked(settings.value("render/filterFavorites", false).toBool());
        matchProject_->setChecked(settings.value("render/filterMatch", false).toBool());
        {
            const struct { const char* key; const char* text; } vid[] = {
                {"resolution", "Resolution"}, {"framerate", "Frame rate"}, {"ratio", "Ratio"}, {"pixelratio", "Pixel ratio"},
                {"fieldorder", "Field order"}, {"colordepth", "Color depth"}, {"hdr", "HDR"}, {"colorspace", "Color space"}};
            const struct { const char* key; const char* text; } aud[] = {
                {"samplerate", "Sample rate"}, {"channels", "Channel count"}, {"bitdepth", "Bit depth"}};
            filters->addSeparator();
            QMenu* mv = filters->addMenu("Match project video settings");
            mv->setObjectName("matchVideoMenu");
            for (const auto& m : vid) {
                QAction* a = mv->addAction(m.text);
                a->setCheckable(true);
                a->setObjectName(QString("match_") + m.key);
                a->setChecked(settings.value(QString("render/match_") + m.key, false).toBool());
                matchChecks_[m.key] = a;
            }
            QMenu* ma = filters->addMenu("Match project audio settings");
            ma->setObjectName("matchAudioMenu");
            for (const auto& m : aud) {
                QAction* a = ma->addAction(m.text);
                a->setCheckable(true);
                a->setObjectName(QString("match_") + m.key);
                a->setChecked(settings.value(QString("render/match_") + m.key, false).toBool());
                matchChecks_[m.key] = a;
            }
            filters->addSeparator();
            showAll_ = filters->addAction("Show all formats");
            showAll_->setCheckable(true);
            showAll_->setObjectName("showAllFormats");
            showAll_->setChecked(settings.value("render/showAll", false).toBool());
            showAll_->setToolTip("Every format and template, whatever the Match filters say");
        }
        filter_->setMenu(filters);
        auto* help = new QToolButton(setup);
        help->setObjectName("renderHelp");
        help->setText("?");
        help->setToolTip("About this dialog");
        auto* filterRow = new QHBoxLayout();
        filterRow->addWidget(filter_);
        filterRow->addStretch(1);
        filterRow->addWidget(help);
        sv->addWidget(search_, 0, 0);
        sv->addLayout(filterRow, 0, 1);

        auto* formatsLabel = new QLabel("Formats", setup);
        formatsLabel->setObjectName("formatsLabel");
        auto* templatesLabel = new QLabel("Templates", setup);
        templatesLabel->setObjectName("templatesLabel");
        sv->addWidget(formatsLabel, 1, 0);
        sv->addWidget(templatesLabel, 1, 1);
        formatList_ = new QListWidget(setup);
        formatList_->setObjectName("formats");
        formatList_->setMinimumHeight(150);
        formatList_->setUniformItemSizes(true);
        tree_ = new QTreeWidget(setup);
        tree_->setObjectName("templates");
        tree_->setColumnCount(2);
        tree_->setHeaderHidden(true);
        tree_->setRootIsDecorated(false);
        tree_->header()->setStretchLastSection(false);
        tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        tree_->header()->setSectionResizeMode(1, QHeaderView::Fixed);
        tree_->setColumnWidth(1, 28);
        tree_->header()->moveSection(1, 0);
        tree_->setMinimumHeight(150);
        sv->addWidget(formatList_, 2, 0);
        sv->addWidget(tree_, 2, 1);
        sv->setRowStretch(2, 1);

        auto* infoLabel = new QLabel("Template Info:", setup);
        infoLabel->setObjectName("templateInfoLabel");
        infoLabel->setAlignment(Qt::AlignRight | Qt::AlignTop);
        info_ = new QLabel(setup);
        info_->setObjectName("templateInfo");
        info_->setWordWrap(true);
        info_->setFrameShape(QFrame::StyledPanel);
        info_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        info_->setMinimumHeight(64);
        info_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        sv->addWidget(infoLabel, 3, 0);
        sv->addWidget(info_, 3, 1);

        renderOptions_ = new QToolButton(setup);
        renderOptions_->setObjectName("renderOptions");
        renderOptions_->setText("Render Options");
        renderOptions_->setPopupMode(QToolButton::InstantPopup);
        renderOptions_->setToolButtonStyle(Qt::ToolButtonTextOnly);
        auto* options = new QMenu(renderOptions_);
        options->setObjectName("renderOptionsMenu");
        auto check = [&](const char* text, const char* name, const char* key, bool def) {
            QAction* a = options->addAction(text);
            a->setObjectName(name);
            a->setCheckable(true);
            a->setChecked(settings.value(QString("render/") + key, def).toBool());
            return a;
        };
        regionOnly_ = check("Render loop region only", "regionOnly", "regionOnly", false);
        stretch_ = check("Stretch video to fill output frame size (do not letterbox)", "stretch", "stretch", false);
        rotation_ = check("Use project output rotation setting", "useRotation", "useRotation", true);
        markers_ = check("Save project markers in media file", "saveMarkers", "saveMarkers", false);
        markers_->setToolTip("Markers and regions become the file's chapters (MP4, MOV, MKV, WMV)");
        projectPath_ = check("Save project as path reference in rendered file", "saveProjectPath", "projectPath", false);
        projectPath_->setEnabled(!app.path().empty());
        loudness_ = check("Save loudness log file next to media file", "loudnessLog", "loudnessLog", false);
        loudness_->setToolTip("Measures the rendered sound (ITU-R BS.1770 / EBU R 128, or ATSC A/85 as Options > Loudness Meters says) into <name>_loud.txt beside the file");
        multichannel_ = options->addAction("Enable multichannel mapping");
        multichannel_->setObjectName("multichannelMapping");
        multichannel_->setCheckable(true);
        multichannel_->setToolTip("Render the master's and the buses' outputs to channels of their own in one file (Channels... chooses them)");
        channelsAction_ = options->addAction("Channels...");
        channelsAction_->setObjectName("channelsButton");
        options->addSeparator();
        swap_ = check("Swap video files on render", "swapFiles", "swapFiles", false);
        swap_->setToolTip("Renders to a temporary file and, when it is finished, puts it in place of the old file, so a failed or stopped render leaves the old file alone");
        summary_ = check("Save the project's summary (Project Properties > Summary) in the file", "saveSummary", "summary", true);
        for (const QString& e : settings.value("render/channelMap").toStringList()) {
            const QStringList f = e.split(',');
            if (f.size() != 3) continue;
            RenderSpec::ChannelSource c;
            c.bus = f[0].toLongLong();
            c.channel = f[1].toInt();
            c.mono = f[2] == "1";
            channelMap_.push_back(c);
        }
        QObject::connect(channelsAction_, &QAction::triggered, this, [this] {
            if (editChannelMapping(this, channelMap_)) {
                QStringList l;
                for (const auto& c : channelMap_) l << QString("%1,%2,%3").arg((qlonglong)c.bus).arg(c.channel).arg(c.mono ? 1 : 0);
                QSettings().setValue("render/channelMap", l);
                multichannel_->setChecked(!channelMap_.empty());
            }
        });
        QObject::connect(multichannel_, &QAction::toggled, this, [this](bool on) {
            if (on && channelMap_.empty()) QTimer::singleShot(0, this, [this] {
                if (!editChannelMapping(this, channelMap_) || channelMap_.empty()) multichannel_->setChecked(false);
                else {
                    QStringList l;
                    for (const auto& c : channelMap_) l << QString("%1,%2,%3").arg((qlonglong)c.bus).arg(c.channel).arg(c.mono ? 1 : 0);
                    QSettings().setValue("render/channelMap", l);
                }
            });
        });
        const bool region = app.loopEnd > app.loopStart;
        if (mode == NewTrack) {
            regionOnly_->setChecked(true);
            regionOnly_->setEnabled(false);
            regionOnly_->setText("Render the time selection (or the whole project) only");
        } else if (!region) {
            regionOnly_->setChecked(false);
            regionOnly_->setEnabled(false);
            regionOnly_->setToolTip("No loop region is set");
        } else {
            regionOnly_->setToolTip(QString("%1 to %2").arg(qs(timecode(app.loopStart, app.project().settings.fps())), qs(timecode(app.loopEnd, app.project().settings.fps()))));
        }
        renderOptions_->setMenu(options);
        customize_ = new QPushButton("Customize Template...", setup);
        customize_->setObjectName("customizeTemplate");
        auto* about = new QPushButton("About...", setup);
        about->setObjectName("aboutFormat");
        auto* templateButtons = new QHBoxLayout();
        templateButtons->addWidget(customize_);
        templateButtons->addStretch(1);
        templateButtons->addWidget(about);
        sv->addWidget(renderOptions_, 4, 0, Qt::AlignLeft);
        sv->addLayout(templateButtons, 4, 1);

        auto* fileRow = new QGridLayout();
        fileRow->setHorizontalSpacing(8);
        folder_ = new QComboBox(setup);
        folder_->setObjectName("outputFolder");
        folder_->setEditable(true);
        folder_->setInsertPolicy(QComboBox::NoInsert);
        for (const QString& d : settings.value("render/recentFolders").toStringList())
            if (!d.isEmpty()) folder_->addItem(QDir::toNativeSeparators(d));
        browse_ = new QPushButton("Browse...", setup);
        browse_->setObjectName("browse");
        auto* projectFolder = new QPushButton("Project Location", setup);
        projectFolder->setObjectName("projectFolder");
        projectFolder->setToolTip("Put the file in the folder the project is saved in");
        projectFolder->setEnabled(!app.path().empty());
        name_ = new QLineEdit(setup);
        name_->setObjectName("outputName");
        fileRow->addWidget(new QLabel("Folder:", setup), 0, 0);
        fileRow->addWidget(folder_, 0, 1);
        fileRow->addWidget(browse_, 0, 2);
        fileRow->addWidget(projectFolder, 0, 3);
        fileRow->addWidget(new QLabel("Name:", setup), 1, 0);
        fileRow->addWidget(name_, 1, 1, 1, 3);
        // Render loop region only, on the dialog as in VEGAS (kept in step with the same switch in Render Options)
        regionBox_ = new QCheckBox(regionOnly_->text(), setup);
        regionBox_->setObjectName("regionOnlyBox");
        regionBox_->setChecked(regionOnly_->isChecked());
        regionBox_->setEnabled(regionOnly_->isEnabled());
        regionBox_->setToolTip(regionOnly_->toolTip());
        if (mode != NewTrack && region)
            regionBox_->setText(QString("Render loop region only (%1 to %2)")
                                    .arg(qs(timecode(app.loopStart, app.project().settings.fps())), qs(timecode(app.loopEnd, app.project().settings.fps()))));
        QObject::connect(regionBox_, &QCheckBox::toggled, regionOnly_, [this](bool on) { if (regionOnly_->isChecked() != on) regionOnly_->setChecked(on); });
        QObject::connect(regionOnly_, &QAction::toggled, regionBox_, [this](bool on) { if (regionBox_->isChecked() != on) regionBox_->setChecked(on); });
        fileRow->addWidget(regionBox_, 2, 1, 1, 3);
        fileRow->setColumnStretch(1, 1);
        sv->addLayout(fileRow, 5, 0, 1, 2);

        space_ = new QLabel(setup);
        space_->setObjectName("freeSpace");
        estimate_ = new QLabel(setup);
        estimate_->setObjectName("estimate");
        estimate_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sv->addWidget(space_, 6, 0);
        sv->addWidget(estimate_, 6, 1);
        renderButton_ = new QPushButton("Render", setup);
        renderButton_->setObjectName("renderButton");
        renderButton_->setDefault(true);
        auto* cancelSetup = new QPushButton("Cancel", setup);
        cancelSetup->setObjectName("cancelSetup");
        auto* buttons = new QHBoxLayout();
        buttons->addStretch(1);
        buttons->addWidget(renderButton_);
        buttons->addWidget(cancelSetup);
        sv->addLayout(buttons, 7, 0, 1, 2);
        stack_->addWidget(setup);
        QObject::connect(help, &QToolButton::clicked, this, [this] {
            QMessageBox::information(this, "Render As",
                                     "Choose a format and a template, a folder and a name, and click Render.\n\n"
                                     "Render Options changes how the project is rendered. Customize Template... changes the template's settings. "
                                     "Project Location puts the file beside the project file. The star makes a template a favourite; "
                                     "the Filters menu shows only the templates you want.");
        });

        // page 2: the rendering progress, as in VEGAS 18 ("Rendering: N%" in the title)
        auto* prog = new QWidget(stack_);
        auto* pv = new QVBoxLayout(prog);
        pv->setSpacing(6);
        pathLabel_ = new ElidedLabel(prog);
        pathLabel_->setObjectName("progressPath");
        bar_ = new QProgressBar(prog);
        bar_->setObjectName("progressBar");
        bar_->setRange(0, 1000);
        bar_->setTextVisible(false);
        bar_->setFixedHeight(6);
        auto makeLabel = [prog](const char* name, Qt::Alignment al) {
            auto* l = new QLabel(prog);
            l->setObjectName(name);
            l->setAlignment(al | Qt::AlignVCenter);
            return l;
        };
        percentOf_ = makeLabel("percentOf", Qt::AlignLeft);
        remaining_ = makeLabel("remainingTime", Qt::AlignRight);
        renderedSize_ = makeLabel("renderedSize", Qt::AlignLeft);
        elapsedLabel_ = makeLabel("elapsedTime", Qt::AlignRight);
        auto* counts = new QGridLayout();
        counts->addWidget(percentOf_, 0, 0);
        counts->addWidget(remaining_, 0, 1);
        counts->addWidget(renderedSize_, 1, 0);
        counts->addWidget(elapsedLabel_, 1, 1);
        result_ = new QLabel(prog);
        result_->setObjectName("resultLabel");
        result_->setWordWrap(true);
        result_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        closeWhenDone_ = new QCheckBox("Close dialog when rendering is complete", prog);
        closeWhenDone_->setObjectName("closeWhenDone");
        closeWhenDone_->setChecked(settings.value("render/closeWhenDone", false).toBool());
        closeWhenDone_->setVisible(mode == RenderAs);
        auto fold = [prog](const QString& title, const char* name, QWidget* body, bool open) {
            auto* box = new QWidget(prog);
            auto* bv = new QVBoxLayout(box);
            bv->setContentsMargins(0, 0, 0, 0);
            bv->setSpacing(2);
            auto* head = new QToolButton(box);
            head->setObjectName(name);
            head->setText(title);
            head->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            head->setAutoRaise(true);
            head->setCheckable(true);
            head->setChecked(open);
            head->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
            QFont hf = head->font();
            hf.setBold(true);
            head->setFont(hf);
            body->setParent(box);
            body->setVisible(open);
            bv->addWidget(head);
            bv->addWidget(body);
            QObject::connect(head, &QToolButton::toggled, box, [head, body](bool on) {
                head->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
                body->setVisible(on);
            });
            return box;
        };
        struct PropRow { const char* label; const char* name; QLabel** value; };
        auto rows = [prog](const std::vector<PropRow>& list) {
            auto* body = new QWidget(prog);
            auto* g = new QGridLayout(body);
            g->setContentsMargins(18, 0, 0, 0);
            g->setVerticalSpacing(2);
            int r = 0;
            for (const PropRow& e : list) {
                auto* value = new QLabel(body);
                value->setObjectName(e.name);
                value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                *e.value = value;
                g->addWidget(new QLabel(e.label, body), r, 0);
                g->addWidget(value, r, 1);
                ++r;
            }
            g->setColumnStretch(0, 1);
            return body;
        };
        QWidget* estBody = rows({{"Free Disk Space:", "freeDiskSpace", &estFree_}, {"Rendering Duration:", "renderingDuration", &estDuration_},
                                 {"Time of Completion:", "timeOfCompletion", &estCompletion_}});
        QWidget* perfBody = rows({{"Average Speed:", "averageSpeed", &perfAverage_}, {"Current Speed:", "currentSpeed", &perfCurrent_},
                                  {"CPU Usage:", "cpuUsage", &perfCpu_}, {"Memory Usage:", "memoryUsage", &perfMemory_}});
        auto* optBox = new QWidget(prog);
        auto* ob = new QVBoxLayout(optBox);
        ob->setContentsMargins(18, 0, 0, 0);
        ob->setSpacing(2);
        openFolderWhenDone_ = new QCheckBox("Open folder when rendering is complete", optBox);
        openFolderWhenDone_->setObjectName("openFolderWhenDone");
        openFolderWhenDone_->setChecked(settings.value("render/openFolderWhenDone", true).toBool());
        ob->addWidget(openFolderWhenDone_);
        ob->addWidget(closeWhenDone_);
        shutdown_ = new QCheckBox("Shut down computer after render completes", optBox);
        shutdown_->setObjectName("shutdownWhenDone");
        ob->addWidget(shutdown_);
        pv->addWidget(pathLabel_);
        pv->addWidget(bar_);
        pv->addLayout(counts);
        pv->addSpacing(6);
        pv->addWidget(fold("Estimated Properties", "estimatedFold", estBody, true));
        pv->addWidget(fold("Performance", "performanceFold", perfBody, true));
        optionsFold_ = fold("Options", "optionsFold", optBox, true);
        pv->addWidget(optionsFold_);
        optionsFold_->setVisible(mode == RenderAs);
        pv->addWidget(result_);
        pv->addStretch(1);
        auto* pb = new QHBoxLayout();
        openFolder_ = new QPushButton("Open Folder", prog);
        openFolder_->setObjectName("openFolder");
        openFile_ = new QPushButton("Open", prog);
        openFile_->setObjectName("openFile");
        cancelRender_ = new QPushButton("Cancel", prog);
        cancelRender_->setObjectName("cancelRender");
        closeButton_ = new QPushButton("Close", prog);
        closeButton_->setObjectName("closeRender");
        pb->addWidget(openFolder_);
        pb->addWidget(openFile_);
        pb->addWidget(cancelRender_);
        pb->addWidget(closeButton_);
        pv->addLayout(pb);
        stack_->addWidget(prog);

        QString folder = settings.value("render/folder", QDir::homePath()).toString();
        if (!app.path().empty()) folder = QFileInfo(qs(app.path())).absolutePath();
        QString stem = app.path().empty() ? QString("Untitled") : QFileInfo(qs(app.path())).completeBaseName();
        if (!suggested.isEmpty()) {
            folder = QFileInfo(suggested).absolutePath();
            stem = QFileInfo(suggested).completeBaseName();
        }
        folder_->setEditText(QDir::toNativeSeparators(folder));
        name_->setText(stem);
        QObject::connect(formatList_, &QListWidget::currentItemChanged, [this](QListWidgetItem*, QListWidgetItem*) { fillTemplates(); });
        fillFormats();
        selectTemplate(settings.value(mode == NewTrack ? "render/newTrackTemplate" : "render/template", "avc/Internet HD 1080p 29.97 fps").toString());
        if (!current()) selectTemplate("avc/Internet HD 1080p 29.97 fps");
        templateChanged();

        clock_ = new QTimer(this);
        clock_->setInterval(250);
        QObject::connect(clock_, &QTimer::timeout, [this] { showTimes(); });
        QObject::connect(browse_, &QPushButton::clicked, [this] { browse(); });
        QObject::connect(projectFolder, &QPushButton::clicked, [this] {
            folder_->setEditText(QDir::toNativeSeparators(QFileInfo(qs(AppState::get().path())).absolutePath()));
            showSpace();
        });
        QObject::connect(folder_, &QComboBox::editTextChanged, [this](const QString&) { showSpace(); });
        QObject::connect(name_, &QLineEdit::editingFinished, [this] { nameEdited(); });
        QObject::connect(search_, &QLineEdit::textChanged, [this](const QString&) { fillFormats(); });
        for (QAction* a : {favoritesOnly_, matchProject_, videoOnly_, audioOnly_, showAll_})
            QObject::connect(a, &QAction::toggled, [this](bool) { fillFormats(); });
        for (auto& m : matchChecks_) QObject::connect(m.second, &QAction::toggled, [this](bool) { fillFormats(); });
        QObject::connect(tree_, &QTreeWidget::currentItemChanged, [this](QTreeWidgetItem*, QTreeWidgetItem*) { templateChanged(); });
        QObject::connect(tree_, &QTreeWidget::itemClicked, [this](QTreeWidgetItem* it, int column) {
            if (column == 1 && it && it->data(0, kRoleTemplate).toInt() >= 0) toggleFavorite(it);
        });
        QObject::connect(tree_, &QTreeWidget::itemDoubleClicked, [this](QTreeWidgetItem* it, int column) {
            if (column == 0 && it && it->data(0, kRoleTemplate).toInt() >= 0) start();
        });
        QObject::connect(customize_, &QPushButton::clicked, [this] { customizeTemplate(); });
        QObject::connect(about, &QPushButton::clicked, [this] { aboutFormat(); });
        QObject::connect(regionOnly_, &QAction::toggled, [this](bool) { showEstimate(); });
        QObject::connect(renderButton_, &QPushButton::clicked, [this] { start(); });
        QObject::connect(cancelSetup, &QPushButton::clicked, [this] { reject(); });
        QObject::connect(cancelRender_, &QPushButton::clicked, [this] { askStop(); });
        QObject::connect(closeButton_, &QPushButton::clicked, [this] { accept(); });
        QObject::connect(openFile_, &QPushButton::clicked, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(shownPath_)); });
        QObject::connect(openFolder_, &QPushButton::clicked, [this] { openContainingFolder(shownPath_); });
        showSpace();
    }

    ~RenderAsDialog() override { stopAndJoin(); }

    void reject() override
    {
        stopAndJoin();
        QDialog::reject();
    }

    QString renderedPath() const { return rendered_ ? outPath_ : QString(); }

private:
    const Format* currentFormat() const
    {
        QListWidgetItem* it = formatList_->currentItem();
        if (!it) return nullptr;
        const int fi = it->data(kRoleFormat).toInt();
        return fi >= 0 && fi < (int)formats_.size() ? &formats_[(size_t)fi] : nullptr;
    }
    const Tmpl* current() const
    {
        QTreeWidgetItem* it = tree_->currentItem();
        if (!it) return nullptr;
        const int fi = it->data(0, kRoleFormat).toInt(), ti = it->data(0, kRoleTemplate).toInt();
        if (fi < 0 || fi >= (int)formats_.size() || ti < 0 || ti >= (int)formats_[(size_t)fi].templates.size()) return nullptr;
        return &formats_[(size_t)fi].templates[(size_t)ti];
    }
    QString keyOf(const Format& f, const Tmpl& t) const { return f.key + "/" + t.name; }
    QStringList favorites() const { return QSettings().value("render/favorites").toStringList(); }

    bool matchesProject(const Format& f, const Tmpl& t) const
    {
        const ProjectSettings& ps = AppState::get().project().settings;
        if (!f.hasVideo || !t.video) return false;
        const bool sizeOk = t.width == 0 || (t.width == ps.width && t.height == ps.height);
        const bool rateOk = t.fpsNum == 0 || (long long)t.fpsNum * ps.fpsDen == (long long)ps.fpsNum * t.fpsDen;
        return sizeOk && rateOk;
    }

    // The Filters > Match project video/audio settings checks that are on: does the template suit the project?
    bool matchesChecked(const Format& f, const Tmpl& t) const
    {
        if (showAll_->isChecked()) return true;
        const ProjectSettings& ps = AppState::get().project().settings;
        auto on = [this](const char* k) { auto it = matchChecks_.find(k); return it != matchChecks_.end() && it->second->isChecked(); };
        if (f.hasVideo && t.video) {
            if (on("resolution") && t.width != 0 && (t.width != ps.width || t.height != ps.height)) return false;
            if (on("framerate") && t.fpsNum != 0 && (long long)t.fpsNum * ps.fpsDen != (long long)ps.fpsNum * t.fpsDen) return false;
            if (on("ratio") && t.width != 0 && t.height != 0 && ps.height != 0) {
                const double a = (double)t.width / t.height * (t.par > 0 ? t.par : ps.pixelAspect), b = (double)ps.width / ps.height * ps.pixelAspect;
                if (std::fabs(a - b) > 0.02 * b) return false;
            }
            if (on("pixelratio") && t.par > 0 && std::fabs(t.par - ps.pixelAspect) > 1e-3) return false;
            if (on("fieldorder") && !t.field.isEmpty() && t.field.toStdString() != ps.fieldOrder) return false;
            const bool deepFormat = f.alphaPossible || t.picture == "exr" || t.picture == "tiff" || t.name.contains("10-bit") || t.name.contains("ProRes", Qt::CaseInsensitive);
            const bool deepProject = ps.pixelFormat != "8bit";
            if (on("colordepth") && deepProject && !deepFormat) return false;
            if (on("hdr") && !(deepProject && (deepFormat || !t.colorSpace.isEmpty() || t.name.contains("HDR", Qt::CaseInsensitive)))) return false;
            if (on("colorspace") && !t.colorSpace.isEmpty() && t.colorSpace.toStdString() != ps.viewTransform) return false;
        }
        if (f.hasAudio && t.audio) {
            if (on("samplerate") && t.sampleRate != 0 && t.sampleRate != ps.sampleRate) return false;
            if (on("channels") && t.channels != ps.audioChannels) return false;
            if (on("bitdepth") && !f.audioRate && t.bits != ps.audioBits) return false;
        }
        return true;
    }

    bool templateShown(const Format& f, const Tmpl& t, const QStringList& favs, const QString& text) const
    {
        if (favoritesOnly_->isChecked() && !favs.contains(keyOf(f, t))) return false;
        if (matchProject_->isChecked() && !showAll_->isChecked() && !matchesProject(f, t)) return false;
        if (!matchesChecked(f, t)) return false;
        if (!text.isEmpty() && !t.name.contains(text, Qt::CaseInsensitive) && !f.name.contains(text, Qt::CaseInsensitive)) return false;
        return true;
    }

    bool filtersOn() const
    {
        if (favoritesOnly_->isChecked() || matchProject_->isChecked() || videoOnly_->isChecked() || audioOnly_->isChecked()) return true;
        if (showAll_->isChecked()) return false;
        for (const auto& m : matchChecks_)
            if (m.second->isChecked()) return true;
        return false;
    }

    // The Formats list: VEGAS's formats in alphabetical order, those with a template the filters and search let
    // through (a format Reno can't make is greyed while nothing narrows the list).
    void fillFormats()
    {
        const QString keep = currentFormat() ? currentFormat()->key : QString();
        const QStringList favs = favorites();
        const QString text = search_->text().trimmed();
        filter_->setText(filtersOn() ? "Filters On" : "Filters Off");
        std::vector<int> order;
        for (int fi = 0; fi < (int)formats_.size(); ++fi) order.push_back(fi);
        std::stable_sort(order.begin(), order.end(), [this](int a, int b) { return formats_[(size_t)a].name.compare(formats_[(size_t)b].name, Qt::CaseInsensitive) < 0; });
        QSignalBlocker block(formatList_);
        formatList_->clear();
        QListWidgetItem* again = nullptr;
        for (int fi : order) {
            const Format& f = formats_[(size_t)fi];
            if (videoOnly_->isChecked() && !f.hasVideo) continue;
            if (audioOnly_->isChecked() && f.hasVideo) continue;
            int shown = 0;
            for (const Tmpl& t : f.templates) shown += templateShown(f, t, favs, text) ? 1 : 0;
            const bool greyed = !f.unavailable.isEmpty();
            if (greyed ? (text.isEmpty() ? filtersOn() : !f.name.contains(text, Qt::CaseInsensitive)) : shown == 0) continue;
            auto* it = new QListWidgetItem(f.name, formatList_);
            it->setData(kRoleFormat, fi);
            it->setToolTip(greyed ? f.unavailable : f.blurb);
            if (greyed) it->setFlags(it->flags() & ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable));
            if (f.key == keep) again = it;
        }
        if (!again && formatList_->count() > 0) again = formatList_->item(0);
        if (again) formatList_->setCurrentItem(again);
        block.unblock();
        fillTemplates();
    }

    void fillTemplates()
    {
        const Format* f = currentFormat();
        const QStringList favs = favorites();
        const QString text = search_->text().trimmed();
        QSignalBlocker block(tree_);
        tree_->clear();
        QTreeWidgetItem* again = nullptr;
        QTreeWidgetItem* first = nullptr;
        const int fi = f ? (int)(f - formats_.data()) : -1;
        if (f)
            for (int ti = 0; ti < (int)f->templates.size(); ++ti) {
                const Tmpl& t = f->templates[(size_t)ti];
                if (!templateShown(*f, t, favs, text)) continue;
                const bool fav = favs.contains(keyOf(*f, t));
                auto* it = new QTreeWidgetItem(tree_);
                it->setText(1, fav ? QString::fromUtf8("\u2605") : QString::fromUtf8("\u2606"));
                it->setTextAlignment(1, Qt::AlignCenter);
                it->setToolTip(1, fav ? "A favourite (click to take it off)" : "Click to make it a favourite");
                it->setText(0, t.custom ? t.name + "  (custom)" : t.name);
                it->setToolTip(0, templateInfo(*f, t));
                it->setData(0, kRoleFormat, fi);
                it->setData(0, kRoleTemplate, ti);
                if (fav) it->setForeground(1, QColor(240, 190, 40));
                if (!first) first = it;
                if (t.name == lastTemplate_.value(f->key)) again = it;
            }
        if (!again) again = first;
        if (again) tree_->setCurrentItem(again);
        block.unblock();
        templateChanged();
    }

    void selectTemplate(const QString& key)
    {
        for (int fi = 0; fi < (int)formats_.size(); ++fi) {
            const Format& f = formats_[(size_t)fi];
            for (const Tmpl& t : f.templates) {
                if (keyOf(f, t) != key) continue;
                lastTemplate_[f.key] = t.name;
                for (int k = 0; k < formatList_->count(); ++k)
                    if (formatList_->item(k)->data(kRoleFormat).toInt() == fi) {
                        if (formatList_->currentRow() == k) fillTemplates();
                        else formatList_->setCurrentRow(k);
                        if (QTreeWidgetItem* c = tree_->currentItem()) tree_->scrollToItem(c);
                        return;
                    }
                return;
            }
        }
    }

    void toggleFavorite(QTreeWidgetItem* it)
    {
        const Format& f = formats_[(size_t)it->data(0, kRoleFormat).toInt()];
        const Tmpl& t = f.templates[(size_t)it->data(0, kRoleTemplate).toInt()];
        QStringList favs = favorites();
        const QString k = keyOf(f, t);
        if (favs.contains(k)) favs.removeAll(k);
        else favs << k;
        QSettings().setValue("render/favorites", favs);
        lastTemplate_[f.key] = t.name;
        fillTemplates();
    }

    void templateChanged()
    {
        const Format* f = currentFormat();
        const Tmpl* t = current();
        customize_->setEnabled(t != nullptr);
        renderButton_->setEnabled(t != nullptr);
        if (!t) {
            info_->setText(f ? (f->unavailable.isEmpty() ? f->blurb : f->unavailable) : QString("Choose a template."));
            showEstimate();
            return;
        }
        lastTemplate_[f->key] = t->name;
        info_->setText(templateInfo(*f, *t));
        stretch_->setEnabled(f->hasVideo && t->video);
        rotation_->setEnabled(f->hasVideo && t->video);
        markers_->setEnabled(f->key == "avc" || f->key == "hevc" || f->key == "mov" || f->key == "mkv" || f->key == "wmv" || f->key == "xavcs" || f->key == "m4a" ||
                              f->key == "webm" || f->key == "alac" || f->key == "ffmpeg");
        loudness_->setEnabled(f->hasAudio && t->audio);
        {
            const bool mapping = takesChannelMapping(*f) && t->audio && !t->multipleMono;
            multichannel_->setEnabled(mapping);
            channelsAction_->setEnabled(mapping);
        }
        showExtension();
        showEstimate();
    }

    QString extension() const
    {
        const Format* f = currentFormat();
        const Tmpl* t = current();
        if (!f) return QString("mp4");
        if (f->sequence && t) return pictureExt(*t);
        return fileExt(*f, t);
    }

    void showExtension()
    {
        const QString ext = extension();
        QString n = name_->text().trimmed();
        if (!shownExt_.isEmpty() && n.endsWith("." + shownExt_, Qt::CaseInsensitive)) n.chop(shownExt_.size() + 1);
        else if (n.endsWith("." + ext, Qt::CaseInsensitive)) n.chop(ext.size() + 1);
        name_->setText(n + "." + ext);
        shownExt_ = ext;
    }

    QString folder() const { return QDir::fromNativeSeparators(folder_->currentText().trimmed()); }
    QString stem() const
    {
        QString n = name_->text().trimmed();
        const QString ext = extension();
        if (n.endsWith("." + ext, Qt::CaseInsensitive)) n.chop(ext.size() + 1);
        else if (!shownExt_.isEmpty() && n.endsWith("." + shownExt_, Qt::CaseInsensitive)) n.chop(shownExt_.size() + 1);
        return n;
    }
    QString outputPath() const
    {
        return QDir(folder()).filePath(stem() + "." + extension());
    }

    void nameEdited()
    {
        const QString n = QDir::fromNativeSeparators(name_->text().trimmed());
        if (n.contains('/')) {
            const QFileInfo fi(n);
            if (fi.isAbsolute()) folder_->setEditText(QDir::toNativeSeparators(fi.absolutePath()));
            name_->setText(fi.fileName());
        }
    }

    void browse()
    {
        const Format* f = currentFormat();
        const QString filter = f ? QString("%1 (*.%2)").arg(f->name, extension()) : QString("All files (*)");
        const QString file = QFileDialog::getSaveFileName(this, mode_ == NewTrack ? "Render to New Track" : "Render As", outputPath(), filter);
        if (file.isEmpty()) return;
        const QFileInfo fi(file);
        folder_->setEditText(QDir::toNativeSeparators(fi.absolutePath()));
        name_->setText(fi.completeBaseName() + "." + extension());
        showSpace();
    }

    void showSpace()
    {
        const QStorageInfo st(folder());
        if (st.isValid() && st.bytesAvailable() > 0) space_->setText(QString("Free disk space: %1.").arg(diskText(st.bytesAvailable())));
        else space_->setText(QDir(folder()).exists() ? QString() : QString("(this folder does not exist)"));
        showEstimate();
    }

    double range(double* from = nullptr) const
    {
        const AppState& app = AppState::get();
        double a = 0, b = app.project().length();
        if (mode_ == NewTrack) {
            a = from_;
            b = to_;
        } else if (regionOnly_->isChecked() && app.loopEnd > app.loopStart) {
            a = app.loopStart;
            b = app.loopEnd;
        }
        if (from) *from = a;
        return b - a;
    }

    void showEstimate()
    {
        const Format* f = currentFormat();
        const Tmpl* t = current();
        if (!f || !t) {
            estimate_->setText(QString());
            return;
        }
        const qint64 bytes = estimateBytes(*f, *t, AppState::get().project().settings, range());
        estimate_->setText(bytes < 0 ? QString("Estimated file size: depends on the picture") : QString("Estimated file size: %1").arg(estimateText(bytes)));
    }

    void openContainingFolder(const QString& file)
    {
#ifdef _WIN32
        // Explorer reads "/select," and the path as written: QProcess would quote the whole argument when the path has a
        // space, and Explorer would then open Documents instead
        QProcess p;
        p.setProgram("explorer.exe");
        p.setNativeArguments("/select,\"" + QDir::toNativeSeparators(QFileInfo(file).absoluteFilePath()) + "\"");
        p.startDetached();
#else
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(file).absolutePath()));
#endif
    }

    void customizeTemplate()
    {
        QTreeWidgetItem* it = tree_->currentItem();
        if (!it || !current()) return;
        const int fi = it->data(0, kRoleFormat).toInt();
        CustomizeDialog dlg(this, formats_, fi, *current());
        const int r = dlg.exec();
        Format& f = formats_[(size_t)fi];
        QString pickName = dlg.saved_;
        if (r == QDialog::Accepted) {
            Tmpl t = dlg.result();
            bool same = false;
            for (const Tmpl& o : f.templates)
                if (o.name == t.name && o.custom && o.toJson() == t.toJson()) same = true;
            if (!same) {
                // changed and not saved: kept for this session as "(Untitled)", as VEGAS does
                t.name = "(Untitled)";
                t.custom = true;
                bool replaced = false;
                for (Tmpl& o : f.templates)
                    if (o.name == t.name) {
                        o = t;
                        replaced = true;
                    }
                if (!replaced) f.templates.insert(f.templates.begin(), t);
            }
            pickName = t.name;
        }
        fillFormats();
        if (!pickName.isEmpty()) selectTemplate(f.key + "/" + pickName);
    }

    void aboutFormat()
    {
        const Format* f = currentFormat();
        if (!f) return;
        QString text = QString("<b>%1</b><br>%2<br><br>Files: *.%3<br>").arg(f->name.toHtmlEscaped(), f->blurb.toHtmlEscaped(), extension());
        if (!f->videoFamily.empty()) {
            text += "<br>Picture encoders in this FFmpeg:<br>";
            std::vector<EncoderMode> modes = videoEncoderModes(f->videoFamily, f->videoFamily == "h264" || f->videoFamily == "hevc");
            for (const EncoderMode& m : modes)
                text += QString("&nbsp;&nbsp;%1: %2<br>").arg(qs(m.encoder), !m.present ? QString("not in this FFmpeg") : (f->videoFamily == "h264" || f->videoFamily == "hevc") ? (m.works ? QString("works") : QString("does not start here")) : QString("present"));
        }
        text += QString("<br>FFmpeg %1").arg(qs(mediaBackendVersion()).toHtmlEscaped());
        QMessageBox box(QMessageBox::Information, "About", text, QMessageBox::Ok, this);
        box.setObjectName("aboutFormatBox");
        box.setTextFormat(Qt::RichText);
        box.exec();
    }

    void start()
    {
        AppState& app = AppState::get();
        const Format* f = currentFormat();
        const Tmpl* t = current();
        if (!f || !t) {
            QMessageBox::information(this, windowTitle(), "Please select a template.");
            return;
        }
        nameEdited();
        if (stem().isEmpty()) {
            QMessageBox::information(this, windowTitle(), "Type a name for the file.");
            return;
        }
        if (stem().contains(QRegularExpression("[<>:\"|?*]"))) {
            QMessageBox::warning(this, windowTitle(), "The file name contains characters a file name cannot have (< > : \" | ? *).");
            return;
        }
        if (!QDir(folder()).exists()) {
            if (QMessageBox::question(this, windowTitle(), QString("The folder %1 does not exist. Create it?").arg(QDir::toNativeSeparators(folder()))) != QMessageBox::Yes) return;
            if (!QDir().mkpath(folder())) {
                QMessageBox::warning(this, windowTitle(), QString("The folder %1 could not be made.").arg(QDir::toNativeSeparators(folder())));
                return;
            }
        }
        QString out = outputPath();
        QString writerPath = out;
        if (f->sequence) {
            QString s = stem();
            s.replace("%", "%%");
            writerPath = QDir(folder()).filePath(s + "_%06d." + pictureExt(*t));
        }
        QString firstFile = f->sequence ? QDir(folder()).filePath(stem() + "_000000." + pictureExt(*t)) : out;
        finalPath_.clear();
        if (swap_ && swap_->isChecked() && !f->sequence && QFileInfo::exists(out)) {
            finalPath_ = out;
            writerPath = QDir(folder()).filePath(stem() + ".rendering." + QFileInfo(out).suffix());
        }
        if (QFileInfo::exists(firstFile) &&
            QMessageBox::question(this, windowTitle(), QString("%1 already exists. Do you want to replace it?").arg(QFileInfo(firstFile).fileName())) != QMessageBox::Yes)
            return;

        double from = 0;
        const double len = range(&from);
        if (len <= 0.0) {
            QMessageBox::information(this, windowTitle(), "There is nothing to render: the timeline is empty.");
            return;
        }

        // Preferences > General, "Render large Wave files as Wave64" and "Allow Wave renders up to 4 GB": a .wav render
        // that would exceed what a Wave file can hold (2 GB, or 4 GB when allowed) goes to a Wave64 file (.w64) instead
        if (f->key == "wav" && !f->sequence && vprefs::useWave64(estimateBytes(*f, *t, app.project().settings, len))) {
            auto asW64 = [](QString path) { return path.left(path.lastIndexOf('.')) + ".w64"; };
            out = asW64(out);
            firstFile = asW64(firstFile);
            writerPath = asW64(writerPath);
        }

        RenderChoices choices;
        choices.writerPath = writerPath.toStdString();
        choices.stretch = stretch_->isChecked();
        choices.useRotation = rotation_->isChecked();
        choices.markers = markers_->isChecked() && markers_->isEnabled();
        if (loudness_->isChecked() && loudness_->isEnabled()) choices.loudnessLog = QDir(folder()).filePath(stem() + "_loud.txt").toStdString();
        choices.loudnessAtsc = QSettings().value("loudness/mode").toString() == "atsc";
        if (multichannel_->isChecked() && multichannel_->isEnabled()) choices.channelMap = channelMap_;
        choices.summary = summary_->isChecked();
        if (projectPath_->isChecked()) choices.projectPath = app.path();
        RenderSpec spec;
        QString specError;
        if (!makeRenderSpec(*f, *t, choices, app.project().settings, &spec, &specError)) {
            QMessageBox::information(this, windowTitle(), specError);
            return;
        }

        QSettings settings;
        settings.setValue("render/folder", folder());
        QStringList recent = settings.value("render/recentFolders").toStringList();
        recent.removeAll(folder());
        recent.prepend(folder());
        while (recent.size() > 10) recent.removeLast();
        settings.setValue("render/recentFolders", recent);
        settings.setValue(mode_ == NewTrack ? "render/newTrackTemplate" : "render/template", keyOf(*f, *t));
        if (mode_ == RenderAs) settings.setValue("render/regionOnly", regionOnly_->isChecked());
        settings.setValue("render/stretch", stretch_->isChecked());
        settings.setValue("render/useRotation", rotation_->isChecked());
        settings.setValue("render/saveMarkers", markers_->isChecked());
        settings.setValue("render/loudnessLog", loudness_->isChecked());
        settings.setValue("render/projectPath", projectPath_->isChecked());
        settings.setValue("render/summary", summary_->isChecked());
        settings.setValue("render/swapFiles", swap_->isChecked());
        settings.setValue("render/filterFavorites", favoritesOnly_->isChecked());
        settings.setValue("render/filterMatch", matchProject_->isChecked());
        settings.setValue("render/showAll", showAll_->isChecked());
        for (auto& m : matchChecks_) settings.setValue(QString("render/match_") + QString::fromStdString(m.first), m.second->isChecked());

        outPath_ = finalPath_.isEmpty() ? out : writerPath;
        shownPath_ = firstFile;
        if (spec.multipleMono) {
            const std::vector<std::string> monos = multipleMonoPaths(out.toStdString(), AudioEngine::projectChannels(app.project()));
            if (!monos.empty()) outPath_ = shownPath_ = QString::fromStdString(monos[0]);
        }
        sequence_ = f->sequence;
        sequenceStem_ = QDir(folder()).filePath(stem() + "_");
        pathLabel_->setText(QDir::toNativeSeparators(f->sequence ? firstFile : out));
        pathLabel_->setToolTip(pathLabel_->text());
        bar_->setValue(0);
        setWindowTitle("Rendering: 0%");
        result_->setText("");
        cancelRender_->show();
        cancelRender_->setEnabled(true);
        closeButton_->hide();
        openFile_->setVisible(mode_ == RenderAs);
        openFolder_->setVisible(mode_ == RenderAs);
        openFile_->setEnabled(false);
        openFolder_->setEnabled(false);
        estimatedBytes_ = estimateBytes(*f, *t, app.project().settings, len);
        setMinimumSize(0, 0);
        stack_->setCurrentIndex(1);
        for (int k = 0; k < stack_->count(); ++k) stack_->widget(k)->setSizePolicy(k == 1 ? QSizePolicy::Preferred : QSizePolicy::Ignored, k == 1 ? QSizePolicy::Preferred : QSizePolicy::Ignored);
        setMinimumWidth(430);
        resize(460, stack_->widget(1)->sizeHint().height() + 24);
        elapsed_.start();
        frameNow_ = lastFrame_ = lastTick_ = fpsShown_ = 0;
        cpuStart_ = processCpuSeconds();
        clock_->start();
        running_ = true;
        fraction_ = 0;

        logbook::write(logbook::Kind::Renders, logbook::Basic,
                       QString("Render started: %1 (%2, %3), %4 s from %5 s").arg(QDir::toNativeSeparators(firstFile), f->name, t->name).arg(len, 0, 'f', 3).arg(from, 0, 'f', 3));
        logbook::write(logbook::Kind::Renders, logbook::Detailed,
                       QString("  project %1 x %2 at %3 fps, %4 tracks, %5 events").arg(app.project().settings.width).arg(app.project().settings.height)
                           .arg(app.project().settings.fps(), 0, 'f', 3).arg(app.project().tracks.size()).arg([&app] { size_t n = 0; for (const Track& tr : app.project().tracks) n += tr.events.size(); return n; }()));
        job_.reset(new RenderJob());
        const std::shared_ptr<RenderJob> job = job_;
        RenderAsDialog* self = this;
        Project copy = *app.snapshot();
        const double to = from + len;
        renderFrom_ = from;
        renderTo_ = to;
        thread_ = std::thread([job, self, copy, spec, from, to]() mutable {
            std::string err, desc;
            bool ok = false;
            auto last = std::chrono::steady_clock::now();
            try {
                AppState& a = AppState::get();
                ok = renderProjectAs(copy, a.fx, spec, from, to,
                                     [&](const ExportProgress& pr) {
                                         const auto now = std::chrono::steady_clock::now();
                                         if (now - last > std::chrono::milliseconds(40)) {
                                             last = now;
                                             const ExportProgress snap = pr;
                                             AppState::post([job, self, snap] {
                                                 if (job->alive.load()) self->progress(snap.fraction, snap.frame, snap.frames);
                                             });
                                         }
                                         return !job->cancel.load();
                                     },
                                     &err, a.mediaOpener, &desc);
            } catch (const std::exception& e) {
                ok = false;
                err = e.what();
            } catch (...) {
                ok = false;
                err = "an unexpected error";
            }
            const bool cancelled = job->cancel.load();
            logbook::write(logbook::Kind::Renders, logbook::Basic,
                           cancelled ? QString("Render cancelled") : ok ? QString("Render finished") : QString("Render failed: %1").arg(QString::fromStdString(err)));
            if (ok && !desc.empty()) logbook::write(logbook::Kind::Renders, logbook::Detailed, "  " + QString::fromStdString(desc).replace('\n', "\n  "));
            AppState::post([job, self, ok, cancelled, err, desc] {
                if (job->alive.load()) self->finished(ok, cancelled, err, desc);
            });
        });
    }

    static QString pictureExt(const Tmpl& t) { return rt::pictureExt(t); }

    void askStop()
    {
        if (!running_) return;
        job_->cancel.store(true);
        cancelRender_->setEnabled(false);
        result_->setText("Cancelling...");
    }

    void removeOutput()
    {
        if (sequence_) {
            for (int k = 0; k < 10000000; ++k) {
                const QString f = sequenceStem_ + QString("%1").arg(k, 6, 10, QChar('0')) + "." + QFileInfo(shownPath_).suffix();
                if (!QFile::remove(f)) break;
            }
        } else {
            QFile::remove(outPath_);
        }
    }

    void stopAndJoin()
    {
        if (job_) {
            job_->cancel.store(true);
            if (thread_.joinable()) thread_.join();
            job_->alive.store(false);
            if (running_) {
                running_ = false;
                removeOutput();
            }
        }
    }

    void showTimes()
    {
        const double el = elapsed_.elapsed() / 1000.0;
        const qint64 now = sequence_ ? 0 : QFileInfo(outPath_).size();
        const int pct = (int)(fraction_ * 100);
        qint64 total = estimatedBytes_;
        if (!sequence_ && fraction_ > 0.02) total = (qint64)(now / fraction_);
        percentOf_->setText(total > 0 && !sequence_ ? QString("%1% of %2").arg(pct).arg(megaText(total)) : QString("%1%").arg(pct));
        remaining_->setText(QString("Remaining: %1").arg(hmsText(fraction_ > 0.02 && running_ ? el * (1.0 - fraction_) / fraction_ : 0.0)));
        renderedSize_->setText(sequence_ ? QString() : QString("Rendered: %1").arg(megaText(now)));
        elapsedLabel_->setText(QString("Elapsed: %1").arg(hmsText(el)));
        const QStorageInfo disk(QFileInfo(outPath_).absolutePath());
        estFree_->setText(disk.isValid() ? diskText(disk.bytesAvailable()) : QString("-"));
        if (fraction_ > 0.02 && running_) {
            const double whole = el / fraction_;
            estDuration_->setText(hmsText(whole));
            estCompletion_->setText(QDateTime::currentDateTime().addMSecs((qint64)((whole - el) * 1000)).toString("HH:mm:ss"));
        } else if (running_) {
            estDuration_->setText("-");
            estCompletion_->setText("-");
        }
        const double fpsNow = lastTick_ > 0 && el > lastTick_ ? (frameNow_ - lastFrame_) / (el - lastTick_) : 0;
        if (el - lastTick_ > 0.9) {
            lastTick_ = el;
            lastFrame_ = frameNow_;
            if (fpsNow > 0) fpsShown_ = fpsNow;
        }
        const double cpu = processCpuSeconds();
        const int cores = std::max(1, QThread::idealThreadCount());
        perfAverage_->setText(QString("%1 fps").arg(el > 0 ? frameNow_ / el : 0.0, 0, 'f', 2));
        perfCurrent_->setText(QString("%1 fps").arg(fpsShown_, 0, 'f', 2));
        perfCpu_->setText(QString("%1 %").arg(el > 0.5 ? std::min(100.0, (cpu - cpuStart_) / el / cores * 100.0) : 0.0, 0, 'f', 2));
        perfMemory_->setText(megaText(processMemory()));
    }

public:
    void progress(double fraction, qint64 frame, qint64 frames)
    {
        if (!running_) return;
        fraction_ = std::min(1.0, std::max(0.0, fraction));
        frameNow_ = (double)frame;
        bar_->setValue((int)(fraction_ * 1000));
        setWindowTitle(QString("Rendering: %1%").arg((int)(fraction_ * 100)));
        (void)frames;
        showTimes();
    }

    void finished(bool ok, bool cancelled, const std::string& err, const std::string& desc)
    {
        if (thread_.joinable()) thread_.join();
        running_ = false;
        clock_->stop();
        showTimes();
        cancelRender_->hide();
        closeButton_->show();
        closeButton_->setDefault(true);
        if (ok && !finalPath_.isEmpty()) {
            QFile::remove(finalPath_);
            if (QFile::rename(outPath_, finalPath_)) outPath_ = finalPath_;
            else {
                ok = false;
                removeOutput();
                setWindowTitle("Render failed");
                result_->setText("The finished file could not be put in place of the old one.");
                return;
            }
        }
        if (ok) {
            rendered_ = true;
            bar_->setValue(1000);
            setWindowTitle("Render Complete");
            QString text = QString("Rendering complete: %1").arg(QDir::toNativeSeparators(shownPath_));
            if (!desc.empty()) text += "\n" + qs(desc);
            if (!sequence_) text += QString("\nFile size: %1").arg(sizeText(QFileInfo(outPath_).size()));
            result_->setText(text);
            // the project's closed captions (CC1, CC2) in a Scenarist .scc file beside the file, as VEGAS writes one for
            // formats that can't hold them
            if (mode_ == RenderAs && !sequence_) {
                std::vector<Caption> caps;
                for (const Caption& c : projectCaptions(AppState::get().project()))
                    if (c.time >= renderFrom_ - 1e-9 && c.time < renderTo_ && c.channel <= 2) caps.push_back(c);
                if (!caps.empty()) {
                    const QFileInfo fi(outPath_);
                    const std::string scc = QDir(fi.absolutePath()).filePath(fi.completeBaseName() + ".scc").toStdString();
                    if (writeFile(scc, sccText(caps, renderFrom_))) text += QString("\nClosed captions: %1").arg(QDir::toNativeSeparators(qs(scc)));
                    result_->setText(text);
                }
            }
            if (elapsed_.elapsed() > 60000) QApplication::beep();
            QSettings().setValue("render/openFolderWhenDone", openFolderWhenDone_->isChecked());
            if (mode_ == RenderAs && openFolderWhenDone_->isChecked())
                QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(sequence_ ? shownPath_ : outPath_).absolutePath()));
            openFile_->setVisible(mode_ == RenderAs);
            openFolder_->setVisible(mode_ == RenderAs);
            openFile_->setEnabled(true);
            openFolder_->setEnabled(true);
            AppState::get().status(QString("Rendering complete: %1").arg(QDir::toNativeSeparators(shownPath_)).toStdString());
            QSettings().setValue("render/closeWhenDone", closeWhenDone_->isChecked());
            if (mode_ == RenderAs && shutdown_->isChecked()) {
#ifdef _WIN32
                QProcess::startDetached("shutdown", {"/s", "/t", "30"});
#else
                QProcess::startDetached("shutdown", {"-h", "+1"});
#endif
            }
            if (mode_ == NewTrack || closeWhenDone_->isChecked()) QTimer::singleShot(0, this, [this] { accept(); });
        } else if (cancelled) {
            removeOutput();
            setWindowTitle("Render cancelled");
            result_->setText("The render was stopped. The unfinished file was deleted.");
        } else {
            removeOutput();
            setWindowTitle("Render failed");
            result_->setText(qs(err.empty() ? std::string("The file could not be written.") : err));
        }
    }

private:
    Mode mode_;
    double from_, to_;
    std::vector<Format> formats_;
    QStackedWidget* stack_ = nullptr;
    QComboBox* folder_ = nullptr;
    QLineEdit *name_ = nullptr, *search_ = nullptr;
    QToolButton* filter_ = nullptr;
    QAction *favoritesOnly_ = nullptr, *matchProject_ = nullptr, *videoOnly_ = nullptr, *audioOnly_ = nullptr, *showAll_ = nullptr;
    std::map<std::string, QAction*> matchChecks_;
    QListWidget* formatList_ = nullptr;
    QMap<QString, QString> lastTemplate_;
    QToolButton* renderOptions_ = nullptr;
    QAction* channelsAction_ = nullptr;
    QString shownExt_;
    QTreeWidget* tree_ = nullptr;
    QLabel *info_ = nullptr, *space_ = nullptr, *estimate_ = nullptr;
    QPushButton *browse_ = nullptr, *customize_ = nullptr, *renderButton_ = nullptr, *cancelRender_ = nullptr, *closeButton_ = nullptr,
                *openFile_ = nullptr, *openFolder_ = nullptr;
    QCheckBox* regionBox_ = nullptr;
    QAction *regionOnly_ = nullptr, *stretch_ = nullptr, *rotation_ = nullptr, *markers_ = nullptr, *loudness_ = nullptr,
            *projectPath_ = nullptr, *summary_ = nullptr, *swap_ = nullptr, *multichannel_ = nullptr;
    QCheckBox *closeWhenDone_ = nullptr, *shutdown_ = nullptr;
    QString finalPath_;
    QLabel* result_ = nullptr;
    QLabel *pathLabel_ = nullptr, *percentOf_ = nullptr, *remaining_ = nullptr, *renderedSize_ = nullptr, *elapsedLabel_ = nullptr;
    QLabel *estFree_ = nullptr, *estDuration_ = nullptr, *estCompletion_ = nullptr, *perfAverage_ = nullptr, *perfCurrent_ = nullptr,
           *perfCpu_ = nullptr, *perfMemory_ = nullptr;
    QWidget* optionsFold_ = nullptr;
    QCheckBox* openFolderWhenDone_ = nullptr;
    qint64 estimatedBytes_ = -1;
    std::vector<RenderSpec::ChannelSource> channelMap_;
    double frameNow_ = 0, lastFrame_ = 0, lastTick_ = 0, fpsShown_ = 0, cpuStart_ = 0;
    double renderFrom_ = 0, renderTo_ = 0;
    QProgressBar* bar_ = nullptr;
    QTimer* clock_ = nullptr;
    QElapsedTimer elapsed_;
    QString outPath_, shownPath_, sequenceStem_;
    bool sequence_ = false;
    double fraction_ = 0;
    bool running_ = false, rendered_ = false;
    std::shared_ptr<RenderJob> job_;
    std::thread thread_;
};

}  // namespace

void runRenderAsDialog(QWidget* parent)
{
    RenderAsDialog dlg(parent, RenderAsDialog::RenderAs);
    dlg.exec();
}

void runRenderDialog(QWidget* parent) { runRenderAsDialog(parent); }

bool runRenderToNewTrackDialog(QWidget* parent, double from, double to, const QString& suggested, QString* path)
{
    RenderAsDialog dlg(parent, RenderAsDialog::NewTrack, from, to, suggested);
    dlg.exec();
    const QString p = dlg.renderedPath();
    if (path) *path = p;
    return !p.isEmpty();
}

QStringList renderTemplateNames()
{
    QStringList names;
    for (const Format& f : builtInFormats())
        for (const Tmpl& t : f.templates) names << f.name + "/" + t.name;
    return names;
}

}  // namespace app
}  // namespace reno
