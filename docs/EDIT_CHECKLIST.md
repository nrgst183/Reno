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
| C41 | Automatic Crossfades on: drag event B 1 s onto the end of event A (video and audio), then drag B away again to leave a gap | Vegas 8 (tutorial video 20:14-20:17) keeps a 1 s fade-in curve on B and shows no fade-out on A. Save before and after: B's fade-in length / curve, A's fade-out length. (Reno leaves the stored fades alone and draws the crossfade from the overlap only.) |
| C42 | Drag a fade-in handle (top left corner of an event) and a fade-out handle; also just hover over each | Write down the exact tooltip text that follows the pointer while dragging (Vegas 8: "Fade offset 00:00:13.08", the fade's length) and the one when hovering (maybe "Fade-in offset ... Drag to adjust the length of the fade applied to this event"). Reno shows "Fade offset <time>" while dragging. |
| C43 | Drag an event along the timeline (slowly, a few seconds) | Is there a small box above the ruler with how far it has moved ("+1.06", "-1.24": seconds.frames) and a line back to where it started? Which text, where, is it also shown while snapping? Reno draws it in the band above the ruler. |
| C44 | Empty timeline with one empty video and one empty audio track: drag a clip from Project Media to the blank area below the tracks and drop it; then drop another onto the empty tracks | Vegas 8 (35:18-35:22) made two NEW tracks after the existing ones (video then audio below) and did not use the empty ones. Which tracks does VEGAS 18 use, and in which order? Where does the drop outline show? |
| C45 | Select two video events whose audio is not grouped with them, drag them far to the right | Vegas 8 (tutorial 46:32-46:44, after Event > Group > Remove From, then dragging only the video event right) tints the whole audio event pale red for as long as the video event is dragged away from it (also shown at 42:47). While they are in step it is the blue selected look. What is the tint (out-of-sync mark? ghost?), does it stay after the drop, does VEGAS 18 do it? Screenshot. |
| C46 | Pan slider of an audio track header while dragging and the pan envelope tooltip | Vegas 8 writes "18 % L" (a space before the %); Reno writes "18% L". Which does VEGAS 18 write? |

## Items from the tutorial re-watch (tut3b)

| # | Do this in VEGAS | Write down |
|---|---|---|
| C47 | Overlap two events by about 3 s (auto crossfades on), look at the overlap in the video and the audio row | Vegas 8 (tutorial 23:44) puts a small white box with the overlap length ("3.09", seconds.frames) in the middle of the crossfade in both rows. Reno now draws it too. Does VEGAS 18 show it in both rows, always or only when selected, in which format (hours? frames only?) |
| C48 | Drag the left edge of a crossfade (or the start of the later event) leftwards to lengthen the overlap | Vegas 8 (23:43-23:44) grew the crossfade to the left with the cursor and selection left alone. Which edge did VEGAS move, did the later event's start trim, does the label follow? |
| C49 | A clip dropped with picture and sound: click only the audio half, then only the picture half (no group changes) | Vegas 8 shows only the clicked half in the selected look in some frames (13:53, 17:42, 19:48, 16:04) and both in others (15:59). Which halves select in VEGAS 18? Screenshot of each. |
| C50 | Drag an audio event's gain line all the way down | Vegas 8 tooltip reads "Gain is -Inf. dB" (with a full stop after Inf, tutorial 45:06); Reno writes "Gain is -Inf dB". What does VEGAS 18 write? |
| C51 | Group a picture+audio pair, use Event > Group > Remove From on the picture, drag the picture away and drop it, look at the audio event | Vegas 8 (46:40-46:44, 42:47, 47:54) shows part of the audio event pale red where its picture used to be. Extent of the tint, does it stay after the drop, does it show a number? (Extends C45.) |

## Items from the tutorial re-watch (tut3a)

| # | Do this in VEGAS | Write down |
|---|---|---|
| T1 | Drag an event over the end of its neighbour on the same track (Automatic Crossfades on) and keep the button down | The beginner tutorial shows a white fade-in curve and a white box with the overlap length (seconds.frames) on the dragged event, and a signed amount ("-0.16", "+0.05", frames after the dot) on the strip above the ruler. Reno does the same; check that the number is the overlap, that it is the amount moved since the press, and whether the curve turns into the blue X only when the button is let go. Screenshot while holding. |
| T2 | The same drag amount while trimming an edge and while Ctrl+dragging an edge (time stretch) | The tutorial shows it for the stretch ("-0.19"): is it the change of the edge position in time, and is it the same for a plain trim, for Alt+drag (slip) and for a fade handle (which has its own tooltip)? |
| T3 | Event Pan/Crop: Anchor Creation tool, draw a mask and close it | New path: Feather type should read None with Feather (%) greyed (the tutorial). Does Apply to FX start at No, Mode Positive, Anti alias Yes, Opacity 100? While the path is still open the picture should not be dimmed; once closed, everything outside goes dark. |
| T4 | Alt+drag the body of an event (slip) | The tutorial's illustration shows the preview split into "In Point" and "Out Point" frames and an orange bar over the event. Does the preview show both frames while slipping? Screenshot while holding. |
| T5 | Expanded Edit (double-click an event edge, or Numpad 5) and drag the cut | What does the preview show (both frames of the two events at the cut?) and how do the events look? Does Update 482's preference exist to turn the double-click off? |
| T6 | Right-click an empty part of an audio track | The video lane ends with Insert Empty Event / Insert Text Media... / Insert Generated Media... / Insert Slideshow... Which of these does an audio lane show, and does Insert Empty Event appear inside a time selection at the top or the bottom? |
| T7 | Drag a transition from the Transitions tab onto the timeline and hold | The tutorial (38:24-38:28) shows a popup grid of small preset tiles under the pointer while dragging, one tile highlighted blue under the pointer. What is it (a preset picker shown on drop?), where does the transition land (on a cut, on an event start), and how long is it? |
| T8 | Event Pan/Crop: move the gizmo with the cursor not at the event start | The tutorial's text (27:00): any adjustment made away from the first keyframe adds a keyframe at that time; to change the whole look, edit the first keyframe. Check that Reno's Pan/Crop does the same and that it also happens when Anchor/Position boxes are typed. |
| T9 | Audio FX: try Automate (envelope for a plug-in parameter) on a plug-in in an audio EVENT FX chain, a TRACK FX chain and the master FX | The tutorial (49:50-50:30) says audio plug-ins cannot be automated at the "Audio FX level" (event FX); only track-level FX offer Automate. Which of the three offers it, and which plug-ins (DirectX, VST) are listed? |

## What to send back

The zip with the saved `.veg` files and your notes. If you have the time, also screenshot the timeline for C01, C05 and C16.
| C41 | Looping events (the 20 s `media_tone.wav` on an event 50 s long): Split with S exactly on a loop point (20 s into the event, Quantize to Frames on), one frame before it and one frame after | Save: take offset of the right half (0 or 20 s?), the loop point lines shown on the event. RenoEdits.cs `loop_split_points` does the same through the API; this checks the program's own Split. |
| C42 | Loop off (Properties or the Loop switch) on an event longer than its media, then drag the right edge further out and back | Does the event stay as long as it was when Loop is switched off? Does a looping event stop at anything? Save. |
| C43 | Looping event with the take starting 5 s into the file: drag the left edge later by 25 s (past a loop point) and earlier by 10 s (before the take start) | Save: take offset after each (wrapped inside the file or counted in the take's own loop?). |
| C44 | Event Properties: change Playback rate from 1 to 2 on an event whose take starts 2 s into the file (type it in the dialog), then back | Save: the take offset and the length. RenoEdits.cs `playback_rate` shows the API keeps the offset in timeline time (the media position it starts at changes); this checks that the dialog does too (Reno's dialog keeps the media position). |
