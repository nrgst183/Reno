// The pages of the reproductions of VEGAS's sfppack2 / sfresfilter effects (included inside class Page in fxpages.cpp).

public:
    // Which pages this file has, and making them (the constructor and hasBuiltinPage call these, so a new page needs no
    // change in fxpages.cpp)
    static bool pack2Has(const std::string& uid) { return uid == "reno.noisegate" || uid == "reno.graphiceq" || uid == "reno.parametriceq"; }

private:
    bool pack2Page(const std::string& uid, int* width)
    {
        if (uid == "reno.noisegate") {
            noiseGate();
            *width = 400;
        } else if (uid == "reno.graphiceq") {
            graphicEq();
            *width = 560;
        } else if (uid == "reno.parametriceq") {
            parametricEq();
            *width = 470;
        } else {
            return false;
        }
        return true;
    }

    // ============================================================================================ Noise Gate
    // VEGAS's Noise Gate page: the threshold fader on the left, attack and release time on the right.
    void noiseGate()
    {
        QHBoxLayout* h = new QHBoxLayout();
        h->setSpacing(6);
        h->addWidget(gainColumn(2, "Threshold le<u>v</u>el", true, 104));
        QWidget* holder = new QWidget(this);
        holder->setFixedWidth(264);
        QVBoxLayout* r = new QVBoxLayout(holder);
        r->setContentsMargins(0, 0, 0, 0);
        r->setSpacing(8);
        SMap a;
        a.dec = 0;
        r->addWidget(block(0, "A<u>t</u>tack time", "ms", a));
        SMap rel;
        rel.dec = 0;
        r->addWidget(block(1, "R<u>e</u>lease time", "ms", rel));
        r->addStretch(1);
        h->addWidget(holder, 1);
        root_->addLayout(h);
    }

    // ============================================================================================ Graphic EQ
    // VEGAS's Graphic EQ page: three views of one curve - 10 band and 20 band faders and the envelope graph (in all of
    // them the faders on the right are the accuracy and the output gain). The band faders move trapezoids of the
    // envelope (veggeq::bandToPoints, as the plug-in's own page does); the envelope's points can be dragged.
    veggeq::Settings geqCache_;
    veggeq::Settings geqSettings() const
    {
        veggeq::Settings s;
        if (h_.state) veggeq::decode(h_.state(), &s);
        return s;
    }
    void geqStore(const veggeq::Settings& s, const std::string& what)
    {
        if (updating_ || !h_.setState) return;
        h_.setState(veggeq::encode(s), what);
        syncAll();
    }

    // one fader of a band view: band b (0..19), w the trapezoid's half width (250 on the 20 band view, 500 on the 10 band one)
    QWidget* geqBand(int b, int w, const QString& center, int colW, int fontPx, const QString& objName)
    {
        QWidget* col = new QWidget(this);
        col->setFixedWidth(colW);
        QVBoxLayout* v = new QVBoxLayout(col);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(3);
        QLabel* readout = text(QString(), Qt::AlignHCenter | Qt::AlignVCenter);
        readout->setFixedHeight(16);
        readout->setStyleSheet(QString("font-size: %1px;").arg(fontPx));
        v->addWidget(readout);
        DxSlider* s = new DxSlider(Qt::Vertical, this);
        s->setObjectName(objName);
        s->setRange(0, 720);
        s->setSingleStep(10);
        s->setPageStep(30);
        s->setFixedHeight(118);
        v->addWidget(s, 0, Qt::AlignHCenter);
        QLabel* cap = text(center, Qt::AlignHCenter | Qt::AlignTop);
        cap->setStyleSheet(QString("font-size: %1px;").arg(fontPx));
        v->addWidget(cap);
        QObject::connect(s, &QSlider::sliderPressed, s, [this, b] {
            if (h_.beginEdit) h_.beginEdit(strfmt("Change band %d gain", b + 1));
        });
        QObject::connect(s, &QSlider::sliderReleased, s, [this] { endDrag(); });
        QObject::connect(s, &QSlider::valueChanged, s, [this, b, w](int pos) {
            if (updating_) return;
            const int tenths = pos - 480;
            veggeq::Settings st = geqSettings();
            if (st.band[b] == tenths) return;
            st.band[b] = tenths;
            int32_t p[veggeq::kMaxPoints + 8][2] = {};
            for (int k = 0; k < st.count; ++k) p[k][0] = st.pt[k][0], p[k][1] = st.pt[k][1];
            int n = st.count;
            veggeq::bandToPoints(p, &n, b, tenths, w);
            st.count = n;
            for (int k = 0; k < n; ++k) st.pt[k][0] = p[k][0], st.pt[k][1] = p[k][1];
            geqStore(st, strfmt("Change band %d gain", b + 1));
        });
        sync_.push_back([this, b, s, readout] {
            const int tenths = geqCache_.band[b];
            QSignalBlocker blk(s);
            if (!s->isSliderDown()) s->setValue(tenths + 480);
            readout->setText(tenths <= -480 ? QString("-Inf.") : QString::number(tenths / 10.0, 'f', 1));
        });
        return col;
    }

    QWidget* geqBandsView(bool twenty)
    {
        static const char* const names20[20] = {"20", "28", "40", "56", "80", "113", "160", "225", "320", "450", "640", "900", "1.3k", "1.8k", "2.5k", "3.6k", "5.1k", "7.2k", "10k", "15k"};
        QWidget* view = new QWidget(this);
        QHBoxLayout* h = new QHBoxLayout(view);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);
        QVBoxLayout* left = new QVBoxLayout();
        left->setSpacing(0);
        QLabel* gl = text("<u>G</u>ain:<br>(-Inf. to 24 dB)");
        gl->setFixedHeight(32);
        left->addWidget(gl);
        left->addStretch(1);
        left->addWidget(text("Center (Hz):"));
        QWidget* lw = new QWidget(this);
        lw->setFixedWidth(112);
        lw->setLayout(left);
        h->addWidget(lw);
        const int colW = twenty ? 30 : 56;
        for (int b = 0; b < veggeq::kBands; ++b) {
            if (!twenty && b % 2 == 0) continue;
            h->addWidget(geqBand(b, twenty ? 250 : 500, names20[b], colW, twenty ? 10 : 12, QString(twenty ? "dxGeq20Band%1" : "dxGeq10Band%1").arg(b)));
        }
        h->addStretch(1);
        return view;
    }

    static QString geqFreqText(int x)
    {
        const double f = veggeq::freqOf(x);
        return groupedNumber(std::floor(f + 0.5), 0);
    }
    static QString geqDbText(int y)
    {
        if (y <= -500) return QString("-Inf.");
        return QString::number(veggeq::posDb(y), 'f', 1);
    }

    QWidget* geqEnvelopeView()
    {
        QWidget* view = new QWidget(this);
        QVBoxLayout* v = new QVBoxLayout(view);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(1);
        QHBoxLayout* cap = new QHBoxLayout();
        cap->addWidget(text("Fr<u>e</u>quency graph:"));
        QLabel* readout = text(QString(), Qt::AlignRight | Qt::AlignVCenter);
        readout->setObjectName("dxGraphReadout");
        cap->addWidget(readout, 1);
        v->addLayout(cap);
        CurveGraph* g = new CurveGraph(this);
        g->setObjectName("dxGraph");
        g->setFixedSize(QSize(500, 172));
        g->margins = QMargins(40, 4, 16, 20);
        g->xMin = 0;
        g->xMax = 10000;
        g->yMin = -500;
        g->yMax = 400;
        g->maxPoints = veggeq::kMaxPoints;
        graphs_.push_back(g);
        g->readout = [readout](const QString& t) { readout->setText(t); };
        g->describe = [](const CurveGraph::Pt& p) { return QString("%1 Hz, %2 dB").arg(geqFreqText(p.x), geqDbText(p.y)); };
        g->describeBare = g->describe;
        g->paintGrid = [](QPainter& p, const CurveGraph& gr) {
            const QRectF pl = gr.plot();
            p.setPen(QPen(QColor(70, 70, 70), 1));
            for (int k = 0; k <= 10; ++k) {
                const double x = std::round(gr.at(k * 1000, 0).x()) + 0.5;
                p.drawLine(QPointF(x, pl.top()), QPointF(x, pl.bottom()));
            }
            static const int ys[10] = {400, 300, 200, 100, 0, -100, -200, -300, -400, -500};
            static const char* const yl[10] = {"+24", "+12", "+6", "+3", "dB 0", "-3", "-6", "-12", "-24", "-Inf."};
            static const char* const xl[11] = {"20", "40", "80", "160", "320", "640", "1.3k", "2.5k", "5.1k", "10k", "20k"};
            p.setPen(QPen(QColor(150, 150, 150), 1));
            for (int k = 0; k < 10; ++k) {
                const double y = std::round(gr.at(0, ys[k]).y()) + 0.5;
                p.drawLine(QPointF(pl.left(), y), QPointF(pl.right(), y));
            }
            p.setPen(Qt::black);
            for (int k = 0; k < 10; ++k) {
                const double y = gr.at(0, ys[k]).y();
                p.drawText(QRectF(0, y - 7, pl.left() - 3, 14), Qt::AlignRight | Qt::AlignVCenter, yl[k]);
            }
            for (int k = 0; k <= 10; ++k) {
                const double x = gr.at(k * 1000, 0).x();
                p.drawText(QRectF(x - 20, pl.bottom() + 1, 40, 14), Qt::AlignHCenter | Qt::AlignTop, xl[k]);
            }
        };
        g->edited = [this, g](int phase) {
            veggeq::Settings s = geqSettings();
            s.count = (int)g->pts.size();
            for (int k = 0; k < s.count; ++k) s.pt[k][0] = g->pts[(size_t)k].x, s.pt[k][1] = g->pts[(size_t)k].y;
            if (phase == 0 && h_.beginEdit) h_.beginEdit("Edit Graphic EQ envelope");
            geqStore(s, "Edit Graphic EQ envelope");
            if (phase == 2) endDrag();
        };
        sync_.push_back([this, g] {
            if (g->dragging()) return;
            g->pts.clear();
            for (int k = 0; k < geqCache_.count; ++k) g->pts.push_back(CurveGraph::Pt{geqCache_.pt[k][0], geqCache_.pt[k][1]});
        });
        v->addWidget(g);
        v->addStretch(1);
        return view;
    }

    void graphicEq()
    {
        sync_.push_back([this] { geqCache_ = geqSettings(); });          // first: the others read this
        QHBoxLayout* top = new QHBoxLayout();
        top->setSpacing(6);
        QTabWidget* tabs = new QTabWidget(this);
        tabs->setObjectName("dxGeqTabs");
        tabs->addTab(geqBandsView(false), "10 Band");
        tabs->addTab(geqBandsView(true), "20 Band");
        tabs->addTab(geqEnvelopeView(), "Envelope");
        top->addWidget(tabs, 1);
        top->addWidget(gainColumn(1, "<u>O</u>utput gain", false, 96), 0, Qt::AlignTop);
        root_->addLayout(top);

        QHBoxLayout* bottom = new QHBoxLayout();
        bottom->setSpacing(6);
        QLabel* al = text("A<u>c</u>curacy:");
        bottom->addWidget(al);
        QComboBox* acc = combo(fixed(0), {"Low (fast, good for high bands)", "Medium (great for mid and high bands)", "High (slow, great for all bands)"}, "dxParam0");
        acc->setMinimumWidth(240);
        al->setBuddy(acc);
        bottom->addWidget(acc);
        QPushButton* reset = button("Rese&t");
        reset->setObjectName("dxReset");
        QObject::connect(reset, &QPushButton::clicked, reset, [this, tabs] {
            veggeq::Settings s = geqSettings();
            s.count = 2;
            s.pt[0][0] = 0;
            s.pt[0][1] = 0;
            s.pt[1][0] = 10000;
            s.pt[1][1] = 0;
            if (tabs->currentIndex() != 2)
                for (int32_t& b : s.band) b = 0;
            geqStore(s, "Reset Graphic EQ");
        });
        bottom->addWidget(reset);
        bottom->addStretch(1);
        root_->addLayout(bottom);
    }

    // ============================================================================================ Parametric EQ
    // VEGAS's Parametric EQ page: output gain and amount faders on the left; on the right the filter style, the
    // frequency and width (slider and edit box each), the response of the filter and the accuracy.
    QWidget* pmeqRow(int i, const QString& label, bool log, int dec, const QString& objName)
    {
        QWidget* w = new QWidget(this);
        QVBoxLayout* v = new QVBoxLayout(w);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(1);
        v->addWidget(text(label));
        QHBoxLayout* h = new QHBoxLayout();
        h->setSpacing(4);
        SMap m;
        m.log = log;
        m.dec = dec;
        QLineEdit* box = editBox(62);
        DxSlider* s = slider(Qt::Horizontal, fixed(i), m, objName, box, nullptr);
        h->addWidget(s, 1);
        h->addWidget(box);
        v->addLayout(h);
        return w;
    }

    // the response of the filter at the settings now, over 20 Hz to 20 kHz (log) and -60 to +20 dB
    void pmeqPaint(QPainter& p, const QRectF& pl)
    {
        vegpm::Settings st;
        st.style = (int)std::lround(get(0));
        st.freq = (int)std::lround(get(1));
        st.width = (int)std::lround(get(2) * 10);
        st.amount = (int)std::lround(get(3) * 10);
        st.outGain = (int)std::lround(get(4) * 10);
        vegpm::sanitize(st);
        const int rate = 44100, n = 1024;
        std::vector<double> m((size_t)n, 1.0);
        vegpm::design(m.data(), n, rate, st);
        auto Y = [&](double db) { return pl.bottom() - (std::max(-60.0, std::min(20.0, db)) + 60.0) / 80.0 * pl.height(); };
        p.setPen(QPen(QColor(190, 190, 190), 1));
        for (int k = 0; k < 5; ++k) {
            const double y = std::round(Y(-60.0 + 20.0 * k)) + 0.5;
            p.drawLine(QPointF(pl.left(), y), QPointF(pl.right(), y));
        }
        p.setPen(QPen(QColor(110, 110, 110), 1));
        const double y0 = std::round(Y(0)) + 0.5;
        p.drawLine(QPointF(pl.left(), y0), QPointF(pl.right(), y0));
        QPainterPath path;
        const int w = std::max(2, (int)pl.width());
        for (int x = 0; x < w; ++x) {
            const double f = 20.0 * std::pow(1000.0, (double)x / (w - 1));
            int b = (int)(f / (rate * 0.5) * (n - 1) + 0.5);
            b = std::max(0, std::min(n - 1, b));
            const double db = m[(size_t)b] > 1e-6 ? 20.0 * std::log10(m[(size_t)b]) : -60.0;
            const QPointF pt(pl.left() + x, Y(db));
            if (x == 0) path.moveTo(pt);
            else path.lineTo(pt);
        }
        p.setPen(QPen(QColor(0, 0, 160), 1));
        p.drawPath(path);
    }

    void parametricEq()
    {
        QHBoxLayout* top = new QHBoxLayout();
        top->setSpacing(6);
        top->addWidget(gainColumn(4, "<u>O</u>utput gain", false, 76), 0, Qt::AlignTop);
        top->addWidget(gainColumn(3, "A<u>m</u>ount", false, 76), 0, Qt::AlignTop);
        QWidget* holder = new QWidget(this);
        QVBoxLayout* r = new QVBoxLayout(holder);
        r->setContentsMargins(0, 0, 0, 0);
        r->setSpacing(6);
        QHBoxLayout* st = new QHBoxLayout();
        QLabel* sl = text("F<u>i</u>lter style:");
        st->addWidget(sl);
        QComboBox* style = combo(fixed(0), {"Low-frequency shelf", "High-frequency shelf", "Band-pass", "Band-notch/boost"}, "dxParam0");
        sl->setBuddy(style);
        st->addWidget(style, 1);
        r->addLayout(st);
        r->addWidget(pmeqRow(1, "<u>F</u>requency (Hz):", true, 0, "dxParam1"));
        r->addWidget(pmeqRow(2, "<u>W</u>idth (0.1 to 5 oct.):", false, 1, "dxParam2"));
        Graph* g = graph(QSize(190, 66));
        g->setObjectName("dxPmeqPreview");
        g->margins = QMargins(1, 1, 1, 1);
        g->draw = [this](QPainter& p, const QRectF& pl) { pmeqPaint(p, pl); };
        r->addWidget(g);
        top->addWidget(holder, 1);
        root_->addLayout(top);

        QHBoxLayout* bottom = new QHBoxLayout();
        bottom->setSpacing(6);
        QLabel* al = text("A<u>c</u>curacy:");
        bottom->addWidget(al);
        QComboBox* acc = combo(fixed(5), {"Low (Fast, good for high frequencies)", "Medium (great for mid and high frequencies)", "High (Slow, great for all frequencies)"}, "dxParam5");
        al->setBuddy(acc);
        bottom->addWidget(acc, 1);
        root_->addLayout(bottom);
    }
