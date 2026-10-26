// VEGAS's Plug-In Manager (View > Window > Plug-In Manager, Ctrl+Alt+1): every plug-in Reno found, in folders
// (Favorites, Video FX, Transitions, Media Generators, Audio FX), plus the ones that failed, with check result,
// format and file. Plug-ins can be dragged onto the timeline or double-clicked (onActivated). Favorites are saved.
#pragma once
#include "panels.h"
#include <QWidget>
#include <memory>

namespace reno {
namespace app {

class PluginManager : public QWidget {
public:
    explicit PluginManager(QWidget* parent = nullptr);
    ~PluginManager() override;

    std::function<void(FxBrowser::Kind kind, const std::string& data)> onActivated;

    QStringList favorites() const;
    void setFavorite(const QString& key, bool on);
    void setFilter(const QString& text);
    void refresh();

    int count(const QString& folderPath) const;
    QStringList keys(const QString& folderPath) const;
    QWidget* tree() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace app
}  // namespace reno
