// The pages of the ExpressFX effects of VEGAS's sfxpfx1_x64.dll (Distortion, Flange/Wah-Wah, Reverb, Stutter), included
// inside class Page in fxpages.cpp. Each one copies the plug-in's dialog: positions in dialog units (xp*Du), one unit
// being 1.75 by 1.875 pixels here, with the logo band at the top left out.

public:
    static bool xp1Has(const std::string& uid)
    {
        return uid == "reno.xpdistortion" || uid == "reno.xpstutter" || uid == "reno.xpflange" || uid == "reno.xpreverb";
    }

private:
    bool xp1Page(const std::string& uid, int* width)
    {
        if (uid == "reno.xpdistortion") {
            xpDistortion();
            *width = 404;
        } else if (uid == "reno.xpflange") {
            xpFlange();
            *width = 466;
        } else if (uid == "reno.xpreverb") {
            xpReverb();
            *width = 404;
        } else if (uid == "reno.xpstutter") {
            xpStutter();
            *width = 404;
        } else {
            return false;
        }
        return true;
    }

    // ---- the dialog-unit layout shared by the four pages
    QWidget* xpBox(int wDu, int hDu)
    {
        QWidget* box = new QWidget(this);
        box->setFixedSize((int)std::lround(wDu * 1.75), (int)std::lround((hDu - 14) * 1.875));
        root_->addWidget(box);
        return box;
    }
    template <class W> W* xpPut(QWidget* box, W* w, int x, int y, int cw, int ch)
    {
        w->setParent(box);
        w->setGeometry((int)std::lround(x * 1.75), (int)std::lround((y - 14) * 1.875), (int)std::lround(cw * 1.75), (int)std::lround(ch * 1.875));
        w->show();
        return w;
    }
    QLabel* xpLabel(QWidget* box, const QString& t, int x, int y, int cw, int ch, Qt::Alignment a = Qt::AlignLeft | Qt::AlignVCenter)
    {
        return xpPut(box, text(t, a), x, y, cw, ch);
    }
    // A caption "(Name) (a to b unit):" over a horizontal trackbar with its edit box at the right (the dialog's pair):
    // the label at (x, y), the trackbar 10 below it, the box beside the trackbar.
    void xpTrack(QWidget* box, int i, const QString& caption, int x, int y, int trackW, int boxX, int boxW, int dec, const QString& name, int stepCount = 0)
    {
        QLabel* l = xpLabel(box, caption, x, y, 100, 8);
        SMap m;
        m.dec = dec;
        m.stepCount = stepCount;
        m.comma = false;
        QLineEdit* e = editBox(52);
        DxSlider* s = slider(Qt::Horizontal, fixed(i), m, name, e, nullptr);
        xpPut(box, s, x, y + 10, trackW, 12);
        xpPut(box, e, boxX, y + 10, boxW, 12);
        l->setBuddy(s);
    }
    // The mix fader at the right of a page: the effect's level above, the original's below (percent each, as the
    // plug-in's own readouts), the fader between. 'i' is the mix setting in percent of the effect.
    QLabel* xpMix(QWidget* box, int i, int x, const QString& effectName)
    {
        QLabel* name = xpLabel(box, effectName, x, 17, 36, 8, Qt::AlignHCenter | Qt::AlignVCenter);
        QLabel* top = xpLabel(box, QString(), x, 25, 36, 8, Qt::AlignHCenter | Qt::AlignVCenter);
        QLabel* bottom = xpLabel(box, QString(), x, 112, 36, 8, Qt::AlignHCenter | Qt::AlignVCenter);
        QLabel* orig = xpLabel(box, "<u>O</u>riginal", x, 120, 36, 8, Qt::AlignHCenter | Qt::AlignVCenter);
        SMap m;
        m.dec = 1;
        DxSlider* s = slider(Qt::Vertical, fixed(i), m, QString("dxParam%1").arg(i), nullptr, nullptr);
        xpPut(box, s, x + 12, 33, 12, 79);
        orig->setBuddy(s);
        sync_.push_back([=] {
            const double v = get(i);
            top->setText(QString::number(v, 'f', 1) + " %");
            bottom->setText(QString::number(100.0 - v, 'f', 1) + " %");
        });
        return name;
    }

    // ============================================================================================ ExpressFX Distortion
    // Dialog 3001: Distortion and Slew trackbars with their boxes, "Enable hard clipping", the mix fader.
    void xpDistortion()
    {
        QWidget* box = xpBox(230, 133);
        xpTrack(box, 1, "<u>D</u>istortion (0.0 to 100.0 %):", 5, 28, 150, 157, 30, 1, "dxParam1");
        xpTrack(box, 0, "S<u>l</u>ew (0.0 to 100.0 %):", 5, 54, 150, 157, 30, 1, "dxParam0");
        QCheckBox* hard = checkBox(2, "&Enable hard clipping");
        xpPut(box, hard, 5, 88, 110, 10);
        xpMix(box, 3, 190, "Distortion");
    }

    // ============================================================================================ ExpressFX Stutter
    // Dialog 4001: Silence and Sound length trackbars with their boxes, "Add space", the mix fader.
    void xpStutter()
    {
        QWidget* box = xpBox(230, 133);
        xpTrack(box, 0, "Si<u>l</u>ence (0.01 to 200.00 ms):", 5, 28, 150, 161, 30, 2, "dxParam0", 19999);
        xpTrack(box, 1, "Soun<u>d</u> (0.01 to 200.00 ms):", 5, 54, 150, 161, 30, 2, "dxParam1", 19999);
        QCheckBox* space = checkBox(2, "Add spa&ce");
        xpPut(box, space, 5, 87, 90, 10);
        xpMix(box, 3, 191, "Stutter");
    }

    // ============================================================================================ ExpressFX Flange/Wah-Wah
    // Dialog 1001: the effect (two radio buttons), Rate and Depth trackbars with their boxes, the mix fader whose top
    // caption names the effect.
    void xpFlange()
    {
        QWidget* box = xpBox(265, 133);
        QLabel* el = xpLabel(box, "E<u>f</u>fect:", 5, 22, 28, 8);
        QRadioButton* fl = new QRadioButton("&Flange", this);
        QRadioButton* wah = new QRadioButton("&Wah-Wah", this);
        fl->setObjectName("dxParam0_0");
        wah->setObjectName("dxParam0_1");
        xpPut(box, fl, 39, 21, 42, 10);
        xpPut(box, wah, 81, 21, 52, 10);
        el->setBuddy(fl);
        QObject::connect(fl, &QRadioButton::toggled, fl, [=](bool on) {
            if (on) commit(0, 0);
        });
        QObject::connect(wah, &QRadioButton::toggled, wah, [=](bool on) {
            if (on) commit(0, 2);
        });
        xpTrack(box, 1, "Ra<u>t</u>e (0.001 to 20.000 Hz):", 5, 38, 170, 178, 40, 3, "dxParam1", 19999);
        xpTrack(box, 2, "<u>D</u>epth (1 to 100 %):", 5, 64, 170, 178, 40, 0, "dxParam2");
        QLabel* name = xpMix(box, 3, 224, "Wah-Wah");
        sync_.push_back([=] {
            const bool w = get(0) >= 0.5;
            QSignalBlocker b1(fl), b2(wah);
            fl->setChecked(!w);
            wah->setChecked(w);
            name->setText(w ? "Wah-Wah" : "Flange");
        });
    }

    // ============================================================================================ ExpressFX Reverb
    // Dialog 2001: the room type (a sorted drop-down list), Room size and Liveliness trackbars with their boxes, the mix
    // fader. Room size runs in half percents (the plug-in keeps 0..200), Liveliness in whole percents.
    void xpReverb()
    {
        QWidget* box = xpBox(230, 133);
        QLabel* tl = xpLabel(box, "Room <u>t</u>ype:", 5, 21, 47, 8);
        QComboBox* type = new QComboBox(this);
        type->setObjectName("dxParam0");
        const QStringList names = choicesOf("reno.xpreverb", 0);
        std::vector<int> order;
        for (int k = 0; k < names.size(); ++k) order.push_back(k);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return QString::compare(names[a], names[b], Qt::CaseInsensitive) < 0; });
        for (int k : order) type->addItem(names[k], k);
        xpPut(box, type, 57, 19, 130, 12);
        tl->setBuddy(type);
        QObject::connect(type, QOverload<int>::of(&QComboBox::currentIndexChanged), type, [=](int k) {
            if (k >= 0) commit(0, type->itemData(k).toInt());
        });
        sync_.push_back([=] {
            QSignalBlocker b(type);
            type->setCurrentIndex(std::max(0, type->findData((int)std::lround(get(0)))));
        });
        xpTrack(box, 1, "Room si<u>z</u>e (0.0 to 100.0 %):", 5, 42, 150, 157, 30, 1, "dxParam1", 200);
        xpTrack(box, 2, "<u>L</u>iveliness (0 to 100 %):", 5, 68, 150, 157, 30, 0, "dxParam2");
        xpMix(box, 3, 190, "Reverb");
    }
