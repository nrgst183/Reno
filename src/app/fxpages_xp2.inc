// The pages of the reproductions of VEGAS's ExpressFX effects from sfxpfx2_x64.dll: Delay, Chorus, Equalization and
// Amplitude Modulation (included inside class Page in fxpages.cpp). Each copies the dialog of the plug-in's own property
// page (its controls, texts and places in dialog units) and its rules: the boxes take a number and keep the tenths
// below it (the plug-in cuts, it does not round), the fader sets the mix of the effect and the original.

public:
    static bool xp2Has(const std::string& uid) { return uid == "reno.xpdelay" || uid == "reno.xpchorus" || uid == "reno.xpeq"; }

private:
    bool xp2Page(const std::string& uid, int* width)
    {
        if (uid == "reno.xpdelay") {
            xpDelay();
            *width = 410;
        } else if (uid == "reno.xpchorus") {
            xpChorus();
            *width = 410;
        } else if (uid == "reno.xpeq") {
            xpEq();
            *width = 450;
        } else {
            return false;
        }
        return true;
    }

    // ---- placing controls in dialog units (as the multi-tap page does)
    static constexpr double kXpX = 1.75, kXpY = 1.875;
    QWidget* xpBox(int dlgW, int dlgH)
    {
        QWidget* box = new QWidget(this);
        box->setFixedSize((int)std::lround(dlgW * kXpX), (int)std::lround((dlgH - 14) * kXpY));
        root_->addWidget(box);
        return box;
    }
    template <class W>
    W* xpPut(QWidget* box, W* w, int x, int y, int cw, int ch)
    {
        w->setParent(box);
        w->setGeometry((int)std::lround(x * kXpX), (int)std::lround((y - 14) * kXpY), (int)std::lround(cw * kXpX), (int)std::lround(ch * kXpY));
        w->show();
        return w;
    }
    QLabel* xpLabel(QWidget* box, const QString& t, int x, int y, int cw, int ch, Qt::Alignment a = Qt::AlignLeft | Qt::AlignVCenter)
    {
        return xpPut(box, text(t, a), x, y, cw, ch);
    }
    // a slider (a trackbar of the plug-in, or its fader) that sets setting i in steps of 1 / scale
    DxSlider* xpSlider(QWidget* box, Qt::Orientation o, int i, double scale, const QString& name, int x, int y, int cw, int ch)
    {
        DxSlider* s = new DxSlider(o, this);
        s->setObjectName(name);
        const Rng r = rng(i);
        s->setRange((int)std::lround(r.lo * scale), (int)std::lround(r.hi * scale));
        QObject::connect(s, &QSlider::valueChanged, s, [=](int pos) {
            if (updating_) return;
            commit(i, (double)pos / scale);
        });
        QObject::connect(s, &QSlider::sliderPressed, s, [=] { beginDrag(i); });
        QObject::connect(s, &QSlider::sliderReleased, s, [=] { endDrag(); });
        sync_.push_back([=] {
            QSignalBlocker b(s);
            if (!s->isSliderDown()) s->setValue((int)std::lround(get(i) * scale));
        });
        return xpPut(box, s, x, y, cw, ch);
    }
    // an edit box for setting i, shown with 'dec' digits; what is typed is cut to the step 1 / scale, as the plug-in does
    QLineEdit* xpEdit(QWidget* box, int i, double scale, int dec, const QString& name, int x, int y, int cw, int ch)
    {
        QLineEdit* e = new QLineEdit(this);
        e->setObjectName(name);
        QObject::connect(e, &QLineEdit::editingFinished, e, [=] {
            if (updating_) return;
            double v = 0;
            if (!parseNumber(e->text(), &v)) { syncAll(); return; }
            v = std::trunc(v * scale) / scale;
            if (std::fabs(v - get(i)) > 0.5 / scale) commit(i, v);
            else syncAll();
        });
        sync_.push_back([=] {
            if (!e->hasFocus()) e->setText(QString::number(get(i), 'f', dec));
        });
        return xpPut(box, e, x, y, cw, ch);
    }
    // "12.3 %": a value of the fader as the plug-in's readouts show it
    static QString xpPercent(double v) { return QString::number(v, 'f', 1) + " %"; }

    // ============================================================================================ ExpressFX Delay
    // VEGAS's ExpressFX Delay page (dialog 1001, 230 x 133): the delay (0.0 to 1000.0 ms) and decay (0.0 to 90.0 %) with a
    // trackbar and a box each, and the fader that mixes the delayed sound ("Delay") against the "Original" with its two
    // readouts.
    void xpDelay()
    {
        QWidget* box = xpBox(230, 133);
        xpLabel(box, "D<u>e</u>lay (0.0 to 1000.0 ms):", 5, 28, 140, 8);
        xpSlider(box, Qt::Horizontal, 0, 10, "dxParam0", 5, 38, 150, 12);
        xpEdit(box, 0, 10, 1, "dxParam0Edit", 158, 38, 30, 12);
        xpLabel(box, "De<u>c</u>ay (0.0 to 90.0 %):", 5, 54, 140, 8);
        xpSlider(box, Qt::Horizontal, 1, 10, "dxParam1", 5, 64, 150, 12);
        xpEdit(box, 1, 10, 1, "dxParam1Edit", 158, 64, 30, 12);
        xpLabel(box, "Delay", 190, 17, 36, 8, Qt::AlignHCenter | Qt::AlignVCenter);
        QLabel* top = xpLabel(box, "", 190, 25, 36, 8, Qt::AlignHCenter | Qt::AlignVCenter);
        xpSlider(box, Qt::Vertical, 2, 10, "dxParam2", 202, 33, 10, 79);
        QLabel* bottom = xpLabel(box, "", 190, 112, 36, 8, Qt::AlignHCenter | Qt::AlignVCenter);
        xpLabel(box, "<u>O</u>riginal", 190, 120, 36, 8, Qt::AlignHCenter | Qt::AlignVCenter);
        sync_.push_back([=] {
            top->setText(xpPercent(get(2)));
            bottom->setText(xpPercent(100.0 - get(2)));
        });
    }

    // ============================================================================================ ExpressFX Chorus
    // VEGAS's ExpressFX Chorus page (dialog 3001, 230 x 132): the size as three radio buttons (1, 2 or 3 voices), the rate
    // (0.001 to 20.000 Hz) and depth (1 to 100 %) with a trackbar and a box each, and the fader that mixes the chorus against
    // the "Original" with its two readouts.
    void xpChorus()
    {
        QWidget* box = xpBox(230, 132);
        xpLabel(box, "Si<u>z</u>e:", 5, 20, 18, 8);
        QButtonGroup* group = new QButtonGroup(box);
        group->setExclusive(true);
        for (int k = 0; k < 3; ++k) {
            QRadioButton* r = new QRadioButton(QString::number(k + 1), this);
            r->setObjectName(QString("dxParam0_%1").arg(k));
            group->addButton(r, k);
            xpPut(box, r, 39 + 30 * k, 20, 20, 10);
            QObject::connect(r, &QRadioButton::toggled, r, [=](bool on) {
                if (on && !updating_) commit(0, k + 1);
            });
        }
        sync_.push_back([=] {
            const int k = (int)std::lround(std::max(1.0, std::min(3.0, get(0)))) - 1;
            if (QAbstractButton* b = group->button(k)) {
                QSignalBlocker blocker(b);
                b->setChecked(true);
            }
        });
        xpLabel(box, "Ra<u>t</u>e (0.001 to 20.000 Hz):", 5, 37, 140, 8);
        xpSlider(box, Qt::Horizontal, 1, 1000, "dxParam1", 5, 47, 135, 12);
        xpEdit(box, 1, 1000, 3, "dxParam1Edit", 145, 47, 30, 12);
        xpLabel(box, "<u>D</u>epth (1 to 100 %):", 5, 63, 140, 8);
        xpSlider(box, Qt::Horizontal, 2, 1, "dxParam2", 5, 73, 135, 12);
        xpEdit(box, 2, 1, 0, "dxParam2Edit", 145, 73, 30, 12);
        xpLabel(box, "Chorus", 187, 16, 36, 8, Qt::AlignHCenter | Qt::AlignVCenter);
        QLabel* top = xpLabel(box, "", 187, 24, 36, 8, Qt::AlignHCenter | Qt::AlignVCenter);
        xpSlider(box, Qt::Vertical, 3, 10, "dxParam3", 199, 32, 10, 79);
        QLabel* bottom = xpLabel(box, "", 187, 111, 36, 8, Qt::AlignHCenter | Qt::AlignVCenter);
        xpLabel(box, "<u>O</u>riginal", 187, 119, 36, 8, Qt::AlignHCenter | Qt::AlignVCenter);
        sync_.push_back([=] {
            top->setText(xpPercent(get(3)));
            bottom->setText(xpPercent(100.0 - get(3)));
        });
    }

    // ============================================================================================ ExpressFX Equalization
    // VEGAS's ExpressFX Equalization page (dialog 2001, 255 x 133): three faders for the bass, the middle and the treble
    // (-25.0 to 25.0 dB, each with its reading above and its name below) and the fader that mixes the equalized sound
    // ("EQ") against the "Original" with its two readouts.
    static QString xpDb(double v) { return QString::number(v, 'f', 1) + " dB"; }
    void xpEq()
    {
        QWidget* box = xpBox(255, 133);
        struct Band { const char* name; int x; int param; };
        const Band bands[3] = {{"<u>B</u>ass", 46, 0}, {"<u>M</u>id", 86, 1}, {"<u>T</u>reble", 126, 2}};
        for (const Band& b : bands) {
            QLabel* reading = xpLabel(box, "", b.x, 25, 36, 8, Qt::AlignHCenter | Qt::AlignVCenter);
            xpSlider(box, Qt::Vertical, b.param, 10, QString("dxParam%1").arg(b.param), b.x + 13, 33, 10, 79);
            xpLabel(box, b.name, b.x, 112, 36, 8, Qt::AlignHCenter | Qt::AlignVCenter);
            const int param = b.param;
            sync_.push_back([=] { reading->setText(xpDb(get(param))); });
        }
        xpLabel(box, "EQ", 166, 17, 36, 8, Qt::AlignHCenter | Qt::AlignVCenter);
        QLabel* top = xpLabel(box, "", 166, 25, 36, 8, Qt::AlignHCenter | Qt::AlignVCenter);
        xpSlider(box, Qt::Vertical, 3, 10, "dxParam3", 178, 33, 10, 79);
        QLabel* bottom = xpLabel(box, "", 166, 112, 36, 8, Qt::AlignHCenter | Qt::AlignVCenter);
        xpLabel(box, "<u>O</u>riginal", 166, 120, 36, 8, Qt::AlignHCenter | Qt::AlignVCenter);
        sync_.push_back([=] {
            top->setText(xpPercent(get(3)));
            bottom->setText(xpPercent(100.0 - get(3)));
        });
    }
