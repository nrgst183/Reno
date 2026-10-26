// The pages of Reno's built-in audio effects: see fxpages.h.
#include "fxpages.h"
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPen>
#include <QPolygonF>
#include <QImage>
#include <QDir>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace reno {
namespace app {

namespace {

const double kPi = 3.14159265358979323846;

// ------------------------------------------------------------------------------------------ small widgets
// VEGAS's DirectX pages are Windows-classic: a trackbar is a thin sunken groove with a small grey thumb (style sheet on
// the slider, so it looks the same under either of Reno's looks).
class DxSlider : public QSlider {
public:
    DxSlider(Qt::Orientation o, QWidget* parent) : QSlider(o, parent)
    {
        setFocusPolicy(Qt::StrongFocus);
        setTickPosition(QSlider::NoTicks);
        const bool horizontal = o == Qt::Horizontal;
        setStyleSheet(horizontal
                          ? "QSlider::groove:horizontal { height: 2px; background: #9a9a9a; border: 1px solid #c8c8c8; border-top-color: #7a7a7a; border-left-color: #7a7a7a; }"
                            "QSlider::handle:horizontal { background: #f0f0f0; border: 1px solid #6c6c6c; border-bottom-color: #3c3c3c; border-right-color: #3c3c3c; width: 7px; margin: -7px 0; }"
                            "QSlider::handle:horizontal:hover { background: #ffffff; }"
                            "QSlider::groove:horizontal:disabled { background: #cfcfcf; border-color: #dcdcdc; }"
                            "QSlider::handle:horizontal:disabled { background: #e6e6e6; border-color: #b4b4b4; }"
                          : "QSlider::groove:vertical { width: 2px; background: #9a9a9a; border: 1px solid #c8c8c8; border-top-color: #7a7a7a; border-left-color: #7a7a7a; }"
                            "QSlider::handle:vertical { background: #f0f0f0; border: 1px solid #6c6c6c; border-bottom-color: #3c3c3c; border-right-color: #3c3c3c; height: 18px; margin: 0 -6px; }"
                            "QSlider::handle:vertical:hover { background: #ffffff; }"
                            "QSlider::groove:vertical:disabled { background: #cfcfcf; border-color: #dcdcdc; }"
                            "QSlider::handle:vertical:disabled { background: #e6e6e6; border-color: #b4b4b4; }");
    }

protected:
    void wheelEvent(QWheelEvent* e) override { e->ignore(); }
};

// A drawing in a white box with a grid. Mouse positions are passed on as fractions of the plot (x from the left, y from
// the bottom), outside 0..1 when the mouse is outside.
class Graph : public QWidget {
public:
    std::function<void(QPainter&, const QRectF& plot)> draw;
    std::function<void(const QPointF& at, int phase)> mouse;
    QMargins margins = QMargins(30, 6, 8, 18);

    explicit Graph(QWidget* parent) : QWidget(parent) {}
    QRectF plot() const { return QRectF(rect().marginsRemoved(margins)); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.fillRect(rect(), palette().color(QPalette::Window));
        const QRectF pl = plot();
        p.fillRect(pl, Qt::white);
        p.setFont(font());
        p.setPen(Qt::black);
        if (draw) draw(p, pl);
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton || !mouse) return;
        down_ = true;
        mouse(frac(e->position()), 0);
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (down_ && mouse) mouse(frac(e->position()), 1);
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton || !down_) return;
        down_ = false;
        if (mouse) mouse(frac(e->position()), 2);
    }

private:
    QPointF frac(const QPointF& at) const
    {
        const QRectF pl = plot();
        return QPointF((at.x() - pl.left()) / std::max(1.0, pl.width()), 1.0 - (at.y() - pl.top()) / std::max(1.0, pl.height()));
    }
    bool down_ = false;
};

// 12345.6 as "12,345.6" (VEGAS's page edit boxes group thousands).
QString groupedNumber(double v, int decimals)
{
    QString s = QString::number(v, 'f', decimals);
    const int dot = s.indexOf('.');
    const int end = dot < 0 ? (int)s.size() : dot;
    const int start = s.startsWith('-') ? 1 : 0;
    for (int i = end - 3; i > start; i -= 3) s.insert(i, ',');
    return s;
}

// A number in a range's caption, with no more digits than needed.
QString plainNumber(double v)
{
    QString s = QString::number(v, 'f', std::fabs(v - std::round(v)) < 1e-9 ? 0 : 2);
    return groupedNumber(s.toDouble(), std::fabs(v - std::round(v)) < 1e-9 ? 0 : 2);
}

bool parseNumber(QString t, double* out)
{
    t = t.trimmed();
    t.remove(',');
    int n = 0;
    while (n < t.size() && (t[n].isDigit() || t[n] == '.' || ((t[n] == '-' || t[n] == '+') && n == 0) || t[n] == 'e' || t[n] == 'E')) ++n;
    bool ok = false;
    const double v = t.left(n).toDouble(&ok);
    if (ok && std::isfinite(v)) {
        *out = v;
        return true;
    }
    return false;
}

// What a slider and its boxes show.
struct SMap {
    bool log = false;
    int dec = 1;
    bool inf = false;
    bool comma = true;
};


// The tick of a checked box: a small image made once in the temp folder (a style sheet can only take a file for it).
QString checkImage()
{
    static QString path;
    if (path.isEmpty()) {
        QImage im(11, 11, QImage::Format_ARGB32);
        im.fill(Qt::white);
        QPainter p(&im);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(Qt::black, 1.6));
        p.drawPolyline(QPolygonF({QPointF(2, 5.5), QPointF(4.5, 8), QPointF(9, 2.5)}));
        p.end();
        path = QDir::temp().filePath("reno_fx_check.png");
        im.save(path, "PNG");
    }
    return path;
}

typedef std::function<int()> Ix;
Ix fixed(int i) { return [i] { return i; }; }

// ------------------------------------------------------------------------------------------ the page
class Page : public DxPage {
public:
    Page(QWidget* parent, const std::string& uid, const DxHost& host) : DxPage(parent), h_(host)
    {
        setObjectName("fxDxPage");
        setAutoFillBackground(true);
        QPalette pal;
        const QColor face(240, 240, 240);
        pal.setColor(QPalette::Window, face);
        pal.setColor(QPalette::WindowText, Qt::black);
        pal.setColor(QPalette::Base, Qt::white);
        pal.setColor(QPalette::AlternateBase, QColor(233, 233, 233));
        pal.setColor(QPalette::Text, Qt::black);
        pal.setColor(QPalette::Button, QColor(225, 225, 225));
        pal.setColor(QPalette::ButtonText, Qt::black);
        pal.setColor(QPalette::BrightText, Qt::white);
        pal.setColor(QPalette::Highlight, QColor(0, 120, 215));
        pal.setColor(QPalette::HighlightedText, Qt::white);
        pal.setColor(QPalette::Light, Qt::white);
        pal.setColor(QPalette::Midlight, QColor(227, 227, 227));
        pal.setColor(QPalette::Mid, QColor(160, 160, 160));
        pal.setColor(QPalette::Dark, QColor(105, 105, 105));
        pal.setColor(QPalette::Shadow, QColor(70, 70, 70));
        pal.setColor(QPalette::PlaceholderText, QColor(120, 120, 120));
        pal.setColor(QPalette::ToolTipBase, QColor(255, 255, 225));
        pal.setColor(QPalette::ToolTipText, Qt::black);
        for (QPalette::ColorRole r : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) pal.setColor(QPalette::Disabled, r, QColor(109, 109, 109));
        pal.setColor(QPalette::Disabled, QPalette::Base, face);
        pal.setColor(QPalette::Disabled, QPalette::Button, QColor(204, 204, 204));
        setPalette(pal);
        // The application's dark look sets text colours by style sheet, which would override the palette: set them again here.
        setStyleSheet(QString("QLabel { background: transparent; } QLabel, QCheckBox, QRadioButton { color: black; } QLabel:disabled, QCheckBox:disabled, QRadioButton:disabled { color: #6d6d6d; } "
                      "QRadioButton::indicator { width: 11px; height: 11px; border-radius: 6px; border: 1px solid #333333; background: white; } QRadioButton::indicator:checked { background: qradialgradient(cx:0.5, cy:0.5, radius:0.5, fx:0.5, fy:0.5, stop:0 black, stop:0.45 black, stop:0.5 white, stop:1 white); } QRadioButton::indicator:disabled { border-color: #aaaaaa; background: #f0f0f0; } QCheckBox::indicator { width: 11px; height: 11px; border: 1px solid #333333; background: white; } QCheckBox::indicator:checked { image: url(%1); } QCheckBox::indicator:disabled { border-color: #aaaaaa; background: #f0f0f0; } QLineEdit { background: white; color: black; border: 1px solid #7a7a7a; selection-background-color: #0078d7; selection-color: white; } QLineEdit:disabled { background: #f0f0f0; color: #6d6d6d; border-color: #bcbcbc; }").arg(checkImage()));
        QFont f = QApplication::font();
#ifdef _WIN32
        f.setFamily("Segoe UI");
        f.setPointSizeF(9);
#else
        f.setPixelSize(12);
#endif
        setFont(f);

        root_ = new QVBoxLayout(this);
        root_->setContentsMargins(8, 6, 8, 8);
        root_->setSpacing(4);
        int width = 430;
        if (uid == "reno.distortion") distortion();
        else if (uid == "reno.vibrato") vibrato();
        else if (uid == "reno.pitchshift") pitchShift();
        else if (uid == "reno.chorus") chorus();
        else if (uid == "reno.flangewah") flangeWah();
        else if (uid == "reno.ampmod") ampMod();
        else if (uid == "reno.simpledelay") simpleDelay();
        else if (uid == "reno.reverb") reverb();
        else if (uid == "reno.moderneq") { modernEq(); width = 560; }
        layout()->activate();
        const QSize s = sizeHint();
        setFixedSize(std::max(width, s.width()), s.height());
        syncAll();
    }

    void refresh() override { syncAll(); }

private:
    DxHost h_;
    QVBoxLayout* root_ = nullptr;
    bool updating_ = false;
    std::vector<std::function<void()>> sync_;
    std::vector<QWidget*> graphs_;

    struct Rng { double lo = 0, hi = 1; };
    Rng rng(int i) const
    {
        Rng r;
        if (!h_.range || !h_.range(i, &r.lo, &r.hi) || !(r.hi > r.lo)) { r.lo = 0; r.hi = 1; }
        return r;
    }
    double get(int i) const { return h_.get ? h_.get(i) : 0.0; }
    double clampTo(int i, double v) const
    {
        const Rng r = rng(i);
        return std::max(r.lo, std::min(r.hi, v));
    }
    void commit(int i, double v)
    {
        if (updating_ || !h_.set) return;
        h_.set(i, clampTo(i, v));
        syncAll();
    }
    void syncAll()
    {
        const bool was = updating_;
        updating_ = true;
        for (auto& f : sync_) f();
        for (QWidget* g : graphs_) g->update();
        updating_ = was;
    }
    void beginDrag(int i)
    {
        if (h_.begin) h_.begin(i);
    }
    void endDrag()
    {
        if (h_.end) h_.end();
    }

    int steps(const SMap& m, const Rng& r) const { return (m.dec == 0 && r.hi - r.lo <= 2000 && !m.log) ? (int)std::lround(r.hi - r.lo) : 1000; }
    int toPos(double v, const SMap& m, const Rng& r) const
    {
        double t;
        if (m.log && r.lo > 0) t = std::log(std::max(v, r.lo) / r.lo) / std::log(r.hi / r.lo);
        else t = (v - r.lo) / (r.hi - r.lo);
        return (int)std::lround(std::max(0.0, std::min(1.0, t)) * steps(m, r));
    }
    double fromPos(int pos, const SMap& m, const Rng& r) const
    {
        const double t = std::max(0.0, std::min(1.0, (double)pos / steps(m, r)));
        if (m.log && r.lo > 0) return r.lo * std::pow(r.hi / r.lo, t);
        return r.lo + t * (r.hi - r.lo);
    }
    QString edit(double v, const SMap& m, const Rng& r) const
    {
        if (m.inf && v <= r.lo + 0.05) return "-Inf.";
        return m.comma ? groupedNumber(v, m.dec) : QString::number(v, 'f', m.dec);
    }
    QString rangeText(int i, const SMap& m, const QString& unit) const
    {
        const Rng r = rng(i);
        const QString lo = m.inf ? QString("-Inf.") : plainNumber(r.lo);
        return QString("(%1 to %2%3)").arg(lo, plainNumber(r.hi), unit.isEmpty() ? QString() : " " + unit);
    }

    // ---- the controls
    // A slider for setting ix(), with an edit box and/or readout showing its value.
    DxSlider* slider(Qt::Orientation o, Ix ix, const SMap& m, const QString& name, QLineEdit* box, QLabel* readout, const QString& readUnit = QString())
    {
        DxSlider* s = new DxSlider(o, this);
        s->setObjectName(name);
        s->setRange(0, 1000);
        QObject::connect(s, &QSlider::valueChanged, s, [=](int pos) {
            if (updating_) return;
            commit(ix(), fromPos(pos, m, rng(ix())));
        });
        QObject::connect(s, &QSlider::sliderPressed, s, [=] { beginDrag(ix()); });
        QObject::connect(s, &QSlider::sliderReleased, s, [=] { endDrag(); });
        if (box) {
            box->setObjectName(name + "Edit");
            QObject::connect(box, &QLineEdit::editingFinished, box, [=] {
                if (updating_) return;
                const int i = ix();
                const Rng r = rng(i);
                if (box->text() == edit(get(i), m, r)) return;
                double v = 0;
                if (m.inf && box->text().trimmed().startsWith("-inf", Qt::CaseInsensitive)) v = r.lo;
                else if (!parseNumber(box->text(), &v)) { syncAll(); return; }
                commit(i, v);
            });
        }
        sync_.push_back([=] {
            const int i = ix();
            const Rng r = rng(i);
            const double v = get(i);
            QSignalBlocker b(s);
            s->setRange(0, steps(m, r));
            if (!s->isSliderDown()) s->setValue(toPos(v, m, r));
            if (box && !box->hasFocus()) box->setText(edit(v, m, r));
            if (readout) readout->setText(m.inf && v <= r.lo + 0.05 ? QString("-Inf.") : QString::number(v, 'f', m.dec) + (readUnit.isEmpty() ? QString() : " " + readUnit));
        });
        return s;
    }

    QLineEdit* editBox(int width = 58)
    {
        QLineEdit* e = new QLineEdit(this);
        e->setFixedSize(width, 21);
        e->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        return e;
    }

    QLabel* text(const QString& t, Qt::Alignment a = Qt::AlignLeft | Qt::AlignVCenter)
    {
        QLabel* l = new QLabel(t, this);
        l->setTextFormat(Qt::RichText);
        l->setAlignment(a);
        return l;
    }

    QWidget* gainColumn(int i, const QString& name, bool inf = true)
    {
        QWidget* col = new QWidget(this);
        QVBoxLayout* v = new QVBoxLayout(col);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(3);
        SMap m;
        m.inf = inf;
        m.dec = 1;
        QLabel* readout = text(QString(), Qt::AlignHCenter | Qt::AlignVCenter);
        readout->setFixedHeight(16);
        v->addWidget(readout);
        DxSlider* s = slider(Qt::Vertical, fixed(i), m, QString("dxParam%1").arg(i), nullptr, readout, "dB");
        s->setFixedHeight(118);
        v->addWidget(s, 0, Qt::AlignHCenter);
        QLabel* cap = text(name + "<br>" + rangeText(i, m, "dB"), Qt::AlignHCenter | Qt::AlignTop);
        v->addWidget(cap);
        col->setFixedWidth(84);
        return col;
    }

    QWidget* block(int i, const QString& name, const QString& unit, SMap m)
    {
        QWidget* w = new QWidget(this);
        QVBoxLayout* v = new QVBoxLayout(w);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(1);
        QHBoxLayout* top = new QHBoxLayout();
        top->setSpacing(4);
        QLabel* l = text(name + " " + rangeText(i, m, unit) + ":");
        top->addWidget(l, 1);
        QLineEdit* box = editBox(62);
        top->addWidget(box);
        v->addLayout(top);
        DxSlider* s = slider(Qt::Horizontal, fixed(i), m, QString("dxParam%1").arg(i), box, nullptr);
        v->addWidget(s);
        l->setBuddy(s);
        return w;
    }

    QWidget* lineRow(int labelW, const Ix& ix, const QString& name, const SMap& m, const QString& objName, QCheckBox** check = nullptr, QLineEdit** boxOut = nullptr,
                     DxSlider** sliderOut = nullptr)
    {
        QWidget* w = new QWidget(this);
        QHBoxLayout* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(6);
        if (check) {
            *check = new QCheckBox(w);
            (*check)->setFixedWidth(16);
            h->addWidget(*check);
        }
        QLabel* l = text(name);
        l->setFixedWidth(labelW);
        h->addWidget(l);
        QLineEdit* box = editBox();
        DxSlider* s = slider(Qt::Horizontal, ix, m, objName, box, nullptr);
        l->setBuddy(s);
        h->addWidget(s, 1);
        h->addWidget(box);
        if (boxOut) *boxOut = box;
        if (sliderOut) *sliderOut = s;
        return w;
    }

    QCheckBox* checkBox(int i, const QString& label)
    {
        QCheckBox* c = new QCheckBox(label, this);
        c->setObjectName(QString("dxParam%1").arg(i));
        QObject::connect(c, &QCheckBox::toggled, c, [=](bool on) { commit(i, on ? 1 : 0); });
        sync_.push_back([=] {
            QSignalBlocker b(c);
            c->setChecked(get(i) >= 0.5);
        });
        return c;
    }

    QComboBox* combo(const Ix& ix, const QStringList& items, const QString& name)
    {
        QComboBox* c = new QComboBox(this);
        c->setObjectName(name);
        c->addItems(items);
        QObject::connect(c, QOverload<int>::of(&QComboBox::currentIndexChanged), c, [=](int k) {
            if (k >= 0) commit(ix(), k);
        });
        sync_.push_back([=] {
            QSignalBlocker b(c);
            c->setCurrentIndex((int)std::lround(std::max(0.0, std::min((double)items.size() - 1, get(ix())))));
        });
        return c;
    }

    QWidget* radios(int i, const QStringList& items, bool vertical = false)
    {
        QWidget* w = new QWidget(this);
        QBoxLayout* h = vertical ? static_cast<QBoxLayout*>(new QVBoxLayout(w)) : static_cast<QBoxLayout*>(new QHBoxLayout(w));
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(vertical ? 2 : 14);
        QButtonGroup* g = new QButtonGroup(w);
        g->setExclusive(true);
        for (int k = 0; k < items.size(); ++k) {
            QRadioButton* r = new QRadioButton(items[k], w);
            r->setObjectName(QString("dxParam%1_%2").arg(i).arg(k));
            g->addButton(r, k);
            h->addWidget(r);
            QObject::connect(r, &QRadioButton::toggled, r, [=](bool on) {
                if (on) commit(i, k);
            });
        }
        if (!vertical) h->addStretch(1);
        sync_.push_back([=] {
            const int k = (int)std::lround(std::max(0.0, std::min((double)items.size() - 1, get(i))));
            if (QAbstractButton* b = g->button(k)) {
                QSignalBlocker blocker(b);
                b->setChecked(true);
            }
        });
        return w;
    }

    QPushButton* button(const QString& t)
    {
        QPushButton* b = new QPushButton(t, this);
        b->setFocusPolicy(Qt::TabFocus);
        return b;
    }

    Graph* graph(const QSize& size)
    {
        Graph* g = new Graph(this);
        g->setFixedSize(size);
        graphs_.push_back(g);
        return g;
    }

    QVBoxLayout* twoColumns(std::initializer_list<std::pair<int, const char*>> gains, int rightW = 300)
    {
        QHBoxLayout* h = new QHBoxLayout();
        h->setSpacing(6);
        QHBoxLayout* left = new QHBoxLayout();
        left->setSpacing(0);
        for (const auto& g : gains) left->addWidget(gainColumn(g.first, g.second));
        h->addLayout(left);
        QVBoxLayout* right = new QVBoxLayout();
        right->setSpacing(6);
        QWidget* holder = new QWidget(this);
        holder->setFixedWidth(rightW);
        QVBoxLayout* inner = new QVBoxLayout(holder);
        inner->setContentsMargins(0, 0, 0, 0);
        inner->setSpacing(7);
        right->addWidget(holder);
        h->addLayout(right, 1);
        root_->addLayout(h);
        return inner;
    }

    static QRectF dot(const QPointF& c, double r) { return QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r); }

    // ============================================================================================ Distortion
    // Distortion: the graph is the transfer curve (level in against level out, both dB, from -84 up to 0 dB: "-Inf.").
    // Pulling it with the mouse sets Drive.
    double distortionOut(double inDb) const
    {
        const double drive = std::pow(10.0, get(0) / 20.0);
        const int shape = (int)std::lround(get(1));
        const double x = std::pow(10.0, inDb / 20.0);
        double y;
        switch (shape) {
        case 1: y = std::min(1.0, x * drive); break;
        case 2: {
            double t = x * drive * 0.25 + 0.25;
            t -= std::floor(t);
            y = std::fabs(4 * std::fabs(t - 0.5) - 1);
            break;
        }
        case 3: {
            const double levels = std::pow(2.0, std::lround(get(3)) - 1);
            y = std::round(x * levels) / levels;
            break;
        }
        default: y = std::tanh(x * drive); break;
        }
        return y > 1e-9 ? 20.0 * std::log10(y) : -200.0;
    }

    void distortion()
    {
        QHBoxLayout* top = new QHBoxLayout();
        top->setSpacing(0);
        top->addWidget(gainColumn(4, "<u>D</u>ry out"));
        top->addWidget(gainColumn(5, "<u>W</u>et out"));
        top->addSpacing(8);
        QVBoxLayout* gv = new QVBoxLayout();
        gv->setSpacing(1);
        gv->addWidget(text("<u>G</u>raph:"));
        Graph* g = graph(QSize(262, 236));
        g->margins = QMargins(32, 4, 8, 16);
        g->draw = [this](QPainter& p, const QRectF& pl) {
            const double lo = -84, hi = 0;
            auto X = [&](double db) { return pl.left() + (db - lo) / (hi - lo) * pl.width(); };
            auto Y = [&](double db) { return pl.bottom() - (db - lo) / (hi - lo) * pl.height(); };
            p.setPen(QPen(QColor(70, 70, 70), 1));
            for (int k = 0; k <= 7; ++k) {
                const double db = lo + k * 12;
                p.drawLine(QPointF(std::round(X(db)) + 0.5, pl.top()), QPointF(std::round(X(db)) + 0.5, pl.bottom()));
                p.drawLine(QPointF(pl.left(), std::round(Y(db)) + 0.5), QPointF(pl.right(), std::round(Y(db)) + 0.5));
            }
            p.setPen(Qt::black);
            const QFontMetrics fm(p.font());
            for (int k = 0; k <= 7; ++k) {
                const double db = lo + k * 12;
                const QString t = k == 0 ? QString("-Inf.") : QString::number((int)db);
                p.drawText(QRectF(X(db) - 20, pl.bottom() + 1, 40, 14), Qt::AlignHCenter | Qt::AlignTop, t);
                p.drawText(QRectF(0, Y(db) - 7, pl.left() - 2, 14), Qt::AlignRight | Qt::AlignVCenter, t);
            }
            QPainterPath path;
            bool first = true;
            for (int px = 0; px <= (int)pl.width(); ++px) {
                const double inDb = lo + px / pl.width() * (hi - lo);
                const double out = std::max(lo, std::min(hi, distortionOut(inDb)));
                const QPointF pt(pl.left() + px, Y(out));
                if (first) path.moveTo(pt);
                else path.lineTo(pt);
                first = false;
            }
            p.setClipRect(pl.adjusted(-1, -1, 1, 1));
            p.setRenderHint(QPainter::Antialiasing, true);
            QPen pen(Qt::black, 1);
            pen.setStyle(Qt::DashLine);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
            p.setPen(Qt::black);
            p.setBrush(Qt::white);
            p.drawRect(dot(QPointF(pl.left() + 1, pl.bottom() - 1), 2.5));
            p.drawRect(dot(QPointF(pl.right() - 1, pl.top() + 1), 2.5));
        };
        g->mouse = [this, g](const QPointF& at, int phase) {
            if (phase == 0) beginDrag(0);
            if (phase != 2) {
                // the curve is pulled to the pointer: Drive is how far above the input line it is
                const double inDb = -84 + std::max(0.0, std::min(1.0, at.x())) * 84, outDb = -84 + std::max(0.0, std::min(1.0, at.y())) * 84;
                commit(0, std::max(0.0, std::min(60.0, std::round((outDb - inDb) * 2) / 2)));
            } else {
                endDrag();
            }
            g->update();
        };
        gv->addWidget(g);
        top->addLayout(gv, 1);
        root_->addLayout(top);

        QHBoxLayout* shape = new QHBoxLayout();
        shape->setSpacing(6);
        shape->addWidget(text("Shape:"));
        QComboBox* c = combo(fixed(1), {"Soft clip", "Hard clip", "Fold", "Bit crush"}, "dxParam1");
        c->setFixedWidth(110);
        shape->addWidget(c);
        shape->addStretch(1);
        QPushButton* reset = button("Reset");
        reset->setObjectName("dxReset");
        QObject::connect(reset, &QPushButton::clicked, reset, [this] {
            commit(0, 20);
            commit(1, 0);
            commit(2, 8000);
            commit(3, 6);
        });
        shape->addWidget(reset);
        root_->addLayout(shape);

        SMap drive;
        drive.dec = 1;
        root_->addWidget(lineRow(190, fixed(0), "Drive (0 to 60 dB):", drive, "dxParam0"));
        SMap tone;
        tone.log = true;
        tone.dec = 0;
        QCheckBox* lp = nullptr;
        QLineEdit* toneBox = nullptr;
        DxSlider* toneSlider = nullptr;
        root_->addWidget(lineRow(176, fixed(2), "Low-pass start freq. (Hz):", tone, "dxParam2", &lp, &toneBox, &toneSlider));
        lp->setObjectName("dxLowPass");
        QObject::connect(lp, &QCheckBox::toggled, lp, [this](bool on) { commit(2, on ? 8000 : 20000); });
        sync_.push_back([=] {
            QSignalBlocker b(lp);
            const bool on = get(2) < 19999.5;
            lp->setChecked(on);
            toneSlider->setEnabled(on);
            toneBox->setEnabled(on);
        });
        SMap bits;
        bits.dec = 0;
        QLineEdit* bitsBox = nullptr;
        DxSlider* bitsSlider = nullptr;
        root_->addWidget(lineRow(190, fixed(3), "Crush bits (1 to 16):", bits, "dxParam3", nullptr, &bitsBox, &bitsSlider));
        sync_.push_back([=] {
            const bool on = (int)std::lround(get(1)) == 3;
            bitsSlider->setEnabled(on);
            bitsBox->setEnabled(on);
        });
    }

    // ============================================================================================ Vibrato
    // Vibrato: the graph is the pitch over one turn of the modulation (semitones, up to the setting's 2); its handle pulls the depth.
    static double waveAt(int w, double ph)
    {
        ph -= std::floor(ph);
        if (w == 1) return ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph;
        if (w == 2) return ph < 0.5 ? 1 : -1;
        return std::sin(2 * kPi * ph);
    }

    void vibrato()
    {
        QHBoxLayout* top = new QHBoxLayout();
        top->setSpacing(0);
        top->addWidget(gainColumn(4, "<u>O</u>utput gain"));
        // Semitones: the depth in cents on the setting, shown in semitones
        {
            QWidget* col = new QWidget(this);
            QVBoxLayout* v = new QVBoxLayout(col);
            v->setContentsMargins(0, 0, 0, 0);
            v->setSpacing(3);
            QLabel* readout = text(QString(), Qt::AlignHCenter | Qt::AlignVCenter);
            readout->setFixedHeight(16);
            v->addWidget(readout);
            SMap m;
            m.dec = 0;
            DxSlider* s = slider(Qt::Vertical, fixed(1), m, "dxParam1", nullptr, nullptr);
            s->setFixedHeight(118);
            v->addWidget(s, 0, Qt::AlignHCenter);
            v->addWidget(text("Se<u>m</u>itones<br>(0 to 2)", Qt::AlignHCenter | Qt::AlignTop));
            sync_.push_back([=] { readout->setText(QString::number(get(1) / 100.0, 'f', 2)); });
            col->setFixedWidth(84);
            top->addWidget(col);
        }
        top->addSpacing(8);
        QVBoxLayout* gv = new QVBoxLayout();
        gv->setSpacing(1);
        gv->addWidget(text("<u>G</u>raph:"));
        Graph* g = graph(QSize(250, 178));
        g->margins = QMargins(32, 4, 4, 14);
        g->draw = [this](QPainter& p, const QRectF& pl) {
            const double range = 2.0;
            const double depth = get(1) / 100.0;
            const int w = (int)std::lround(get(2));
            auto X = [&](double t) { return pl.left() + t * pl.width(); };
            auto Y = [&](double st) { return pl.center().y() - st / range * (pl.height() / 2); };
            QPen dotted(QColor(60, 60, 60), 1, Qt::DotLine);
            p.setPen(dotted);
            p.drawRect(pl.adjusted(0.5, 0.5, -0.5, -0.5));
            p.setPen(Qt::black);
            p.drawLine(QPointF(pl.left(), std::round(pl.center().y()) + 0.5), QPointF(pl.right(), std::round(pl.center().y()) + 0.5));
            p.drawLine(QPointF(std::round(pl.center().x()) + 0.5, pl.top()), QPointF(std::round(pl.center().x()) + 0.5, pl.bottom()));
            p.drawText(QRectF(0, pl.top() - 6, pl.left() - 3, 12), Qt::AlignRight | Qt::AlignVCenter, "2.00");
            p.drawText(QRectF(0, pl.center().y() - 6, pl.left() - 3, 12), Qt::AlignRight | Qt::AlignVCenter, "0");
            p.drawText(QRectF(0, pl.bottom() - 6, pl.left() - 3, 12), Qt::AlignRight | Qt::AlignVCenter, "-2.00");
            p.drawText(QRectF(pl.left(), pl.bottom() + 1, pl.width(), 13), Qt::AlignHCenter | Qt::AlignTop, "Time");
            QPainterPath path;
            const int n = std::max(2, (int)pl.width());
            for (int k = 0; k <= n; ++k) {
                const double t = (double)k / n;
                const QPointF pt(X(t), Y(waveAt(w, t) * depth));
                if (k == 0) path.moveTo(pt);
                else path.lineTo(pt);
            }
            p.setClipRect(pl.adjusted(-1, -1, 1, 1));
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(QPen(Qt::black, 1));
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
            p.setBrush(Qt::white);
            const double sq = 3.5;
            p.drawRect(QRectF(X(0.25) - sq, Y(depth) - sq, 2 * sq, 2 * sq));
            p.drawRect(QRectF(X(0) - sq, Y(0) - sq, 2 * sq, 2 * sq));
            p.drawRect(QRectF(X(1) - sq, Y(0) - sq, 2 * sq, 2 * sq));
        };
        g->mouse = [this, g](const QPointF& at, int phase) {
            if (phase == 0) beginDrag(1);
            if (phase != 2) commit(1, std::round(std::max(0.0, std::min(1.0, (at.y() - 0.5) * 2)) * 2.0 * 100.0));
            else endDrag();
            g->update();
        };
        gv->addWidget(g);
        QHBoxLayout* rs = new QHBoxLayout();
        rs->addStretch(1);
        QPushButton* reset = button("Reset");
        reset->setObjectName("dxReset");
        QObject::connect(reset, &QPushButton::clicked, reset, [this] {
            commit(1, 40);
            commit(2, 0);
        });
        rs->addWidget(reset);
        gv->addLayout(rs);
        top->addLayout(gv, 1);
        root_->addLayout(top);

        QHBoxLayout* wf = new QHBoxLayout();
        wf->setSpacing(6);
        wf->addWidget(text("Waveform:"));
        wf->addWidget(radios(2, {"Sine", "Triangle", "Square"}), 1);
        root_->addLayout(wf);
        SMap rate;
        rate.log = true;
        rate.dec = 3;
        root_->addWidget(block(0, "<u>M</u>odulation freq.", "Hz", rate));
        SMap ph;
        ph.dec = 0;
        root_->addWidget(block(3, "<u>S</u>tereo phase", "degrees", ph));
    }

    // ============================================================================================ the other effects
    void pitchShift()
    {
        QVBoxLayout* r = twoColumns({{3, "<u>D</u>ry out"}, {4, "<u>E</u>ffect out"}});
        SMap st;
        st.dec = 0;
        r->addWidget(block(0, "<u>S</u>emitones", "", st));
        r->addWidget(block(1, "<u>C</u>ents", "", st));
        SMap win;
        win.dec = 0;
        r->addWidget(block(2, "<u>W</u>indow", "ms", win));
        r->addStretch(1);
    }

    void chorus()
    {
        QVBoxLayout* r = twoColumns({{0, "<u>D</u>ry out"}, {1, "<u>C</u>horus out"}});
        SMap one;
        one.dec = 0;
        r->addWidget(block(2, "<u>V</u>oices", "", one));
        SMap d1;
        d1.dec = 1;
        r->addWidget(block(3, "De<u>l</u>ay", "ms", d1));
        r->addWidget(block(4, "D<u>e</u>pth", "ms", d1));
        SMap rate;
        rate.dec = 2;
        rate.log = true;
        r->addWidget(block(5, "<u>R</u>ate", "Hz", rate));
        SMap pc;
        pc.dec = 0;
        r->addWidget(block(6, "<u>F</u>eedback", "%", pc));
        r->addWidget(block(7, "<u>S</u>tereo spread", "%", pc));
    }

    void flangeWah()
    {
        QVBoxLayout* r = twoColumns({{5, "<u>D</u>ry out"}, {6, "<u>E</u>ffect out"}});
        QHBoxLayout* eff = new QHBoxLayout();
        eff->setSpacing(6);
        eff->addWidget(text("Effect:"));
        eff->addWidget(radios(0, {"Flange", "Wah-wah", "Phaser"}), 1);
        r->addLayout(eff);
        SMap rate;
        rate.dec = 2;
        rate.log = true;
        r->addWidget(block(1, "<u>R</u>ate", "Hz", rate));
        SMap pc;
        pc.dec = 0;
        r->addWidget(block(2, "De<u>p</u>th", "%", pc));
        r->addWidget(block(3, "De<u>l</u>ay / frequency", "%", pc));
        r->addWidget(block(4, "<u>F</u>eedback / resonance", "%", pc));
    }

    void ampMod()
    {
        QVBoxLayout* r = twoColumns({{4, "<u>O</u>utput gain"}});
        // output range does not reach silence: its lowest value reads as it is
        SMap rate;
        rate.dec = 2;
        rate.log = true;
        r->addWidget(block(0, "Modulation <u>f</u>req.", "Hz", rate));
        SMap pc;
        pc.dec = 0;
        r->addWidget(block(1, "Modulation <u>d</u>epth", "%", pc));
        QHBoxLayout* wf = new QHBoxLayout();
        wf->setSpacing(6);
        wf->addWidget(text("Waveform:"));
        r->addLayout(wf);
        r->addWidget(radios(2, {"Sine", "Triangle", "Square", "Sawtooth"}));
        r->addWidget(checkBox(3, "Stereo <u>p</u>an (left and right take turns)"));
        r->addStretch(1);
    }

    void simpleDelay()
    {
        QVBoxLayout* r = twoColumns({{0, "<u>D</u>ry out"}, {1, "D<u>e</u>lay out"}});
        SMap ms;
        ms.dec = 0;
        r->addWidget(block(2, "Delay <u>t</u>ime", "ms", ms));
        r->addWidget(block(3, "<u>F</u>eedback", "%", ms));
        r->addWidget(checkBox(4, "<u>P</u>ing-pong (the echoes go from side to side)"));
        r->addStretch(1);
    }

    void reverb()
    {
        QVBoxLayout* r = twoColumns({{3, "<u>D</u>ry out"}, {4, "<u>R</u>everb out"}});
        SMap pc;
        pc.dec = 0;
        r->addWidget(block(0, "Room <u>s</u>ize", "%", pc));
        r->addWidget(block(1, "Da<u>m</u>ping", "%", pc));
        r->addWidget(block(2, "<u>W</u>idth", "%", pc));
        r->addStretch(1);
    }

    // ============================================================================================ Modern Equalizer
    // Modern Equalizer: eight bands, each Enabled, Type, Frequency, Gain, Q, Channel, Slope (setting band * 7 + that),
    // then Global Scale, Mono Below, Stereo Width, Output Gain (56..59). The graph is their combined response; a band's
    // point is pulled with the mouse (frequency across, gain up and down).
    int band_ = 0;
    int eq(int k) const { return band_ * 7 + k; }

    // Response of one band at 'freq' in dB (same filters as the effect: biquads; 12 dB per octave is one, 24 two, up to three).
    double bandDb(int b, double freq, double scale) const
    {
        const int o = b * 7;
        if (get(o) < 0.5) return 0.0;
        const int type = (int)std::lround(get(o + 1));
        const int slope = (int)std::lround(get(o + 6));
        int stages = (type <= 1 || type == 4) ? std::max(1, (slope + 2) / 2) : 1;
        stages = std::min(stages, 3);
        const double sr = 48000.0, f0 = std::max(10.0, std::min(get(o + 2), sr * 0.45));
        double db = 0;
        for (int st = 0; st < stages; ++st) {
            const double Q = std::max(0.05, st == 0 ? get(o + 4) : 0.71), gain = get(o + 3) * scale;
            const double w0 = 2 * kPi * f0 / sr, cs = std::cos(w0), sn = std::sin(w0), A = std::pow(10.0, gain / 40.0);
            const double alpha = sn / (2 * Q), sa = 2 * std::sqrt(A) * (sn / 2 * std::sqrt(2.0));
            double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
            switch (type) {
            case 0: b0 = (1 + cs) / 2; b1 = -(1 + cs); b2 = (1 + cs) / 2; a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha; break;
            case 1: b0 = (1 - cs) / 2; b1 = 1 - cs; b2 = (1 - cs) / 2; a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha; break;
            case 2:
                b0 = A * ((A + 1) - (A - 1) * cs + sa); b1 = 2 * A * ((A - 1) - (A + 1) * cs); b2 = A * ((A + 1) - (A - 1) * cs - sa);
                a0 = (A + 1) + (A - 1) * cs + sa; a1 = -2 * ((A - 1) + (A + 1) * cs); a2 = (A + 1) + (A - 1) * cs - sa;
                break;
            case 3:
                b0 = A * ((A + 1) + (A - 1) * cs + sa); b1 = -2 * A * ((A - 1) + (A + 1) * cs); b2 = A * ((A + 1) + (A - 1) * cs - sa);
                a0 = (A + 1) - (A - 1) * cs + sa; a1 = 2 * ((A - 1) - (A + 1) * cs); a2 = (A + 1) - (A - 1) * cs - sa;
                break;
            case 4: b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha; break;
            default: b0 = 1 + alpha * A; b1 = -2 * cs; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cs; a2 = 1 - alpha / A; break;
            }
            const double w = 2 * kPi * std::max(1.0, std::min(freq, sr * 0.499)) / sr;
            const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2 * w);
            const std::complex<double> num = b0 + b1 * z1 + b2 * z2, den = a0 + a1 * z1 + a2 * z2;
            const double mag = std::abs(num) / std::max(1e-12, std::abs(den));
            db += 20.0 * std::log10(std::max(1e-9, mag));
        }
        return db;
    }

    void modernEq()
    {
        Graph* g = graph(QSize(544, 224));
        g->margins = QMargins(32, 6, 8, 16);
        g->setObjectName("dxEqGraph");
        auto fx = [](const QRectF& pl, double hz) { return pl.left() + std::log(hz / 20.0) / std::log(1000.0) * pl.width(); };
        auto gy = [](const QRectF& pl, double db) { return pl.center().y() - db / 24.0 * (pl.height() / 2); };
        g->draw = [this, fx, gy](QPainter& p, const QRectF& pl) {
            p.setPen(QColor(205, 205, 205));
            for (int db = -24; db <= 24; db += 6) p.drawLine(QPointF(pl.left(), std::round(gy(pl, db)) + 0.5), QPointF(pl.right(), std::round(gy(pl, db)) + 0.5));
            static const double marks[] = {20, 30, 40, 50, 60, 70, 80, 90, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 2000, 3000, 4000, 5000,
                                           6000, 7000, 8000, 9000, 10000, 20000};
            for (double hz : marks) p.drawLine(QPointF(std::round(fx(pl, hz)) + 0.5, pl.top()), QPointF(std::round(fx(pl, hz)) + 0.5, pl.bottom()));
            p.setPen(QColor(110, 110, 110));
            for (int db : {-24, -12, 0, 12, 24}) p.drawLine(QPointF(pl.left(), std::round(gy(pl, db)) + 0.5), QPointF(pl.right(), std::round(gy(pl, db)) + 0.5));
            for (double hz : {100.0, 1000.0, 10000.0}) p.drawLine(QPointF(std::round(fx(pl, hz)) + 0.5, pl.top()), QPointF(std::round(fx(pl, hz)) + 0.5, pl.bottom()));
            p.setPen(Qt::black);
            for (int db : {-24, -12, 0, 12, 24}) p.drawText(QRectF(0, gy(pl, db) - 7, pl.left() - 3, 14), Qt::AlignRight | Qt::AlignVCenter, QString::number(db));
            for (const auto& m : {std::make_pair(20.0, "20"), std::make_pair(100.0, "100"), std::make_pair(1000.0, "1k"), std::make_pair(10000.0, "10k"), std::make_pair(20000.0, "20k")})
                p.drawText(QRectF(fx(pl, m.first) - 20, pl.bottom() + 1, 40, 14), Qt::AlignHCenter | Qt::AlignTop, m.second);
            p.setPen(QColor(60, 60, 60));
            p.setBrush(Qt::NoBrush);
            p.drawRect(pl.adjusted(0.5, 0.5, -0.5, -0.5));
            const double scale = get(56) / 100.0;
            QPainterPath path;
            const int n = std::max(2, (int)pl.width());
            for (int k = 0; k <= n; ++k) {
                const double hz = 20.0 * std::pow(1000.0, (double)k / n);
                double db = 0;
                for (int b = 0; b < 8; ++b) db += bandDb(b, hz, scale);
                db = std::max(-24.0, std::min(24.0, db));
                const QPointF pt(pl.left() + k * pl.width() / n, gy(pl, db));
                if (k == 0) path.moveTo(pt);
                else path.lineTo(pt);
            }
            p.setClipRect(pl.adjusted(1, 1, -1, -1));
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(QPen(QColor(0, 110, 210), 1.6));
            p.drawPath(path);
            p.setClipping(false);
            for (int b = 0; b < 8; ++b) {
                if (get(b * 7) < 0.5) continue;
                const int type = (int)std::lround(get(b * 7 + 1));
                const bool hasGain = type == 2 || type == 3 || type == 5;
                const QPointF c(fx(pl, std::max(20.0, std::min(20000.0, get(b * 7 + 2)))), gy(pl, hasGain ? std::max(-24.0, std::min(24.0, get(b * 7 + 3) * scale)) : 0.0));
                p.setPen(QPen(Qt::black, b == band_ ? 2 : 1));
                p.setBrush(b == band_ ? QColor(255, 214, 90) : QColor(255, 255, 255));
                p.drawEllipse(dot(c, 7.5));
                p.setPen(Qt::black);
                p.drawText(dot(c, 7.5), Qt::AlignCenter, QString::number(b + 1));
            }
        };
        g->mouse = [this, g, fx, gy](const QPointF& at, int phase) {
            const QRectF pl = g->plot();
            const QPointF px(pl.left() + at.x() * pl.width(), pl.bottom() - at.y() * pl.height());
            if (phase == 0) {
                int best = -1;
                double bestD = 11;
                const double scale = get(56) / 100.0;
                for (int b = 0; b < 8; ++b) {
                    if (get(b * 7) < 0.5) continue;
                    const int type = (int)std::lround(get(b * 7 + 1));
                    const bool hasGain = type == 2 || type == 3 || type == 5;
                    const QPointF c(fx(pl, std::max(20.0, std::min(20000.0, get(b * 7 + 2)))), gy(pl, hasGain ? std::max(-24.0, std::min(24.0, get(b * 7 + 3) * scale)) : 0.0));
                    const double d = std::hypot(c.x() - px.x(), c.y() - px.y());
                    if (d < bestD) { bestD = d; best = b; }
                }
                if (best < 0) return;
                dragBand_ = best;
                selectBand(best);
                beginDrag(best * 7 + 2);
                return;
            }
            if (dragBand_ < 0) return;
            if (phase == 1) {
                const int o = dragBand_ * 7;
                const double hz = 20.0 * std::pow(1000.0, std::max(0.0, std::min(1.0, at.x())));
                commit(o + 2, std::round(hz));
                const int type = (int)std::lround(get(o + 1));
                if (type == 2 || type == 3 || type == 5) {
                    const double scale = std::max(0.01, std::fabs(get(56) / 100.0));
                    commit(o + 3, std::round((at.y() - 0.5) * 2 * 24.0 / scale * 10) / 10.0);
                }
            } else {
                dragBand_ = -1;
                endDrag();
            }
        };
        root_->addWidget(g);

        QHBoxLayout* bands = new QHBoxLayout();
        bands->setSpacing(4);
        bands->addWidget(text("Band:"));
        bandButtons_ = new QButtonGroup(this);
        for (int b = 0; b < 8; ++b) {
            QPushButton* pb = new QPushButton(QString::number(b + 1), this);
            pb->setObjectName(QString("dxEqBand%1").arg(b));
            pb->setCheckable(true);
            pb->setFixedSize(28, 22);
            pb->setFocusPolicy(Qt::TabFocus);
            bandButtons_->addButton(pb, b);
            bands->addWidget(pb);
            QObject::connect(pb, &QPushButton::clicked, pb, [this, b] { selectBand(b); });
        }
        bands->addSpacing(10);
        QCheckBox* en = new QCheckBox("&Enabled", this);
        en->setObjectName("dxEqEnabled");
        QObject::connect(en, &QCheckBox::toggled, en, [this](bool on) { commit(eq(0), on ? 1 : 0); });
        sync_.push_back([=] {
            QSignalBlocker b(en);
            en->setChecked(get(eq(0)) >= 0.5);
            for (int k = 0; k < 8; ++k) {
                QAbstractButton* pb = bandButtons_->button(k);
                QSignalBlocker bb(pb);
                pb->setChecked(k == band_);
                QFont f = pb->font();
                f.setBold(get(k * 7) >= 0.5);
                pb->setFont(f);
            }
        });
        bands->addWidget(en);
        bands->addStretch(1);
        root_->addLayout(bands);

        QHBoxLayout* kinds = new QHBoxLayout();
        kinds->setSpacing(6);
        kinds->addWidget(text("Type:"));
        kinds->addWidget(combo([this] { return eq(1); }, {"High Pass", "Low Pass", "Low Shelving", "High Shelving", "Band Pass", "Peaking"}, "dxEqType"));
        kinds->addSpacing(6);
        kinds->addWidget(text("Channel:"));
        kinds->addWidget(combo([this] { return eq(5); }, {"Stereo", "Left", "Right", "Mid", "Side"}, "dxEqChannel"));
        kinds->addSpacing(6);
        kinds->addWidget(text("Slope:"));
        kinds->addWidget(combo([this] { return eq(6); }, {"6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct", "30 dB/oct", "36 dB/oct"}, "dxEqSlope"));
        kinds->addStretch(1);
        root_->addLayout(kinds);

        SMap freq;
        freq.log = true;
        freq.dec = 0;
        root_->addWidget(lineRow(120, [this] { return eq(2); }, "<u>F</u>requency (Hz):", freq, "dxEqFrequency"));
        SMap gain;
        gain.dec = 1;
        root_->addWidget(lineRow(120, [this] { return eq(3); }, "<u>G</u>ain (dB):", gain, "dxEqGain"));
        SMap q;
        q.dec = 2;
        q.log = true;
        root_->addWidget(lineRow(120, [this] { return eq(4); }, "<u>Q</u>:", q, "dxEqQ"));
        QFrame* line = new QFrame(this);
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Sunken);
        root_->addWidget(line);
        SMap pc;
        pc.dec = 0;
        root_->addWidget(lineRow(120, fixed(56), "Global <u>s</u>cale (%):", pc, "dxParam56"));
        root_->addWidget(lineRow(120, fixed(57), "<u>M</u>ono below (Hz):", pc, "dxParam57"));
        root_->addWidget(lineRow(120, fixed(58), "Stereo <u>w</u>idth (%):", pc, "dxParam58"));
        SMap og;
        og.dec = 1;
        root_->addWidget(lineRow(120, fixed(59), "<u>O</u>utput gain (dB):", og, "dxParam59"));
    }

    void selectBand(int b)
    {
        if (b < 0 || b > 7 || b == band_) return;
        band_ = b;
        syncAll();
    }
    int dragBand_ = -1;
    QButtonGroup* bandButtons_ = nullptr;
};

const char* const kUids[] = {"reno.distortion", "reno.vibrato", "reno.pitchshift", "reno.chorus", "reno.flangewah",
                             "reno.ampmod", "reno.simpledelay", "reno.reverb", "reno.moderneq"};

}

bool hasBuiltinPage(const std::string& uid)
{
    for (const char* u : kUids)
        if (uid == u) return true;
    return false;
}

DxPage* makeBuiltinPage(QWidget* parent, const std::string& uid, const DxHost& host)
{
    if (!hasBuiltinPage(uid)) return nullptr;
    return new Page(parent, uid, host);
}

}
}
