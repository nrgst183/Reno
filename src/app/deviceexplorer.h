// VEGAS's Device Explorer (View > Window > Device Explorer, Ctrl+Alt+6): removable drives and camera cards, and the clips on them
// (found where cameras keep them: DCIM, AVCHD, Sony XDCAM and GoPro folders, or anywhere on a drive not laid out like a camera's).
// Import copies the chosen clips (default: all) into "Imported Media" beside the project, or another folder, and adds them to
// Project Media; clips imported before are marked and not copied twice.
#pragma once
#include "appstate.h"
#include <QStringList>
#include <QWidget>
#include <memory>

namespace reno {
namespace app {

class DeviceExplorer : public QWidget {
public:
    explicit DeviceExplorer(QWidget* parent = nullptr);
    ~DeviceExplorer() override;

    struct Device {
        QString root, name;
        qint64 bytesTotal = 0, bytesFree = 0;
        QString kind;                       // "Camera card (AVCHD)", "Removable drive"...
    };
    std::vector<Device> devices() const;
    void refresh();                         // looks for devices again (also every few seconds while shown)
    void selectDevice(const QString& root);
    QStringList clips() const;              // media files on the device shown
    // Copies these files (empty: every clip) into 'folder' (empty: default) and adds them to Project Media. Returns the
    // media added or already there.
    std::vector<uint64_t> importClips(const QStringList& files = QStringList(), const QString& folder = QString());
    QString importFolder() const;

    // For tests: folders treated as connected devices instead of the real ones.
    void setDeviceRoots(const QStringList& roots);

    // Media files a device holds, where cameras keep them or anywhere on it. Static so tests can use it.
    static QStringList findClips(const QString& root, QString* kind = nullptr);

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace app
}  // namespace reno
