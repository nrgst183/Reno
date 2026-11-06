# Editing behaviour: what to check by hand in VEGAS Pro 18

`tools/vegas/RenoEdits.cs` does everything that VEGAS's scripting API and its Edit commands can do (Split, Trim Start/End,
Trim, Delete, Cut, Copy, Paste, Paste Insert, Post-Edit Ripple, Event Heal, Close Gaps, AdjustStartLength, Copy, rate and
property changes) and saves what happened. What it cannot do is drag with the mouse, read the name of an undo step or flip the
timeline's switches. This list is for those. Each item says what to do and what to save; keep each result in its own project
file next to the others (name it after the item number, for example `C07_before.veg` and `C07_after.veg`) and zip the folder.

Use a new project (File > New, 30 fps if you can: Project Properties) and the two test files RenoEdits.cs writes
(`media_tone.wav`, `media_video.avi`, in the folder you gave it; drag them to the timeline from Project Media). Put the cursor and
selections where the item says. Turn Quantize to Frames and Enable Snapping on unless an item says otherwise. If something
differs from what is written, write down what happened in a text file with the same number.

## Switches that change the other edits

| # | Switch | What to check |
|---|--------|---------------|
| C01 | Options > Auto Ripple (Ctrl+L) with each ripple mode (Affected Tracks / Affected Tracks, Bus Tracks, Markers and Regions / All Tracks, Markers and Regions) | Two audio tracks with three events each (end to end), a marker after the first event. Select the first event of the upper track and press Delete. Save after each mode: which events (on both tracks), keyframes and markers moved back? Undo and repeat with Cut. |
| C02 | Auto Ripple, trim the END of an event by dragging it shorter | Same layout. Save: do the later events follow? On the other track too? |
| C03 | Auto Ripple, trim the START of an event by dragging it later | Does the event itself stay where it is (start moves) or move back? Do later events move? |
| C04 | Auto Ripple and Paste, and Paste Insert with it on and off | Copy an event, put the cursor in the gap between two events and in the middle of an event. |
| C05 | Options > Automatic Crossfades (Ctrl+Shift+X) on: drag an event on top of another on the same track by 1 s | Save the project, write down the fade lengths and curve of both events. Drag it out again: does the fade stay? |
| C06 | Automatic Crossfades off: the same drag | Fade lengths. |
| C07 | Options > Ignore Event Grouping, with a video and audio event grouped | Drag one event; trim the end of one; delete one. Which events moved, changed or went? Preferences > Editing > "Cut, copy, and delete grouped events" off: delete one. |

## Dragging

| # | What | Notes |
|---|------|-------|
| C10 | Drag the left edge of an event later by exactly 2 s (hold Shift to turn snapping off if needed) | Save. Take offset, length, fade-in, and for a video event with Pan/Crop keys (set three keys) and a Composite envelope (three points): where are the keys and points now? |
| C11 | Drag the left edge EARLIER than the media start (event that begins at the start of its media) | Does it stop? Save. |
| C12 | Drag the right edge of a looping event past the end of its media | The loop point; save. |
| C13 | Ctrl+drag the right edge (time stretch): event with a velocity envelope and one with a playback rate other than 1 | Save: rate, length. Does the audio keep its pitch (Time Stretch/Pitch Shift)? |
| C14 | Alt+drag the right edge (slip trim) and Alt+drag the left edge | Save: offset, length, start. |
| C15 | Slip tool (or Alt+drag the middle of the event): drag 1 s right | Take offset before / after; with "Slip All Takes" off and on (Options) for an event with two takes. |
| C16 | Slide tool (or Ctrl+Alt+drag the middle) between two events with a gap, then touching | Save: both neighbours' lengths and offsets, and the dragged event's offset. |
| C17 | Slide edit: Ctrl+Alt+drag the edge between two touching events | Both events' start/length/offset. |
| C18 | Ctrl+Alt+Shift+click an event (Split Trim tool) at 3 s; then Ctrl+Alt+Shift+drag to the right | Which half is trimmed, offsets. |
| C19 | Drag an event to another track (audio to audio, video to video); try dropping an audio event on a video track | Save. |
| C20 | Ctrl+drag an event to copy it | Fades, takes, group (is the copy in a new group?), keys. |
| C21 | Shuffle tool: drag an event over its neighbour | Order and positions. |
| C22 | Drag the fade handle of an event's fade-in beyond the event's middle, and beyond the fade-out | Fade lengths of both, and the event after it if they overlap. |
| C23 | Drag an event's left edge onto the previous event's end with snapping on and off | Exact tick position (the snap should be exact). |

## Commands and what they are called

| # | What | Notes |
|---|------|-------|
| C30 | Edit menu: after each of Split, Trim Start, Trim End, Trim, Delete, Cut, Paste, Paste Insert, Event Heal, Close Gaps, Nudge: open Edit and read the name on the Undo item | Write the names down exactly (`Undo Split Event` or whatever it says). |
| C31 | Press S with the cursor on a frame boundary, 1 ms before it and 1 ms after | Save: is the split put on the frame? (Quantize to Frames on / off.) |
| C32 | Split an event that has a fade-in longer than the part before the cursor, then one with the cursor inside the fade-out | Save: fade lengths of both halves. |
| C33 | Split an event with a cross-fade into it (made by C05) at the middle of the cross-fade | Both events' fades and transition. |
| C34 | Split with the Cut-Copy-Delete-grouped preference changed | (Only the first run needs a note.) |
| C35 | Heal two halves of an event that were split, trimmed and moved apart | Event length, offset. |
| C36 | Smart Split (Alt+S) with a time selection over the part to take out of a 10 s event | The five-frame overlap, the Warp Flow transition, the group. Save. |
| C37 | Insert > Time at 3 s for 2 s over an event that crosses 3 s, with markers and keyframes after it | Save. |
| C38 | Edit > Paste Repeat (5 times, end to end; and every 2 s with events 3 s long) | Save. |
| C39 | Edit > Paste Event Attributes / Selectively Paste with each box | Which attributes changed. |
| C40 | Heal/Split of a placeholder (offline) event | Optional. |

## What to send back

The zip with the saved `.veg` files and your notes. If you have the time, also screenshot the timeline for C01, C05 and C16.
| C41 | Looping events (the 20 s `media_tone.wav` on an event 50 s long): Split with S exactly on a loop point (20 s into the event, Quantize to Frames on), one frame before it and one frame after | Save: take offset of the right half (0 or 20 s?), the loop point lines shown on the event. RenoEdits.cs `loop_split_points` does the same through the API; this checks the program's own Split. |
| C42 | Loop off (Properties or the Loop switch) on an event longer than its media, then drag the right edge further out and back | Does the event stay as long as it was when Loop is switched off? Does a looping event stop at anything? Save. |
| C43 | Looping event with the take starting 5 s into the file: drag the left edge later by 25 s (past a loop point) and earlier by 10 s (before the take start) | Save: take offset after each (wrapped inside the file or counted in the take's own loop?). |
| C44 | Event Properties: change Playback rate from 1 to 2 on an event whose take starts 2 s into the file (type it in the dialog), then back | Save: the take offset and the length. RenoEdits.cs `playback_rate` shows the API keeps the offset in timeline time (the media position it starts at changes); this checks that the dialog does too (Reno's dialog keeps the media position). |
