// VEGAS's Trimmer (View > Window > Trimmer, Alt+2): one media file on its own, to look through and pick the part that
// goes on the timeline. A double-click on media in Project Media or the Explorer opens it here.
// 
// The window has the Trimmer history (newest first) and the Add buttons along the top; the image; a strip with the
// file's whole length, waveform and selection (drag across it to select, drag the selection onto the timeline to put
// that part there, click to move the cursor); and a transport bar of its own.
// 
// Keys while it has focus, as in VEGAS: Space plays / stops, I / O set in and out points at the cursor, A (or Enter)
// puts the selection (else the whole file) on the timeline at the timeline's cursor, Shift+A so it ends at the
// cursor, Left / Right step a frame, Home / End go to the start / end, H sorts the history, Ctrl+H removes the file
// from it, Ctrl+Shift+H clears it, T / Shift+T select as long a stretch as the timeline's time selection, Tab chooses
// what is added (image and sound, image only, sound only), Ctrl+Shift+Up / Down make the image or waveform larger,
// Ctrl+E opens the file in the audio editor.
// 
// Trimmer markers and regions: M puts a marker at the Trimmer's cursor, R a region over its selection; S (Save
// Markers/Regions) keeps them with the media in the project (VEGAS writes them into the media file; Reno leaves files
// alone), where the media's events show them (View > Event Media Markers); Shift+S (Auto-Save Trimmer
// Markers/Regions) saves each as it is made. Ctrl+P (Show Video in Preview Window) shows the Trimmer's image in the
// Video Preview as well. The strip's right-click menu has all of these.
#pragma once
#include "appstate.h"
#include <QWidget>
#include <QImage>
#include <memory>

namespace reno {
namespace app {

class TrimmerWindow : public QWidget {
public:
    explicit TrimmerWindow(QWidget* parent = nullptr);
    ~TrimmerWindow() override;

    // Shows a piece of the project's media, or a file (added to Project Media when put on the timeline). False (with a
    // message in the window) if it can't be read or has nothing to show.
    bool openMedia(uint64_t mediaId);
    bool openFile(const QString& path);
    QString currentPath() const;
    uint64_t currentMedia() const;
    double duration() const;

    double position() const;
    void setPosition(double seconds);
    void setSelection(double in, double out);
    double selectionIn() const;
    double selectionOut() const;
    void markIn();
    void markOut();
    void stepFrames(int n);

    // A: the selection (or the whole file) onto the timeline at the timeline's cursor; Shift+A (endAtCursor) so it ends
    // there. Returns the new events (empty if nothing was added).
    std::vector<uint64_t> addToTimeline(bool endAtCursor = false);

    // T (beforeCursor false) / Shift+T: a selection as long as the timeline's time selection, starting (or ending) at
    // the Trimmer's cursor, kept inside the file. False if the timeline has no time selection.
    bool selectTimelineLength(bool beforeCursor);
    enum Streams { BothStreams = 0, VideoOnly = 1, AudioOnly = 2 };
    Streams streams() const;
    void setStreams(Streams s);
    void cycleStreams();
    void changeStreamHeight(int dir);
    bool openInAudioEditor();

    std::vector<MediaItem::Marker> markers() const;
    void insertMarker();
    bool insertRegion();
    void deleteMarker(int index);
    bool saveMarkers();
    bool markersSaved() const;
    void setAutoSaveMarkers(bool on);  // Shift+S toggles it (saved in QSettings "trimmer/autoSaveMarkers")
    bool autoSaveMarkers() const;
    // Ctrl+P: also show the Trimmer's image in the Video Preview (onPreviewPicture gets each one; an empty image when
    // turned off, so the preview goes back to the timeline's). Saved in QSettings "trimmer/videoInPreview".
    void setVideoInPreview(bool on);
    bool videoInPreview() const;
    std::function<void(const QImage&)> onPreviewPicture;

    // Further Trimmer commands: Fit to Fill (four-point editing) stretches or squeezes the selection (or whole file)
    // to fill the timeline's time selection. Enable Timeline Overwrite: what the Add commands put on the timeline
    // replaces what was there on those tracks.
    std::vector<uint64_t> fitToFill();
    void setTimelineOverwrite(bool on);
    bool timelineOverwrite() const;
    bool selectParentMedia();
    bool openParentMedia(uint64_t mediaId);
    bool copySnapshot();
    bool saveSnapshot(const QString& path);
    // Enable Hover Scrub (on by default, saved as "trimmer/hoverScrub"): moving the mouse over the image shows the file
    // at that place (left edge: start, right edge: end).
    void setHoverScrub(bool on);
    bool hoverScrub() const;

    void play();
    void stop();
    bool playing() const;
    void setLoop(bool on);

    QStringList history() const;
    void removeCurrentFromHistory();
    void clearHistory();
    void sortHistory();
    void sortHistory(bool reverse);

    std::function<void()> onWillPlay;

    QImage picture() const;
    bool waveformReady() const;
    std::vector<float> waveform() const;
    QWidget* strip() const;

protected:
    bool event(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace app
}  // namespace reno
