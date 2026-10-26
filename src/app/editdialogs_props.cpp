// Event Properties and Project Properties dialogs.
#include "editdialogs.h"
#include "editdialogs_internal.h"
#include "vprefs.h"
#include "../engine/colormgmt.h"
#include "../engine/timefmt.h"
#include "../engine/stretch.h"
#include "playback.h"
#include "strconv.h"
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QInputDialog>
#include <QMessageBox>
#include <QRegularExpressionValidator>
#include <functional>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QKeyEvent>
#include <QShortcut>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QStorageInfo>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#ifdef RENO_HAVE_QTMULTIMEDIA
#include <QAudioDevice>
#include <QMediaDevices>
#endif
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

namespace reno {
namespace app {

namespace {

using namespace editdlg;

// ================================================================================== Event Properties
class EventPropertiesDialog : public QDialog {
public:
    EventPropertiesDialog(QWidget* parent, uint64_t eventId) : QDialog(parent), eventId_(eventId)
    {
        setObjectName("eventPropertiesDialog");
        AppState& app = AppState::get();
        const Track* tr = nullptr;
        const Event* e = app.project().findEvent(eventId, &tr);
        const bool video = tr && tr->type == TrackType::Video;
        const MediaItem* m = e ? app.project().findMedia(e->mediaId) : nullptr;
        setWindowTitle("Properties");
        setMinimumWidth(460);
        QVBoxLayout* root = new QVBoxLayout(this);
        QTabWidget* tabs = new QTabWidget(this);
        tabs->setObjectName("tabs");
        root->addWidget(tabs);

        QWidget* ev = new QWidget(tabs);
        QFormLayout* f = new QFormLayout(ev);
        name_ = new QLineEdit(ev);
        name_->setObjectName("eventName");
        f->addRow("Active take name:", name_);
        rate_ = new Spin(ev, 0.001, 100.0, 3, 0.1, "playbackRate");
        if (video && m && m->info.hasVideo && !m->info.isStill && m->info.fps > 0) {
            // Conform to Project Frame Rate: the rate at which each media frame lasts one project frame (24 fps footage
            // in a 30 fps project: 1.25)
            QWidget* row = new QWidget(ev);
            QHBoxLayout* rl = new QHBoxLayout(row);
            rl->setContentsMargins(0, 0, 0, 0);
            rl->addWidget(rate_, 1);
            QPushButton* conform = new QPushButton("Conform to Project Frame Rate", row);
            conform->setObjectName("conformRate");
            rl->addWidget(conform);
            const double r = app.project().settings.fps() / m->info.fps;
            QObject::connect(conform, &QPushButton::clicked, this, [this, r] { rate_->setValue(r); });
            f->addRow("Playback rate:", row);
        } else {
            f->addRow("Playback rate:", rate_);
        }
        mute_ = new QCheckBox("Mute", ev);
        mute_->setObjectName("mute");
        f->addRow(QString(), mute_);
        loop_ = new QCheckBox("Loop (start the media again past its end)", ev);
        loop_->setObjectName("loop");
        f->addRow(QString(), loop_);
        reverse_ = new QCheckBox("Reverse (play backwards)", ev);
        reverse_->setObjectName("reverse");
        f->addRow(QString(), reverse_);
        if (video) {
            aspect_ = new QCheckBox("Maintain aspect ratio", ev);
            aspect_->setObjectName("maintainAspect");
            f->addRow(QString(), aspect_);
            flicker_ = new QCheckBox("Reduce interlace flicker", ev);
            flicker_->setObjectName("reduceFlicker");
            f->addRow(QString(), flicker_);
            // VEGAS's three resample switches (Edit > Switches): how frames are made when the media rate differs from the project's
            {
                QWidget* rs = new QWidget(ev);
                QVBoxLayout* rv = new QVBoxLayout(rs);
                rv->setContentsMargins(0, 0, 0, 0);
                rv->setSpacing(2);
                const struct { const char* text; const char* id; } modes[] = {
                    {"Use project resample mode", "project"}, {"Force resample", "force"}, {"Optical flow", "optical"}, {"Disable resample", "off"}};
                for (const auto& md : modes) {
                    QRadioButton* r = new QRadioButton(md.text, rs);
                    r->setObjectName(QString("resample_") + md.id);
                    r->setProperty("mode", QString(md.id));
                    rv->addWidget(r);
                    resample_.push_back(r);
                }
                f->addRow("Resample:", rs);
            }
            gain_ = new Spin(ev, 0, 100, 1, 1, "opacity");
            gain_->setSuffix(" %");
            f->addRow("Opacity:", gain_);
            undersample_ = new Spin(ev, 0.01, 1.0, 3, 0.05, "undersampleRate");
            undersample_->setToolTip("Fraction of the project frame rate at which new images are made (1 = every frame)");
            f->addRow("Undersample rate:", undersample_);
        } else {
            gain_ = new Spin(ev, -60, 24, 1, 0.5, "gainDb");
            gain_->setSuffix(" dB");
            gain_->setSpecialValueText("-inf dB");
            f->addRow("Gain:", gain_);
            invert_ = new QCheckBox("Invert phase", ev);
            invert_->setObjectName("invertPhase");
            f->addRow(QString(), invert_);
            {
                QWidget* nr = new QWidget(ev);
                QHBoxLayout* nl = new QHBoxLayout(nr);
                nl->setContentsMargins(0, 0, 0, 0);
                normalize_ = new QCheckBox("Normalize", nr);
                normalize_->setObjectName("normalize");
                nl->addWidget(normalize_);
                normalizeLabel_ = new QLabel(nr);
                normalizeLabel_->setObjectName("normalizeGain");
                nl->addWidget(normalizeLabel_, 1);
                QPushButton* recalc = new QPushButton("Recalculate", nr);
                recalc->setObjectName("recalculate");
                nl->addWidget(recalc);
                f->addRow(QString(), nr);
                // VEGAS 22's Auto Normalize: the level is re-measured when the event is trimmed or split
                autoNormalize_ = new QCheckBox("Auto Normalize", ev);
                autoNormalize_->setObjectName("autoNormalize");
                f->addRow(QString(), autoNormalize_);
                QObject::connect(autoNormalize_, &QCheckBox::toggled, this, [this](bool on) {
                    if (on && !normalize_->isChecked()) normalize_->setChecked(true);
                });
                QObject::connect(recalc, &QPushButton::clicked, this, [this] { measure(); });
                QObject::connect(normalize_, &QCheckBox::toggled, this, [this](bool on) {
                    if (on && !measured_ && !normalizedBefore_) measure();
                    showNormalize();
                });
            }
        }
        locked_ = new QCheckBox("Lock (the event cannot be moved or trimmed)", ev);
        locked_->setObjectName("locked");
        f->addRow(QString(), locked_);
        fadeIn_ = new Spin(ev, 0, 3600, 3, 0.1, "fadeIn");
        fadeIn_->setSuffix(" s");
        fadeOut_ = new Spin(ev, 0, 3600, 3, 0.1, "fadeOut");
        fadeOut_->setSuffix(" s");
        const std::vector<std::pair<QString, QString>> curves = {
            {"Linear", "0"}, {"Fast", "1"}, {"Slow", "2"}, {"Smooth", "3"}, {"Sharp", "4"}};
        fadeInCurve_ = combo(ev, "fadeInCurve", curves);
        fadeOutCurve_ = combo(ev, "fadeOutCurve", curves);
        auto withCurve = [&](Spin* len, QComboBox* curve) {
            QWidget* w = new QWidget(ev);
            QHBoxLayout* h = new QHBoxLayout(w);
            h->setContentsMargins(0, 0, 0, 0);
            h->addWidget(len, 1);
            h->addWidget(curve);
            return w;
        };
        f->addRow("Fade in:", withCurve(fadeIn_, fadeInCurve_));
        f->addRow("Fade out:", withCurve(fadeOut_, fadeOutCurve_));
        if (!video) {
            channels_ = combo(ev, "channels", {{"Both", "both"}, {"Left Only", "left"}, {"Right Only", "right"}, {"Swap", "swap"}, {"Combine", "combine"}});
            f->addRow("Channels:", channels_);
            // a file with more than two channels: which of them the event plays (Channel n / Channels n/m)
            const MediaItem* em = e ? app.project().findMedia(e->mediaId) : nullptr;
            if (em && em->info.channels > 2) {
                mediaChannels_ = new QComboBox(ev);
                mediaChannels_->setObjectName("mediaChannels");
                const MediaInfo& mi = em->info;
                auto nameOf = [&](int c) {
                    return c < (int)mi.channelNames.size() && !mi.channelNames[(size_t)c].empty() ? qs(mi.channelNames[(size_t)c]) : QString();
                };
                mediaChannels_->addItem(QString("All %1 channels, mixed to stereo").arg(mi.channels), "-1:0");
                for (int c = 0; c < mi.channels && c < 64; ++c) {
                    const QString n = nameOf(c);
                    mediaChannels_->addItem(QString("Channel %1%2").arg(c + 1).arg(n.isEmpty() ? QString() : " (" + n + ")"), QString("%1:1").arg(c));
                }
                for (int c = 0; c + 1 < mi.channels && c < 64; c += 2) {
                    const QString a = nameOf(c), b = nameOf(c + 1);
                    mediaChannels_->addItem(QString("Channels %1/%2%3").arg(c + 1).arg(c + 2).arg(a.isEmpty() || b.isEmpty() ? QString() : " (" + a + "/" + b + ")"),
                                            QString("%1:2").arg(c));
                }
                f->addRow("Media channels:", mediaChannels_);
            }
            buildStretch(ev, f);
        }
        if (e) {
            const double fps = app.project().settings.fps();
            f->addRow("Start:", new QLabel(qs(timecode(e->start, fps)), ev));
            f->addRow("Length:", new QLabel(qs(timecode(e->length, fps)), ev));
        }
        tabs->addTab(ev, video ? "Video Event" : "Audio Event");

        QWidget* mt = new QWidget(tabs);
        QFormLayout* mf = new QFormLayout(mt);
        auto info = [&](const QString& label, const QString& value) {
            QLabel* l = new QLabel(value, mt);
            l->setTextInteractionFlags(Qt::TextSelectableByMouse);
            l->setWordWrap(true);
            mf->addRow(label, l);
        };
        if (m) {
            info("Name:", qs(m->name));
            info("File:", m->generated ? QString("(generated by %1)").arg(qs(m->generator.name)) : QDir::toNativeSeparators(qs(m->path)));
            if (m->offline) info("Status:", "Offline: the file could not be found");
            if (m->info.hasVideo)
                info("Video:", QString("%1 x %2%3%4").arg(m->info.width).arg(m->info.height)
                                   .arg(m->info.isStill ? QString(", still picture") : QString(", %1 fps").arg(m->info.fps, 0, 'f', 3))
                                   .arg(m->info.videoCodec.empty() ? QString() : ", " + qs(m->info.videoCodec)));
            if (m->info.hasAudio)
                info("Audio:", QString("%1 Hz, %2 channel%3%4").arg(m->info.sampleRate).arg(m->info.channels).arg(m->info.channels == 1 ? "" : "s")
                                   .arg(m->info.audioCodec.empty() ? QString() : ", " + qs(m->info.audioCodec)));
            if (m->info.hasVideo && m->fieldOrder() != "none")
                info("Field order:", m->fieldOrder() == "upper" ? "Upper field first (interlaced)" : "Lower field first (interlaced)");
            if (m->info.duration > 0) info("Length:", qs(timecode(m->info.duration, app.project().settings.fps())));
            if (e) info("Starts at:", qs(timecode(e->offset, app.project().settings.fps())) + " into the media");
            if (m->info.hasVideo && !m->generated && AppState::get().showMediaProperties) {
                QPushButton* mp = new QPushButton("Field Order, Pixel Aspect Ratio and Rotation...", mt);
                mp->setObjectName("mediaProperties");
                const uint64_t mid = m->id;
                QObject::connect(mp, &QPushButton::clicked, this, [mid] {
                    AppState::get().showMediaProperties(mid);
                });
                mf->addRow(mp);
            }
        } else {
            info("Media:", "(none)");
        }
        tabs->addTab(mt, "Media");

        QHBoxLayout* buttons = new QHBoxLayout();
        buttons->addStretch(1);
        QPushButton* ok = new QPushButton("OK", this);
        ok->setObjectName("ok");
        ok->setDefault(true);
        QPushButton* cancel = new QPushButton("Cancel", this);
        cancel->setObjectName("cancel");
        buttons->addWidget(ok);
        buttons->addWidget(cancel);
        root->addLayout(buttons);
        QObject::connect(ok, &QPushButton::clicked, this, [this] { apply(); accept(); });
        QObject::connect(cancel, &QPushButton::clicked, this, [this] { reject(); });

        if (e) {
            name_->setText(qs(e->name));
            rate_->setValue(e->rate);
            loop_->setChecked(e->loop);
            reverse_->setChecked(e->reverse);
            if (aspect_) aspect_->setChecked(e->keepAspect);
            if (undersample_) undersample_->setValue(e->undersample);
            if (video) gain_->setValue(std::min(1.0, std::max(0.0, e->gain)) * 100.0);
            else gain_->setValue(e->gain > 0 ? std::max(-60.0, 20.0 * std::log10(e->gain)) : -60.0);
            if (pitch_) pitch_->setValue(e->pitch);
            if (lockPitch_) lockPitch_->setChecked(!e->keepPitch);
            if (invert_) invert_->setChecked(e->invertPhase);
            locked_->setChecked(e->locked);
            mute_->setChecked(e->mute);
            if (flicker_) flicker_->setChecked(e->reduceFlicker);
            for (QRadioButton* r : resample_) {
                const std::string now = (e->resample == "force" || e->resample == "off" || e->resample == "optical") ? e->resample : std::string("project");
                r->setChecked(r->property("mode").toString() == qs(now));
            }
            if (normalize_) {
                normalizeGain_ = e->normalizeGain;
                normalizedBefore_ = e->normalize;
                QSignalBlocker b(normalize_);
                normalize_->setChecked(e->normalize);
                showNormalize();
                if (autoNormalize_) autoNormalize_->setChecked(e->autoNormalize);
            }
            if (method_) {
                ownPitch_ = e->pitch;
                pickData(method_, qs(e->stretchMethod));
                fillModes(qs(e->stretchMode));
                preserve_->setChecked(e->preserveFormants);
                formant_->setValue(e->formant);
                updateStretch();
            }
            fadeIn_->setValue(e->fadeIn);
            fadeOut_->setValue(e->fadeOut);
            // a curve the list does not offer (a hold, from elsewhere) is kept
            if (fadeInCurve_->findData(QString::number(e->fadeInCurve)) < 0) fadeInCurve_->addItem("Hold", QString::number(e->fadeInCurve));
            if (fadeOutCurve_->findData(QString::number(e->fadeOutCurve)) < 0) fadeOutCurve_->addItem("Hold", QString::number(e->fadeOutCurve));
            pickData(fadeInCurve_, QString::number(e->fadeInCurve));
            pickData(fadeOutCurve_, QString::number(e->fadeOutCurve));
            if (channels_) pickData(channels_, qs(e->channels.empty() ? std::string("both") : e->channels));
            if (mediaChannels_) {
                const QString key = e->mediaChannel < 0 ? QString("-1:0") : QString("%1:%2").arg(e->mediaChannel).arg(e->mediaChannels >= 2 ? 2 : 1);
                if (mediaChannels_->findData(key) < 0) mediaChannels_->addItem(QString("Channels from %1").arg(e->mediaChannel + 1), key);
                pickData(mediaChannels_, key);
            }
        }
    }

private:
    void apply()
    {
        AppState& app = AppState::get();
        const Event* e = app.project().findEvent(eventId_);
        if (!e) return;
        Event want = *e;
        want.name = s8(name_->text());
        want.rate = rate_->value();
        want.loop = loop_->isChecked();
        want.reverse = reverse_->isChecked();
        if (aspect_) {
            want.keepAspect = aspect_->isChecked();
            want.gain = gain_->value() / 100.0;
        } else {
            want.gain = gain_->value() <= -60.0 ? 0.0 : std::pow(10.0, gain_->value() / 20.0);
            if (std::fabs(gain_->value() - (e->gain > 0 ? 20.0 * std::log10(e->gain) : -60.0)) < 0.05) want.gain = e->gain;
        }
        if (aspect_ && std::fabs(want.gain - e->gain) < 0.0005) want.gain = e->gain;
        if (pitch_) want.pitch = lockPitch_ && lockPitch_->isChecked() ? 0.0 : pitch_->value();
        if (lockPitch_) want.keepPitch = !lockPitch_->isChecked();
        if (invert_) want.invertPhase = invert_->isChecked();
        want.locked = locked_->isChecked();
        want.mute = mute_->isChecked();
        if (flicker_) want.reduceFlicker = flicker_->isChecked();
        for (QRadioButton* r : resample_)
            if (r->isChecked()) {
                const std::string mode = s8(r->property("mode").toString());
                // "project" is any setting other than the two forced ones: what was there is kept
                if (mode == "project") want.resample = (e->resample == "force" || e->resample == "off" || e->resample == "optical") ? std::string("project") : e->resample;
                else want.resample = mode;
            }
        if (normalize_) {
            want.normalize = normalize_->isChecked();
            want.normalizeGain = want.normalize ? normalizeGain_ : 1.0;
            if (!want.normalize && !e->normalize) want.normalizeGain = e->normalizeGain;
            want.autoNormalize = want.normalize && autoNormalize_ && autoNormalize_->isChecked();
        }
        if (method_) {
            want.stretchMethod = s8(method_->currentData().toString());
            if (want.stretchMethod != "none") want.stretchMode = s8(mode_->currentData().toString());
            if (want.stretchMethod == "none") {
                // None: no stretching and no pitch change; what was set for the others stays
                want.keepPitch = e->keepPitch;
                want.pitch = e->pitch;
            } else if (lockPitch_->isChecked() && e->stretchMethod == want.stretchMethod && !e->keepPitch) {
                want.pitch = e->pitch;
            }
            want.preserveFormants = preserve_->isChecked();
            want.formant = formant_->value();
            if (std::fabs(want.formant - e->formant) < 0.0005) want.formant = e->formant;
            if (std::fabs(want.pitch - e->pitch) < 0.0005) want.pitch = e->pitch;
            // a pitch past the box's +-24 (from the Shift+= keys) shows as 24; left as it was unless changed
            if (pitch_ && std::fabs(want.pitch - std::max(pitch_->minimum(), std::min(pitch_->maximum(), e->pitch))) < 0.0005) want.pitch = e->pitch;
        }
        if (undersample_) {
            want.undersample = std::min(1.0, std::max(0.01, undersample_->value()));
            if (std::fabs(want.undersample - e->undersample) < 0.0005) want.undersample = e->undersample;
        }
        want.fadeIn = std::min(fadeIn_->value(), e->length);
        want.fadeOut = std::min(fadeOut_->value(), e->length);
        if (std::fabs(want.fadeIn - e->fadeIn) < 0.0005) want.fadeIn = e->fadeIn;
        if (std::fabs(want.fadeOut - e->fadeOut) < 0.0005) want.fadeOut = e->fadeOut;
        if (std::fabs(want.rate - e->rate) < 0.0005) want.rate = e->rate;
        want.fadeInCurve = fadeInCurve_->currentData().toInt();
        want.fadeOutCurve = fadeOutCurve_->currentData().toInt();
        if (channels_) want.channels = s8(channels_->currentData().toString());
        if (mediaChannels_) {
            const QStringList fc = mediaChannels_->currentData().toString().split(':');
            want.mediaChannel = fc.size() == 2 ? fc[0].toInt() : -1;
            want.mediaChannels = want.mediaChannel < 0 ? 0 : (fc.size() == 2 && fc[1].toInt() >= 2 ? 2 : 1);
        }
        if (want.name == e->name && want.rate == e->rate && want.loop == e->loop && want.reverse == e->reverse && want.keepAspect == e->keepAspect &&
            want.gain == e->gain && want.pitch == e->pitch && want.keepPitch == e->keepPitch && want.fadeIn == e->fadeIn && want.fadeOut == e->fadeOut &&
            want.undersample == e->undersample &&
            want.fadeInCurve == e->fadeInCurve && want.fadeOutCurve == e->fadeOutCurve && want.channels == e->channels &&
            want.mediaChannel == e->mediaChannel && want.mediaChannels == e->mediaChannels &&
            want.locked == e->locked && want.invertPhase == e->invertPhase && want.stretchMethod == e->stretchMethod && want.stretchMode == e->stretchMode &&
            want.preserveFormants == e->preserveFormants && want.formant == e->formant && want.mute == e->mute &&
            want.reduceFlicker == e->reduceFlicker && want.resample == e->resample && want.normalize == e->normalize &&
            want.normalizeGain == e->normalizeGain && want.autoNormalize == e->autoNormalize)
            return;
        const uint64_t id = eventId_;
        app.edit("Event Properties", [id, want](Project& p) {
            Event* ev = p.findEvent(id);
            if (!ev) return;
            // a new playback rate also goes to events grouped with this one that played at its old rate, so a clip's
            // image and sound stay in step
            if (want.rate != ev->rate && ev->group)
                for (Track& t : p.tracks)
                    for (Event& o : t.events)
                        if (o.id != id && o.group == ev->group && std::fabs(o.rate - ev->rate) < 1e-9) o.rate = want.rate;
            ev->name = want.name;
            ev->rate = want.rate;
            ev->loop = want.loop;
            ev->reverse = want.reverse;
            ev->keepAspect = want.keepAspect;
            ev->gain = want.gain;
            ev->pitch = want.pitch;
            ev->keepPitch = want.keepPitch;
            ev->fadeIn = want.fadeIn;
            ev->fadeOut = want.fadeOut;
            ev->undersample = want.undersample;
            ev->fadeInCurve = want.fadeInCurve;
            ev->fadeOutCurve = want.fadeOutCurve;
            ev->channels = want.channels;
            ev->mediaChannel = want.mediaChannel;
            ev->mediaChannels = want.mediaChannels;
            ev->locked = want.locked;
            ev->invertPhase = want.invertPhase;
            ev->stretchMethod = want.stretchMethod;
            ev->stretchMode = want.stretchMode;
            ev->preserveFormants = want.preserveFormants;
            ev->formant = want.formant;
            ev->mute = want.mute;
            ev->reduceFlicker = want.reduceFlicker;
            ev->resample = want.resample;
            ev->normalize = want.normalize;
            ev->normalizeGain = want.normalizeGain;
            ev->autoNormalize = want.autoNormalize;
        });
    }

    void measure()
    {
        AppState& app = AppState::get();
        const Event* e = app.project().findEvent(eventId_);
        if (!e) return;
        const double peak = app.eventPeak(*e);
        normalizeGain_ = peak > 1e-6 ? std::min(1000.0, AppState::normalizeLevel() / peak) : 1.0;
        measured_ = true;
        showNormalize();
    }
    void showNormalize()
    {
        if (!normalizeLabel_) return;
        const double db = 20.0 * std::log10(std::max(1e-9, normalizeGain_));
        normalizeLabel_->setText(QString("Gain: %1%2 dB").arg(db >= 0 ? "+" : "").arg(db, 0, 'f', 2));
        normalizeLabel_->setEnabled(normalize_->isChecked());
    }

    // VEGAS's "Time stretch / pitch shift" group: method (None, Classic, elastique), stretch attributes (Classic's
    // presets or elastique's modes), Lock to stretch, pitch change, elastique's formants.
    void buildStretch(QWidget* ev, QFormLayout* f)
    {
        QGroupBox* g = new QGroupBox("Time stretch / pitch shift", ev);
        g->setObjectName("timeStretch");
        QFormLayout* gf = new QFormLayout(g);
        method_ = combo(g, "stretchMethod", {{"None", "none"}, {"Classic", "classic"}, {QString::fromUtf8("\xc3\xa9lastique"), "elastique"}});
        gf->addRow("Method:", method_);
        mode_ = new QComboBox(g);
        mode_->setObjectName("stretchMode");
        gf->addRow("Stretch attributes:", mode_);
        lockPitch_ = new QCheckBox("Lock to stretch (the pitch follows the playback rate)", g);
        lockPitch_->setObjectName("lockToRate");
        gf->addRow(QString(), lockPitch_);
        pitch_ = new Spin(g, -24, 24, 3, 0.5, "pitch");
        pitch_->setSuffix(" semitones");
        gf->addRow("Pitch change:", pitch_);
        preserve_ = new QCheckBox("Preserve formants", g);
        preserve_->setObjectName("preserveFormants");
        gf->addRow(QString(), preserve_);
        formant_ = new Spin(g, -12, 12, 3, 0.5, "formantShift");
        formant_->setSuffix(" semitones");
        formant_->setToolTip("Shifts the voice character (formants) without changing pitch: élastique Pro with Preserve formants");
        gf->addRow("Formant shift:", formant_);
        f->addRow(g);
        QObject::connect(method_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { fillModes(QString()); updateStretch(); });
        QObject::connect(mode_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { updateStretch(); });
        QObject::connect(lockPitch_, &QCheckBox::toggled, this, [this] { updateStretch(); });
        QObject::connect(preserve_, &QCheckBox::toggled, this, [this] { updateStretch(); });
        // what the window itself shows while the pitch is locked comes with its signals blocked
        QObject::connect(pitch_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) { ownPitch_ = v; });
        QObject::connect(rate_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] { updateStretch(); });
    }

    // the stretch attributes the method has ('want': which to pick; empty: VEGAS's default for the method)
    void fillModes(const QString& want)
    {
        const QString method = method_->currentData().toString();
        QSignalBlocker b(mode_);
        mode_->clear();
        if (method == "classic") {
            for (const auto& pr : classicPresets()) mode_->addItem(QString::fromUtf8(pr.name), QString::fromUtf8(pr.id));
            pickData(mode_, want.isEmpty() ? QString("A03") : want);
        } else if (method == "elastique") {
            for (const auto& m : elastiqueModes()) mode_->addItem(qs(m.second), qs(m.first));
            pickData(mode_, want.isEmpty() ? QString("pro") : want);
        }
    }

    void updateStretch()
    {
        const QString method = method_->currentData().toString(), mode = mode_->currentData().toString();
        const bool none = method == "none";
        mode_->setEnabled(!none);
        lockPitch_->setEnabled(!none);
        const bool locked = none || lockPitch_->isChecked();
        {
            // locked to the rate (or None): the pitch shown is the rate's own and cannot be set
            QSignalBlocker b(pitch_);
            pitch_->setValue(locked ? 12.0 * std::log2(std::max(0.001, rate_->value())) : ownPitch_);
        }
        pitch_->setEnabled(!locked);
        const bool formants = method == "elastique" && (mode == "pro" || mode == "soloist-mono");
        preserve_->setEnabled(formants);
        formant_->setEnabled(method == "elastique" && mode == "pro" && preserve_->isChecked());
    }

    uint64_t eventId_;
    QLineEdit* name_ = nullptr;
    Spin *rate_ = nullptr, *gain_ = nullptr, *pitch_ = nullptr, *fadeIn_ = nullptr, *fadeOut_ = nullptr, *undersample_ = nullptr;
    QCheckBox *loop_ = nullptr, *reverse_ = nullptr, *aspect_ = nullptr, *lockPitch_ = nullptr, *invert_ = nullptr, *locked_ = nullptr,
              *preserve_ = nullptr;
    QComboBox *fadeInCurve_ = nullptr, *fadeOutCurve_ = nullptr, *channels_ = nullptr, *method_ = nullptr, *mode_ = nullptr;
    QComboBox* mediaChannels_ = nullptr;
    Spin* formant_ = nullptr;
    QCheckBox *mute_ = nullptr, *flicker_ = nullptr, *normalize_ = nullptr;
    std::vector<QRadioButton*> resample_;
    QLabel* normalizeLabel_ = nullptr;
    double normalizeGain_ = 1.0;
    QCheckBox* autoNormalize_ = nullptr;
    bool measured_ = false, normalizedBefore_ = false;
    double ownPitch_ = 0;
};

// ================================================================================== Project Properties
struct Rate { const char* label; int num, den; };
const Rate kRates[] = {
    // VEGAS 18's names as its list shows them; rates it has no name for follow
    {"12.000 (Half Film)", 12, 1}, {"12.500 (Half PAL)", 25, 2}, {"14.985 (Half NTSC)", 15000, 1001}, {"15.000 (Multimedia)", 15, 1},
    {"23.976 (IVTC Film)", 24000, 1001}, {"24.000 (Film)", 24, 1}, {"25.000 (PAL)", 25, 1}, {"29.970 (NTSC)", 30000, 1001},
    {"30.000", 30, 1}, {"48.000", 48, 1}, {"50.000 (Double PAL)", 50, 1}, {"59.940 (Double NTSC)", 60000, 1001},
    {"60.000", 60, 1}, {"100.000", 100, 1}, {"119.880", 120000, 1001}, {"120.000", 120, 1},
};

struct Template { const char* label; int w, h, num, den; double par; const char* field; };
const Template kTemplates[] = {
    {"HD 1080-24p (1920x1080, 23.976 fps)", 1920, 1080, 24000, 1001, 1.0, "none"},
    {"HD 1080-25p (1920x1080, 25.000 fps)", 1920, 1080, 25, 1, 1.0, "none"},
    {"HD 1080-30p (1920x1080, 29.970 fps)", 1920, 1080, 30000, 1001, 1.0, "none"},
    {"HD 1080-50p (1920x1080, 50.000 fps)", 1920, 1080, 50, 1, 1.0, "none"},
    {"HD 1080-60p (1920x1080, 59.940 fps)", 1920, 1080, 60000, 1001, 1.0, "none"},
    {"HD 1080-60i (1920x1080, 29.970 fps)", 1920, 1080, 30000, 1001, 1.0, "upper"},
    {"HD 1080-50i (1920x1080, 25.000 fps)", 1920, 1080, 25, 1, 1.0, "upper"},
    {"HD 720-30p (1280x720, 29.970 fps)", 1280, 720, 30000, 1001, 1.0, "none"},
    {"HD 720-60p (1280x720, 59.940 fps)", 1280, 720, 60000, 1001, 1.0, "none"},
    {"UHD 2160-24p (3840x2160, 23.976 fps)", 3840, 2160, 24000, 1001, 1.0, "none"},
    {"UHD 2160-30p (3840x2160, 29.970 fps)", 3840, 2160, 30000, 1001, 1.0, "none"},
    {"UHD 2160-60p (3840x2160, 59.940 fps)", 3840, 2160, 60000, 1001, 1.0, "none"},
    {"4K DCI 24p (4096x2160, 24.000 fps)", 4096, 2160, 24, 1, 1.0, "none"},
    {"Vertical HD (1080x1920, 30.000 fps)", 1080, 1920, 30, 1, 1.0, "none"},
    {"Square (1080x1080, 30.000 fps)", 1080, 1080, 30, 1, 1.0, "none"},
    {"NTSC DV (720x480, 29.970 fps)", 720, 480, 30000, 1001, 0.9091, "lower"},
    {"NTSC DV Widescreen (720x480, 29.970 fps)", 720, 480, 30000, 1001, 1.2121, "lower"},
    {"NTSC DV 24p (720x480, 23.976 fps)", 720, 480, 24000, 1001, 0.9091, "none"},
    {"PAL DV (720x576, 25.000 fps)", 720, 576, 25, 1, 1.0926, "lower"},
    {"PAL DV Widescreen (720x576, 25.000 fps)", 720, 576, 25, 1, 1.4568, "lower"},
    // the other templates in VEGAS 18's list: 720p at 50, QFHD / 2K / 4K 16:9 at 24, the four 360 cameras, HDV
    {"HD 720-50p (1280x720, 50.000 fps)", 1280, 720, 50, 1, 1.0, "none"},
    {"QFHD 24p (3840x2160, 24.000 fps)", 3840, 2160, 24, 1, 1.0, "none"},
    {"2K 16:9 24p (2048x1152, 24.000 fps)", 2048, 1152, 24, 1, 1.0, "none"},
    {"4K 16:9 24p (4096x2304, 24.000 fps)", 4096, 2304, 24, 1, 1.0, "none"},
    {"360 Samsung Gear (4096x2048, 24.000 fps)", 4096, 2048, 24, 1, 1.0, "none"},
    {"360 GoPro Fusion (3000x1504, 60.000 fps)", 3000, 1504, 60, 1, 1.0, "none"},
    {"360 YI VR (3840x1920, 30.000 fps)", 3840, 1920, 30, 1, 1.0, "none"},
    {"360 Ricoh Theta V (3840x1920, 29.970 fps)", 3840, 1920, 30000, 1001, 1.0, "none"},
    {"HDV 720-30p (1280x720, 29.970 fps)", 1280, 720, 30000, 1001, 1.0, "none"},
    {"HDV 720-25p (1280x720, 25.000 fps)", 1280, 720, 25, 1, 1.0, "none"},
    {"HDV 720-24p (1280x720, 23.976 fps)", 1280, 720, 24000, 1001, 1.0, "none"},
    {"HDV 1080-60i (1440x1080, 29.970 fps)", 1440, 1080, 30000, 1001, 1.3333, "upper"},
    {"HDV 1080-50i (1440x1080, 25.000 fps)", 1440, 1080, 25, 1, 1.3333, "upper"},
    {"HDV 1080-24p (1440x1080, 23.976 fps)", 1440, 1080, 24000, 1001, 1.3333, "none"},
};

struct Par { const char* label; double v; };
const Par kPars[] = {
    {"1.0000 (Square)", 1.0}, {"0.9091 (NTSC DV)", 0.9091}, {"1.2121 (NTSC DV Widescreen)", 1.2121}, {"1.0926 (PAL DV)", 1.0926},
    {"1.4568 (PAL DV Widescreen)", 1.4568}, {"1.3333 (HDV 1080)", 1.3333}, {"2.0000 (Anamorphic 2:1)", 2.0},
};

// A folder box with Browse... and the free space in it underneath (VEGAS's folder rows in Project Properties).
QWidget* folderRow(QWidget* parent, const char* name, const QString& value, const QString& placeholder, QLineEdit** edit)
{
    QWidget* w = new QWidget(parent);
    QVBoxLayout* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    QHBoxLayout* h = new QHBoxLayout();
    QLineEdit* e = new QLineEdit(QDir::toNativeSeparators(value), w);
    e->setObjectName(name);
    e->setPlaceholderText(placeholder);
    QPushButton* browse = new QPushButton("Browse...", w);
    h->addWidget(e, 1);
    h->addWidget(browse);
    v->addLayout(h);
    QLabel* space = new QLabel(w);
    space->setObjectName(QString(name) + "Space");
    v->addWidget(space);
    auto showSpace = [e, space] {
        const QString d = QDir::fromNativeSeparators(e->text().trimmed());
        const QStorageInfo st(d.isEmpty() ? QDir::homePath() : d);
        space->setText(st.isValid() && st.bytesAvailable() > 0 ? QString("Free storage space in selected folder: %1").arg(st.bytesAvailable() >= (1LL << 30) ? QString("%1 Gigabytes").arg(st.bytesAvailable() / double(1LL << 30), 0, 'f', 1) : QString("%1 Megabytes").arg(st.bytesAvailable() / (1024 * 1024)))
                                                               : QString("Free storage space in selected folder: -"));
    };
    showSpace();
    QObject::connect(e, &QLineEdit::textChanged, w, [showSpace](const QString&) { showSpace(); });
    QObject::connect(browse, &QPushButton::clicked, w, [e, w] {
        const QString d = QFileDialog::getExistingDirectory(w->window(), "Folder", QDir::fromNativeSeparators(e->text()));
        if (!d.isEmpty()) e->setText(QDir::toNativeSeparators(d));
    });
    *edit = e;
    return w;
}

// VEGAS 18's Master Display > Customize (Project Properties > Video): the mastering display's colour primaries and white
// point (CIE 1931 x,y), its luminance range, and the programme's content light levels (MaxCLL / MaxFALL). Rendering an
// HDR image writes them into the file (ST 2086 and content light level metadata). The dialog layout is Reno's. True when
// OK was pressed and 'md' changed.
static bool editMasterDisplay(QWidget* parent, MasterDisplay& md)
{
    QDialog dlg(parent);
    dlg.setObjectName("masterDisplayDialog");
    dlg.setWindowTitle("Master Display");
    auto* v = new QVBoxLayout(&dlg);
    auto* grid = new QGridLayout();
    grid->addWidget(new QLabel("x", &dlg), 0, 1);
    grid->addWidget(new QLabel("y", &dlg), 0, 2);
    struct Row { const char* label; const char* name; double* x; double* y; };
    MasterDisplay m = md;
    const Row rows[] = {{"Red primary:", "red", &m.redX, &m.redY}, {"Green primary:", "green", &m.greenX, &m.greenY},
                        {"Blue primary:", "blue", &m.blueX, &m.blueY}, {"White point:", "white", &m.whiteX, &m.whiteY}};
    std::vector<std::pair<QDoubleSpinBox*, double*>> bound;
    int r = 1;
    for (const Row& row : rows) {
        grid->addWidget(new QLabel(row.label, &dlg), r, 0);
        auto* sx = new Spin(&dlg, 0.0, 1.0, 4, 0.001, (std::string(row.name) + "X").c_str());
        auto* sy = new Spin(&dlg, 0.0, 1.0, 4, 0.001, (std::string(row.name) + "Y").c_str());
        sx->setValue(*row.x);
        sy->setValue(*row.y);
        grid->addWidget(sx, r, 1);
        grid->addWidget(sy, r, 2);
        bound.push_back({sx, row.x});
        bound.push_back({sy, row.y});
        ++r;
    }
    auto* maxL = new Spin(&dlg, 0.0001, 10000.0, 1, 10.0, "maxNits");
    auto* minL = new Spin(&dlg, 0.0, 1000.0, 4, 0.001, "minNits");
    maxL->setValue(m.maxNits);
    minL->setValue(m.minNits);
    grid->addWidget(new QLabel("Maximum luminance (cd/m\u00B2):", &dlg), r, 0);
    grid->addWidget(maxL, r, 1);
    grid->addWidget(new QLabel("Minimum luminance (cd/m\u00B2):", &dlg), r + 1, 0);
    grid->addWidget(minL, r + 1, 1);
    auto* cll = new QSpinBox(&dlg);
    cll->setObjectName("maxCll");
    cll->setRange(0, 65535);
    cll->setValue(m.maxCll);
    auto* fall = new QSpinBox(&dlg);
    fall->setObjectName("maxFall");
    fall->setRange(0, 65535);
    fall->setValue(m.maxFall);
    grid->addWidget(new QLabel("Maximum content light level (MaxCLL):", &dlg), r + 2, 0);
    grid->addWidget(cll, r + 2, 1);
    grid->addWidget(new QLabel("Maximum frame-average light level (MaxFALL):", &dlg), r + 3, 0);
    grid->addWidget(fall, r + 3, 1);
    v->addLayout(grid);
    v->addWidget(note("Written into HDR renders (HDR10 or HLG projects rendered through an HDR view transform). 0 leaves a level out.", &dlg));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    v->addWidget(buttons);
    buttons->button(QDialogButtonBox::Ok)->setObjectName("ok");
    buttons->button(QDialogButtonBox::Cancel)->setObjectName("cancel");
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, [&dlg] { dlg.accept(); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, [&dlg] { dlg.reject(); });
    if (dlg.exec() != QDialog::Accepted) return false;
    for (auto& b : bound) *b.second = b.first->value();
    m.maxNits = maxL->value();
    m.minNits = std::min(minL->value(), m.maxNits);
    m.maxCll = cll->value();
    m.maxFall = fall->value();
    m.preset = "custom";
    const bool changed = m != md;
    md = m;
    return changed;
}

class ProjectPropertiesDialog : public QDialog {
public:
    explicit ProjectPropertiesDialog(QWidget* parent) : QDialog(parent)
    {
        setObjectName("projectPropertiesDialog");
        setWindowTitle("Project Properties");
        setMinimumWidth(560);
        resize(600, 700);
        const ProjectSettings& s = AppState::get().project().settings;
        QVBoxLayout* root = new QVBoxLayout(this);
        tabs_ = new QTabWidget(this);
        tabs_->setObjectName("tabs");
        root->addWidget(tabs_);

        QWidget* vt = new QWidget(tabs_);
        QFormLayout* vf = new QFormLayout(vt);
        template_ = new QComboBox(vt);
        template_->setObjectName("template");
        fillTemplates();
        {
            // Save Template and Delete Template buttons beside the list, and Match Media Settings
            QWidget* row = new QWidget(vt);
            QHBoxLayout* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            h->addWidget(template_, 1);
            QPushButton* saveT = new QPushButton("Save Template...", row);
            saveT->setObjectName("saveTemplate");
            saveT->setToolTip("Keeps these video settings as a template of your own, under a name");
            deleteT_ = new QPushButton("Delete", row);
            deleteT_->setObjectName("deleteTemplate");
            deleteT_->setToolTip("Deletes the template of your own chosen in the list");
            QPushButton* match = new QPushButton("Match Media Settings...", row);
            match->setObjectName("matchMediaSettings");
            match->setToolTip("Sets these video settings to match a media file you choose");
            h->addWidget(saveT);
            h->addWidget(deleteT_);
            h->addWidget(match);
            vf->addRow("Template:", row);
            QObject::connect(saveT, &QPushButton::clicked, this, [this] { saveTemplate(); });
            QObject::connect(deleteT_, &QPushButton::clicked, this, [this] { deleteTemplate(); });
            QObject::connect(match, &QPushButton::clicked, this, [this] { matchMediaFile(); });
        }
        width_ = new QSpinBox(vt);
        width_->setObjectName("width");
        width_->setRange(16, 8192);
        height_ = new QSpinBox(vt);
        height_->setObjectName("height");
        height_->setRange(16, 8192);
        QHBoxLayout* size = new QHBoxLayout();
        size->addWidget(width_);
        size->addWidget(new QLabel("x", vt));
        size->addWidget(height_);
        size->addStretch(1);
        vf->addRow("Width / height:", size);
        field_ = combo(vt, "fieldOrder", {{"None (progressive scan)", "none"}, {"Upper field first", "upper"}, {"Lower field first", "lower"}});
        vf->addRow("Field order:", field_);
        vf->addRow(QString(), note("Rendering an interlaced project draws each field at its own moment.", vt));
        par_ = new QComboBox(vt);
        par_->setObjectName("pixelAspect");
        for (const Par& p : kPars) par_->addItem(p.label, p.v);
        vf->addRow("Pixel aspect ratio:", par_);
        hdr_ = combo(vt, "hdrMode", {{"Off", "off"}, {"HDR10", "hdr10"}, {"HLG", "hlg"}});
        hdr_->setToolTip("HDR10 or HLG sets the pixel format to 32-bit floating point (full range) and the matching view transform; Off restores them. "
                         "Renders through an HDR view transform are tagged as HDR and carry the Master Display. Reno has no HDR preview.");
        vf->addRow("HDR Mode:", hdr_);
        rate_ = new QComboBox(vt);
        rate_->setObjectName("frameRate");
        for (const Rate& r : kRates) rate_->addItem(r.label);
        rate_->addItem("Custom...");
        vf->addRow("Frame rate:", rate_);
        num_ = new QSpinBox(vt);
        num_->setObjectName("fpsNum");
        num_->setRange(1, 10000000);
        den_ = new QSpinBox(vt);
        den_->setObjectName("fpsDen");
        den_->setRange(1, 1000000);
        QHBoxLayout* frac = new QHBoxLayout();
        frac->addWidget(num_);
        frac->addWidget(new QLabel("/", vt));
        frac->addWidget(den_);
        rateText_ = new QLabel(vt);
        frac->addWidget(rateText_);
        frac->addStretch(1);
        vf->addRow("Exact rate:", frac);
        stereo_ = combo(vt, "stereo3d", {{"Off", "off"}, {"Side by side (half)", "sbshalf"}, {"Side by side (full)", "sbsfull"}, {"Top/bottom (half)", "tbhalf"},
                                         {"Top/bottom (full)", "tbfull"}, {"Line alternate", "line"}, {"Checkerboard", "checker"},
                                         {"Anaglyphic (red/cyan)", "anaglyph-rc"}, {"Anaglyphic (amber/blue)", "anaglyph-ab"},
                                         {"Anaglyphic (green/magenta)", "anaglyph-gm"}, {"Left only", "left"}, {"Right only", "right"},
                                         {"Blend", "blend"}, {"Difference", "difference"}});
        stereo_->setToolTip("How the left- and right-eye pictures are put together in the output");
        vf->addRow("Stereoscopic 3D mode:", stereo_);
        {
            QWidget* row = new QWidget(vt);
            QHBoxLayout* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            stereoSwap_ = new QCheckBox("Swap Left/Right", row);
            stereoSwap_->setObjectName("stereoSwap");
            crosstalk_ = new Spin(row, 0, 1, 3, 0.01, "crosstalk");
            h->addWidget(stereoSwap_);
            h->addSpacing(12);
            h->addWidget(new QLabel("Crosstalk cancellation:", row));
            h->addWidget(crosstalk_);
            h->addStretch(1);
            vf->addRow(QString(), row);
            QObject::connect(stereo_, &QComboBox::currentIndexChanged, this, [this](int) {
                const bool on = stereo_->currentData().toString() != "off";
                stereoSwap_->setEnabled(on);
                crosstalk_->setEnabled(on);
            });
        }
        out360_ = new QCheckBox("360\u00B0 Output", vt);
        out360_->setObjectName("output360");
        out360_->setToolTip("A 360\u00B0 (equirectangular) video: enables the Video Preview 360\u00B0 mode, and Render As marks files as 360\u00B0");
        vf->addRow(QString(), out360_);
        // VEGAS 18's four pixel formats: depth and range (Reno decodes media to full range in all of them)
        format_ = combo(vt, "pixelFormat", {{"Legacy 8-bit (video levels)", "8bit|video"}, {"8-bit (full range)", "8bit|full"},
                                            {"32-bit floating point (video levels)", "32bit|video"}, {"32-bit floating point (full range)", "32bit|full"}});
        vf->addRow("Pixel format:", format_);
        vf->addRow(QString(), note("32-bit floating point keeps the whole picture's precision between effects and compositing "
                                   "(nothing is clipped until the end); effects that take floating point pictures are given them. "
                                   "Reno reads every file to full-range RGB, whichever range is chosen.", vt));
        gamma_ = combo(vt, "compositingGamma", {{"1.000 (Linear)", "1.000"}, {"2.222 (Video)", "2.222"}});
        gamma_->setToolTip("Available with 32-bit floating point (full range), as in VEGAS: 1.000 (Linear) composites in linear light, 2.222 (Video) as before");
        vf->addRow("Compositing gamma:", gamma_);
        acesVersion_ = combo(vt, "acesVersion", {{"ACES 1.0", "1.0"}, {"ACES 1.1", "1.1"}, {"ACES 0.7", "0.7"}});
        vf->addRow("ACES version:", acesVersion_);
        acesSpace_ = combo(vt, "acesColorSpace", {{"Default (ACES2065-1)", "aces2065-1"}, {"ACEScg", "acescg"}, {"ACEScc", "acescc"}, {"ACEScct", "acescct"}});
        vf->addRow("ACES color space:", acesSpace_);
        // the View transforms the frame is shown and rendered through (colormgmt.h's, for the ACES version chosen)
        view_ = new QComboBox(vt);
        view_->setObjectName("viewTransform");
        fillViews_ = [this](const QString& keep) {
            const QSignalBlocker b(view_);
            view_->clear();
            const std::string version = s8(acesVersion_->currentData().toString());
            for (const ColorSpaceChoice& c : viewTransforms(version)) view_->addItem(qs(c.label), qs(c.id));
            const int k = view_->findData(keep);
            view_->setCurrentIndex(k < 0 ? 0 : k);
        };
        fillViews_("off");
        QObject::connect(acesVersion_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { fillViews_(view_->currentData().toString()); });
        vf->addRow("View transform:", view_);
        lmt_ = combo(vt, "lookTransform", {{"None", "none"}, {"Apply Blue Light Artifact Fix", "bluelight"}});
        vf->addRow("Look modification transform:", lmt_);
        vf->addRow(QString(), note("With 32-bit floating point (full range) and compositing gamma 1.000 (Linear), or a View transform, pictures are "
                                   "composited in linear light; each media's Color space is set in its properties and the View transform shapes the "
                                   "picture shown and rendered. The ACES color space and look transform are kept with the project.", vt));
        auto colourItems = [this] {
            const bool full32 = format_->currentData().toString() == "32bit|full";
            for (QWidget* w : std::initializer_list<QWidget*>{gamma_, acesVersion_, acesSpace_, view_}) w->setEnabled(full32);
            lmt_->setEnabled(full32 && view_->currentData().toString() != "off");
        };
        QObject::connect(format_, &QComboBox::currentIndexChanged, this, [colourItems](int) { colourItems(); });
        QObject::connect(view_, &QComboBox::currentIndexChanged, this, [colourItems](int) { colourItems(); });
        QObject::connect(hdr_, &QComboBox::activated, this, [this, colourItems](int) {
            // HDR mode: HDR10 / HLG set the pixel format and the view transform; Off restores the defaults
            const QString h = hdr_->currentData().toString();
            if (h == "off") { pickData(format_, "8bit|full"); pickData(view_, "off"); }
            else { pickData(format_, "32bit|full"); pickData(view_, h == "hlg" ? "rrt_hlg1000" : "rrt_pq1000"); }
            colourItems();
        });
        // the Master Display is for HDR projects: greyed with HDR mode Off, as in VEGAS
        colourItems_ = [this, colourItems] {
            colourItems();
            const bool hdr = hdr_->currentData().toString() != "off";
            masterCombo_->setEnabled(hdr);
            masterCustomize_->setEnabled(hdr);
        };
        QObject::connect(hdr_, &QComboBox::activated, this, [this](int) { colourItems_(); });
        quality_ = combo(vt, "renderQuality", {{"Draft", "draft"}, {"Preview", "preview"}, {"Good", "good"}, {"Best", "best"}});
        quality_->setToolTip("Quality of resizing and rotating (pan/crop, track motion): Draft uses the nearest pixel, Best takes several samples per pixel");
        vf->addRow("Full-resolution rendering quality:", quality_);
        blur_ = combo(vt, "motionBlur", {{"Gaussian", "gaussian"}, {"Pyramid", "pyramid"}, {"Box", "box"}, {"Asymmetric Gaussian", "asymgaussian"},
                                         {"Asymmetric Pyramid", "asympyramid"}, {"Asymmetric Box", "asymbox"}});
        vf->addRow("Motion blur type:", blur_);
        deint_ = combo(vt, "deinterlace", {{"None", "none"}, {"Blend fields", "blend"}, {"Interpolate fields", "interpolate"}, {"Smart adaptive (GPU only)", "smart"}});
        vf->addRow("Deinterlace method:", deint_);
        resample_ = combo(vt, "resample", {{"Smart resample", "smart"}, {"Force resample", "force"}, {"Optical flow", "optical"}, {"Disable resample", "off"}});
        vf->addRow("Resample mode:", resample_);
        vf->addRow(QString(), note("Motion blur (the video output's motion blur envelope) and interlaced fields are rendered, and in "
                                   "a Best preview; deinterlacing applies to interlaced media in a progressive project (Smart adaptive "
                                   "interpolates in Reno).", vt));
        // VEGAS 18's Master Display: "Custom display" (or one from the list) and Customize, for HDR projects
        {
            auto* row = new QHBoxLayout();
            masterCombo_ = new QComboBox(vt);
            masterCombo_->setObjectName("masterDisplay");
            for (const std::string& id : masterDisplayPresetIds()) masterCombo_->addItem(qs(masterDisplayPresetLabel(id)), qs(id));
            masterCustomize_ = new QPushButton("Customize", vt);
            masterCustomize_->setObjectName("masterDisplayCustomize");
            row->addWidget(masterCombo_, 1);
            row->addWidget(masterCustomize_);
            masterCombo_->setToolTip("The display the HDR picture is graded on: written into HDR renders as mastering display metadata. "
                                     "Available with an HDR mode.");
            vf->addRow("Master Display:", row);
            QObject::connect(masterCombo_, &QComboBox::activated, this, [this](int) {
                masterDisplay_ = masterDisplayPreset(s8(masterCombo_->currentData().toString()), masterDisplay_);
                masterDisplay_.preset = s8(masterCombo_->currentData().toString());
            });
            QObject::connect(masterCustomize_, &QPushButton::clicked, this, [this] {
                if (editMasterDisplay(this, masterDisplay_)) pickData(masterCombo_, "custom");
            });
        }
        adjustSource_ = new QCheckBox("Adjust source media to better match project or render settings", vt);
        adjustSource_->setObjectName("adjustSourceMedia");
        adjustSource_->setToolTip("Kept with the project: Reno scales media to the project either way");
        vf->addRow(QString(), adjustSource_);
        rotation_ = combo(vt, "outputRotation", {{"0\u00B0 (original)", "0"}, {"90\u00B0 clockwise", "90"}, {"180\u00B0", "180"}, {"90\u00B0 counterclockwise", "270"}});
        vf->addRow("Output rotation:", rotation_);
        vf->addRow(QString(), note("File > Render As turns the picture by it when \"Use project output rotation setting\" is ticked (a 1920x1080 project "
                                   "turned 90\u00B0 renders 1080x1920). The preview and the editing are not turned.", vt));
        vf->addRow("Prerendered files folder:", folderRow(vt, "prerenderFolder", qs(AppState::get().prefs.prerenderFolder),
                                                           "(Reno's own folder for this user)", &prerenderFolder_));
        {
            // more than fits on a small screen: the tab scrolls
            QScrollArea* scroll = new QScrollArea(tabs_);
            scroll->setObjectName("videoScroll");
            scroll->setWidgetResizable(true);
            scroll->setFrameShape(QFrame::NoFrame);
            scroll->setWidget(vt);
            tabs_->addTab(scroll, "Video");
        }

        QWidget* at = new QWidget(tabs_);
        QFormLayout* af = new QFormLayout(at);
        // Master bus mode: Stereo or 5.1 Surround (then every track and bus has a surround panner, Ctrl+Alt+3, and the
        // master has six channels), and the LFE's low-pass filter for 5.1
        busMode_ = combo(at, "masterBusMode", {{"Stereo", "2"}, {"5.1 Surround", "6"}});
        af->addRow("Master bus mode:", busMode_);
        lfeLowpass_ = new QCheckBox("Enable low-pass filter on LFE", at);
        lfeLowpass_->setObjectName("lfeLowpass");
        lfeLowpass_->setToolTip("Surround projects only: filters the master LFE channel to low frequencies");
        af->addRow(QString(), lfeLowpass_);
        lfeCutoff_ = combo(at, "lfeCutoff", {{"80", "80"}, {"90", "90"}, {"100", "100"}, {"110", "110"}, {"120", "120"}, {"150", "150"}, {"200", "200"}});
        af->addRow("Cutoff frequency for low-pass filter (Hz):", lfeCutoff_);
        lfeQuality_ = combo(at, "lfeQuality", {{"Best", "best"}, {"Good", "good"}, {"Fast", "fast"}});
        af->addRow("Low-pass filter quality:", lfeQuality_);
        busCount_ = new QSpinBox(at);
        busCount_->setObjectName("busCount");
        busCount_->setRange(0, 26);
        busCount_->setValue((int)AppState::get().project().buses.size());
        busCount_->setToolTip("Buses are added (Bus A, Bus B...) or the last ones removed when OK is pressed; tracks sent to a removed bus go to the master");
        af->addRow("Number of stereo busses:", busCount_);
        rateHz_ = new QComboBox(at);
        rateHz_->setObjectName("sampleRate");
        rateHz_->setEditable(true);
        for (int hz : {8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 192000}) rateHz_->addItem(QString("%1 Hz").arg(hz), hz);
        af->addRow("Sample rate (Hz):", rateHz_);
        bits_ = combo(at, "bitDepth", {{"16", "16"}, {"24", "24"}, {"32 (floating point)", "32"}});
        af->addRow("Bit depth:", bits_);
        af->addRow(QString(), note("The bit depth is saved with the project but not used: Reno mixes in 32-bit floating point, and render depth is set in Render As.", at));
        resampleQ_ = combo(at, "resampleQuality", {{"Draft", "draft"}, {"Preview", "preview"}, {"Good", "good"}, {"Best", "best"}});
        resampleQ_->setToolTip("Kept with the project: Reno resamples and stretches sound one way");
        af->addRow("Resample and stretch quality:", resampleQ_);
        af->addRow("Recorded files folder:", folderRow(at, "recordFolder", qs(AppState::get().prefs.recordFolder), "(\"Recorded\" beside the project)", &recordFolder_));
        tabs_->addTab(at, "Audio");

        QWidget* rt = new QWidget(tabs_);
        QFormLayout* rf = new QFormLayout(rt);
        std::vector<std::pair<QString, QString>> formats;
        for (const TimeFormatInfo& f : timeFormats())
            formats.push_back({QString::fromUtf8(f.menuText).replace("&&", QString(QChar(1))).remove('&').replace(QChar(1), '&'), f.id});
        ruler_ = combo(rt, "rulerFormat", formats);
        rf->addRow("Ruler time format:", ruler_);
        rulerStart_ = new Spin(rt, -360000, 360000, 3, 1, "rulerStart");
        rulerStart_->setSuffix(" s");
        rf->addRow("Ruler start time:", rulerStart_);
        tempo_ = new Spin(rt, 1, 999, 3, 1, "tempo");
        tempo_->setSuffix(" beats per minute");
        rf->addRow("Tempo:", tempo_);
        beats_ = new QSpinBox(rt);
        beats_->setObjectName("beatsPerMeasure");
        beats_->setRange(1, 32);
        rf->addRow("Beats per measure:", beats_);
        noteBeat_ = combo(rt, "noteGetsBeat", {{"1", "1"}, {"2", "2"}, {"4", "4"}, {"8", "8"}, {"16", "16"}, {"32", "32"}});
        rf->addRow("Note that gets one beat:", noteBeat_);
        rf->addRow(QString(), note("Tempo and beats are used by Measures & Beats, the grid and the metronome.", rt));
        tabs_->addTab(rt, "Ruler");

        QWidget* st = new QWidget(tabs_);
        QFormLayout* sf = new QFormLayout(st);
        title_ = new QLineEdit(st);
        title_->setObjectName("title");
        artist_ = new QLineEdit(st);
        artist_->setObjectName("artist");
        engineer_ = new QLineEdit(st);
        engineer_->setObjectName("engineer");
        copyright_ = new QLineEdit(st);
        copyright_->setObjectName("copyright");
        comments_ = new QPlainTextEdit(st);
        comments_->setObjectName("comments");
        comments_->setTabChangesFocus(true);
        comments_->setFixedHeight(90);
        sf->addRow("Title:", title_);
        sf->addRow("Artist:", artist_);
        sf->addRow("Engineer:", engineer_);
        sf->addRow("Copyright:", copyright_);
        sf->addRow("Comments:", comments_);
        sf->addRow(QString(), note("Kept with the project; not written into rendered files yet.", st));
        tabs_->addTab(st, "Summary");

        QWidget* ct = new QWidget(tabs_);
        QFormLayout* cf = new QFormLayout(ct);
        upc_ = new QLineEdit(ct);
        upc_->setObjectName("cdUpc");
        upc_->setMaxLength(13);
        upc_->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9]{0,13}"), upc_));
        cf->addRow("Universal Product Code/Media Catalog Number:", upc_);
        firstTrack_ = new QSpinBox(ct);
        firstTrack_->setObjectName("cdFirstTrack");
        firstTrack_->setRange(1, 99);
        cf->addRow("First track number on disc:", firstTrack_);
        cf->addRow(QString(), note("Kept with the project for disc-at-once CDs (Reno burns no CDs).", ct));
        tabs_->addTab(ct, "Audio CD");

        startAll_ = new QCheckBox("Start all new projects with these settings", this);
        startAll_->setObjectName("startAllNewProjects");
        root->addWidget(startAll_);
        QHBoxLayout* buttons = new QHBoxLayout();
        buttons->addWidget(note("Effects follow a change of size or frame rate; Undo puts everything back.", this), 1);
        QPushButton* ok = new QPushButton("OK", this);
        ok->setObjectName("ok");
        ok->setDefault(true);
        QPushButton* cancel = new QPushButton("Cancel", this);
        cancel->setObjectName("cancel");
        buttons->addWidget(ok);
        buttons->addWidget(cancel);
        root->addLayout(buttons);

        width_->setValue(s.width);
        height_->setValue(s.height);
        pickData(field_, qs(s.fieldOrder));
        int pi = -1;
        for (int k = 0; k < par_->count(); ++k)
            if (std::fabs(par_->itemData(k).toDouble() - s.pixelAspect) < 5e-5) pi = k;
        if (pi < 0) { par_->addItem(QString::number(s.pixelAspect, 'f', 4), s.pixelAspect); pi = par_->count() - 1; }
        par_->setCurrentIndex(pi);
        num_->setValue(s.fpsNum);
        den_->setValue(s.fpsDen);
        int ri = (int)(sizeof(kRates) / sizeof(kRates[0]));
        for (int k = 0; k < (int)(sizeof(kRates) / sizeof(kRates[0])); ++k)
            if ((long long)kRates[k].num * s.fpsDen == (long long)s.fpsNum * kRates[k].den) ri = k;
        rate_->setCurrentIndex(ri);
        pickData(format_, qs(s.pixelFormat == "32bit" ? "32bit" : "8bit") + "|" + qs(s.pixelRange == "video" ? "video" : "full"));
        pickData(hdr_, qs(s.hdrMode));
        masterDisplay_ = s.masterDisplay;
        pickData(masterCombo_, qs(s.masterDisplay.preset));
        pickData(gamma_, s.compositingGamma < 1.5 ? "1.000" : "2.222");
        pickData(acesVersion_, qs(s.acesVersion));
        pickData(acesSpace_, qs(s.acesColorSpace));
        fillViews_(qs(s.viewTransform));
        pickData(lmt_, qs(s.lookTransform));
        pickData(stereo_, qs(s.stereoMode));
        stereoSwap_->setChecked(s.stereoSwap);
        crosstalk_->setValue(s.crosstalk);
        stereoSwap_->setEnabled(s.stereo());
        crosstalk_->setEnabled(s.stereo());
        out360_->setChecked(s.output360);
        adjustSource_->setChecked(s.adjustSourceMedia);
        pickData(resampleQ_, qs(s.resampleQuality));
        pickData(noteBeat_, QString::number(s.noteGetsBeat));
        upc_->setText(qs(s.cdUpc));
        firstTrack_->setValue(s.cdFirstTrack);
        if (colourItems_) colourItems_();
        pickData(quality_, qs(s.renderQuality));
        pickData(blur_, qs(s.motionBlur));
        pickData(deint_, qs(s.deinterlace));
        pickData(resample_, qs(s.resample));
        int hi = rateHz_->findData(s.sampleRate);
        if (hi < 0) { rateHz_->addItem(QString("%1 Hz").arg(s.sampleRate), s.sampleRate); hi = rateHz_->count() - 1; }
        rateHz_->setCurrentIndex(hi);
        pickData(bits_, QString::number(s.audioBits));
        pickData(busMode_, QString::number(s.surround() ? 6 : 2));
        lfeLowpass_->setChecked(s.lfeLowpass);
        {
            const QString cut = QString::number((int)std::lround(s.lfeCutoff));
            if (lfeCutoff_->findData(cut) < 0) lfeCutoff_->addItem(cut, cut);
            pickData(lfeCutoff_, cut);
        }
        pickData(lfeQuality_, qs(s.lfeQuality));
        pickData(ruler_, qs(s.rulerFormat));
        rulerStart_->setValue(s.rulerStart);
        tempo_->setValue(s.tempo);
        beats_->setValue(s.beatsPerMeasure);
        title_->setText(qs(s.title));
        artist_->setText(qs(s.artist));
        engineer_->setText(qs(s.engineer));
        copyright_->setText(qs(s.copyright));
        comments_->setPlainText(qs(s.comments));
        pickData(rotation_, QString::number(s.outputRotation));
        syncRate();
        syncTemplate();

        QObject::connect(template_, &QComboBox::activated, this, [this](int i) {
            if (i <= 0) return;
            const QString key = template_->itemData(i).toString();
            if (key.startsWith("u:")) { loadUserTemplate(key.mid(2)); return; }
            const int bi = key.mid(2).toInt();
            if (bi < 0 || bi >= (int)(sizeof(kTemplates) / sizeof(kTemplates[0]))) return;
            const Template& t = kTemplates[bi];
            width_->setValue(t.w);
            height_->setValue(t.h);
            num_->setValue(t.num);
            den_->setValue(t.den);
            int r = (int)(sizeof(kRates) / sizeof(kRates[0]));
            for (int k = 0; k < r; ++k)
                if ((long long)kRates[k].num * t.den == (long long)t.num * kRates[k].den) { r = k; break; }
            rate_->setCurrentIndex(r);
            for (int k = 0; k < par_->count(); ++k)
                if (std::fabs(par_->itemData(k).toDouble() - t.par) < 5e-5) par_->setCurrentIndex(k);
            pickData(field_, t.field);
            syncTemplate();
        });
        for (QSpinBox* b : {width_, height_, num_, den_})
            QObject::connect(b, &QSpinBox::valueChanged, this, [this](int) { syncTemplate(); syncRateText(); });
        QObject::connect(par_, &QComboBox::currentIndexChanged, this, [this](int) { syncTemplate(); });
        QObject::connect(field_, &QComboBox::currentIndexChanged, this, [this](int) { syncTemplate(); });
        QObject::connect(rate_, &QComboBox::currentIndexChanged, this, [this](int) { syncRate(); syncTemplate(); });
        QObject::connect(ok, &QPushButton::clicked, this, [this] { apply(); accept(); });
        QObject::connect(cancel, &QPushButton::clicked, this, [this] { reject(); });
    }


private:
    void syncTemplate()
    {
        int found = 0;
        for (int k = 0; k < (int)(sizeof(kTemplates) / sizeof(kTemplates[0])); ++k) {
            const Template& t = kTemplates[k];
            if (t.w == width_->value() && t.h == height_->value() && (long long)t.num * den_->value() == (long long)num_->value() * t.den &&
                std::fabs(t.par - par_->currentData().toDouble()) < 5e-5 && field_->currentData().toString() == t.field) { found = k + 1; break; }
        }
        QSignalBlocker b(template_);
        if (!found) {
            for (int k = 0; k < template_->count(); ++k) {
                const QString key = template_->itemData(k).toString();
                if (!key.startsWith("u:")) continue;
                Project tp;
                Json j;
                if (!Json::parse(QSettings().value("projectTemplates/" + key.mid(2)).toString().toStdString(), j) || !tp.fromJson(j)) continue;
                const ProjectSettings& t = tp.settings;
                if (t.width == width_->value() && t.height == height_->value() && (long long)t.fpsNum * den_->value() == (long long)num_->value() * t.fpsDen &&
                    std::fabs(t.pixelAspect - par_->currentData().toDouble()) < 5e-5 && field_->currentData().toString() == qs(t.fieldOrder)) { found = k; break; }
            }
        } else {
            found = template_->findData(QString("b:%1").arg(found - 1));
        }
        template_->setCurrentIndex(std::max(0, found));
        if (deleteT_) deleteT_->setEnabled(template_->currentData().toString().startsWith("u:"));
    }

    // The template list: (Custom), VEGAS's, then the user's own (QSettings "projectTemplates/<name>": settings as Project JSON).
    void fillTemplates()
    {
        QSignalBlocker b(template_);
        template_->clear();
        template_->addItem("(Custom)", QString());
        for (int k = 0; k < (int)(sizeof(kTemplates) / sizeof(kTemplates[0])); ++k) template_->addItem(kTemplates[k].label, QString("b:%1").arg(k));
        QSettings st;
        st.beginGroup("projectTemplates");
        const QStringList names = st.childKeys();
        st.endGroup();
        for (const QString& n : names) template_->addItem(n, "u:" + n);
    }
    void saveTemplate()
    {
        bool ok = false;
        const QString name = QInputDialog::getText(this, "Save Template", "Template name:", QLineEdit::Normal, QString(), &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        if (name.contains('/') || name.contains('\\')) { QMessageBox::warning(this, "Save Template", "A template's name cannot have / or \\ in it."); return; }
        Project tp;
        ProjectSettings& t = tp.settings;
        t.width = width_->value() & ~1;
        t.height = height_->value() & ~1;
        t.fpsNum = num_->value();
        t.fpsDen = den_->value();
        t.pixelAspect = par_->currentData().toDouble();
        t.fieldOrder = s8(field_->currentData().toString());
        const QStringList pf = format_->currentData().toString().split('|');
        t.pixelFormat = s8(pf.value(0));
        t.pixelRange = s8(pf.value(1));
        t.renderQuality = s8(quality_->currentData().toString());
        t.motionBlur = s8(blur_->currentData().toString());
        t.deinterlace = s8(deint_->currentData().toString());
        t.outputRotation = rotation_->currentData().toString().toInt();
        QSettings().setValue("projectTemplates/" + name, QString::fromStdString(tp.toJson().dump()));
        fillTemplates();
        syncTemplate();
    }
    void deleteTemplate()
    {
        const QString key = template_->currentData().toString();
        if (!key.startsWith("u:")) return;
        if (QMessageBox::question(this, "Delete Template", QString("Delete the template \"%1\"?").arg(key.mid(2))) != QMessageBox::Yes) return;
        QSettings().remove("projectTemplates/" + key.mid(2));
        fillTemplates();
        syncTemplate();
    }
    void loadUserTemplate(const QString& name)
    {
        Project tp;
        Json j;
        if (!Json::parse(QSettings().value("projectTemplates/" + name).toString().toStdString(), j) || !tp.fromJson(j)) return;
        const ProjectSettings& t = tp.settings;
        width_->setValue(t.width);
        height_->setValue(t.height);
        num_->setValue(t.fpsNum);
        den_->setValue(t.fpsDen);
        int r = (int)(sizeof(kRates) / sizeof(kRates[0]));
        for (int k = 0; k < r; ++k)
            if ((long long)kRates[k].num * t.fpsDen == (long long)t.fpsNum * kRates[k].den) { r = k; break; }
        rate_->setCurrentIndex(r);
        for (int k = 0; k < par_->count(); ++k)
            if (std::fabs(par_->itemData(k).toDouble() - t.pixelAspect) < 5e-5) par_->setCurrentIndex(k);
        pickData(field_, qs(t.fieldOrder));
        pickData(format_, qs(t.pixelFormat == "32bit" ? "32bit" : "8bit") + "|" + qs(t.pixelRange == "video" ? "video" : "full"));
        pickData(quality_, qs(t.renderQuality));
        pickData(blur_, qs(t.motionBlur));
        pickData(deint_, qs(t.deinterlace));
        pickData(rotation_, QString::number(t.outputRotation));
        syncTemplate();
    }
    void matchMediaFile()
    {
        const QString f = QFileDialog::getOpenFileName(this, "Match Media Settings", QString(), "Media files (*.*)");
        if (f.isEmpty()) return;
        AppState& app = AppState::get();
        std::string err;
        std::unique_ptr<MediaReader> r = app.mediaOpener ? app.mediaOpener(f.toStdString(), &err) : nullptr;
        if (!r || !r->info().hasVideo || r->info().width <= 0) {
            QMessageBox::warning(this, "Match Media Settings", "That file has no picture Reno can read" + (err.empty() ? QString(".") : ": " + qs(err)));
            return;
        }
        const MediaInfo info = r->info();
        width_->setValue(info.width & ~1);
        height_->setValue(info.height & ~1);
        if (info.fps > 0.5 && !info.isStill) {
            const double fr = info.fps;
            int n = 0, d = 1;
            if (std::fabs(fr - std::round(fr)) < 1e-3) { n = (int)std::lround(fr); d = 1; }
            else if (std::fabs(fr * 1.001 - std::round(fr * 1.001)) < 1e-3) { n = (int)std::lround(fr * 1.001) * 1000; d = 1001; }
            else { n = (int)std::lround(fr * 1000); d = 1000; }
            int ri = (int)(sizeof(kRates) / sizeof(kRates[0]));
            for (int k = 0; k < ri; ++k)
                if ((long long)kRates[k].num * d == (long long)n * kRates[k].den) { ri = k; break; }
            rate_->setCurrentIndex(ri);
            num_->setValue(n);
            den_->setValue(d);
        }
        pickData(field_, info.fieldOrder == "upper" || info.fieldOrder == "lower" ? qs(info.fieldOrder) : QString("none"));
        syncTemplate();
    }

    // a standard rate fills in the exact fraction; the fraction is edited only for "Custom..."
    void syncRate()
    {
        const int i = rate_->currentIndex();
        const int n = (int)(sizeof(kRates) / sizeof(kRates[0]));
        const bool custom = i >= n;
        if (!custom) {
            QSignalBlocker b1(num_), b2(den_);
            num_->setValue(kRates[i].num);
            den_->setValue(kRates[i].den);
        }
        num_->setEnabled(custom);
        den_->setEnabled(custom);
        syncRateText();
    }
    void syncRateText() { rateText_->setText(QString("= %1 fps").arg((double)num_->value() / den_->value(), 0, 'f', 3)); }

    void apply()
    {
        AppState& app = AppState::get();
        ProjectSettings s = app.project().settings;
        s.width = width_->value() & ~1;
        s.height = height_->value() & ~1;
        s.fieldOrder = s8(field_->currentData().toString());
        s.pixelAspect = par_->currentData().toDouble();
        s.fpsNum = num_->value();
        s.fpsDen = den_->value();
        {
            const QStringList pf = format_->currentData().toString().split('|');
            s.pixelFormat = s8(pf.value(0, "8bit"));
            s.pixelRange = s8(pf.value(1, "full"));
        }
        s.hdrMode = s8(hdr_->currentData().toString());
        s.masterDisplay = masterDisplay_;
        s.compositingGamma = gamma_->currentData().toString().toDouble();
        s.acesVersion = s8(acesVersion_->currentData().toString());
        s.acesColorSpace = s8(acesSpace_->currentData().toString());
        s.viewTransform = s8(view_->currentData().toString());
        s.lookTransform = s8(lmt_->currentData().toString());
        s.stereoMode = s8(stereo_->currentData().toString());
        s.stereoSwap = stereoSwap_->isChecked();
        s.crosstalk = crosstalk_->value();
        s.output360 = out360_->isChecked();
        s.adjustSourceMedia = adjustSource_->isChecked();
        s.resampleQuality = s8(resampleQ_->currentData().toString());
        s.noteGetsBeat = noteBeat_->currentData().toString().toInt();
        s.cdUpc = s8(upc_->text().trimmed());
        s.cdFirstTrack = firstTrack_->value();
        s.renderQuality = s8(quality_->currentData().toString());
        s.motionBlur = s8(blur_->currentData().toString());
        s.deinterlace = s8(deint_->currentData().toString());
        s.resample = s8(resample_->currentData().toString());
        {
            bool okRate = false;
            const int typed = QString(rateHz_->currentText()).remove("Hz").trimmed().toInt(&okRate);
            s.sampleRate = rateHz_->currentIndex() >= 0 && rateHz_->itemText(rateHz_->currentIndex()) == rateHz_->currentText() ? rateHz_->currentData().toInt()
                                                                                                                               : (okRate && typed >= 8000 && typed <= 384000 ? typed : s.sampleRate);
        }
        s.audioBits = bits_->currentData().toString().toInt();
        s.audioChannels = busMode_->currentData().toString().toInt() == 6 ? 6 : 2;
        s.lfeLowpass = lfeLowpass_->isChecked();
        s.lfeCutoff = lfeCutoff_->currentData().toString().toDouble();
        s.lfeQuality = s8(lfeQuality_->currentData().toString());
        s.rulerFormat = s8(ruler_->currentData().toString());
        s.rulerStart = rulerStart_->value();
        s.tempo = tempo_->value();
        s.beatsPerMeasure = beats_->value();
        s.title = s8(title_->text());
        s.artist = s8(artist_->text());
        s.engineer = s8(engineer_->text());
        s.copyright = s8(copyright_->text());
        s.comments = s8(comments_->toPlainText());
        s.outputRotation = rotation_->currentData().toString().toInt();
        // the folders are Reno's preferences, as VEGAS keeps them
        app.prefs.prerenderFolder = s8(QDir::fromNativeSeparators(prerenderFolder_->text().trimmed()));
        app.prefs.recordFolder = s8(QDir::fromNativeSeparators(recordFolder_->text().trimmed()));
        if (startAll_->isChecked()) {
            Project defaults;
            defaults.settings = s;
            app.prefs.newProjectSettings = defaults.toJson().dump();
        }
        app.prefs.save();
        const ProjectSettings& cur = app.project().settings;
        // numbers shown rounded come back as they were if untouched
        if (std::fabs(s.rulerStart - cur.rulerStart) < 0.0005) s.rulerStart = cur.rulerStart;
        if (std::fabs(s.tempo - cur.tempo) < 0.0005) s.tempo = cur.tempo;
        Project pa, pb;
        pa.settings = s;
        pb.settings = cur;
        const int wantBuses = busCount_->value(), haveBuses = (int)app.project().buses.size();
        if (pa.toJson().get("settings").dump() == pb.toJson().get("settings").dump() && wantBuses == haveBuses) return;
        app.edit("Project Properties", [&app, s, wantBuses](Project& p) {
            app.fx.captureStates(p);
            p.settings = s;
            // Number of stereo busses: new ones named on from the last; those taken away send nothing on
            while ((int)p.buses.size() < wantBuses) {
                Bus b;
                b.id = p.newId();
                b.name = std::string("Bus ") + (char)('A' + (int)(p.buses.size() % 26));
                p.buses.push_back(b);
            }
            while ((int)p.buses.size() > wantBuses) {
                const uint64_t gone = p.buses.back().id;
                p.buses.pop_back();
                for (Track& t : p.tracks) {
                    if (t.output == gone) t.output = 0;
                    t.sends.erase(std::remove_if(t.sends.begin(), t.sends.end(), [gone](const Send& x) { return x.bus == gone; }), t.sends.end());
                }
                for (Bus& b : p.buses)
                    if (b.output == gone) b.output = 0;
            }
        });
    }

    QTabWidget* tabs_ = nullptr;
    std::function<void(const QString&)> fillViews_;
    QComboBox *template_ = nullptr, *field_ = nullptr, *par_ = nullptr, *rate_ = nullptr, *format_ = nullptr, *quality_ = nullptr, *blur_ = nullptr,
              *deint_ = nullptr, *resample_ = nullptr, *rateHz_ = nullptr, *bits_ = nullptr, *ruler_ = nullptr;
    QSpinBox *width_ = nullptr, *height_ = nullptr, *num_ = nullptr, *den_ = nullptr, *beats_ = nullptr;
    QLabel* rateText_ = nullptr;
    QSpinBox* busCount_ = nullptr;
    QComboBox *hdr_ = nullptr, *gamma_ = nullptr, *acesVersion_ = nullptr, *acesSpace_ = nullptr, *view_ = nullptr, *lmt_ = nullptr, *stereo_ = nullptr,
              *resampleQ_ = nullptr, *noteBeat_ = nullptr;
    QCheckBox *stereoSwap_ = nullptr, *out360_ = nullptr, *adjustSource_ = nullptr;
    Spin* crosstalk_ = nullptr;
    QLineEdit* upc_ = nullptr;
    QSpinBox* firstTrack_ = nullptr;
    QPushButton* deleteT_ = nullptr;
    std::function<void()> colourItems_;
    QComboBox* masterCombo_ = nullptr;
    QPushButton* masterCustomize_ = nullptr;
    MasterDisplay masterDisplay_;
    Spin *rulerStart_ = nullptr, *tempo_ = nullptr;
    QLineEdit *title_ = nullptr, *artist_ = nullptr, *engineer_ = nullptr, *copyright_ = nullptr;
    QPlainTextEdit* comments_ = nullptr;
    QComboBox* rotation_ = nullptr;
    QComboBox *busMode_ = nullptr, *lfeCutoff_ = nullptr, *lfeQuality_ = nullptr;
    QCheckBox* lfeLowpass_ = nullptr;
    QLineEdit *prerenderFolder_ = nullptr, *recordFolder_ = nullptr;
    QCheckBox* startAll_ = nullptr;
};

// (Options > Preferences: prefsdialog.cpp)

}

// ================================================================================== entry points
void runEventProperties(QWidget* parent, uint64_t eventId)
{
    if (!AppState::get().project().findEvent(eventId)) return;
    EventPropertiesDialog dlg(parent, eventId);
    dlg.exec();
}

void runProjectPropertiesFull(QWidget* parent)
{
    ProjectPropertiesDialog dlg(parent);
    dlg.exec();
}

void runProjectPropertiesAudio(QWidget* parent)
{
    ProjectPropertiesDialog dlg(parent);
    if (QTabWidget* tabs = dlg.findChild<QTabWidget*>())
        for (int i = 0; i < tabs->count(); ++i)
            if (tabs->tabText(i) == "Audio") tabs->setCurrentIndex(i);
    dlg.exec();
}

}
}
