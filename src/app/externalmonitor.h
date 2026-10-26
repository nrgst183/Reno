// VEGAS's External Monitor Preview (View > Preview on External Monitor, Alt+Shift+4): the preview image full screen on another
// display (View > External Monitor Display, else the one the main window is not on), fitted with black around it.
// Alt+Shift+4 again, Esc or a double-click closes it.
#pragma once
#include <QString>
#include <QWidget>
#include <memory>

namespace reno {
namespace app {

class Playback;

class ExternalMonitor : public QWidget {
public:
    explicit ExternalMonitor(Playback* playback, QWidget* mainWindow);
    ~ExternalMonitor() override;

    void toggle();
    void open();                        // full screen on the chosen display
    void close();
    bool isOpen() const;
    // Display by name (QScreen::name); empty: the one the main window is not on.
    void setScreenName(const QString& name);    // remembered
    QString screenName() const;
    QString screenUsed() const;         // the one it is on (or would go on)
    QImage shown() const;               // image shown now (for tests)

protected:
    void paintEvent(QPaintEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace app
}  // namespace reno
