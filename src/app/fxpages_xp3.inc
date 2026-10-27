// The pages of the ExpressFX effects of VEGAS's sfxpfx3_x64.dll (Dynamics, Graphic EQ, Noise Gate, Time Stretch), included
// inside class Page in fxpages.cpp. Each one copies the plug-in's dialog: positions in dialog units (one unit being 1.75 by
// 1.875 pixels here), with the logo band at the top (14 units) left out.

public:
    static bool xp3Has(const std::string& uid)
    {
        return uid == "reno.xpgeq" || uid == "reno.xpdynamics" || uid == "reno.xpnoisegate";
    }

private:
    bool xp3Page(const std::string& uid, int* width)
    {
        if (uid == "reno.xpgeq") {
            xpGeq();
            *width = 465;
        } else if (uid == "reno.xpdynamics") {
            xpDynamics();
            *width = 420;
        } else if (uid == "reno.xpnoisegate") {
            xpNoiseGate();
            *width = 420;
        } else {
            return false;
        }
        return true;
    }

    // ---- the dialog-unit layout shared by the pages
    QWidget* x3Box(int wDu, int hDu)
    {
        QWidget* box = new QWidget(this);
        box->setFixedSize((int)std::lround(wDu * 1.75), (int)std::lround((hDu - 14) * 1.875));
        root_->addWidget(box);
        return box;
    }
    template <class W> W* x3Put(QWidget* box, W* w, int x, int y, int cw, int ch)
    {
        w->setParent(box);
        w->setGeometry((int)std::lround(x * 1.75), (int)std::lround((y - 14) * 1.875), (int)std::lround(cw * 1.75), (int)std::lround(ch * 1.875));
        w->show();
        return w;
    }
    QLabel* x3Label(QWidget* box, const QString& t, int x, int y, int cw, int ch, Qt::Alignment a = Qt::AlignLeft | Qt::AlignVCenter)
    {
        return x3Put(box, text(t, a), x, y, cw, ch);
    }
    // A plain edit box for setting i: the value times 'shown' with 'dec' digits (grouped by thousands when 'comma'); a typed
    // number is taken back the same way.
    QLineEdit* x3Edit(QWidget* box, int i, int x, int y, int cw, int dec, bool comma)
    {
        QLineEdit* e = editBox((int)std::lround(cw * 1.75));
        e->setObjectName(QString("dxParam%1Edit").arg(i));
        x3Put(box, e, x, y, cw, 12);
        QObject::connect(e, &QLineEdit::editingFinished, e, [=] {
            if (updating_) return;
            double v = 0;
            if (!parseNumber(e->text(), &v)) { syncAll(); return; }
            if (std::fabs(v - get(i)) > 1e-9) commit(i, v);
            else syncAll();
        });
        sync_.push_back([=] {
            if (!e->hasFocus()) e->setText(comma ? groupedNumber(get(i), dec) : QString::number(get(i), 'f', dec));
        });
        return e;
    }
    // A caption over a horizontal trackbar with its edit box at the right (the dialog's pair): caption at (x, y), the
    // trackbar 10 units below it, the box beside it.
    DxSlider* x3Track(QWidget* box, int i, const QString& caption, int x, int y, int trackW, int boxX, int boxW, int dec, bool comma = false)
    {
        QLabel* l = x3Label(box, caption, x, y, 110, 8);
        SMap m;
        m.dec = dec;
        m.comma = comma;
        QLineEdit* e = x3Edit(box, i, boxX, y + 10, boxW, dec, comma);
        DxSlider* s = slider(Qt::Horizontal, fixed(i), m, QString("dxParam%1").arg(i), nullptr, nullptr);
        x3Put(box, s, x, y + 10, trackW, 12);
        l->setBuddy(s);
        return s;
    }

    // ============================================================================================ ExpressFX Dynamics
    // Dialog 1001: the Threshold fader at the left with its reading above and caption below, Ratio, Attack and Release
    // trackbars with their boxes, and "Auto gain compensate".
    void xpDynamics()
    {
        QWidget* box = x3Box(240, 125);
        QLabel* read = x3Label(box, QString(), 15, 17, 30, 8, Qt::AlignHCenter | Qt::AlignVCenter);
        SMap fm;
        fm.dec = 1;
        DxSlider* fader = slider(Qt::Vertical, fixed(0), fm, "dxParam0", nullptr, nullptr);
        x3Put(box, fader, 25, 25, 10, 79);
        QLabel* cap = x3Label(box, "<u>T</u>hreshold<br>(-80 to -0.1 dB)", 5, 104, 52, 16, Qt::AlignHCenter | Qt::AlignTop);
        cap->setBuddy(fader);
        sync_.push_back([=] { read->setText(QString::number(get(0), 'f', 1) + " dB"); });

        // Ratio: "N.N :1", or "Inf.:1" at the top of the range
        QLabel* rl = x3Label(box, "Rati<u>o</u> (1.0 :1 to Inf.:1):", 71, 20, 110, 8);
        SMap rm;
        rm.dec = 1;
        DxSlider* rs = slider(Qt::Horizontal, fixed(1), rm, "dxParam1", nullptr, nullptr);
        x3Put(box, rs, 71, 30, 126, 12);
        rl->setBuddy(rs);
        QLineEdit* re = editBox((int)std::lround(34 * 1.75));
        re->setObjectName("dxParam1Edit");
        x3Put(box, re, 201, 30, 34, 12);
        QObject::connect(re, &QLineEdit::editingFinished, re, [=] {
            if (updating_) return;
            const Rng r = rng(1);
            double v = 0;
            if (re->text().trimmed().startsWith("inf", Qt::CaseInsensitive)) v = r.hi;
            else if (!parseNumber(re->text(), &v)) { syncAll(); return; }
            if (std::fabs(v - get(1)) > 1e-9) commit(1, v);
            else syncAll();
        });
        sync_.push_back([=] {
            if (re->hasFocus()) return;
            const Rng r = rng(1);
            const double v = get(1);
            re->setText(v >= r.hi - 1e-9 ? QString("Inf.:1") : QString::number(v, 'f', 1) + " :1");
        });

        x3Track(box, 2, "Attac<u>k</u> (0.0 to 500.0 ms):", 71, 46, 126, 201, 34, 1, true);
        x3Track(box, 3, "Releas<u>e</u> (0.0 to 5,000.0 ms):", 71, 72, 126, 201, 34, 1, true);
        QCheckBox* auto_ = checkBox(4, "Auto gain &compensate");
        x3Put(box, auto_, 71, 100, 90, 10);
    }

    // ============================================================================================ ExpressFX Noise Gate
    // Dialog 3001: the Threshold fader at the left (reading above, caption below), Attack and Release trackbars with boxes.
    void xpNoiseGate()
    {
        QWidget* box = x3Box(240, 125);
        QLabel* read = x3Label(box, QString(), 15, 17, 30, 8, Qt::AlignHCenter | Qt::AlignVCenter);
        SMap fm;
        fm.dec = 1;
        fm.inf = true;
        DxSlider* fader = slider(Qt::Vertical, fixed(0), fm, "dxParam0", nullptr, nullptr);
        x3Put(box, fader, 25, 25, 10, 79);
        QLabel* cap = x3Label(box, "<u>T</u>hreshold<br>(-Inf. to 0 dB)", 5, 104, 52, 16, Qt::AlignHCenter | Qt::AlignTop);
        cap->setBuddy(fader);
        sync_.push_back([=] {
            const Rng r = rng(0);
            const double v = get(0);
            read->setText(v <= r.lo + 0.05 ? QString("-Inf.") : QString::number(v, 'f', 1) + " dB");
        });
        x3Track(box, 1, "Attac<u>k</u> (1 to 500 ms):", 55, 20, 145, 205, 30, 0, true);
        x3Track(box, 2, "Relea<u>s</u>e (1 to 5,000 ms):", 55, 46, 145, 205, 30, 0, true);
    }

    // ============================================================================================ ExpressFX Graphic EQ
    // Dialog 2001: ten band faders (reading above, centre frequency below), the Output gain fader at the right and Reset.
    void xpGeq()
    {
        QWidget* box = x3Box(265, 130);
        x3Label(box, "<u>G</u>ain:<br>(-Inf. to 24 dB)", 5, 17, 58, 16, Qt::AlignLeft | Qt::AlignTop);
        x3Label(box, "Center (Hz):", 5, 98, 46, 8);
        static const char* const centre[10] = {"28", "56", "113", "225", "450", "900", "1.8k", "3.6k", "7.2k", "15k"};
        for (int b = 0; b < 10; ++b) {
            const int x = 64 + 14 * b;
            QLabel* read = x3Label(box, QString(), x - 2, 19, 14, 8, Qt::AlignHCenter | Qt::AlignVCenter);
            SMap fm;
            fm.dec = 1;
            fm.inf = true;
            DxSlider* f = slider(Qt::Vertical, fixed(b), fm, QString("dxParam%1").arg(b), nullptr, nullptr);
            x3Put(box, f, x, 27, 10, 73);
            x3Label(box, centre[b], x - 2, 100, 14, 8, Qt::AlignHCenter | Qt::AlignVCenter);
            sync_.push_back([=] {
                const Rng r = rng(b);
                const double v = get(b);
                read->setText(v <= r.lo + 0.05 ? QString("-Inf.") : QString::number(v, 'f', 1));
            });
        }
        QLabel* ol = x3Label(box, "<u>O</u>utput gain<br>(-60 to 20 dB)", 212, 108, 48, 16, Qt::AlignHCenter | Qt::AlignTop);
        QLabel* oread = x3Label(box, QString(), 222, 17, 30, 8, Qt::AlignHCenter | Qt::AlignVCenter);
        SMap om;
        om.dec = 1;
        DxSlider* of = slider(Qt::Vertical, fixed(10), om, "dxParam10", nullptr, nullptr);
        x3Put(box, of, 232, 27, 10, 79);
        ol->setBuddy(of);
        sync_.push_back([=] { oread->setText(QString::number(get(10), 'f', 1) + " dB"); });
        QPushButton* reset = button("Rese&t");
        reset->setObjectName("dxReset");
        x3Put(box, reset, 162, 111, 40, 14);
        QObject::connect(reset, &QPushButton::clicked, reset, [this] {
            for (int b = 0; b < 10; ++b) commit(b, 0.0);
        });
    }
