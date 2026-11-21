// Project Media's Main Timeline storyboard (VEGAS's Storyboard Bins > Main Timeline) and Media Tags panel: parts of
// the Project Media window (panels_mediabin.cpp), kept in files of their own.
//
// No moc (the Windows build doesn't run it): connections are lambdas, widgets subclass virtuals.
#pragma once
#include <QScrollArea>
#include <QWidget>
#include <functional>
#include <memory>
#include <vector>

namespace reno {
namespace app {

// The Main Timeline storyboard: the timeline's events in time order as a grid of thumbnail cells, each with its start
// time above it. Changes made here are made on the timeline. A change on the timeline leaves the storyboard out of
// step (its background turns red) until Refresh Storyboard, unless "Automatically synchronize Main Timeline storyboard"
// is on; while it is out of step it cannot be changed (it offers to refresh instead).
//   - Alt+drag a cell to another: the event moves there on the timeline; the event in that spot and those after it move
//     right to make room, and those after its old place move left to close the gap.
//   - Files (Explorer, desktop) or Project Media items dropped on the cells: onto the middle of a cell they replace its
//     media, onto its left or right edge they are inserted before or after it, onto an empty cell they go at the end.
//   - A drag without Alt carries the cell's media (to the timeline, as from Project Media).
class StoryboardView : public QScrollArea {
public:
    explicit StoryboardView(QWidget* parent = nullptr);
    ~StoryboardView() override;
    void refresh();                         // makes the storyboard match the timeline again
    bool inSync() const;
    void setAutoSync(bool on);              // remembered
    bool autoSync() const;
    std::vector<uint64_t> events() const;   // the cells' events, in order
    std::vector<uint64_t> selectedEvents() const;
    // Moves the event of cell 'from' to the place of cell 'to' (to == the number of cells: the end). False if nothing
    // moved (out of step, or the same place).
    bool moveCell(int from, int to);
    // Media dropped on cell 'cell' (as a drop: 0 before it, 1 onto it (replace), 2 after it; a cell past the last: the end).
    bool placeMedia(const std::vector<uint64_t>& mediaIds, int cell, int where);
    QWidget* canvas() const;                // where the cells are drawn (for tests)
    QRect cellRect(int cell) const;         // in canvas()
    std::function<void()> onSelectionChanged;
    // Asks whether to refresh when a change is tried while out of step (tests answer it; default: a message box).
    std::function<bool()> askRefresh;

    struct Impl;        // (public for the cell canvas in panels_storyboard.cpp)

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    std::unique_ptr<Impl> d;
};

// VEGAS's Media Tags panel under the Project Media list (Views > Media Tags): the "Add tags" box with the tags of the
// selected media as chips (a partly filled chip: only some of them have it; the x on a chip takes it off all of them),
// and the Quick Tags (Ctrl+1 .. Ctrl+9 put the tag on the selected media; Ctrl+Shift+1 .. 9 in the box assigns the
// box's text). Typing a tag and Enter puts it on the selected media; Backspace in the empty box takes off the last tag.
class MediaTagsPanel : public QWidget {
public:
    explicit MediaTagsPanel(QWidget* parent = nullptr);
    ~MediaTagsPanel() override;
    void setMedia(const std::vector<uint64_t>& ids);        // the selected media
    void refresh();                                         // after the project changed
    void addTag(const QString& tag);                        // to the selected media (one undo step)
    void removeTag(const QString& tag);
    // Quick Tags (remembered): n 1..9
    static QString quickTag(int n);
    static void setQuickTag(int n, const QString& tag);
    bool applyQuickTag(int n);                              // Ctrl+n; false if there is no such tag or no media
    void editQuickTags();                                   // the Edit Quick Tags dialog
    // Watches 'w' for Ctrl+1 .. Ctrl+9 (the media list and thumbnails).
    void watchKeys(QWidget* w);
    QStringList chips() const;                              // the chips' tags, for tests ("*" after a partial one)
    QRect chipCloseRect(int index) const;                   // the x of a chip, in the chip area widget
    QWidget* chipArea() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace app
}  // namespace reno
