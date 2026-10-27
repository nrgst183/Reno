// Pages of Reno's versions of the sfppack3_x64.dll effects (Amplitude Modulation, Flange/Wah-wah, Gapper/Snipper,
// Smooth/Enhance, Vibrato): each copies the plug-in's own dialog (controls, captions with their mnemonics, layout, ranges).
// This file is part of fxpages.cpp's Page class (it is included inside the class body).

    // ============================================================================================ Smooth/Enhance
    // Dialog 4001: "Operation (-5 to 5):", a trackbar, its value at the right, "Smooth" and "Enhance" under the ends.
    void smoothEnhance()
    {
        QGridLayout* g = new QGridLayout();
        g->setHorizontalSpacing(8);
        g->setVerticalSpacing(2);
        QLabel* cap = text("<u>O</u>peration (-5 to 5):");
        g->addWidget(cap, 0, 0);
        SMap m;
        m.dec = 0;
        QLabel* value = text(QString(), Qt::AlignRight | Qt::AlignVCenter);
        value->setObjectName("dxParam0Value");
        value->setFixedWidth(22);
        DxSlider* s = slider(Qt::Horizontal, fixed(0), m, "dxParam0", nullptr, value);
        cap->setBuddy(s);
        g->addWidget(s, 0, 1);
        g->addWidget(value, 0, 2);
        g->addWidget(text("Smooth"), 1, 1, Qt::AlignLeft);
        g->addWidget(text("Enhance"), 1, 1, Qt::AlignRight);
        g->setColumnStretch(1, 1);
        root_->addLayout(g);
        root_->addStretch(1);
    }

    // ============================================================================================ Gapper/Snipper
    // Dialog 2001: Effect (Gapper (insert) / Snipper (cut)), the frequency of the events with its trackbar (log) and box,
    // the length of one event, the fade of its edges, and the length of the result as a percent of the original.
    veggap::Settings gapSettings() const
    {
        veggap::Settings s;
        s.mode = (int)std::lround(get(0));
        s.frequency = (int)std::lround(get(1) * 10000.0);
        s.length = (int)std::lround(get(2) * 10000.0);
        s.fade = (int)std::lround(get(3));
        veggap::sanitize(s);
        return s;
    }

    void gapperSnipper()
    {
        QHBoxLayout* eff = new QHBoxLayout();
        eff->setSpacing(6);
        QLabel* el = text("<u>E</u>ffect:");
        eff->addWidget(el);
        eff->addWidget(radios(0, {"Gapper (insert)", "Snipper (cut)"}), 1);
        root_->addLayout(eff);
        QFrame* line = new QFrame(this);
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Sunken);
        root_->addWidget(line);

        // frequency (log trackbar) and length (linear), each with its box at the right of the caption
        auto sliderBlock = [this](int i, const QString& caption, const SMap& m) {
            QWidget* w = new QWidget(this);
            QVBoxLayout* v = new QVBoxLayout(w);
            v->setContentsMargins(0, 0, 0, 0);
            v->setSpacing(1);
            QHBoxLayout* top = new QHBoxLayout();
            top->setSpacing(4);
            QLabel* l = text(caption);
            top->addWidget(l, 1);
            QLineEdit* box = editBox(86);
            top->addWidget(box);
            v->addLayout(top);
            DxSlider* s = slider(Qt::Horizontal, fixed(i), m, QString("dxParam%1").arg(i), box, nullptr);
            v->addWidget(s);
            l->setBuddy(s);
            return w;
        };
        SMap fm;
        fm.log = true;
        fm.dec = 4;
        root_->addWidget(sliderBlock(1, "Fre<u>q</u>. to gap/snip events (0.0001 to 1,000 Hz):", fm));
        SMap lm;
        lm.dec = 4;
        root_->addWidget(sliderBlock(2, "<u>L</u>ength of one event (0.0001 to 1.0000 seconds):", lm));

        QHBoxLayout* fade = new QHBoxLayout();
        fade->setSpacing(6);
        QLabel* fl = text("<u>F</u>ade edges of each event (0 to 50 %):");
        fade->addWidget(fl);
        SMap pm;
        pm.dec = 0;
        QLabel* fv = text(QString(), Qt::AlignRight | Qt::AlignVCenter);
        fv->setObjectName("dxParam3Value");
        fv->setFixedWidth(40);
        DxSlider* fs = slider(Qt::Horizontal, fixed(3), pm, "dxParam3", nullptr, fv, "%");
        fs->setFixedWidth(120);
        fl->setBuddy(fs);
        fade->addWidget(fs);
        fade->addWidget(fv);
        fade->addStretch(1);
        root_->addLayout(fade);

        QFrame* line2 = new QFrame(this);
        line2->setFrameShape(QFrame::HLine);
        line2->setFrameShadow(QFrame::Sunken);
        root_->addWidget(line2);
        QHBoxLayout* pc = new QHBoxLayout();
        pc->addWidget(text("Percent of original:"));
        pc->addStretch(1);
        QLabel* pv = text(QString(), Qt::AlignRight | Qt::AlignVCenter);
        pv->setObjectName("dxPercentOfOriginal");
        pc->addWidget(pv);
        root_->addLayout(pc);
        sync_.push_back([this, pv] { pv->setText(QString("%1 %").arg(veggap::percentOfOriginal(gapSettings()))); });
        root_->addStretch(1);
    }

    // ============================================================================================ Vibrato
    // Dialog 3001: the output gain and semitones faders, the graph (time across, +24 to -24 semitones up), "Blend graph edges",
    // "Reset" (the graph), and the modulation frequency (log trackbar and box). The graph is in the saved form only.
    vegvib::Settings vibSettings() const
    {
        vegvib::Settings s;
        if (h_.state) vegvib::decode(h_.state(), &s);
        return s;
    }
    void vibStore(const vegvib::Settings& s, const std::string& what)
    {
        if (updating_ || !h_.setState) return;
        h_.setState(vegvib::encode(s), what);
        syncAll();
    }

    void vibrato()
    {
        QGridLayout* g = new QGridLayout();
        g->setHorizontalSpacing(0);
        g->setVerticalSpacing(6);
        g->addWidget(gainColumn(3, "<u>O</u>utput gain"), 0, 0);
        QLabel* readout = text(QString(), Qt::AlignRight | Qt::AlignVCenter);
        readout->setObjectName("dxGraphReadout");
        // Semitones: a fader of its own beside the gain one
        {
            QWidget* col = new QWidget(this);
            QVBoxLayout* v = new QVBoxLayout(col);
            v->setContentsMargins(0, 0, 0, 0);
            v->setSpacing(3);
            QLabel* spacer = text(QString());
            spacer->setFixedHeight(16);
            v->addWidget(spacer);
            SMap m;
            m.dec = 2;
            DxSlider* s = slider(Qt::Vertical, fixed(0), m, "dxParam0", nullptr, nullptr);
            s->setFixedHeight(118);
            v->addWidget(s, 0, Qt::AlignHCenter);
            QLabel* cap = text("Semi<u>t</u>ones<br>(+/- 24)", Qt::AlignHCenter | Qt::AlignTop);
            s->setToolTip("Semitones");
            v->addWidget(cap);
            col->setFixedWidth(84);
            g->addWidget(col, 0, 1);
        }
        // the graph
        QVBoxLayout* gv = new QVBoxLayout();
        gv->setSpacing(1);
        QHBoxLayout* cap = new QHBoxLayout();
        cap->addWidget(text("<u>G</u>raph:"));
        cap->addWidget(readout, 1);
        gv->addLayout(cap);
        PointGraph* pg = new PointGraph(this);
        pg->setObjectName("dxGraph");
        pg->setFixedSize(QSize(300, 184));
        graphs_.push_back(pg);
        pg->readout = readout;
        pg->maxPoints = vegvib::kMaxPoints;
        pg->xMax = vegvib::kGraphEnd;
        pg->yMin = -vegvib::kGraphTop;
        pg->yMax = vegvib::kGraphTop;
        pg->gridX = 10;
        pg->gridY = 4;
        pg->margins = QMargins(30, 6, 6, 6);
        auto resting = [this, readout] { readout->setText(QString("%1 semitones").arg(get(0), 0, 'f', 2)); };
        pg->describe = [this](const PointGraph::P& p) {
            const double st = (double)p.y / 100.0 * get(0);
            return QString("%1%2 semitones at %3 %").arg(st >= 0 ? "+" : "").arg(st, 0, 'f', 2).arg(p.x / 10.0, 0, 'f', 1);
        };
        pg->leave = resting;
        pg->decorate = [](QPainter& p, const QRectF& pl) {
            p.setPen(Qt::black);
            p.drawText(QRectF(0, pl.top() - 6, pl.left() - 3, 12), Qt::AlignRight | Qt::AlignVCenter, "+24");
            p.drawText(QRectF(0, pl.center().y() - 6, pl.left() - 3, 12), Qt::AlignRight | Qt::AlignVCenter, "0");
            p.drawText(QRectF(0, pl.bottom() - 6, pl.left() - 3, 12), Qt::AlignRight | Qt::AlignVCenter, "-24");
            p.setPen(QPen(QColor(70, 70, 70), 1));
            const double y = std::round(pl.center().y()) + 0.5;
            p.drawLine(QPointF(pl.left(), y), QPointF(pl.right(), y));
        };
        pg->edited = [this, pg](int phase) {
            vegvib::Settings s = vibSettings();
            s.count = (int32_t)pg->pts.size();
            std::memset(s.pts, 0, sizeof s.pts);
            for (int k = 0; k < s.count; ++k) {
                s.pts[2 * k] = pg->pts[(size_t)k].x;
                s.pts[2 * k + 1] = pg->pts[(size_t)k].y;
            }
            if (phase == 0 && h_.beginEdit) h_.beginEdit("Edit Vibrato graph");
            vibStore(s, "Edit Vibrato graph");
            if (phase == 2) endDrag();
        };
        sync_.push_back([this, pg, resting] {
            if (!pg->dragging()) {
                const vegvib::Settings s = vibSettings();
                pg->pts.clear();
                for (int k = 0; k < s.count; ++k) pg->pts.push_back(PointGraph::P{s.pts[2 * k], s.pts[2 * k + 1]});
            }
            if (!pg->underMouse()) resting();
        });
        gv->addWidget(pg);
        QHBoxLayout* under = new QHBoxLayout();
        under->addStretch(1);
        under->addWidget(text("Time"));
        under->addStretch(1);
        QPushButton* reset = button("R&eset");
        reset->setObjectName("dxReset");
        QObject::connect(reset, &QPushButton::clicked, reset, [this] {
            vegvib::Settings s = vibSettings();
            s.count = 2;
            std::memset(s.pts, 0, sizeof s.pts);
            s.pts[2] = vegvib::kGraphEnd;
            vibStore(s, "Reset Vibrato graph");
        });
        under->addWidget(reset);
        gv->addLayout(under);
        g->addLayout(gv, 0, 2);

        g->addWidget(checkBox(2, "B&lend graph edges"), 1, 0, 1, 2, Qt::AlignLeft | Qt::AlignVCenter);
        // the modulation frequency
        QVBoxLayout* fv = new QVBoxLayout();
        fv->setSpacing(1);
        QHBoxLayout* top = new QHBoxLayout();
        top->setSpacing(4);
        QLabel* fl = text("<u>M</u>odulation freq. (0.001 to 1,000 Hz):");
        top->addWidget(fl, 1);
        QLineEdit* box = editBox(70);
        top->addWidget(box);
        fv->addLayout(top);
        SMap fm;
        fm.log = true;
        fm.dec = 3;
        DxSlider* fs = slider(Qt::Horizontal, fixed(1), fm, "dxParam1", box, nullptr);
        fl->setBuddy(fs);
        fv->addWidget(fs);
        g->addLayout(fv, 1, 2);
        g->setColumnStretch(2, 1);
        root_->addLayout(g);
        root_->addStretch(1);
    }

    // ============================================================================================ Amplitude Modulation
    // Dialog 6001: the Dry out, Wet out and Amplitude faders, the graph (time across, 0 down to -Inf. dB), "Blend graph edges",
    // "Reset" (the graph), the modulation frequency (log trackbar and box), the stereo pan, the low-pass filter (check, log
    // trackbar and box), "Tempo sync" with the period's multiplier (box and spinner) and the period's note length (combo).
    // With tempo sync on the frequency is not used, with it off the period is not. The graph is in the saved form only.
    vegamp::Settings ampSettings() const
    {
        vegamp::Settings s;
        if (h_.state) vegamp::decode(h_.state(), &s);
        return s;
    }
    void ampStore(const vegamp::Settings& s, const std::string& what)
    {
        if (updating_ || !h_.setState) return;
        h_.setState(vegamp::encode(s), what);
        syncAll();
    }

    void ampMod()
    {
        QGridLayout* g = new QGridLayout();
        g->setHorizontalSpacing(0);
        g->setVerticalSpacing(6);
        QWidget* dryCol = gainColumn(6, "<u>D</u>ry out");
        QWidget* wetCol = gainColumn(7, "<u>W</u>et out");
        dryCol->setFixedWidth(96);
        wetCol->setFixedWidth(100);
        g->addWidget(dryCol, 0, 0);
        g->addWidget(wetCol, 0, 1);
        QLabel* readout = text(QString(), Qt::AlignRight | Qt::AlignVCenter);
        readout->setObjectName("dxGraphReadout");
        // Amplitude: a fader of its own (its value is shown in the graph's readout, as the dialog has no text for it)
        {
            QWidget* col = new QWidget(this);
            QVBoxLayout* v = new QVBoxLayout(col);
            v->setContentsMargins(0, 0, 0, 0);
            v->setSpacing(3);
            QLabel* spacer = text(QString());
            spacer->setFixedHeight(16);
            v->addWidget(spacer);
            SMap m;
            m.inf = true;
            m.dec = 1;
            DxSlider* s = slider(Qt::Vertical, fixed(0), m, "dxParam0", nullptr, nullptr);
            s->setFixedHeight(118);
            v->addWidget(s, 0, Qt::AlignHCenter);
            v->addWidget(text("A<u>m</u>plitude<br>" + rangeText(0, m, "dB"), Qt::AlignHCenter | Qt::AlignTop));
            col->setFixedWidth(96);
            g->addWidget(col, 0, 2);
        }
        // the graph
        QVBoxLayout* gv = new QVBoxLayout();
        gv->setSpacing(1);
        QHBoxLayout* cap = new QHBoxLayout();
        cap->addWidget(text("<u>G</u>raph:"));
        cap->addWidget(readout, 1);
        gv->addLayout(cap);
        PointGraph* pg = new PointGraph(this);
        pg->setObjectName("dxGraph");
        pg->setFixedSize(QSize(300, 178));
        graphs_.push_back(pg);
        pg->readout = readout;
        pg->maxPoints = vegamp::kMaxPoints;
        pg->xMax = vegamp::kGraphEnd;
        pg->yMin = vegamp::kGraphLow;
        pg->yMax = 0;
        pg->gridX = 10;
        pg->gridY = 4;
        pg->margins = QMargins(34, 6, 6, 6);
        auto resting = [this, readout] {
            const double a = get(0);
            readout->setText(a <= -79.95 ? QString("-Inf. dB") : QString("%1 dB").arg(a, 0, 'f', 1));
        };
        pg->describe = [this](const PointGraph::P& p) {
            const double db = (double)p.y / 100.0 * -get(0);
            return QString("%1 dB at %2 %").arg(db, 0, 'f', 1).arg(p.x / 10.0, 0, 'f', 1);
        };
        pg->leave = resting;
        pg->decorate = [](QPainter& p, const QRectF& pl) {
            p.setPen(Qt::black);
            p.drawText(QRectF(0, pl.top() - 6, pl.left() - 3, 12), Qt::AlignRight | Qt::AlignVCenter, "0");
            p.drawText(QRectF(0, pl.center().y() - 6, pl.left() - 3, 12), Qt::AlignRight | Qt::AlignVCenter, "-45");
            p.drawText(QRectF(0, pl.bottom() - 6, pl.left() - 3, 12), Qt::AlignRight | Qt::AlignVCenter, "-Inf.");
        };
        pg->edited = [this, pg](int phase) {
            vegamp::Settings s = ampSettings();
            s.count = (int32_t)pg->pts.size();
            std::memset(s.pts, 0, sizeof s.pts);
            for (int k = 0; k < s.count; ++k) {
                s.pts[2 * k] = pg->pts[(size_t)k].x;
                s.pts[2 * k + 1] = pg->pts[(size_t)k].y;
            }
            if (phase == 0 && h_.beginEdit) h_.beginEdit("Edit Amplitude Modulation graph");
            ampStore(s, "Edit Amplitude Modulation graph");
            if (phase == 2) endDrag();
        };
        sync_.push_back([this, pg, resting] {
            if (!pg->dragging()) {
                const vegamp::Settings s = ampSettings();
                pg->pts.clear();
                for (int k = 0; k < s.count; ++k) pg->pts.push_back(PointGraph::P{s.pts[2 * k], s.pts[2 * k + 1]});
            }
            if (!pg->underMouse()) resting();
        });
        gv->addWidget(pg);
        QHBoxLayout* under = new QHBoxLayout();
        under->addStretch(1);
        under->addWidget(text("Time"));
        under->addStretch(1);
        QPushButton* reset = button("R&eset");
        reset->setObjectName("dxReset");
        QObject::connect(reset, &QPushButton::clicked, reset, [this] {
            vegamp::Settings s = ampSettings();
            s.count = 2;
            std::memset(s.pts, 0, sizeof s.pts);
            s.pts[1] = -50;
            s.pts[2] = vegamp::kGraphEnd;
            s.pts[3] = -50;
            ampStore(s, "Reset Amplitude Modulation graph");
        });
        under->addWidget(reset);
        gv->addLayout(under);
        g->addLayout(gv, 0, 3);
        g->addWidget(checkBox(5, "B&lend graph edges"), 1, 0, 1, 3, Qt::AlignLeft | Qt::AlignVCenter);
        g->setColumnStretch(3, 1);
        root_->addLayout(g);

        // the modulation frequency (not used with tempo sync)
        SMap fm;
        fm.log = true;
        fm.dec = 3;
        QWidget* freqRow = lineRow(210, fixed(1), "Mod. <u>f</u>req. (0.001 to 5,000 Hz):", fm, "dxParam1");
        root_->addWidget(freqRow);
        SMap pm;
        pm.dec = 0;
        root_->addWidget(lineRow(210, fixed(2), "S<u>t</u>ereo pan  (0 to 100 %):", pm, "dxParam2"));
        // the low-pass filter: the check turns the trackbar on
        SMap lm;
        lm.log = true;
        lm.dec = 0;
        QCheckBox* lc = nullptr;
        QLineEdit* lpBox = nullptr;
        DxSlider* lpSlider = nullptr;
        root_->addWidget(lineRow(194, fixed(4), "L<u>o</u>w-pass start freq. (Hz):", lm, "dxParam4", &lc, &lpBox, &lpSlider));
        lc->setObjectName("dxParam3");
        QObject::connect(lc, &QCheckBox::toggled, lc, [this](bool on) { commit(3, on ? 1 : 0); });
        sync_.push_back([=] {
            QSignalBlocker b1(lc);
            const bool on = get(3) >= 0.5;
            lc->setChecked(on);
            lpSlider->setEnabled(on);
            lpBox->setEnabled(on);
        });
        // tempo sync with the period: the multiplier with its spinner, and the note length
        QCheckBox* ts = checkBox(10, "Tempo &sync");
        root_->addWidget(ts);
        QWidget* periodRow = new QWidget(this);
        QHBoxLayout* ph = new QHBoxLayout(periodRow);
        ph->setContentsMargins(0, 0, 0, 0);
        ph->setSpacing(6);
        ph->addWidget(text("Modulation <u>p</u>eriod"));
        QDoubleSpinBox* mult = new QDoubleSpinBox(this);
        mult->setObjectName("dxParam9");
        mult->setDecimals(3);
        mult->setRange(0.001, 32.0);
        mult->setSingleStep(1.0);       // the plug-in's spinner steps by one
        mult->setKeyboardTracking(false);
        mult->setFixedWidth(86);
        QObject::connect(mult, QOverload<double>::of(&QDoubleSpinBox::valueChanged), mult, [this](double v) {
            if (!updating_) commit(9, v);
        });
        ph->addWidget(mult);
        QStringList notes = {"64th Note Triplet", "64th Note", "Dotted 64th Note", "32nd Note Triplet", "32nd Note", "Dotted 32nd Note", "16th Note Triplet", "16th Note",
                             "Dotted 16th Note", "8th Note Triplet", "8th Note", "Dotted 8th Note", "Quarter Note Triplet", "Quarter Note", "Dotted Quarter Note",
                             "Half Note Triplet", "Half Note", "Dotted Half Note", "Measures"};
        QComboBox* per = combo(fixed(8), notes, "dxParam8");
        ph->addWidget(per, 1);
        root_->addWidget(periodRow);
        sync_.push_back([=] {
            QSignalBlocker b(mult);
            if (!mult->hasFocus()) mult->setValue(get(9));
            const bool sync = get(10) >= 0.5;
            freqRow->setEnabled(!sync);
            periodRow->setEnabled(sync);
        });
        root_->addStretch(1);
    }

    // ============================================================================================ Flange/Wah-wah
    // Dialog 1001: Effect (Flange / Phaser / Wah-Wah), the two gain faders (Dry out, Wet out), then Rate (box and trackbar),
    // Depth, Center frequency (box and trackbar), Resonance, "Tempo sync" with the period (multiplier spinner and note length).
    // Center frequency and Resonance do not work for the Flange (they are turned off then).
    void flangeWah()
    {
        QHBoxLayout* eff = new QHBoxLayout();
        eff->setSpacing(6);
        eff->addWidget(text("E<u>f</u>fect:"));
        eff->addWidget(radios(0, {"Flange", "Phaser", "Wah-Wah"}), 1);
        root_->addLayout(eff);
        QFrame* line = new QFrame(this);
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Sunken);
        root_->addWidget(line);

        QHBoxLayout* body = new QHBoxLayout();
        body->setSpacing(6);
        QWidget* dryCol = gainColumn(6, "<u>D</u>ry out");
        QWidget* wetCol = gainColumn(5, "<u>W</u>et out");
        dryCol->setFixedWidth(96);
        wetCol->setFixedWidth(100);
        body->addWidget(dryCol, 0, Qt::AlignTop);
        body->addWidget(wetCol, 0, Qt::AlignTop);

        QVBoxLayout* r = new QVBoxLayout();
        r->setSpacing(4);
        // a trackbar with its caption and a box (Rate, Center frequency)
        auto boxed = [this](int i, const QString& caption, const SMap& m) {
            QWidget* w = new QWidget(this);
            QVBoxLayout* v = new QVBoxLayout(w);
            v->setContentsMargins(0, 0, 0, 0);
            v->setSpacing(1);
            QHBoxLayout* top = new QHBoxLayout();
            top->setSpacing(4);
            QLabel* l = text(caption);
            top->addWidget(l, 1);
            QLineEdit* box = editBox(70);
            top->addWidget(box);
            v->addLayout(top);
            DxSlider* s = slider(Qt::Horizontal, fixed(i), m, QString("dxParam%1").arg(i), box, nullptr);
            v->addWidget(s);
            l->setBuddy(s);
            return w;
        };
        // a trackbar with its caption and the value in percent at the right (Depth, Resonance)
        auto percent = [this](int i, const QString& caption) {
            QWidget* w = new QWidget(this);
            QVBoxLayout* v = new QVBoxLayout(w);
            v->setContentsMargins(0, 0, 0, 0);
            v->setSpacing(1);
            QHBoxLayout* top = new QHBoxLayout();
            top->setSpacing(4);
            QLabel* l = text(caption);
            top->addWidget(l, 1);
            QLabel* val = text(QString(), Qt::AlignRight | Qt::AlignVCenter);
            val->setObjectName(QString("dxParam%1Value").arg(i));
            val->setFixedWidth(50);
            top->addWidget(val);
            v->addLayout(top);
            SMap m;
            m.dec = 0;
            DxSlider* s = slider(Qt::Horizontal, fixed(i), m, QString("dxParam%1").arg(i), nullptr, val, "%");
            v->addWidget(s);
            l->setBuddy(s);
            return w;
        };
        SMap rm;
        rm.log = true;
        rm.dec = 3;
        r->addWidget(boxed(3, "Ra<u>t</u>e (0.001 to 20.0 Hz):", rm));
        QWidget* depth = percent(4, "D<u>e</u>pth (1 to 100 %):");
        r->addWidget(depth);
        SMap cm;
        cm.log = true;
        cm.dec = 0;
        QWidget* center = boxed(1, "<u>C</u>enter frequency (Hz):", cm);
        r->addWidget(center);
        QWidget* res = percent(2, "Res<u>o</u>nance (1 to 100 %):");
        r->addWidget(res);
        QCheckBox* ts = checkBox(9, "Tempo &sync");
        r->addWidget(ts);
        r->addWidget(text("<u>P</u>eriod"));
        QWidget* periodRow = new QWidget(this);
        QHBoxLayout* ph = new QHBoxLayout(periodRow);
        ph->setContentsMargins(0, 0, 0, 0);
        ph->setSpacing(6);
        QDoubleSpinBox* mult = new QDoubleSpinBox(this);
        mult->setObjectName("dxParam8");
        mult->setDecimals(3);
        mult->setRange(0.001, 32.0);
        mult->setSingleStep(1.0);       // the plug-in's spinner steps by one
        mult->setKeyboardTracking(false);
        mult->setFixedWidth(86);
        QObject::connect(mult, QOverload<double>::of(&QDoubleSpinBox::valueChanged), mult, [this](double v) {
            if (!updating_) commit(8, v);
        });
        ph->addWidget(mult);
        QStringList notes = {"64th Note Triplet", "64th Note", "Dotted 64th Note", "32nd Note Triplet", "32nd Note", "Dotted 32nd Note", "16th Note Triplet", "16th Note",
                             "Dotted 16th Note", "8th Note Triplet", "8th Note", "Dotted 8th Note", "Quarter Note Triplet", "Quarter Note", "Dotted Quarter Note",
                             "Half Note Triplet", "Half Note", "Dotted Half Note", "Measures"};
        ph->addWidget(combo(fixed(7), notes, "dxParam7"), 1);
        r->addWidget(periodRow);
        r->addStretch(1);
        body->addLayout(r, 1);
        root_->addLayout(body);
        sync_.push_back([=] {
            QSignalBlocker b(mult);
            if (!mult->hasFocus()) mult->setValue(get(8));
            const bool sync = get(9) >= 0.5;
            periodRow->setEnabled(sync);
            // the rate is a frequency unless it follows the tempo; the flange has no center frequency or resonance
            const bool flange = (int)std::lround(get(0)) == 0;
            center->setEnabled(!flange);
            res->setEnabled(!flange);
        });
        root_->addStretch(1);
    }
