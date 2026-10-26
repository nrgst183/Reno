// Commands from the first part of VEGAS Pro 18's manual (pages 11-113: the window, working with projects, adding
// media) that Reno lacked, put into the main window by installManualExtras: File > Import > Media from Project / EDL
// Text File / Broadcast Wave Format; File > Export > VEGAS Project Archive / EDL Text File; Insert > Insert
// Subtitles; Tools > Advanced Save and Generate Loudness Log; Options > Metronome, Loudness Meters, Customize
// Toolbar; Help items and the capture, CD and online items VEGAS has (greyed, with the reason); the "match project
// settings to the first video?" question; and what some Preferences > General items do while Reno runs.
#pragma once
#include "appstate.h"
#include <QString>
#include <string>
#include <vector>

class QMainWindow;
class QMenu;
class QWidget;

#include <functional>
#include <QString>
class QWidget;

namespace reno {
namespace app {

class MainWindow;

void installManualExtras(MainWindow* w);

// ---- the pieces, also for tests
// Makes the project's video settings the media's: frame size, frame rate, field order (one undo step 'what').
void matchProjectVideoToMedia(uint64_t mediaId, const std::string& what = "Match Project Video Settings");

// VEGAS's dialog for a file that can't be found when a project opens ("The following file could not be found in the
// specified
// location" with Search for missing file / Specify a new location or replacement file / Ignore ...). One dialog per
// offline media.
int resolveMissingMedia(QWidget* parent, const std::function<int(const QString& path, QString* file)>& choose = nullptr);

// VEGAS's EDL text file (File > Export / Import > EDL Text File): one line per event, semicolon-separated, times in
// milliseconds, as VEGAS writes it. Import builds a new project: events on one video and up to four audio tracks.
bool writeEdl(const Project& p, const QString& file, QString* error);
bool readEdl(const QString& file, Project& out, QString* error);

struct ArchiveOptions {
    bool nested = true, media = true, proxies = false, excludeUnused = true;
};
bool archiveProject(const QString& projectFile, const ArchiveOptions& o, QString* error, int* copied = nullptr);

// File > Import > Media from Project: adds the media of another project (not generated, not offline) to this one,
// with its bins when 'bins' (in a bin named after the project unless 'merge'). Returns how many were added.
int importMediaFromProject(const QString& file, bool bins, bool merge, QString* error);

bool bwfTimeReference(const QString& file, double* seconds);
// File > Import > Broadcast Wave Format. arrange: 0 across tracks, 1 across time; order: 0 by time stamp, 1 by name;
// relativeToCursor: offset by the cursor (else at the ruler time the file gives). Returns the events made.
int importBroadcastWave(const QStringList& files, int arrange, int order, bool relativeToCursor, QString* error);

// Subtitles: lines of text and their times (start, length; length 0: default 5 s, one after another from the
// project start). From a .txt (one subtitle per line, or tab-separated start, end, length, text) or .csv file.
struct Subtitle {
    double start = -1, length = 0;
    QString text;
};
std::vector<Subtitle> readSubtitleFile(const QString& file, double fps, QString* error);
int insertSubtitles(const std::vector<Subtitle>& subs, const QString& preset);

QString backupFolder(const QString& kind);
// Writes a backup copy of the project now (of that kind), keeping no more than the kind allows. False if nothing was
// written (no changes, no project).
bool writeBackup(const QString& kind);
QStringList backupFiles(bool currentProjectOnly);

}  // namespace app
}  // namespace reno
