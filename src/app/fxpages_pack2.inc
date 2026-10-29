// The pages of the reproductions of VEGAS's sfppack2 / sfresfilter effects (included inside class Page in fxpages.cpp).

public:
    // Which pages this file has, and making them (the constructor and hasBuiltinPage call these, so a new page needs no
    // change in fxpages.cpp)
    static bool pack2Has(const std::string& uid) { return uid == "reno.noisegate" || uid == "reno.graphiceq" || uid == "reno.parametriceq" || uid == "reno.paragraphiceq" || uid == "reno.graphicdynamics" || uid == "reno.multibanddynamics" || uid == "reno.resonantfilter"; }

private:
    bool pack2Page(const std::string& uid, int* width)
    {
        if (uid == "reno.noisegate") {
            noiseGate();
            *width = 400;
        } else if (uid == "reno.graphiceq") {
            graphicEq();
            *width = 700;
        } else if (uid == "reno.parametriceq") {
            parametricEq();
            *width = 470;
        } else if (uid == "reno.paragraphiceq") {
            paragraphicEq();
            *width = 600;
        } else if (uid == "reno.graphicdynamics") {
            graphicDynamics();
            *width = 530;
        } else if (uid == "reno.multibanddynamics") {
            multiBandDynamics();
            *width = 660;
        } else if (uid == "reno.resonantfilter") {
            resonantFilter();
            *width = 480;
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

    // one fader of a band view (dialog 6003 / 6004), b the band (0..19), w the trapezoid's half width (250 on the 20 band view, 500 on the 10 band
    // one): the reading above (rx), the fader (fx, 10 wide, 100 high at y 26) and the centre frequency below (cx)
    void geqBand(const Placed& pl, int b, int w, int fx, int rx, int rw, int cx, int cw, const QString& center, const QString& objName, int digit)
    {
        QLabel* readout = putLabel(pl, QString(), rx, 17, rw, 8, Qt::AlignHCenter | Qt::AlignVCenter);
        DxSlider* s = new DxSlider(Qt::Vertical, this);
        s->setObjectName(objName);
        s->setRange(0, 720);
        s->setSingleStep(10);
        s->setPageStep(30);
        put(pl, s, fx, 26, 10, 100);
        if (digit >= 0) digitMnemonic(s, digit);
        putLabel(pl, center, cx, 127, cw, 8, Qt::AlignHCenter | Qt::AlignVCenter);
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
    }

    // The three views each have the output gain fader, the Accuracy box and Reset (the dialogs repeat them); 'tag' is added to the names of the ones
    // after the first view's, so every name is its own.
    void geqCommon(const Placed& pl, const QString& tag, QTabWidget* tabs)
    {
        QLabel* rd = putLabel(pl, QString(), 340, 17, 30, 8, Qt::AlignHCenter | Qt::AlignVCenter);
        SMap om;
        om.inf = true;
        om.dec = 1;
        DxSlider* of = slider(Qt::Vertical, fixed(1), om, "dxParam1" + tag, nullptr, rd, "dB");
        put(pl, of, 350, 26, 10, 100);
        QLabel* ol = putLabel(pl, "<u>O</u>utput gain<br>(-60 to 20 dB)", 332, 128, 48, 16, Qt::AlignHCenter | Qt::AlignTop);
        ol->setBuddy(of);
        QLabel* al = putLabel(pl, "A<u>c</u>curacy:", 5, 140, 40, 8);
        QComboBox* acc = combo(fixed(0), {"Low (fast, good for high bands)", "Medium (great for mid and high bands)", "High (slow, great for all bands)"}, "dxParam0" + tag);
        put(pl, acc, 55, 138, 164, 12);
        al->setBuddy(acc);
        QPushButton* reset = button("Rese&t");
        reset->setObjectName("dxReset" + tag);
        put(pl, reset, 223, 137, 50, 14);
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
    }

    Placed geqSheet()
    {
        Placed pl;
        pl.box = new QWidget(this);
        pl.box->setFixedSize(Placed::px(385), Placed::py(156 - 14));
        return pl;
    }

    QWidget* geqBandsView(bool twenty, QTabWidget* tabs)
    {
        static const char* const names20[20] = {"20", "28", "40", "56", "80", "113", "160", "225", "320", "450", "640", "900", "1.3k", "1.8k", "2.5k", "3.6k", "5.1k", "7.2k", "10k", "15k"};
        const Placed pl = geqSheet();
        putLabel(pl, "<u>G</u>ain:<br>(-Inf. to 24 dB)", 5, 17, 49, 16, Qt::AlignLeft | Qt::AlignTop);
        putLabel(pl, "Center (Hz):", 5, 127, 46, 8);
        if (twenty) {
            for (int b = 0; b < 20; ++b) geqBand(pl, b, 250, 54 + 14 * b, 52 + 14 * b, 14, 52 + 14 * b, 14, names20[b], QString("dxGeq20Band%1").arg(b), (b % 2 == 0) ? b / 2 : -1);
        } else {
            static const char* const centres10[10] = {"28", "56", "113", "225", "450", "900", "1.8k", "3.6k", "7.2k", "15k"};
            for (int k = 0; k < 10; ++k) geqBand(pl, 2 * k + 1, 500, 66 + 27 * k, 58 + 27 * k, 25, 61 + 27 * k, 20, centres10[k], QString("dxGeq10Band%1").arg(2 * k + 1), k);
        }
        geqCommon(pl, twenty ? "_20" : "", tabs);
        return pl.box;
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

    QWidget* geqEnvelopeView(QTabWidget* tabs)
    {
        // dialog 6005: the graph's plot is 304x100 at 26,26 with the dB marks to its left and the frequencies under it
        const Placed pl = geqSheet();
        putLabel(pl, "Fr<u>e</u>quency graph:", 26, 17, 56, 8);
        QLabel* readout = putLabel(pl, QString(), 186, 17, 144, 8, Qt::AlignRight | Qt::AlignVCenter);
        readout->setObjectName("dxGraphReadout");
        CurveGraph* g = new CurveGraph(this);
        g->setObjectName("dxGraph");
        put(pl, g, 5, 26, 330, 110);
        g->margins = QMargins(Placed::px(21), 0, Placed::px(5), Placed::py(10));
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
        geqCommon(pl, "_env", tabs);
        return pl.box;
    }

    void graphicEq()
    {
        sync_.push_back([this] { geqCache_ = geqSettings(); });          // first: the others read this
        QTabWidget* tabs = new QTabWidget(this);
        tabs->setObjectName("dxGeqTabs");
        tabs->addTab(geqBandsView(false, tabs), "10 Band");
        tabs->addTab(geqBandsView(true, tabs), "20 Band");
        tabs->addTab(geqEnvelopeView(tabs), "Envelope");
        root_->addWidget(tabs);
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

    // ============================================================================================ Paragraphic EQ
    // VEGAS's Paragraphic EQ page: the dry and wet faders and the response graph on top; four bands (gain fader, width,
    // center frequency) and the low and high shelf below.
    // Dry out and Wet out: the reading above, the fader (10x60 units), the caption with its range below
    void pgeqFader(const Placed& pl, int i, const QString& name, bool inf, int fx, int capX, int capW, const QString& range)
    {
        SMap m;
        m.inf = inf;
        QLabel* readout = putLabel(pl, QString(), fx - 10, 17, 30, 8, Qt::AlignHCenter | Qt::AlignVCenter);
        DxSlider* s = slider(Qt::Vertical, fixed(i), m, QString("dxParam%1").arg(i), nullptr, readout, "dB");
        put(pl, s, fx, 26, 10, 60);
        QLabel* cap = putLabel(pl, name + "<br>" + range, capX, 86, capW, 16, Qt::AlignHCenter | Qt::AlignTop);
        cap->setBuddy(s);
    }

    // one band (dialog units from x0): the gain fader with its reading, the width trackbar with its reading, the centre frequency box and trackbar
    void pgeqBand(const Placed& pl, int k)
    {
        const int x0 = 66 * k;
        SMap gm;
        QLabel* gr = putLabel(pl, QString(), 91 + x0, 108, 30, 8, Qt::AlignRight | Qt::AlignVCenter);
        DxSlider* gs = slider(Qt::Vertical, fixed(2 + k), gm, QString("dxParam%1").arg(2 + k), nullptr, gr, "dB");
        put(pl, gs, 71 + x0, 108, 10, 51);
        digitMnemonic(gs, k);
        SMap wm;
        QLabel* wr = putLabel(pl, QString(), 91 + x0, 123, 30, 8, Qt::AlignRight | Qt::AlignVCenter);
        DxSlider* ws = slider(Qt::Horizontal, fixed(6 + k), wm, QString("dxParam%1").arg(6 + k), nullptr, wr, "oct.");
        put(pl, ws, 86 + x0, 131, 35, 12);
        SMap fm;
        fm.log = true;
        fm.dec = 0;
        QLineEdit* box = putEdit(pl, 86 + x0, 146, 35, 12);
        DxSlider* fs = slider(Qt::Horizontal, fixed(10 + k), fm, QString("dxParam%1").arg(10 + k), box, nullptr);
        put(pl, fs, 71 + x0, 160, 64, 12);
    }

    // one shelf: the check box with its caption, the frequency (trackbar and edit box) and the gain (trackbar and reading)
    void pgeqShelf(const Placed& pl, int on, int freq, int gain, const QString& label, int y, int checkW)
    {
        QCheckBox* c = new QCheckBox(label, this);
        c->setObjectName(QString("dxParam%1").arg(on));
        put(pl, c, 5, y, checkW, 10);
        SMap fm;
        fm.log = true;
        fm.dec = 0;
        QLineEdit* box = putEdit(pl, 220, y - 2, 35, 12);
        DxSlider* fs = slider(Qt::Horizontal, fixed(freq), fm, QString("dxParam%1").arg(freq), box, nullptr);
        put(pl, fs, 129, y - 2, 88, 12);
        SMap gm;
        gm.inf = true;
        QLabel* gr = putLabel(pl, QString(), 301, y + 2, 30, 8, Qt::AlignRight | Qt::AlignVCenter);
        DxSlider* gs = slider(Qt::Horizontal, fixed(gain), gm, QString("dxParam%1").arg(gain), nullptr, gr, "dB");
        put(pl, gs, 259, y - 2, 40, 12);
        QObject::connect(c, &QCheckBox::toggled, c, [this, on](bool v) { commit(on, v ? 1 : 0); });
        sync_.push_back([=] {
            QSignalBlocker b(c);
            const bool v = get(on) >= 0.5;
            c->setChecked(v);
            fs->setEnabled(v);
            box->setEnabled(v);
            gs->setEnabled(v);
        });
    }

    void pgeqPaint(QPainter& p, const QRectF& pl)
    {
        vegpg::Settings st;
        st.dry = (int)std::lround(get(0) * 10);
        st.wet = (int)std::lround(get(1) * 10);
        for (int k = 0; k < 4; ++k) {
            st.gain[k] = (int)std::lround(get(2 + k) * 10);
            st.width[k] = (int)std::lround(get(6 + k) * 10);
            st.freq[k] = (int)std::lround(get(10 + k));
        }
        st.lowOn = (int)std::lround(get(14));
        st.highOn = (int)std::lround(get(15));
        st.lowFreq = (int)std::lround(get(16));
        st.highFreq = (int)std::lround(get(17));
        st.lowGain = (int)std::lround(get(18) * 10);
        st.highGain = (int)std::lround(get(19) * 10);
        vegpg::sanitize(st);
        vegpg::Dsp d;
        d.setRate(44100);
        d.setSettings(st);
        auto Y = [&](double db) { return pl.bottom() - (std::max(-25.0, std::min(25.0, db)) + 25.0) / 50.0 * pl.height(); };
        auto X = [&](double f) { return pl.left() + std::log(f / 20.0) / std::log(2.0) / 10.0 * pl.width(); };
        p.setPen(QPen(QColor(200, 200, 200), 1));
        for (int k = 1; k < 10; ++k) {
            const double x = std::round(X(20.0 * std::pow(2.0, k))) + 0.5;
            p.drawLine(QPointF(x, pl.top()), QPointF(x, pl.bottom()));
        }
        p.setPen(QPen(QColor(150, 150, 150), 1));
        for (double db : {25.0, 0.0, -25.0}) {
            const double y = std::round(Y(db)) + 0.5;
            p.drawLine(QPointF(pl.left(), y), QPointF(pl.right(), y));
        }
        QPainterPath path;
        const int w = std::max(2, (int)pl.width());
        for (int x = 0; x < w; ++x) {
            const double f = 20.0 * std::pow(2.0, 10.0 * x / (w - 1));
            const QPointF pt(pl.left() + x, Y(d.responseDb(f)));
            if (x == 0) path.moveTo(pt);
            else path.lineTo(pt);
        }
        p.setPen(QPen(QColor(0, 0, 160), 1));
        p.drawPath(path);
        p.setPen(Qt::black);
        p.drawText(QRectF(0, Y(25) - 7, pl.left() - 3, 14), Qt::AlignRight | Qt::AlignVCenter, "+25");
        p.drawText(QRectF(0, Y(0) - 7, pl.left() - 3, 14), Qt::AlignRight | Qt::AlignVCenter, "dB 0");
        p.drawText(QRectF(0, Y(-25) - 7, pl.left() - 3, 14), Qt::AlignRight | Qt::AlignVCenter, "-25");
        static const char* const xl[11] = {"20", "40", "80", "160", "320", "640", "1.3k", "2.6k", "5.1k", "10k", "20k"};
        for (int k = 0; k <= 10; ++k) {
            const double x = X(20.0 * std::pow(2.0, k));
            p.drawText(QRectF(x - 20, pl.bottom() + 1, 40, 14), Qt::AlignHCenter | Qt::AlignTop, xl[k]);
        }
    }

    // ============================================================================================ Graphic Dynamics
    // VEGAS's Graphic Dynamics page (dialog 2000), placed as the dialog places things (dialog units): the output gain fader, the
    // graph of the level out against the level in (with a readout of the gain at the point under the pointer), the two check
    // boxes, Reset Graph, attack and release (a slider and a box each) and the threshold and ratio sliders, which move the
    // graph's points the way the plug-in's own page does (veggd::setThreshold, veggd::setRatio).
    veggd::Settings gdSettings() const
    {
        veggd::Settings s;
        if (h_.state) veggd::decode(h_.state(), &s);
        return s;
    }
    void gdStore(const veggd::Settings& s, const std::string& what)
    {
        if (updating_ || !h_.setState) return;
        h_.setState(veggd::encode(s), what);
        syncAll();
    }

    void graphicDynamics()
    {
        const double kx = 1.75, ky = 1.875;
        QWidget* box = new QWidget(this);
        box->setFixedSize((int)std::lround(288 * kx), (int)std::lround((238 - 16) * ky));
        root_->addWidget(box);
        auto put = [=](QWidget* w, int x, int y, int cw, int ch) {
            w->setParent(box);
            w->setGeometry((int)std::lround(x * kx), (int)std::lround((y - 16) * ky), (int)std::lround(cw * kx), (int)std::lround(ch * ky));
            w->show();
            return w;
        };
        auto label = [=](const QString& t, int x, int y, int cw, int ch, Qt::Alignment a = Qt::AlignLeft | Qt::AlignVCenter) {
            QLabel* l = text(t, a);
            put(l, x, y, cw, ch);
            return l;
        };

        // output gain: the fader with its readout above and its caption below
        {
            SMap db;
            db.inf = false;
            QLabel* r = label(QString(), 27, 17, 35, 8, Qt::AlignHCenter | Qt::AlignVCenter);
            DxSlider* s = slider(Qt::Vertical, fixed(4), db, "dxParam4", nullptr, r, "dB");
            put(s, 40, 26, 10, 70);
            label("<u>O</u>utput gain<br>(-25 to 25 dB)", 21, 97, 48, 16, Qt::AlignHCenter | Qt::AlignTop);
        }

        // the graph
        label("<u>G</u>raph:", 113, 17, 32, 8);
        QLabel* readout = label(QString(), 147, 17, 136, 8, Qt::AlignRight | Qt::AlignVCenter);
        readout->setObjectName("dxGraphReadout");
        EnvelopeGraph* g = new EnvelopeGraph(this);
        g->setObjectName("dxGraph");
        g->maxPts = (size_t)veggd::kMaxPoints;
        g->margins = QMargins(26, 4, 8, 16);
        graphs_.push_back(g);
        g->readout = readout;
        g->format = [](const vegdist::Point& p) {
            auto f = [](int v) { return QString::number(v / 10.0, 'f', 1); };
            return QString("Gain of %1 dB at %2, %3 dB").arg(f(p.y - p.x), f(p.x), f(p.y));
        };
        g->edited = [this, g](int phase) {
            veggd::Settings s = gdSettings();
            s.count = (int32_t)std::min<size_t>(g->pts.size(), (size_t)veggd::kMaxPoints);
            std::memset(s.pt, 0, sizeof s.pt);
            for (int k = 0; k < s.count; ++k) {
                s.pt[k][0] = g->pts[(size_t)k].x;
                s.pt[k][1] = g->pts[(size_t)k].y;
            }
            veggd::graphEdited(s);
            if (phase == 0 && h_.beginEdit) h_.beginEdit("Edit Graphic Dynamics graph");
            gdStore(s, "Edit Graphic Dynamics graph");
            if (phase == 2) endDrag();
        };
        sync_.push_back([this, g] {
            if (g->dragging()) return;
            const veggd::Settings s = gdSettings();
            g->pts.clear();
            for (int k = 0; k < s.count; ++k) g->pts.push_back(vegdist::Point{s.pt[k][0], s.pt[k][1]});
        });
        put(g, 113 - 15, 26 - 2, 170 + 15 + 5, 139 + 2 + 9);
        label("O<br>U<br>T", 88, 84, 8, 24, Qt::AlignHCenter | Qt::AlignVCenter);
        label("IN", 193, 172, 8, 8);

        // the check boxes and Reset Graph
        put(checkBox(2, "Auto gain <u>c</u>ompensate"), 5, 122, 88, 10);
        put(checkBox(3, "S<u>y</u>nc channel gains"), 5, 135, 87, 10);
        QPushButton* reset = button("Rese&t Graph");
        reset->setObjectName("dxResetGraph");
        QObject::connect(reset, &QPushButton::clicked, reset, [this] {
            veggd::Settings s = gdSettings();
            veggd::resetGraph(s);
            gdStore(s, "Reset graph");
        });
        put(reset, 5, 151, 84, 14);

        // attack and release: a slider and a box each
        auto timeRow = [=](int i, const QString& cap, int ly, int sy, int by) {
            label(cap, 5, ly, 95, 8);
            SMap m;
            m.dec = 1;
            QLineEdit* e = editBox(52);
            DxSlider* s = slider(Qt::Horizontal, fixed(i), m, QString("dxParam%1").arg(i), e, nullptr);
            put(s, 5, sy, 135, 12);
            put(e, 105, by, 35, 12);
        };
        timeRow(0, "Attac<u>k</u> (0 to 500 ms):", 182, 193, 180);
        timeRow(1, "R<u>e</u>lease (0 to 5,000 ms):", 208, 221, 208);

        // threshold and ratio: they act on the graph's points
        auto graphSlider = [=](int lo, int hi, const QString& name, const std::string& what, std::function<int(const veggd::Settings&)> get,
                               std::function<void(veggd::Settings&, int)> set, std::function<QString(int)> show, QLabel* readoutLabel) {
            DxSlider* s = new DxSlider(Qt::Horizontal, this);
            s->setObjectName(name);
            s->setRange(lo, hi);
            QObject::connect(s, &QSlider::valueChanged, s, [=](int pos) {
                if (updating_) return;
                veggd::Settings st = gdSettings();
                set(st, pos);
                gdStore(st, what);
            });
            QObject::connect(s, &QSlider::sliderPressed, s, [=] {
                if (h_.beginEdit) h_.beginEdit(what.c_str());
            });
            QObject::connect(s, &QSlider::sliderReleased, s, [=] { endDrag(); });
            sync_.push_back([=] {
                const int v = get(gdSettings());
                QSignalBlocker b(s);
                if (!s->isSliderDown()) s->setValue(v);
                readoutLabel->setText(show(v));
            });
            return s;
        };
        label("Threshol<u>d</u> (-80 to -0.1 dB):", 148, 182, 87, 8);
        QLabel* thrRead = label(QString(), 248, 182, 35, 8, Qt::AlignRight | Qt::AlignVCenter);
        thrRead->setObjectName("dxThresholdReadout");
        put(graphSlider(
                -800, -1, "dxThreshold", "Change \"Threshold\"", [](const veggd::Settings& s) { return (int)s.threshold; },
                [](veggd::Settings& s, int v) { veggd::setThreshold(s, v); }, [](int v) { return QString::number(v / 10.0, 'f', 1) + " dB"; }, thrRead),
            148, 193, 135, 12);
        label("Rat<u>i</u>o (1.0:1 to Inf.:1):", 148, 209, 85, 8);
        QLabel* ratRead = label(QString(), 248, 209, 35, 8, Qt::AlignRight | Qt::AlignVCenter);
        ratRead->setObjectName("dxRatioReadout");
        put(graphSlider(
                10, 500, "dxRatio", "Change \"Ratio\"", [](const veggd::Settings& s) { return (int)s.ratio; },
                [](veggd::Settings& s, int v) { veggd::setRatio(s, v); },
                [](int v) { return v >= 500 ? QString("Inf.:1") : QString("%1 :1").arg(QString::number(v / 10.0, 'f', 1)); }, ratRead),
            148, 221, 135, 12);
    }

    // ============================================================================================ Multi-Band Dynamics
    // VEGAS's Multi-Band Dynamics page (dialogs 5000 and 5001, the tabs "Bands 1 & 2" and "Bands 3 & 4"), placed as the
    // dialog places things (dialog units): for each band the threshold, amount and gain faders, attack and release (a
    // slider and a box each), the type, the frequency, the width (only for the band-notch), Bypass and Solo; beside the
    // tabs the output gain fader, below them Sync channels. The level meters (each band's left and right level and its reduction)
    // and "Enable meters" are here; the meter choice list beside each band and "Capture threshold" are not (not worked out).
    void mbBand(QWidget* box, int b, int y0)
    {
        const double kx = 1.75, ky = 1.875;
        const int o = b * 10;
        auto put = [=](QWidget* w, int x, int y, int cw, int ch) {
            w->setParent(box);
            w->setGeometry((int)std::lround(x * kx), (int)std::lround((y - 16) * ky), (int)std::lround(cw * kx), (int)std::lround(ch * ky));
            w->show();
            return w;
        };
        auto label = [=](const QString& tx, int x, int y, int cw, int ch, Qt::Alignment a = Qt::AlignLeft | Qt::AlignVCenter) {
            QLabel* l = text(tx, a);
            put(l, x, y, cw, ch);
            return l;
        };
        const QString n = QString::number(b + 1);
        // the three faders
        struct Fader { int idx; int x, fx; const char* cap; const char* unit; };
        const Fader fd[3] = {{o, 5, 20, "Threshol<u>d</u>", "dB"}, {o + 1, 40, 52, "Amount", ":1"}, {o + 2, 72, 85, "Gain", "dB"}};
        for (int k = 0; k < 3; ++k) {
            SMap m;
            m.inf = false;
            QLabel* r = label(QString(), fd[k].x, y0, 35, 8, Qt::AlignHCenter | Qt::AlignVCenter);
            DxSlider* s = slider(Qt::Vertical, fixed(fd[k].idx), m, QString("dxParam%1").arg(fd[k].idx), nullptr, r, fd[k].unit);
            put(s, fd[k].fx, y0 + 9, 10, 68);
            QString cap = fd[k].cap;
            if (k == 0 && (b % 2) == 1) cap = "Thresho<u>l</u>d";
            label(cap, fd[k].x, y0 + 78, 35, 8, Qt::AlignHCenter | Qt::AlignVCenter);
        }
        // attack and release
        auto timeRow = [=](int idx, const QString& cap, int ly) {
            label(cap, 110, y0 + ly, 100, 8);
            SMap m;
            m.dec = 1;
            QLineEdit* e = editBox(52);
            DxSlider* s = slider(Qt::Horizontal, fixed(idx), m, QString("dxParam%1").arg(idx), e, nullptr);
            put(s, 110, y0 + ly + 9, 100, 12);
            put(e, 213, y0 + ly + 9, 35, 12);
        };
        timeRow(o + 3, (b % 2) ? "Attack t<u>i</u>me (0 to 500 ms):" : "Attac<u>k</u> time (0 to 500 ms):", 0);
        timeRow(o + 4, "Release time (0 to 5,000 ms):", 23);
        // type
        label((b % 2) ? "Typ<u>e</u>:" : "<u>T</u>ype:", 110, y0 + 52, 24, 8);
        QComboBox* ty = combo(fixed(o + 5), {"Low-shelf", "Band-notch", "High-shelf"}, QString("dxParam%1").arg(o + 5));
        put(ty, 135, y0 + 50, 113, 14);
        // frequency
        QLabel* fl = label(QString(), 110, y0 + 66, 138, 8);
        {
            SMap m;
            m.log = true;
            m.dec = 0;
            QLineEdit* e = editBox(52);
            DxSlider* s = slider(Qt::Horizontal, fixed(o + 6), m, QString("dxParam%1").arg(o + 6), e, nullptr);
            put(s, 110, y0 + 75, 100, 12);
            put(e, 213, y0 + 75, 35, 12);
            fl->setBuddy(s);
        }
        // width
        QLabel* wl = label("Width (0.3 to 3 oct.):", 110, y0 + 92, 70, 8);
        QLabel* wv = label(QString(), 213, y0 + 92, 35, 8, Qt::AlignRight | Qt::AlignVCenter);
        SMap wm;
        wm.dec = 1;
        DxSlider* ws = slider(Qt::Horizontal, fixed(o + 7), wm, QString("dxParam%1").arg(o + 7), nullptr, wv);
        put(ws, 182, y0 + 90, 29, 12);
        sync_.push_back([=] {
            const int type = (int)std::lround(get(o + 5));
            static const char* const names[3] = {"Below (50 to 15,000 Hz):", "Center (50 to 15,000 Hz):", "Above (50 to 15,000 Hz):"};
            fl->setText(names[std::max(0, std::min(2, type))]);
            const bool band = type == 1;         // only the band-notch has a width
            wl->setEnabled(band);
            wv->setEnabled(band);
            ws->setEnabled(band);
        });
        // bypass and solo
        put(checkBox(o + 8, QString("Band <u>%1</u> bypass").arg(n)), 8, y0 + 91, 66, 10);
        put(checkBox(o + 9, "Solo"), 75, y0 + 91, 30, 10);
        // meters: the band's left and right level (-60 to 0 dB) and, narrow, the reduction (up to 30 dB)
        LevelMeter* lv = new LevelMeter(box, true, 2, -60.0, false);
        lv->setObjectName(QString("dxMeterBand%1").arg(b + 1));
        put(lv, 255, y0, 36, 86);
        LevelMeter* rd = new LevelMeter(box, true, 1, -30.0, true);
        rd->setObjectName(QString("dxMeterReduction%1").arg(b + 1));
        put(rd, 293, y0, 12, 86);
        addMeterPoll([=] {
            const float a0 = h_.meter(b * 3), a1 = h_.meter(b * 3 + 1), g = h_.meter(b * 3 + 2);
            if (!mbMeters_) {
                lv->clear();
                rd->clear();
                return;
            }
            lv->feed(0, meterDb(a0, -60.0));
            lv->feed(1, meterDb(a1, -60.0));
            rd->feed(0, g > 0.f && g < 1.f ? -20.0 * std::log10((double)g) : 0.0, 0.8);
        });
    }
    bool mbMeters_ = true;


    // One tab is a whole dialog (5000 / 5001): two bands, the output gain fader at the right, and under them Sync channels and Enable meters. The
    // second tab's own copies of the ones every tab has carry 'tag' at the end of their names.
    QWidget* mbTab(int first, const QString& tag)
    {
        const double kx = 1.75, ky = 1.875;
        QWidget* box = new QWidget(this);
        box->setFixedSize((int)std::lround(362 * kx), (int)std::lround((256 - 16) * ky));
        auto put = [=](QWidget* w, int x, int y, int cw, int ch) {
            w->setParent(box);
            w->setGeometry((int)std::lround(x * kx), (int)std::lround((y - 16) * ky), (int)std::lround(cw * kx), (int)std::lround(ch * ky));
            w->show();
            return w;
        };
        mbBand(box, first, 17);
        mbBand(box, first + 1, 128);
        for (int y : {123, 234}) {
            QFrame* sep = new QFrame(box);
            sep->setFrameShape(QFrame::HLine);
            sep->setFrameShadow(QFrame::Sunken);
            sep->setGeometry((int)std::lround(8 * kx), (int)std::lround((y - 16) * ky), (int)std::lround(297 * kx), 2);
        }
        // the output gain
        SMap om;
        om.dec = 1;
        QLabel* orr = text(QString(), Qt::AlignHCenter | Qt::AlignVCenter);
        put(orr, 316, 17, 33, 8);
        DxSlider* of = slider(Qt::Vertical, fixed(41), om, "dxParam41" + tag, nullptr, orr, "dB");
        put(of, 328, 25, 10, 78);
        QLabel* ol = text("<u>O</u>utput gain<br>(-25 to 25 dB)", Qt::AlignHCenter | Qt::AlignTop);
        put(ol, 309 - 4, 104, 48 + 8, 16);
        ol->setBuddy(of);
        // Sync channels and Enable meters
        QCheckBox* sy = checkBox(40, "S&ync channels");
        sy->setObjectName("dxParam40" + tag);
        put(sy, 8, 241, 86, 10);
        // "Enable meters" is kept in the plug-in's saved form (the settings it keeps for its window, field 46)
        QCheckBox* em = new QCheckBox("Enable &meters", this);
        em->setObjectName("dxMbEnableMeters" + tag);
        put(em, 100, 241, 62, 10);
        QObject::connect(em, &QCheckBox::toggled, em, [this](bool on) {
            if (updating_ || !h_.state || !h_.setState) return;
            vegmb::Settings s;
            if (!vegmb::decode(h_.state(), &s)) return;
            s.extra[8] = on ? 1 : 0;
            mbMeters_ = on;
            h_.setState(vegmb::encode(s), "Change \"Enable meters\"");
            syncAll();
        });
        sync_.push_back([this, em] {
            vegmb::Settings s;
            if (h_.state && vegmb::decode(h_.state(), &s)) mbMeters_ = s.extra[8] != 0;
            QSignalBlocker b(em);
            em->setChecked(mbMeters_);
        });
        return box;
    }

    void multiBandDynamics()
    {
        QTabWidget* tabs = new QTabWidget(this);
        tabs->setObjectName("dxMbTabs");
        tabs->addTab(mbTab(0, QString()), "Bands 1 && 2");
        tabs->addTab(mbTab(2, "_b34"), "Bands 3 && 4");
        root_->addWidget(tabs, 0, Qt::AlignLeft | Qt::AlignTop);
        root_->addStretch(1);
    }

    // ============================================================================================ Resonant Filter
    // VEGAS's Resonant Filter page (dialog 102), placed as the dialog places things (dialog units): the frequency,
    // resonance and wet / dry as a slider and a box each, the filter type (low, band, high pass) and the filter order
    // (second, fourth). The sliders are the plug-in's own: one step for each Hz (frequency) or each tenth of a percent
    // (the other two), from the left end to the right end in a straight line. (The dialog's logo strip is not here.)
    struct RfRadio {
        const char* text;
        int x, y, w;
    };
    void resonantFilter()
    {
        const double kx = 1.75, ky = 1.875;
        QWidget* box = new QWidget(this);
        box->setFixedSize((int)std::lround(252 * kx), (int)std::lround((104 - 16) * ky));
        auto put = [=](QWidget* w, int x, int y, int cw, int ch) {
            w->setParent(box);
            w->setGeometry((int)std::lround(x * kx), (int)std::lround((y - 16) * ky), (int)std::lround(cw * kx), (int)std::lround(ch * ky));
            w->show();
            return w;
        };
        auto row = [=](int idx, const QString& cap, int ly, int ty, int dec, int stepCount) {
            QLabel* l = text(cap);
            put(l, 5, ly, 55, 8);
            SMap m;
            m.dec = dec;
            m.stepCount = stepCount;
            QLineEdit* e = editBox(52);
            DxSlider* s = slider(Qt::Horizontal, fixed(idx), m, QString("dxParam%1").arg(idx), e, nullptr);
            put(s, 61, ty, 145, 12);
            put(e, 209, ty, 40, 12);
            l->setBuddy(s);
        };
        row(0, "<u>F</u>requency (Hz):", 20, 18, 0, 19980);
        row(1, "<u>R</u>esonance (%):", 37, 35, 1, 1000);
        row(5, "<u>W</u>et / Dry (%):", 55, 53, 1, 1000);
        auto radioRow = [=](int idx, const QString& cap, int ly, std::initializer_list<RfRadio> items) {
            put(text(cap), 5, ly, 55, 8);
            QButtonGroup* g = new QButtonGroup(box);
            g->setExclusive(true);
            int k = 0;
            for (const auto& it : items) {
                QRadioButton* r = new QRadioButton(QString::fromLatin1(it.text), box);
                r->setObjectName(QString("dxParam%1_%2").arg(idx).arg(k));
                g->addButton(r, k);
                put(r, it.x, it.y, it.w, 10);
                const int kk = k;
                QObject::connect(r, &QRadioButton::toggled, r, [=](bool on) {
                    if (on) commit(idx, kk);
                });
                ++k;
            }
            sync_.push_back([=] {
                const int k2 = (int)std::lround(std::max(0.0, std::min(1.0 + (idx == 2 ? 1 : 0), get(idx))));
                if (QAbstractButton* b = g->button(k2)) {
                    QSignalBlocker blocker(b);
                    b->setChecked(true);
                }
            });
        };
        radioRow(2, "Filter type:", 74, {{"&Low Pass", 61, 73, 47}, {"&Band Pass", 110, 73, 50}, {"&High Pass", 165, 73, 47}});
        radioRow(3, "Filter order:", 93, {{"&Second", 61, 92, 41}, {"F&ourth", 110, 92, 36}});
        root_->addWidget(box, 0, Qt::AlignLeft | Qt::AlignTop);
        root_->addStretch(1);
    }

    void paragraphicEq()
    {
        const Placed pl = placedPage(338, 211);
        pgeqFader(pl, 0, "<u>D</u>ry out", true, 21, 5, 49, "(-Inf. to 25 dB)");
        pgeqFader(pl, 1, "<u>W</u>et out", false, 71, 53, 46, "(-25 to 25 dB)");
        Graph* g = new Graph(this);
        g->setObjectName("dxPgeqGraph");
        put(pl, g, 95, 16, 240, 84);
        g->margins = QMargins(Placed::px(20), Placed::py(1), Placed::px(2), Placed::py(10));
        g->draw = [this](QPainter& p, const QRectF& pl2) { pgeqPaint(p, pl2); };
        putLabel(pl, "<u>G</u>ain (-25 to 25 dB):", 5, 108, 64, 8);
        putLabel(pl, "Width<br>(0.3 to 2.5 oct.):", 5, 121, 66, 16, Qt::AlignLeft | Qt::AlignTop);
        putLabel(pl, "Center frequency<br>(20 to 15,000 Hz):", 5, 158, 62, 16, Qt::AlignLeft | Qt::AlignTop);
        for (int k = 0; k < 4; ++k) pgeqBand(pl, k);
        pgeqShelf(pl, 14, 16, 18, "Enable l&ow-shelf (-Inf. to 25 dB):", 181, 124);
        pgeqShelf(pl, 15, 17, 19, "&Enable high-shelf (-Inf. to 25 dB):", 194, 124);
    }
