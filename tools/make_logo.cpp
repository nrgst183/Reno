// Writes Reno's logo (src/app/logo.cpp) as PNG files and as resources/reno.ico (PNG-compressed entries).
//   make_logo <output folder>      (run with QT_QPA_PLATFORM=offscreen)
#include "../src/app/logo.h"
#include <QBuffer>
#include <QFile>
#include <QGuiApplication>
#include <vector>

using namespace reno::app;

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    const QString out = argc > 1 ? QString(argv[1]) : QString(".");
    const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 128, 256};
    std::vector<QByteArray> pngs;
    for (int s : sizes) {
        QImage img = renoLogo(s);
        img.save(out + QString("/reno_%1.png").arg(s));
        QByteArray b;
        QBuffer buf(&b);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
        pngs.push_back(b);
    }
    // ICONDIR, ICONDIRENTRY per image, then the PNGs
    QByteArray ico;
    auto u16 = [&](int v) { ico.append(char(v & 0xff)); ico.append(char((v >> 8) & 0xff)); };
    auto u32 = [&](quint32 v) { for (int k = 0; k < 4; ++k) ico.append(char((v >> (8 * k)) & 0xff)); };
    const int n = (int)pngs.size();
    u16(0); u16(1); u16(n);
    quint32 offset = 6 + 16 * n;
    for (int k = 0; k < n; ++k) {
        const int s = sizes[k];
        ico.append(char(s >= 256 ? 0 : s));
        ico.append(char(s >= 256 ? 0 : s));
        ico.append(char(0)); ico.append(char(0));
        u16(1); u16(32);
        u32((quint32)pngs[k].size());
        u32(offset);
        offset += (quint32)pngs[k].size();
    }
    for (const auto& b : pngs) ico.append(b);
    QFile f(out + "/reno.ico");
    if (!f.open(QIODevice::WriteOnly)) return 1;
    f.write(ico);
    return 0;
}
