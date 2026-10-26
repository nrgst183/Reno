// Small VEGAS dialogs: Command Properties with VEGAS's command templates.
#pragma once
#include "appstate.h"
#include <QString>
#include <string>

class QWidget;

namespace reno {
namespace app {

// VEGAS's Command Properties: command (URL, Text, WMClosedCaption... or typed), parameter, comment, position (ruler
// format) and a Template list
// (templates saved by name; Reno keeps them in its settings, VEGAS in cmdtemp.xml). 'title' is the window title. False on Cancel.
struct CommandProps {
    std::string command = "URL", parameter, comment;
    double position = 0;
};
bool commandPropertiesDialog(QWidget* parent, CommandProps& props, const QString& title = QString("Command Properties"));
// Command templates kept (name -> settings), for tests.
std::vector<std::pair<std::string, CommandProps>> commandTemplates();
void saveCommandTemplate(const std::string& name, const CommandProps& props);

}  // namespace app
}  // namespace reno
