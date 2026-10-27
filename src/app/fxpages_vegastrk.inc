// The pages of the effects VEGAS puts on every track and bus (Volume, Pan, Track Noise Gate, Track Compressor, Dither,
// Track EQ), copied from the dialogs in sffrgpnv_x64.dll and sftrkfx1_x64.dll. Included by fxpages.cpp inside class
// Page, so these use its helpers (slider, text, editBox, root_ ...). Dialog units there are 1.6 pixels or so here.

    // ---- Volume
    float trkVolumeGain() const
    {
        float g = 1.f;
        if (h_.state) vegforge::decode(vegforge::volume(), h_.state(), &g);
        return g;
    }
    static QString trkVolumeText(float g)
    {
        if (!(g > 0.f)) return "-Inf.";
        const double db = 20.0 * std::log10((double)g);
        return db <= -60.0 ? QString("-Inf.") : QString::number(db, 'f', 3);
    }

    // "Volume (dB):", a trackbar that is linear in the gain (0 to 2 in thousandths, 1000 is 0 dB) and a box that takes dB
    // (any value up to 66 dB, past the trackbar's end; "-Inf." for silence).
    void trkVolume()
    {
        QHBoxLayout* row = new QHBoxLayout();
        row->setSpacing(6);
        QLabel* l = text("<u>V</u>olume (dB):");
        l->setFixedWidth(84);
        row->addWidget(l);
        DxSlider* s = new DxSlider(Qt::Horizontal, this);
        s->setObjectName("dxParam0");
        s->setRange(0, 2000);
        l->setBuddy(s);
        row->addWidget(s, 1);
        QLineEdit* box = editBox(66);
        box->setObjectName("dxParam0Edit");
        row->addWidget(box);
        root_->addSpacing(14);
        root_->addLayout(row);
        QObject::connect(s, &QSlider::valueChanged, s, [this](int pos) {
            if (updating_) return;
            commit(0, pos / 1000.0);
        });
        QObject::connect(s, &QSlider::sliderPressed, s, [this] { beginDrag(0); });
        QObject::connect(s, &QSlider::sliderReleased, s, [this] { endDrag(); });
        QObject::connect(box, &QLineEdit::editingFinished, box, [this, box] {
            if (updating_) return;
            double db = 0;
            float g;
            const QString t = box->text().trimmed();
            if (t.startsWith("-inf", Qt::CaseInsensitive)) g = 0.f;
            else if (parseNumber(t, &db)) g = (float)std::pow(10.0, db / 20.0);
            else { syncAll(); return; }
            g = vegforge::limit(vegforge::volume(), g);
            if (trkVolumeText(g) == trkVolumeText(trkVolumeGain())) { syncAll(); return; }
            if (g <= 2.f) commit(0, g);
            else if (h_.setState) {
                h_.setState(vegforge::encode(vegforge::volume(), g), "Change \"Volume\"");
                syncAll();
            }
        });
        sync_.push_back([this, s, box] {
            const float g = trkVolumeGain();
            QSignalBlocker b(s);
            if (!s->isSliderDown()) s->setValue((int)std::lround(std::min(2.f, g) * 1000.f));
            if (!box->hasFocus()) box->setText(trkVolumeText(g));
        });
    }

    // ---- Pan
    // "Pan (%):" and a trackbar from -1000 to 1000 that runs against the setting (its left end is +1: all left), with the
    // two readouts above it: "Left: <setting in %>" and "Right: <minus that>".
    void trkPan()
    {
        QGridLayout* g = new QGridLayout();
        g->setHorizontalSpacing(6);
        g->setVerticalSpacing(2);
        QLabel* leftText = text(QString());
        leftText->setObjectName("dxPanLeft");
        QLabel* rightText = text(QString(), Qt::AlignRight | Qt::AlignVCenter);
        rightText->setObjectName("dxPanRight");
        g->addWidget(leftText, 0, 1, Qt::AlignLeft);
        g->addWidget(rightText, 0, 1, Qt::AlignRight);
        QLabel* l = text("<u>P</u>an (%):");
        l->setFixedWidth(60);
        g->addWidget(l, 1, 0);
        DxSlider* s = new DxSlider(Qt::Horizontal, this);
        s->setObjectName("dxParam0");
        s->setRange(-1000, 1000);
        l->setBuddy(s);
        g->addWidget(s, 1, 1);
        g->setColumnStretch(1, 1);
        root_->addSpacing(10);
        root_->addLayout(g);
        QObject::connect(s, &QSlider::valueChanged, s, [this](int pos) {
            if (updating_) return;
            commit(0, -pos / 1000.0);
        });
        QObject::connect(s, &QSlider::sliderPressed, s, [this] { beginDrag(0); });
        QObject::connect(s, &QSlider::sliderReleased, s, [this] { endDrag(); });
        sync_.push_back([this, s, leftText, rightText] {
            const int pos = (int)std::lround(-get(0) * 1000.0);
            QSignalBlocker b(s);
            if (!s->isSliderDown()) s->setValue(pos);
            leftText->setText(QString("Left: %1 %").arg(QString::number(pos == 0 ? 0.0 : -pos / 10.0, 'f', 1)));
            rightText->setText(QString("Right: %1 %").arg(QString::number(pos / 10.0, 'f', 1)));
        });
    }

    // ---- helpers for the pages below
    // A trackbar for setting i that has exactly 'steps' positions from the setting's smallest to its biggest value, with an
    // edit box (which takes any value in range).
    DxSlider* trkSlider(int i, int steps, const SMap& m, const QString& name, QLineEdit* box)
    {
        DxSlider* s = new DxSlider(Qt::Horizontal, this);
        s->setObjectName(name);
        s->setRange(0, steps);
        QObject::connect(s, &QSlider::valueChanged, s, [=](int pos) {
            if (updating_) return;
            const Rng r = rng(i);
            commit(i, r.lo + (double)pos * (r.hi - r.lo) / steps);
        });
        QObject::connect(s, &QSlider::sliderPressed, s, [=] { beginDrag(i); });
        QObject::connect(s, &QSlider::sliderReleased, s, [=] { endDrag(); });
        if (box) {
            box->setObjectName(name + "Edit");
            QObject::connect(box, &QLineEdit::editingFinished, box, [=] {
                if (updating_) return;
                const Rng r = rng(i);
                if (box->text() == edit(get(i), m, r)) return;
                double v = 0;
                if (!parseNumber(box->text(), &v)) { syncAll(); return; }
                commit(i, v);
            });
        }
        sync_.push_back([=] {
            const Rng r = rng(i);
            const double v = get(i);
            QSignalBlocker b(s);
            if (!s->isSliderDown()) s->setValue((int)std::lround((v - r.lo) / (r.hi - r.lo) * steps));
            if (box && !box->hasFocus()) box->setText(edit(v, m, r));
        });
        return s;
    }

    // a caption with its edit box on the right, and the trackbar under it
    QWidget* trkBlock(int i, const QString& caption, int steps, const SMap& m)
    {
        QWidget* w = new QWidget(this);
        QVBoxLayout* v = new QVBoxLayout(w);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(1);
        QHBoxLayout* top = new QHBoxLayout();
        top->setSpacing(4);
        QLabel* l = text(caption);
        top->addWidget(l, 1);
        QLineEdit* box = editBox(62);
        top->addWidget(box);
        v->addLayout(top);
        DxSlider* s = trkSlider(i, steps, m, QString("dxParam%1").arg(i), box);
        v->addWidget(s);
        l->setBuddy(s);
        return w;
    }
    // ---- Track Noise Gate
    // The threshold fader on the left (-Inf. to 0 dB, one tenth of a dB a step), attack and release as trackbars in whole ms.
    void trkGate()
    {
        QHBoxLayout* h = new QHBoxLayout();
        h->setSpacing(10);
        QWidget* fader = gainColumn(0, "Threshold le<u>v</u>el");
        fader->setFixedWidth(100);
        h->addWidget(fader);
        QVBoxLayout* right = new QVBoxLayout();
        right->setSpacing(14);
        SMap m;
        m.dec = 0;
        right->addWidget(trkBlock(1, "A<u>t</u>tack time (0 to 500 ms):", 500, m));
        right->addWidget(trkBlock(2, "R<u>e</u>lease time (1 to 5,000 ms):", 4999, m));
        right->addStretch(1);
        h->addLayout(right, 1);
        root_->addLayout(h);
    }

    // ---- Track Compressor
    // Input and output gain (-Inf. to 25 dB), threshold, amount, attack and release as trackbars with a box each (tenths of a
    // unit a step), and the two switches. (VEGAS's level meters are not drawn.)
    void trkComp()
    {
        SMap g;
        g.inf = true;
        SMap one;
        QVBoxLayout* col = new QVBoxLayout();
        col->setSpacing(7);
        // the meters (dialog: the input's and the output's level as two bars each, and the reduction); range 48 dB
        auto levelMeter = [this](const char* name, int first) {
            LevelMeter* m = new LevelMeter(this, false, 2, -48.0, false);
            m->setObjectName(name);
            m->setFixedSize(411, 50);
            addMeterPoll([this, m, first] {
                for (int c = 0; c < 2; ++c) m->feed(c, meterDb(h_.meter(first + c), -48.0));
            });
            return m;
        };
        col->addWidget(levelMeter("dxMeterInput", 0));
        col->addWidget(trkBlock(0, "<u>I</u>nput gain (dB):", 850, g));
        col->addWidget(levelMeter("dxMeterOutput", 2));
        col->addWidget(trkBlock(1, "<u>O</u>utput gain (dB):", 850, g));
        {
            QHBoxLayout* rr = new QHBoxLayout();
            rr->setSpacing(8);
            QLabel* rl = text("Reduction (dB):");
            rl->setFixedWidth(105);
            rr->addWidget(rl);
            LevelMeter* m = new LevelMeter(this, false, 1, -30.0, true);
            m->setObjectName("dxMeterReduction");
            m->setFixedSize(303, 22);
            addMeterPoll([this, m] {
                const float g2 = h_.meter(4);
                m->feed(0, g2 > 0.f && g2 < 1.f ? -20.0 * std::log10((double)g2) : 0.0, 0.8);
            });
            rr->addWidget(m);
            rr->addStretch(1);
            col->addLayout(rr);
        }
        col->addWidget(trkBlock(2, "<u>T</u>hreshold (dB):", 600, one));
        col->addWidget(trkBlock(3, "Amount (<u>x</u>:1):", 490, one));
        col->addWidget(trkBlock(4, "Attac<u>k</u> (ms):", 5000, one));
        col->addWidget(trkBlock(5, "Re<u>l</u>ease (ms):", 50000, one));
        QHBoxLayout* checks = new QHBoxLayout();
        checks->setSpacing(14);
        checks->addWidget(checkBox(6, "Auto gain &compensation"));
        checks->addWidget(checkBox(7, "S&mooth saturation"));
        checks->addStretch(1);
        col->addLayout(checks);
        root_->addLayout(col);
    }

    // ---- Dither
    // Three lists (word length, dither type, noise shaping); the settings live in the saved form, so they go through setState.
    vegdither::Settings trkDitherSettings() const
    {
        vegdither::Settings s;
        if (h_.state) vegdither::decode(h_.state(), &s);
        return s;
    }
    void trkDither()
    {
        struct Row {
            const char* caption;
            const char* name;
            int count;
            const char* (*itemName)(int);
            const char* what;
        };
        const Row rows[3] = {{"<u>Q</u>uantization depth:", "dxDepth", 7, vegdither::depthName, "Change \"Quantization depth\""},
                             {"<u>D</u>ither type:", "dxDitherType", 6, vegdither::typeName, "Change \"Dither type\""},
                             {"<u>N</u>oise shaping:", "dxNoiseShaping", 3, vegdither::shapeName, "Change \"Noise shaping\""}};
        root_->addSpacing(8);
        for (int r = 0; r < 3; ++r) {
            QHBoxLayout* row = new QHBoxLayout();
            row->setSpacing(8);
            QLabel* l = text(rows[r].caption);
            l->setFixedWidth(120);
            row->addWidget(l);
            QComboBox* c = new QComboBox(this);
            c->setObjectName(rows[r].name);
            for (int k = 0; k < rows[r].count; ++k) c->addItem(rows[r].itemName(k));
            c->setFixedWidth(160);
            l->setBuddy(c);
            row->addWidget(c);
            row->addStretch(1);
            root_->addLayout(row);
            QObject::connect(c, QOverload<int>::of(&QComboBox::currentIndexChanged), c, [this, r, rows](int k) {
                if (k < 0 || updating_ || !h_.setState) return;
                vegdither::Settings s = trkDitherSettings();
                (r == 0 ? s.depth : r == 1 ? s.type : s.shape) = k;
                h_.setState(vegdither::encode(s), rows[r].what);
                syncAll();
            });
            sync_.push_back([this, r, c] {
                const vegdither::Settings s = trkDitherSettings();
                const int k = r == 0 ? s.depth : r == 1 ? s.type : s.shape;
                QSignalBlocker b(c);
                c->setCurrentIndex(k < c->count() ? k : -1);      // (a preset's noise shaping 3 is outside the list: blank)
            });
        }
        root_->addStretch(1);
    }

    // ---- Track EQ
    // The "EQ" sheet: the response graph (the curve of the four bands, a handle on each band's frequency and gain: drag it,
    // the one clicked is the band shown below), then that band's enable, mode and trackbars; the "Channels" sheet has the six
    // channel switches. The band shown is the page's own (not a setting).
    int eqSel_ = 0;
    vegeq::Settings trkEqSettings() const
    {
        double x[vegeq::kParams];
        for (int i = 0; i < vegeq::kParams; ++i) x[i] = get(i);
        return vegeq::fromParams(x, eqSel_, 0);
    }
    static double trkEqRound(double v, int dec)
    {
        const double k = std::pow(10.0, dec);
        return std::floor(v * k + 0.5) / k;
    }
    // A trackbar of 'steps' positions over the range of setting ix() (log or linear), values rounded to m.dec digits, and its box.
    QWidget* trkEqRow(const QString& caption, int base, int steps, const SMap& m, const QString& name, double shownLo = 0, double shownHi = 0)
    {
        QWidget* w = new QWidget(this);
        QHBoxLayout* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(6);
        QLabel* l = text(caption);
        l->setFixedWidth(116);
        h->addWidget(l);
        DxSlider* s = new DxSlider(Qt::Horizontal, this);
        s->setObjectName(name);
        s->setRange(0, steps);
        QLineEdit* box = editBox(54);
        box->setObjectName(name + "Edit");
        h->addWidget(s, 1);
        h->addWidget(box);
        l->setBuddy(s);
        auto ix = [this, base] { return base + eqSel_; };
        QObject::connect(s, &QSlider::valueChanged, s, [=](int pos) {
            if (updating_) return;
            Rng r = rng(ix());
            if (shownHi > shownLo) { r.lo = shownLo; r.hi = shownHi; }
            const double t = (double)pos / steps;
            const double v = m.log ? r.lo * std::pow(r.hi / r.lo, t) : r.lo + t * (r.hi - r.lo);
            commit(ix(), trkEqRound(v, m.dec));
        });
        QObject::connect(s, &QSlider::sliderPressed, s, [=] { beginDrag(ix()); });
        QObject::connect(s, &QSlider::sliderReleased, s, [=] { endDrag(); });
        QObject::connect(box, &QLineEdit::editingFinished, box, [=] {
            if (updating_) return;
            const Rng r = rng(ix());
            if (box->text() == edit(get(ix()), m, r)) return;
            double v = 0;
            if (!parseNumber(box->text(), &v)) { syncAll(); return; }
            commit(ix(), trkEqRound(v, m.dec));
        });
        sync_.push_back([=] {
            Rng r = rng(ix());
            if (shownHi > shownLo) { r.lo = shownLo; r.hi = shownHi; }
            const double v = get(ix());
            const double t = m.log ? std::log(std::max(v, r.lo) / r.lo) / std::log(r.hi / r.lo) : (v - r.lo) / (r.hi - r.lo);
            QSignalBlocker b(s);
            if (!s->isSliderDown()) s->setValue((int)std::lround(std::max(0.0, std::min(1.0, t)) * steps));
            if (!box->hasFocus()) box->setText(edit(v, m, r));
        });
        return w;
    }
    void trkEq()
    {
        QWidget* sheetEq = new QWidget(this);
        QVBoxLayout* col = new QVBoxLayout(sheetEq);
        col->setContentsMargins(0, 0, 0, 0);
        col->setSpacing(6);

        Graph* g = new Graph(this);
        g->setObjectName("dxEqGraph");
        g->setFixedHeight(120);
        g->margins = QMargins(30, 6, 8, 18);
        graphs_.push_back(g);
        g->draw = [this](QPainter& p, const QRectF& pl) {
            const double lo = std::log(20.0), hi = std::log(20000.0);
            auto xAt = [&](double hz) { return pl.left() + (std::log(hz) - lo) / (hi - lo) * pl.width(); };
            auto yAt = [&](double db) { return pl.bottom() - (db + 15.0) / 30.0 * pl.height(); };
            p.setPen(QColor(210, 210, 210));
            const double marks[] = {20, 40, 80, 160, 320, 640, 1280, 2560, 5120, 10240, 20000};
            const char* names[] = {"20", "40", "80", "160", "320", "640", "1.2k", "2.6k", "5.1k", "10k", "20k"};
            for (double f : marks) p.drawLine(QPointF(xAt(f), pl.top()), QPointF(xAt(f), pl.bottom()));
            for (double d : {-15.0, -7.5, 0.0, 7.5, 15.0}) p.drawLine(QPointF(pl.left(), yAt(d)), QPointF(pl.right(), yAt(d)));
            p.setPen(QColor(150, 150, 150));
            p.drawLine(QPointF(pl.left(), yAt(0)), QPointF(pl.right(), yAt(0)));
            p.setPen(Qt::black);
            QFont small = p.font();
            small.setPointSizeF(std::max(6.0, small.pointSizeF() - 1.5));
            p.setFont(small);
            for (int k = 0; k < 11; ++k)
                p.drawText(QRectF(xAt(marks[k]) - 16, pl.bottom() + 2, 32, 14), Qt::AlignHCenter | Qt::AlignTop, names[k]);
            p.drawText(QRectF(0, pl.top() - 6, pl.left() - 3, 12), Qt::AlignRight | Qt::AlignVCenter, "+15");
            p.drawText(QRectF(0, yAt(0) - 6, pl.left() - 3, 12), Qt::AlignRight | Qt::AlignVCenter, "0");
            p.drawText(QRectF(0, pl.bottom() - 6, pl.left() - 3, 12), Qt::AlignRight | Qt::AlignVCenter, "-15");
            const vegeq::Settings s = trkEqSettings();
            vegeq::Eq eq;
            eq.prepare(s, 48000.0, false);
            QPainterPath path;
            const int n = std::max(2, (int)pl.width());
            for (int k = 0; k < n; ++k) {
                const double hz = std::exp(lo + (hi - lo) * k / (n - 1));
                const double db = eq.idle ? 0.0 : std::max(-15.0, std::min(15.0, eq.responseDb(hz)));
                const QPointF pt(pl.left() + pl.width() * k / (n - 1), yAt(db));
                if (k == 0) path.moveTo(pt);
                else path.lineTo(pt);
            }
            p.setClipRect(pl);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(QPen(QColor(0, 110, 0), 1.5));
            p.drawPath(path);
            p.setClipping(false);
            for (int b = 0; b < 4; ++b) {
                const QPointF c(xAt(s.freq[b]), yAt(std::max(-15.0, std::min(15.0, s.gain[b] / 10.0))));
                const bool on = s.enable[b] != 0;
                p.setPen(QPen(on ? QColor(180, 0, 0) : QColor(130, 130, 130), 1));
                p.setBrush(b == eqSel_ ? (on ? QColor(180, 0, 0) : QColor(130, 130, 130)) : QColor(Qt::white));
                p.drawRect(QRectF(c.x() - 3.5, c.y() - 3.5, 7, 7));
                p.setPen(Qt::black);
                p.drawText(QRectF(c.x() + 5, c.y() - 14, 12, 12), Qt::AlignLeft | Qt::AlignVCenter, QString::number(b + 1));
            }
        };
        g->mouse = [this, g](const QPointF& at, int phase) {
            const QRectF pl = g->plot();
            const double lo = std::log(20.0), hi = std::log(20000.0);
            if (phase == 0) {
                const vegeq::Settings s = trkEqSettings();
                int best = -1;
                double bd = 14 * 14;
                for (int b = 0; b < 4; ++b) {
                    const double x = (std::log((double)s.freq[b]) - lo) / (hi - lo);
                    const double y = (std::max(-15.0, std::min(15.0, s.gain[b] / 10.0)) + 15.0) / 30.0;
                    const double dx = (at.x() - x) * pl.width(), dy = (at.y() - y) * pl.height();
                    if (dx * dx + dy * dy <= bd) {
                        bd = dx * dx + dy * dy;
                        best = b;
                    }
                }
                if (best < 0) {
                    eqDrag_ = false;
                    return;
                }
                eqSel_ = best;
                eqDrag_ = true;
                beginDrag(best);
                syncAll();
            } else if (eqDrag_) {
                const double x = std::max(0.0, std::min(1.0, at.x())), y = std::max(0.0, std::min(1.0, at.y()));
                commit(eqSel_, trkEqRound(std::exp(lo + (hi - lo) * x), 0));
                commit(4 + eqSel_, trkEqRound(y * 30.0 - 15.0, 1));
                if (phase == 2) {
                    eqDrag_ = false;
                    endDrag();
                }
            }
        };
        col->addWidget(g);

        QFrame* box = new QFrame(this);
        box->setFrameShape(QFrame::StyledPanel);
        QVBoxLayout* bv = new QVBoxLayout(box);
        bv->setSpacing(4);
        QHBoxLayout* top = new QHBoxLayout();
        top->setSpacing(10);
        QLabel* title = text(QString());
        title->setObjectName("dxEqBand");
        top->addWidget(title);
        QCheckBox* en = new QCheckBox("&Enabled", this);
        en->setObjectName("dxEqEnabled");
        QObject::connect(en, &QCheckBox::toggled, en, [this](bool on) { commit(20 + eqSel_, on ? 1 : 0); });
        top->addWidget(en);
        QComboBox* mode = combo([this] { return 16 + eqSel_; }, QStringList() << "Low Shelf" << "Band" << "High Shelf", "dxEqMode");
        top->addWidget(mode);
        top->addStretch(1);
        bv->addLayout(top);
        SMap fm;
        fm.log = true;
        fm.dec = 0;
        SMap gm;
        SMap bm;
        SMap rm;
        rm.dec = 0;
        bv->addWidget(trkEqRow("&Frequency (Hz):", 0, 1000, fm, "dxEqFreq"));
        bv->addWidget(trkEqRow("&Gain (dB):", 4, 300, gm, "dxEqGain", -15, 15));
        QWidget* bw = trkEqRow("&Bandwidth (oct):", 8, 39, bm, "dxEqBandwidth");
        QWidget* ro = trkEqRow("&Rolloff (dB/oct):", 12, 3, rm, "dxEqRolloff");
        bv->addWidget(bw);
        bv->addWidget(ro);
        col->addWidget(box);
        sync_.push_back([this, title, en, bw, ro] {
            title->setText(QString("Band %1").arg(eqSel_ + 1));
            QSignalBlocker b(en);
            en->setChecked(get(20 + eqSel_) >= 0.5);
            const bool band = (int)std::lround(get(16 + eqSel_)) == 1;
            bw->setVisible(band);
            ro->setVisible(!band);
        });

        QWidget* sheetCh = new QWidget(this);
        QVBoxLayout* cv = new QVBoxLayout(sheetCh);
        cv->setSpacing(10);
        const char* chn[6] = {"Enable &Left", "Enable &Right", "Enable &Center", "Enable Le&ft Surround", "Enable Ri&ght Surround", "Enable LF&E"};
        for (int k = 0; k < 6; ++k) cv->addWidget(checkBox(24 + k, chn[k]));
        cv->addStretch(1);
        sheetCh->hide();

        QHBoxLayout* tabs = new QHBoxLayout();
        tabs->setSpacing(4);
        QPushButton* t1 = new QPushButton("EQ", this);
        QPushButton* t2 = new QPushButton("Channels", this);
        t1->setObjectName("dxEqTabEq");
        t2->setObjectName("dxEqTabChannels");
        for (QPushButton* t : {t1, t2}) {
            t->setCheckable(true);
            t->setAutoExclusive(true);
            tabs->addWidget(t);
        }
        t1->setChecked(true);
        tabs->addStretch(1);
        QObject::connect(t1, &QPushButton::toggled, sheetEq, [sheetEq, sheetCh](bool on) {
            sheetEq->setVisible(on);
            sheetCh->setVisible(!on);
        });
        root_->addLayout(tabs);
        root_->addWidget(sheetEq);
        root_->addWidget(sheetCh);
        root_->addStretch(1);
    }
    bool eqDrag_ = false;

    // the page for 'uid' if it is one of these effects (width: the page's width in pixels)
    bool trkPage(const std::string& uid, int* width)
    {
        if (uid == "reno.trkvolume") {
            trkVolume();
            *width = 410;
            return true;
        }
        if (uid == "reno.trkpan") {
            trkPan();
            *width = 410;
            return true;
        }
        if (uid == "reno.trkgate") {
            trkGate();
            *width = 430;
            return true;
        }
        if (uid == "reno.trkcomp") {
            trkComp();
            *width = 410;
            return true;
        }
        if (uid == "reno.trkdither") {
            trkDither();
            *width = 340;
            return true;
        }
        if (uid == "reno.trkeq") {
            trkEq();
            *width = 430;
            return true;
        }
        return false;
    }
