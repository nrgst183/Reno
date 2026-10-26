// File > Render As: output folder and name, formats and templates (VEGAS's, mapped onto FFmpeg encoders),
// Customize Template, custom templates, Render Options, and a progress page.
#pragma once
#include <QString>
#include <QStringList>
#include <QWidget>

namespace reno {
namespace app {

// File > Render As. Returns when the dialog is closed.
void runRenderAsDialog(QWidget* parent);

// Tools > Render to New Track (Ctrl+M): the same dialog, rendering 'from'..'to' to 'suggested'.
// True when a file was rendered; *path is where.
bool runRenderToNewTrackDialog(QWidget* parent, double from, double to, const QString& suggested, QString* path);

QStringList renderTemplateNames();

}  // namespace app
}  // namespace reno
