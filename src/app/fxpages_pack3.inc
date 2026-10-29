// Pages of Reno's versions of the sfppack3_x64.dll effects (Amplitude Modulation, Flange/Wah-wah, Gapper/Snipper,
// Smooth/Enhance, Vibrato): each copies the plug-in's own dialog (controls, captions with their mnemonics, layout, ranges).
// This file is part of fxpages.cpp's Page class (it is included inside the class body).

    // ============================================================================================ Smooth/Enhance
    // Dialog 4001: "Operation (-5 to 5):", a trackbar, its value at the right, "Smooth" and "Enhance" under the ends.
    void smoothEnhance()
    {
        const Placed pl = placedPage(220, 45);
        QLabel* cap = putLabel(pl, "<u>O</u>peration (-5 to 5):", 5, 19, 72, 8);
        SMap m;
        m.dec = 0;
        QLabel* value = putLabel(pl, QString(), 200, 19, 12, 8, Qt::AlignRight | Qt::AlignVCenter);
        value->setObjectName("dxParam0Value");
        putTrack(pl, 0, m, 77, 17, 115, nullptr, value, cap);
        putLabel(pl, "Smooth", 77, 32, 28, 8);
        putLabel(pl, "Enhance", 159, 32, 33, 8, Qt::AlignRight | Qt::AlignVCenter);
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
        const Placed pl = placedPage(225, 134);
        putLabel(pl, "<u>E</u>ffect:", 5, 18, 26, 8);
        {
            QButtonGroup* g = new QButtonGroup(pl.box);
            g->setExclusive(true);
            const char* const names[2] = {"Gapper (insert)", "Snipper (cut)"};
            const int xs[2] = {55, 147}, ws[2] = {62, 53};
            for (int k = 0; k < 2; ++k) {
                QRadioButton* r = new QRadioButton(names[k], this);
                r->setObjectName(QString("dxParam0_%1").arg(k));
                g->addButton(r, k);
                put(pl, r, xs[k], 18, ws[k] + 14, 10);
                QObject::connect(r, &QRadioButton::toggled, r, [=](bool on) {
                    if (on) commit(0, k);
                });
            }
            sync_.push_back([=] {
                const int k = (int)std::lround(std::max(0.0, std::min(1.0, get(0))));
                if (QAbstractButton* b = g->button(k)) {
                    QSignalBlocker blocker(b);
                    b->setChecked(true);
                }
            });
        }
        auto hline = [&](int y) {
            QFrame* line = new QFrame(this);
            line->setFrameShape(QFrame::HLine);
            line->setFrameShadow(QFrame::Sunken);
            put(pl, line, 5, y, 215, 2);
        };
        hline(30);
        // frequency (log trackbar) and length (linear), each with its box at the right of the caption
        QLabel* fcap = putLabel(pl, "Fre<u>q</u>. to gap/snip events (0.0001 to 1,000 Hz):", 5, 37, 154, 8);
        QLineEdit* fbox = putEdit(pl, 180, 35, 40, 12);
        SMap fm;
        fm.log = true;
        fm.dec = 4;
        putTrack(pl, 1, fm, 5, 48, 215, fbox, nullptr, fcap);
        QLabel* lcap = putLabel(pl, "<u>L</u>ength of one event (0.0001 to 1.0000 seconds):", 5, 67, 160, 8);
        QLineEdit* lbox = putEdit(pl, 180, 65, 40, 12);
        SMap lm;
        lm.dec = 4;
        putTrack(pl, 2, lm, 5, 78, 215, lbox, nullptr, lcap);

        QLabel* fl = putLabel(pl, "<u>F</u>ade edges of each event (0 to 50 %):", 5, 98, 127, 8);
        SMap pm;
        pm.dec = 0;
        QLabel* fv = putLabel(pl, QString(), 196, 98, 24, 8, Qt::AlignRight | Qt::AlignVCenter);
        fv->setObjectName("dxParam3Value");
        putTrack(pl, 3, pm, 132, 96, 64, nullptr, fv, fl, "%");
        hline(114);
        putLabel(pl, "Percent of original:", 5, 121, 108, 8);
        QLabel* pv = putLabel(pl, QString(), 170, 121, 50, 8, Qt::AlignRight | Qt::AlignVCenter);
        pv->setObjectName("dxPercentOfOriginal");
        sync_.push_back([this, pv] { pv->setText(QString("%1 %").arg(veggap::percentOfOriginal(gapSettings()))); });
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
        const Placed pl = placedPage(287, 176);
        putFader(pl, 3, 25, 26, 93, 5, 50, "<u>O</u>utput gain<br>(-Inf. to 24 dB)", 5, 50);
        // Semitones: a fader of its own beside the gain one (its value is shown in the graph's readout)
        {
            SMap m;
            m.dec = 2;
            DxSlider* s = slider(Qt::Vertical, fixed(0), m, "dxParam0", nullptr, nullptr);
            put(pl, s, 69, 26, 10, 93);
            putLabel(pl, "Semi<u>t</u>ones<br>(+/- 24)", 48, 120, 51, 16, Qt::AlignHCenter | Qt::AlignTop);
            s->setToolTip("Semitones");
        }
        putLabel(pl, "<u>G</u>raph:", 112, 17, 36, 8);
        QLabel* readout = putLabel(pl, QString(), 158, 17, 124, 8, Qt::AlignRight | Qt::AlignVCenter);
        readout->setObjectName("dxGraphReadout");
        // the graph (its axis texts are the dialog's labels to its left)
        PointGraph* pg = put(pl, new PointGraph(this), 92, 23, 194, 106);
        pg->setObjectName("dxGraph");
        graphs_.push_back(pg);
        pg->readout = readout;
        pg->maxPoints = vegvib::kMaxPoints;
        pg->xMax = vegvib::kGraphEnd;
        pg->yMin = -vegvib::kGraphTop;
        pg->yMax = vegvib::kGraphTop;
        pg->gridX = 10;
        pg->gridY = 4;
        pg->margins = QMargins(Placed::px(20), 6, 6, 6);
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
        putLabel(pl, "Time", 169, 127, 48, 8, Qt::AlignHCenter | Qt::AlignVCenter);
        QPushButton* reset = put(pl, button("R&eset"), 242, 128, 40, 14);
        reset->setObjectName("dxReset");
        QObject::connect(reset, &QPushButton::clicked, reset, [this] {
            vegvib::Settings s = vibSettings();
            s.count = 2;
            std::memset(s.pts, 0, sizeof s.pts);
            s.pts[2] = vegvib::kGraphEnd;
            vibStore(s, "Reset Vibrato graph");
        });
        put(pl, checkBox(2, "B&lend graph edges"), 5, 154, 75, 10);
        // the modulation frequency
        QLabel* fl = putLabel(pl, "<u>M</u>odulation freq. (0.001 to 1,000 Hz):", 112, 148, 122, 8);
        QLineEdit* box = putEdit(pl, 242, 146, 40, 12);
        SMap fm;
        fm.log = true;
        fm.dec = 3;
        putTrack(pl, 1, fm, 112, 158, 170, box, nullptr, fl);
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
        const Placed pl = placedPage(323, 218);
        putFader(pl, 6, 20, 25, 76, 2, 45, "<u>D</u>ry out<br>(-Inf. to 0 dB)");
        putFader(pl, 7, 67, 25, 76, 49, 49, "<u>W</u>et out<br>(-Inf. to 24 dB)");
        // Amplitude: a fader of its own (its value is shown in the graph's readout, as the dialog has no text for it)
        {
            SMap m;
            m.inf = true;
            m.dec = 1;
            DxSlider* s = slider(Qt::Vertical, fixed(0), m, "dxParam0", nullptr, nullptr);
            put(pl, s, 114, 25, 10, 76);
            putLabel(pl, "A<u>m</u>plitude<br>(-Inf. to 0 dB)", 89, 102, 61, 16, Qt::AlignHCenter | Qt::AlignTop);
        }
        QLabel* readout = putLabel(pl, QString(), 219, 17, 99, 8, Qt::AlignRight | Qt::AlignVCenter);
        readout->setObjectName("dxGraphReadout");
        putLabel(pl, "<u>G</u>raph:", 158, 17, 28, 8);
        // the graph (its axis texts are the dialog's labels to its left)
        PointGraph* pg = put(pl, new PointGraph(this), 140, 23, 182, 101);
        pg->setObjectName("dxGraph");
        graphs_.push_back(pg);
        pg->readout = readout;
        pg->maxPoints = vegamp::kMaxPoints;
        pg->xMax = vegamp::kGraphEnd;
        pg->yMin = vegamp::kGraphLow;
        pg->yMax = 0;
        pg->gridX = 10;
        pg->gridY = 4;
        pg->margins = QMargins(Placed::px(18), 6, 6, 6);
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
        putLabel(pl, "Time", 222, 122, 32, 8, Qt::AlignHCenter | Qt::AlignVCenter);
        QPushButton* reset = put(pl, button("R&eset"), 273, 122, 45, 14);
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
        put(pl, checkBox(5, "B&lend graph edges"), 5, 124, 80, 10);

        // the modulation frequency (not used with tempo sync)
        QLabel* freqCap = putLabel(pl, "Mod. <u>f</u>req. (0.001 to 5,000 Hz):", 5, 143, 105, 8);
        QLineEdit* freqBox = putEdit(pl, 283, 140, 35, 12);
        SMap fm;
        fm.log = true;
        fm.dec = 3;
        DxSlider* freqS = putTrack(pl, 1, fm, 118, 140, 160, freqBox, nullptr, freqCap);
        QLabel* panCap = putLabel(pl, "S<u>t</u>ereo pan  (0 to 100 %):", 5, 158, 105, 8);
        QLineEdit* panBox = putEdit(pl, 283, 156, 35, 12);
        SMap pm;
        pm.dec = 0;
        putTrack(pl, 2, pm, 118, 156, 160, panBox, nullptr, panCap);
        // the low-pass filter: the check turns the trackbar on
        QCheckBox* lc = put(pl, new QCheckBox(this), 5, 173, 105, 10);
        lc->setText("L&ow-pass start freq. (Hz):");
        lc->setObjectName("dxParam3");
        QLineEdit* lpBox = putEdit(pl, 283, 172, 35, 12);
        SMap lm;
        lm.log = true;
        lm.dec = 0;
        DxSlider* lpSlider = putTrack(pl, 4, lm, 118, 172, 160, lpBox, nullptr, nullptr);
        QObject::connect(lc, &QCheckBox::toggled, lc, [this](bool on) { commit(3, on ? 1 : 0); });
        sync_.push_back([=] {
            QSignalBlocker b1(lc);
            const bool on = get(3) >= 0.5;
            lc->setChecked(on);
            lpSlider->setEnabled(on);
            lpBox->setEnabled(on);
        });
        // tempo sync with the period: the multiplier with its spinner, and the note length
        put(pl, checkBox(10, "Tempo &sync"), 5, 188, 54, 10);
        QLabel* perCap = putLabel(pl, "Modulation <u>p</u>eriod", 5, 202, 105, 8);
        QDoubleSpinBox* mult = new QDoubleSpinBox(this);
        mult->setObjectName("dxParam9");
        mult->setDecimals(3);
        mult->setRange(0.001, 32.0);
        mult->setSingleStep(1.0);       // the plug-in's spinner steps by one
        mult->setKeyboardTracking(false);
        put(pl, mult, 118, 200, 50, 12);
        perCap->setBuddy(mult);
        QObject::connect(mult, QOverload<double>::of(&QDoubleSpinBox::valueChanged), mult, [this](double v) {
            if (!updating_) commit(9, v);
        });
        QStringList notes = {"64th Note Triplet", "64th Note", "Dotted 64th Note", "32nd Note Triplet", "32nd Note", "Dotted 32nd Note", "16th Note Triplet", "16th Note",
                             "Dotted 16th Note", "8th Note Triplet", "8th Note", "Dotted 8th Note", "Quarter Note Triplet", "Quarter Note", "Dotted Quarter Note",
                             "Half Note Triplet", "Half Note", "Dotted Half Note", "Measures"};
        QComboBox* per = put(pl, combo(fixed(8), notes, "dxParam8"), 171, 200, 147, 14);
        sync_.push_back([=] {
            QSignalBlocker b(mult);
            if (!mult->hasFocus()) mult->setValue(get(9));
            const bool sync = get(10) >= 0.5;
            freqCap->setEnabled(!sync);
            freqBox->setEnabled(!sync);
            freqS->setEnabled(!sync);
            perCap->setEnabled(sync);
            mult->setEnabled(sync);
            per->setEnabled(sync);
        });
    }

    // ============================================================================================ Flange/Wah-wah
    // Dialog 1001: Effect (Flange / Phaser / Wah-Wah), the two gain faders (Dry out, Wet out), then Rate (box and trackbar),
    // Depth, Center frequency (box and trackbar), Resonance, "Tempo sync" with the period (multiplier spinner and note length).
    // Center frequency and Resonance do not work for the Flange (they are turned off then).
    void flangeWah()
    {
        const Placed pl = placedPage(242, 197);
        putLabel(pl, "E<u>f</u>fect:", 9, 19, 20, 8);
        {
            QButtonGroup* g = new QButtonGroup(pl.box);
            g->setExclusive(true);
            const char* const names[3] = {"Flange", "Phaser", "Wah-Wah"};
            const int xs[3] = {52, 119, 186}, ws[3] = {37, 38, 49};
            for (int k = 0; k < 3; ++k) {
                QRadioButton* r = new QRadioButton(names[k], this);
                r->setObjectName(QString("dxParam0_%1").arg(k));
                g->addButton(r, k);
                put(pl, r, xs[k], 18, ws[k] + 14, 10);
                QObject::connect(r, &QRadioButton::toggled, r, [=](bool on) {
                    if (on) commit(0, k);
                });
            }
            sync_.push_back([=] {
                const int k = (int)std::lround(std::max(0.0, std::min(2.0, get(0))));
                if (QAbstractButton* b = g->button(k)) {
                    QSignalBlocker blocker(b);
                    b->setChecked(true);
                }
            });
        }
        {
            QFrame* line = new QFrame(this);
            line->setFrameShape(QFrame::HLine);
            line->setFrameShadow(QFrame::Sunken);
            put(pl, line, 5, 29, 232, 2);
        }
        putFader(pl, 6, 22, 44, 131, 5, 49, "<u>D</u>ry out<br>(-Inf. to 12 dB)", 12, 30);
        putFader(pl, 5, 75, 44, 131, 55, 49, "<u>W</u>et out<br>(-Inf. to 12 dB)", 65, 30);

        QLabel* rateCap = putLabel(pl, "Ra<u>t</u>e (0.001 to 20.0 Hz):", 107, 37, 85, 8);
        QLineEdit* rateBox = putEdit(pl, 197, 35, 40, 12);
        SMap rm;
        rm.log = true;
        rm.dec = 3;
        putTrack(pl, 3, rm, 107, 51, 130, rateBox, nullptr, rateCap);

        SMap pm;
        pm.dec = 0;
        pm.noGap = true;
        QLabel* depthCap = putLabel(pl, "D<u>e</u>pth (1 to 100 %):", 107, 67, 66, 8);
        QLabel* depthVal = putLabel(pl, QString(), 211, 67, 26, 8, Qt::AlignRight | Qt::AlignVCenter);
        depthVal->setObjectName("dxParam4Value");
        putTrack(pl, 4, pm, 107, 79, 130, nullptr, depthVal, depthCap, "%");

        QLabel* centerCap = putLabel(pl, "<u>C</u>enter frequency (Hz):", 107, 97, 76, 8);
        QLineEdit* centerBox = putEdit(pl, 197, 95, 40, 12);
        SMap cm;
        cm.log = true;
        cm.dec = 0;
        DxSlider* centerS = putTrack(pl, 1, cm, 107, 111, 130, centerBox, nullptr, centerCap);

        QLabel* resCap = putLabel(pl, "Res<u>o</u>nance (1 to 100 %):", 107, 127, 82, 8);
        QLabel* resVal = putLabel(pl, QString(), 211, 127, 26, 8, Qt::AlignRight | Qt::AlignVCenter);
        resVal->setObjectName("dxParam2Value");
        DxSlider* resS = putTrack(pl, 2, pm, 107, 139, 130, nullptr, resVal, resCap, "%");

        put(pl, checkBox(9, "<u>T</u>empo sync"), 107, 157, 54, 10);
        QLabel* perCap = putLabel(pl, "<u>P</u>eriod", 107, 169, 85, 8);
        QDoubleSpinBox* mult = new QDoubleSpinBox(this);
        mult->setObjectName("dxParam8");
        mult->setDecimals(3);
        mult->setRange(0.001, 32.0);
        mult->setSingleStep(1.0);       // the plug-in's spinner steps by one
        mult->setKeyboardTracking(false);
        put(pl, mult, 107, 179, 50, 12);
        perCap->setBuddy(mult);
        QObject::connect(mult, QOverload<double>::of(&QDoubleSpinBox::valueChanged), mult, [this](double v) {
            if (!updating_) commit(8, v);
        });
        QStringList notes = {"64th Note Triplet", "64th Note", "Dotted 64th Note", "32nd Note Triplet", "32nd Note", "Dotted 32nd Note", "16th Note Triplet", "16th Note",
                             "Dotted 16th Note", "8th Note Triplet", "8th Note", "Dotted 8th Note", "Quarter Note Triplet", "Quarter Note", "Dotted Quarter Note",
                             "Half Note Triplet", "Half Note", "Dotted Half Note", "Measures"};
        QComboBox* per = put(pl, combo(fixed(7), notes, "dxParam7"), 160, 179, 77, 14);
        sync_.push_back([=] {
            QSignalBlocker b(mult);
            if (!mult->hasFocus()) mult->setValue(get(8));
            const bool sync = get(9) >= 0.5;
            perCap->setEnabled(sync);
            mult->setEnabled(sync);
            per->setEnabled(sync);
            // the flange has no center frequency or resonance
            const bool flange = (int)std::lround(get(0)) == 0;
            for (QWidget* w : std::initializer_list<QWidget*>{centerCap, centerBox, centerS, resCap, resVal, resS}) w->setEnabled(!flange);
        });
    }
