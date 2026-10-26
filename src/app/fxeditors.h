// Editors the effects window draws for VEGAS effects whose settings need a control of their own: curve graphs (Color Curves,
// Color Grading), colour wheels, LUT pickers, picture pickers (Color Match) and the LUT Filter file picker. They serve both VEGAS's
// plug-in and Reno's built-in one, knowing settings by name and text format.
// An editor stands for one row of the window: it reads settings through show() and changes them through the window's callbacks;
// the window keeps the undo steps and keyframes (a drag is begin() ... end()).
#pragma once
#include <QImage>
#include <QString>
#include <QWidget>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace reno {
namespace app {

// One setting as an editor sees it.
struct EdValue {
    bool present = false;                   // the effect has the setting
    std::string text;                       // String / Custom
    std::vector<double> v;                  // numbers
    double lo = 0, hi = 1;                  // range the setting allows (first number)
    bool enabled = true;
};
typedef std::map<std::string, EdValue> EdValues;

class FxEditor : public QWidget {
public:
    explicit FxEditor(QWidget* parent) : QWidget(parent) {}
    // Settings at the cursor.
    virtual void showValues(const EdValues& values) = 0;

    // What the window gives it:
    std::function<void()> begin;            // a change that goes on (a drag) starts: one undo step until end()
    std::function<void()> end;
    std::function<void(const std::string& name, const std::string& text)> setText;
    std::function<void(const std::string& name, const std::vector<double>& v)> setNumbers;
    std::function<QImage()> previewFrame;   // picture in the preview now (may be empty)
    std::function<void(const std::string& message)> tell;       // message for the user (e.g. a picture could not be read)
};

// An editor the window makes for a setting.
struct EditorSpec {
    std::string plugin;                     // effect identifier
    std::string anchor;                     // setting whose row it replaces
    std::string kind;                       // "colorcurves", "gradingcurves", "wheel", "lutslot", "colormatch"
    std::string label;                      // row label (empty: the setting's own)
    std::vector<std::string> uses;          // every setting it shows or changes (anchor first); their own rows are left out
    std::vector<std::string> keyed;         // settings its keyframe button sets and removes keyframes on, and reset restores
};

// The editor for setting 'param' of effect 'plugin' (null if it is not an anchor) ...
const EditorSpec* findEditorSpec(const std::string& plugin, const std::string& param);
// ... and whether the setting belongs to one without being its anchor (its row is left out).
bool isEditorMember(const std::string& plugin, const std::string& param);
// All the editors of an effect.
std::vector<const EditorSpec*> editorSpecsFor(const std::string& plugin);
FxEditor* makeEditor(const EditorSpec& spec, QWidget* parent);

// How the editors and the window's Browse button ask for a file: the file dialog unless a test replaces it. Returns the
// path ("" if none).
typedef std::function<QString(QWidget* parent, const QString& title, const QString& startAt, const QString& filter)> FileChooser;
FileChooser& fileChooser();

// A text setting VEGAS lets the user pick a file for (LUT Filter's LUTFilename): the file dialog filter, or null.
const char* filePickFilter(const std::string& plugin, const std::string& param);

// Color Match: a picture's statistics as the effect keeps them ("internaldata"). Returns six numbers: mean L, std L,
// mean a, std a, mean b, std b.
bool colorMatchStatistics(const QImage& image, double out[6]);

}  // namespace app
}  // namespace reno
