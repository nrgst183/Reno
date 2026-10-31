# Reno developer notes

Reference material for working on Reno. For what Reno is and how to build it, see the [README](../README.md).

## Source layout

| Folder | Contents |
| --- | --- |
| `src/core` | Shared code: images, JSON, loading DLLs, child processes, copy-on-write vectors |
| `src/ofx` | The OpenFX host (video effects), Reno's built-in effects, transitions and generators, and the Vulkan compute path |
| `src/audiofx` | Audio hosts (VST2, VST3, DirectX), Reno's built-in audio effects and its versions of VEGAS's eFX effects, and the 32-bit VST2 bridge client |
| `src/vst32` | `reno_vst32`, the 32-bit helper process of the bridge (built separately) |
| `src/vegas` | Reading and writing `.veg` projects |
| `src/media` | Reading and writing media files through FFmpeg |
| `src/engine` | The project model, compositor, mixer, rendering, plug-in list, scripting session |
| `src/app` | The Qt interface. It has one look, VEGAS Pro 18's (`applyTheme()` in `mainwindow.cpp` sets the dark palette and style sheet; the sizes and colours of the timeline are in `timeline_internal.h` / `timeline_support.cpp`). A look saved in the preferences by an older build (`prefs/theme`: "reno", "modern") is not read |
| `src/scripting` | `ScriptPortal.Vegas`, the C# implementation of VEGAS's scripting API |
| `src/cli` | `reno_cli`: the command-line renderer and the plug-in checker |
| `tests`, `tools` | Test programs; generators and comparison scripts |

Details worth knowing:

- `src/ofx` presents itself to plug-ins the way VEGAS does (host name, blue-first 8-bit frames, VEGAS's extra suites,
  the effect's own window at the top of its panel) because VEGAS's stock effects and NewBlue's check for it.
  `vegas_ext.h` lists what VEGAS adds to OpenFX. `builtins*.cpp` are the built-in effects, transitions and
  generators; `probe.cpp` is the plug-in test used by `reno_cli`'s checking commands; `fxcompare.cpp` is
  `reno_cli vegasfx`.
- `vk*.cpp` is the Vulkan path for built-in effects. `vkapi` opens `vulkan-1.dll` / `libvulkan.so.1` at run time and
  fetches every function by name, so there is no import library. `vkdevice` handles device choice, buffers, pipelines
  and running a plan of compute passes; `vkeffects` maps built-ins to kernels and settings to push constants;
  `vkcheck` is `reno_cli vkcheck`. The kernels are written in Python (`tools/gpukernels.py`), compiled to SPIR-V by
  `tools/spvdsl.py`; `tools/build_shaders.sh` regenerates `vkshaders/*.spv.h`, which are committed. Preferences >
  Video > GPU API picks Vulkan, OpenGL or off; any failure falls back to the CPU.
- Speed of the Vulkan path (`vkdevice.cpp`, `gpupick.*`, `fxbench.cpp`):
  - A frame goes up as bytes (8-bit pictures are unpacked on the card) and comes back as bytes. A chain of built-in
    effects on one event, track or output keeps the frame on the card: `Engine::runFx` tells the effect before the last
    one of the chain (`RenderOptions::deferGpuOutput`) to leave its download out, and the frame is then marked
    `Image::gpuHold` (its pixels are only on the card). The next built-in effect finds it by `Image::serial`
    (`gpu::FrameIO::inSerial`). Anything else that reads the pixels calls `Image::sync()` first (`clone`, `converted`,
    `stretchImage`, `fxClipGetImage` for anything but a built-in GPU filter, the CPU path of the built-ins), which
    downloads the frame (`gpu::materialize`). The values that stay on the card are the packed 8-bit ones, so a chain
    gives the same bytes as running the effects one by one (vkchain_test compares them). `RENO_VK_NO_DEFER=1` turns it off.
  - Descriptor sets, pipeline lookups, staging and device buffers, the command pool and the pipeline cache (on disk)
    are kept between frames.
  - `reno_cli gpupick` (and Preferences > Video > GPU effects, run behind the splash screen once per card + driver +
    Reno version) times every built-in effect on the CPU and on the card and keeps the quicker, in `gpu_pick.txt` in
    the data folder. An effect whose GPU picture differs from the CPU's in that measurement goes to the CPU. "Always the
    card" or GPU API "Off" overrides it.
  - `reno_cli fxbench [--cpu|--gpu|--both] [--size WxH] [--chain a,b,c]` prints milliseconds per frame per effect (CPU,
    GPU kernels only, GPU with transfers) and a chain four ways (CPU; card one by one; card with the frame kept there
    but downloaded; card with one upload and one download). `tests/perf/fxbench.cpp` is a different program: it
    hashes the CPU effects' output to compare two builds byte for byte.
    `tests/perf/afxbench.cpp` does the same for the built-in audio effects (sample hashes, `--time`). For instruction
    counts use `valgrind --tool=callgrind` on these programs with a small size / `--secs 1`.
  - CPU speed-ups keep the output byte for byte: tables of pure functions of a byte, constants hoisted, the same operation
    order (no FMA), AVX2 versions picked at run time (`src/core/cpufeat.h`, `RENO_NO_AVX2=1` turns them off) only where
    the arithmetic is the same operation for operation (the blur rows and columns, the Deriche loops).
  - Kernels: the Deriche Gaussian runs one thread per line and channel (the channels never mix), `deriche_pack` gathers
    them. The kernels' doubles are emulated in 32-bit integer ops (`tools/sf64.py`), because SwiftShader (the only card
    the tests can use) has no float64, and hardware doubles run at 1/32-1/64 rate on GeForce cards; a different order or
    precision would change bits, so those kernels keep their arithmetic and gain only parallelism.
- The interface uses Qt without moc: no `Q_OBJECT`, no signal/slot declarations. Use lambdas with `QObject::connect`
  and `std::function` callbacks.
- The engine works on copies of the `Project`, a plain value. `AppState` (`src/app/appstate.*`) owns the live
  project, undo and change notifications.
- `src/vegas/vegtemplates.cpp` is generated by `tests/vegas/maketemplates.py` from sample projects.

## VEGAS projects

`.veg` is not a published format. The readers and writers (`src/vegas`) were worked out from sample projects, from
VEGAS's scripting API, and from VEGAS's own program files. `src/vegas/vegformat.h` documents the layout; the comment
above each reader or writer says which parts are confirmed and which are guesses.

- Saving changes only what was changed in Reno. A project opened and saved unchanged comes out byte for byte the
  same, including parts Reno cannot use (older non-OpenFX video effects, effect automation envelopes, track motion).
- New media, tracks, events, effects, envelopes, markers, buses and groups are made from records VEGAS wrote, of the
  same kind, with every count, id and cross-reference made consistent.
- After opening, a window lists what Reno shows differently from VEGAS (and keeps in the file). After saving, a
  window lists what the `.veg` could not hold and was left out. Nothing is written to a `.veg` that VEGAS itself
  does not write.
- Not written yet, because no sample shows where VEGAS stores them: changed track motion, sends to buses,
  transitions, the Custom compositing mode (a compositing plug-in), track groups, effects and envelopes on the video
  output and the master, a bus's pan position, a master solo, VST3 plug-ins, surround pans, multichannel channel
  choice, Trimmer markers, Red Eye Reduction areas, and keyframe interpolation other than Linear for keys that did
  not come from a `.veg`. The Color Grading panel's effect and adjustment tracks have no VEGAS equivalent.
- Written from the files of a Settings Matrix run (`tools/scripts/Reno Settings Matrix.cs`, one change per file;
  `vegmatrix_write_test` rebuilds each change from its base and compares the records byte for byte with VEGAS's):
  a bus's mute, solo and pan type and the master's mute, the compositing parent/child setting (one level), an
  event's undersample rate, Maintain Aspect Ratio, Reduce Interlace Flicker and Force Resample. Optical flow
  resampling was not in the run and is still left out.
- Other `.veg` files used as media (nested projects) are read for frame size, frame rate and length. Edit > Nested
  Timelines creates and opens them.
- Older (pre-OpenFX) VEGAS effects Swirl, Pinch/Punch, Wave, TV Simulator and Light Rays map to Reno's built-ins of
  the same names (`src/vegas/veglegacyfx.*`). The "managed" CoreGraphics effects (Rays, Glint, Starburst, Fill
  Light, Defocus, Soft Contrast) map to OpenFX stand-ins (`vegmanaged.*`). Which units were guessed is noted in
  those headers.
- Audio effects: DirectX effects keep exactly the bytes the plug-in saved (VEGAS's own plug-ins refuse them if the
  length prefix is missing). VST effects use a parameter program, or the plug-in's own chunk for plug-ins that
  keep state that way. The tick in VEGAS's effect chain (Effect.Bypass) is the one on/off flag per plug-in.
- An effect whose plug-in is missing stays in the chain with its settings and is saved back unchanged.

## Timing details

Measured against VEGAS's own mixes (`.sfap0` proxies) and renders:

- AAC in `.mp4`/`.mov`/`.m4a`: VEGAS drops the first 2112 samples the decoder produces (the encoder delay) after the
  file's edit list. Reno does the same.
- An event plays the timeline samples nearest its start and end and reads media from its offset rounded to a sample
  of the file.
- Quickfade Audio Edits is off by default: VEGAS's renders show no fade at the edges of very short events.
- A render's sound is as long as the project in whole samples (floor of length x rate). With picture it ends at the
  nearest sample to the last frame's end.
- A file at another sample rate than the project is resampled per event, from the offset's own file sample, by a
  sinc.
- A frame shows the event whose first tick is at or before the frame's tick and whose end tick is after it, where the
  frame's tick is its time rounded up.
- Media length, frame rate and sample rate are read from the files when a `.veg` is opened; the `.veg` does not hold
  them.
- élastique answers a pitch change late (up) or early (down) by about L x (f - 1) samples, with L about 1050
  (Efficient), 2500 (Pro), 900 (Soloist) and f the pitch factor. Reno shifts its stretcher's output by that amount
  (`StretchParams::delay`).
- The ruler numbers whole steps of a second's frames at any frame rate (every 15 frames at 165 fps).

## VEGAS's files beside media

Reno writes and reads these in VEGAS's own formats, so a project VEGAS opened is quick in Reno and the other way
round.

**Audio proxies** (`<project>.veg.sfap0`) hold the mixed sound of a nested project. They are built in the background
and, once current, nested projects are played, scrubbed, drawn and rendered from them (VEGAS reads a nested
project's sound only from its proxy). A proxy is current when it was made from the `.veg` as it is now, using VEGAS's
mark, the `.veg`'s last-change time to 100 ns. Reno also rebuilds one when a file the nested project uses has
changed, which VEGAS does not notice. Tools > Rebuild Offline Nested Audio builds missing, stale and cancelled
proxies. Preference: Audio > Create audio proxy files for nested projects.

**Peak files** (`<media>.sfk`) hold a file's waveform. A current `.sfk` beside a file gives its waveform at once.
Reno writes them with VEGAS's numbers, which VEGAS then uses. Where the folder cannot be written, and for
files with more than two channels, waveforms are cached in Reno's own folder (the channels of up to eight are each
drawn from the file's own channel pairs, not a stereo mix). Rebuild Audio Peaks is Shift+F5.

Formats:

- `.sfap0` is a Sony Wave64 file: `riff` (GUID 66666972-912E-11CF-A5D6-28DB04C10000, size of the whole file), `wave`,
  `fmt ` (format 3, 32-bit float, 2 channels, the nested project's sample rate, block 8), `data` (interleaved
  samples, floor(project length x rate) of them), a Sonic Foundry summary list (925F94BC-525A-11D2-86DC-00C04F8EDB8A:
  "TCOD" "0" and "TCDO" the end time in 100 ns as UTF-16 text, chunk size rounded up to 8), and the staleness mark
  (chunk 1D185A19-DD87-11D3-8102-00C04F60C0FA: version 1, FILETIME of the `.veg`'s last change, channels, rate,
  bits). VEGAS rebuilds a proxy unless the mark's time is the `.veg`'s and the format is the one wanted. A 0-byte
  `.sfap0` is what a cancelled VEGAS build leaves.
- `.sfk`: "SFPK", version 1, header size 64, the media's last-change time as an MS-DOS date and time in local time
  (seconds rounded up to even), 16 (bits), channels, 256 (samples per peak), length in samples (64-bit), the time again
  in UTC. From byte 64, for each block and channel, the lowest and highest sample as 16-bit numbers. Integer PCM
  gives its top 16 bits; anything decoded to float gives its rounded samples. There are ceil(length / 256) blocks,
  each also taking in the first sample of the next. VEGAS 18 also writes `<media>-0-<length in 100 ns>-1.sfk` for
  media whose sound it takes at a length of its own (an `.mp4`'s AAC at its video's length, a nested project at its
  length in the parent); Reno reads those too.

## Design notes

**Large projects.** A track's events are a copy-on-write vector (`src/core/cowvec.h`), shared between copies of a
project until a copy changes them, so edits, undo steps and snapshots copy only the tracks they touch. Reading
through a const reference never copies; the non-const `begin()`, `end()` and `operator[]` do. `Project::findEvent`
uses an index for projects of 192 events or more. Auto-save of a big project runs on a background thread. `.veg`
reading and writing format and parse GUIDs by hand, which was most of the load time.

**Stutter edits.** Events that are costly to make (time-stretched or pitched, or from a file at another sample rate)
are rendered once from their first sample into a 256 MB cache and played from there; identical events share one
entry. The events of the next 2.5 seconds are made on a worker thread, and when nothing plays the files from the
cursor are made ready. `AudioEngine::setEventCacheLimit(0)` turns the cache off.

**Plug-in delay compensation** (`engine_audio.cpp`, `AudioBlockMixer::planDelays`). Every plug-in's
`latencySamples()` is made up for. An event's own effects read the event's sound that many samples ahead; a track's
effects delay everything it outputs; where paths meet (a bus, the master) the faster ones wait in a delay line
(`AudioEngine::pdc_`) for the slowest. The whole mix runs "lambda" samples ahead (the longest total path), and after
a seek or loop lambda samples are mixed and discarded first. Left out: 5.1 projects, the bus tap, and plug-ins whose
latency changes while playing (those are primed again). Test: `tests/engine/pdc_test.cpp`.

**Recording and input busses** (`recording.cpp`, `recorder.cpp`, `engine_audio.cpp` `mixInputBuses`). Each armed track has a
`RecordInput` (device, first channel, mono/stereo, or an input bus, and Input Monitoring). `planRecordSources` groups
tracks on the same input into one source; `InputHub` keeps each open card's sound (a ring for readers that take blocks,
listeners for the ones that take it as it comes); `RecordRouter` splits a card's interleaved channels to each source's
mono/stereo file. `planRecordSpan`/`commitRecording` decide where it lands: new events cut back what is under them;
with selected events (or a time selection, punch-in) the recording is a take of the event, one per loop pass. Input
busses (`Project::inputBuses`) are mixed before the buses: live sound (`AudioEngine::liveInput`) through insert FX, sends,
fader and pan to the master, a bus, the 5.1 panner or nowhere. Tests: `recording_test`, `console_test`,
`audio_gaps_ui_test`.

**Multithreading and VEGAS's own effects.** VEGAS runs its own effects' work on one thread: its OpenFX
multithread suite does the work in a single call and reports no processors. Splitting the work among processors made
Cookie Cutter cut a squashed copy of its shape in every band, and seven other effects drew differently. VEGAS's own
effects (VEGAS, Sony and MAGIX) get VEGAS's behaviour; other plug-ins get several threads.

**Presets** for VEGAS effects are kept inside each setting's range, as VEGAS does (Cookie Cutter's own presets give
Repeat X and Y as 0, which the effect cannot take).

**Real plug-ins or built-ins.** Where VEGAS is installed Reno uses its OpenFX plug-ins and falls back on the built-in
reproductions (`src/ofx/builtins*.cpp`, same identifiers and settings) for those that are missing or fail.
Preferences > Video > "VEGAS effects" (`prefs/vegasFxSource`; `reno_cli --vegas-fx real|builtin`) can force the
built-ins, which is faster and the same on every platform.

**eFX audio effects.** VEGAS Pro 18 brings MAGIX's essentialFX effects (Limiter, Compressor, Reverb, TubeStage,
Phaser, ChorusFlanger, DeEsser, Gate, StereoDelay, TremoloPan, VocalStrip; VST2 plug-ins `eFX_*.dll` in
`MAGIX Plugins\essentialFX\x64`). They are tied to MAGIX licensing that only VEGAS can satisfy, so elsewhere they run
restricted (the Limiter does no limiting). `src/audiofx/efx*.cpp` are Reno's own versions, written from MAGIX's manual
and the behaviour of the presets, with exactly the real ones' 31 settings (indices, names, 0..1 meaning, value
wording), so a project's saved settings load and save unchanged and the slot stays a VST2 one with the same id.
Every setting's map from 0..1 to its value, wording and default is fitted to a sweep of the real plug-ins (41 points
per setting); `efx_defs.cpp` lists them (shapes Lin, Rat, Pow, Gain, Steps) and `tests/engine/efx_test.cpp` compares
against the sweep file when it is present. The sound itself is Reno's: the Limiter and Compressor were fitted to a
VEGAS render, the others follow the manual. Preference: Video > "VEGAS eFX audio effects" (`prefs/vegasEfxSource`;
`reno_cli --efx real|builtin`).

**Time stretching.** Methods as in VEGAS: None (speed and pitch change together), Classic (a splicing stretcher
with VEGAS's 19 presets and its splice lengths, crossfades and search ranges) and élastique. élastique is zplane's
library and only VEGAS may use it, so Reno has its own algorithms of the same kind for the four modes: Pro (phase
vocoder with locked partials, optional formant preservation and shift), Efficient (lighter, with a level correction
fitted to VEGAS's renders, see `PhaseVocoder::levelGain`), Soloist (Monophonic) and Soloist (Speech)
(pitch-synchronous). They sound close to VEGAS's, not the same.

## 32-bit VST2 plug-ins

Reno is 64-bit and cannot load a 32-bit DLL. `reno_vst32.exe`, a 32-bit program beside `reno.exe`, loads the plug-in
with the same VST2 host code and is driven over a pair of pipes (open, parameters, programs, state, `.fxp`/`.fxb`,
activate, latency, the editor window) and shared memory (audio blocks and parameters, with two events for hand-over;
no polling, roughly 15-20 microseconds per block on top of the plug-in's own work). The protocol is in
`src/audiofx/vst32proto.h`, the plumbing in `vst32ipc.*`, the client in `vst32client.cpp`.

- Plug-in discovery also searches the 32-bit folders. Every DLL is inspected first (machine type, whether it exports
  `VSTPluginMain`). Bridged plug-ins show "(32-bit, bridged)" in the Plug-in Manager and Plug-In Chooser.
- The plug-in's window is created by `reno_vst32.exe` as a child window of the effects window's panel.
- A crash or hang takes only `reno_vst32.exe` down. Every wait has a limit (a block: 4 s plus ten times its length;
  other calls 15 s; opening 90 s). The effect then passes sound through, the effects window shows "(stopped)" with
  the reason, and the process restarts with the last state the next time playback starts.
- `reno_vst32.exe` exits when Reno does and logs to `reno_vst32.log` in `%TEMP%`.
- Off Windows nothing is bridged; the bridge's source builds there as an ordinary program, which is how it is tested.

## Scripting

Reno runs VEGAS's `.cs`, `.js` and `.vb` scripts (and compiled `.dll` scripts). Tools > Scripting > Run Script picks
one; scripts in VEGAS's Script Menu folders, `tools/scripts` and a `Script Menu` folder beside `reno.exe` are listed
below it. A script's whole run is one undo step.

`src/scripting/ScriptPortal.Vegas.cs` implements the .NET assembly scripts are written against, with the classes and
members of VEGAS Pro 18's. The first run compiles it with the .NET Framework 4 that ships with Windows
(`csc.exe`), keeps the result in Reno's data folder, starts the .NET Framework inside Reno as VEGAS does, and
compiles and runs the script. Calls edit a copy of the project, which becomes the project when the script ends.
Members Reno cannot support exist so scripts compile, but throw `NotSupportedException` naming the member
(`ScriptPortal.Vegas.Unsupported.cs`): dockable extensions, rendering image sequences and the rest of `RenderArgs`, presets of audio effects, a track
group's name and collapsed state, and the stereo 3D settings of track motion (lens separation, depth adjust, scale factors).
A track motion key's 3D members (PositionZ, Depth, RotationX/Y, RotationOffsetZ, OrientationX/Y) are fractions of the frame's width
and degrees in Reno, given to scripts in pixels and radians; VEGAS's own signs for the X and Y rotations are not confirmed. Reno gives scripts the project's 360/HDR/stereo 3D settings
(stereo swap and crosstalk only for the project's current mode), track groups (grouping, muting and soloing together),
media bins, `Vegas.Render` and `Project.Render` (Render As's formats and templates, `RenderArgs` with a file, template, range, selection, markers and rotation; the call returns when the file is done) and the presets of OpenFX effects (`Effect.Preset`, `CurrentPreset`, `Presets`). Deleting the `scripting` folder in Reno's data folder forces a recompile.

The C++ side is `src/engine/scriptapi.h` and `scriptsession.*`; the menu is `src/app/scripting.*`.

## Media formats

Anything FFmpeg can read imports, whatever it is called. The extension lists are in `src/media/formats.cpp`. Notes:

- Seeking in Ogg, Opus and WebM is sample-exact. A frame's time stamp alone is not trusted after a seek.
- Files that do not state their length (browser WebM, bare H.264/HEVC/MPEG-2/AV1 streams, animated PNG) or whose
  length is only FFmpeg's estimate have their packets scanned for it (the last 512 MB of larger files).
- Variable frame rate video shows each picture at its own time.
- Transparency reaches the compositor from PNG, APNG, GIF, TIFF, Targa, WebP, OpenEXR, QuickTime Animation, ProRes
  4444, FFV1 and VP8/VP9 with alpha in WebM. The last needs FFmpeg's libvpx decoders.
- Phone video rotation (display matrix) is applied. 10- and 12-bit video keeps its precision in 32-bit projects
  (except rotated or deinterlaced video). HDR10 and HLG are converted to SDR.
- Many-channel sound is mixed down to stereo, except 5.1 and the multichannel import.
- Animated WebP is decoded by Reno (`src/media/webpanim.cpp`) because FFmpeg 7.0 cannot.
- HEIC/HEIF/AVIF stills are read by Reno (`src/media/heif.cpp`) with FFmpeg's HEVC and AV1 decoders: single pictures,
  grids (iPhone photos), an alpha picture, rotation, mirror and crop. Image sequences and overlays go to FFmpeg
  (7.1 and later read the former). AV1/AVIF needs dav1d or libaom (the downloaded FFmpeg 8.1 has both).
- Not readable: MIDI (there is no synthesizer or sound bank in Reno to make sound from notes).

## Smart rendering

Render As > "Smart render" (`RenderSpec::smartRender`, `reno_cli --smart-render`) copies stretches of a source video file
into the render without decoding or compressing them (`src/engine/smartplan.*` plans, `src/media/smartrender.*` reads the
source's groups of pictures, `FFWriter::writeVideoCopy` writes them). The rest of the file is encoded as usual; the
encoder is flushed before each copy and started again after it, so every copy begins with a key frame of the source and
the encoded frames after it begin with the encoder's own. The render's description says "(smart render: N of M frames
copied)" or why nothing was.

It applies to a single-pass render of video only (not two-pass, not alpha, not scaled, not HDR, not an audio-only file or
multiple mono). A frame is copied when all of this holds:
- The file's codec is the source's: H.264, HEVC, MPEG-1/2 or an intra-only codec (MJPEG, ProRes, DNxHD, ...), the container
  accepts it as it is, and the picture size, pixel format, frame rate, pixel shape, field order and (for the codecs that
  keep colour tags outside the stream) colour description are the same. For H.264/HEVC the source's parameter sets (SPS, PPS,
  VPS) must equal those this render's encoder writes, so the render's profile, level and encoder settings have to match
  the source's; the encoder's own SEI note is not compared. The frame rate of the file is the project's exactly.
- The frame is shown by exactly one plain video event: unmuted track at 100%, "srcalpha", with no effects, track motion,
  envelopes, shadow/glow, compositing group, adjustment track or solo; the event with no fade, transition, opacity, pan/crop,
  mask, envelopes, speed change, reversing, undersampling, resampling or flicker reduction; no other event on the same
  frames (a lower track cannot show through). The project is 8-bit, progressive, not colour managed, with no output
  rotation, video-out effects or envelopes, and the same size as the file and the source.
- The event's media is a plain video file (not a nested project, image sequence, subclip, generated or offline media, nor one
  with red-eye, alpha, colour space, stereo 3D, pixel shape, rotation or field order overrides) and the event sits on the
  file's frames (within 1/20 of a frame), with the render starting on a project frame.
- Only whole closed groups of the source inside that stretch are copied. A group is closed when it starts with an IDR (H.264,
  HEVC: an open-GOP encoder such as x265 by default leaves none), an MPEG-1/2 group header with closed_gop set, or is one
  intra frame. A group that cuts across the event's edge is encoded again.
- H.264/HEVC are copied only into containers that keep the parameter sets once (MP4, MOV, Matroska; global header); for MPEG-TS and
  the like the file is encoded in full. Whatever stops a check, that file is simply encoded in full.
- Sound is not affected: it is mixed and written as always. The test decodes the outputs with every error fatal, checks the times
  at the cuts (rising decoding times, no gap or repeat in shown times), the pictures beside each cut against an ordinary render
  (30 dB or better) and the sound's start and end against an ordinary render's.
Not done: copying across a cut between two events of one file, copying an event with the sound of its own file, VC-1/WMV,
VP9/AV1, and audio streams.

## Command line: reno_cli

`reno_cli` renders without a window: a `.veg`, a `.reno` or a single media file, to any Render As format. It uses
the same templates, the same template-to-settings code (`src/app/rendertemplates.cpp`) and the same renderer as
File > Render As, and opens the project with its plug-ins as File > Open does. It uses Reno's settings, so run Reno
once first if you changed preferences or made templates; it does not change them.

    reno_cli project.veg
    reno_cli project.veg -o D:\out\video.mp4 -t "MAGIX AVC/AAC MP4/Internet HD 1080p 29.97 fps" -y
    reno_cli project.veg -o song.wav --audio-only --region "Chorus"
    reno_cli project.veg -o frames\shot.png -t "Image Sequence/PNG" --loop-region 5,6
    reno_cli clip.mov -o clip.mp4 --size 1280x720 --fps 30 --crf 20
    reno_cli --list-templates 1080p -v
    reno_cli --help

With no template, the format is picked from the output's extension. Templates are named "Format/Template"; the format
may be a short name (avc, hevc, wmv, avi, mov, mxf, xavcs, wav, w64, mp3, flac, ogg, m4a, ac3, wma, images, mkv,
webm, ogv, opus, gif, apng, aiff, alac) and the template alone is enough if only one format has it.

Template overrides: `--size WxH` (or `--width`/`--height`), `--fps`, `--video-codec`, `--video-bitrate`,
`--max-bitrate`, `--cbr`, `--crf`, `--two-pass`, `--smart-render`, `--encode-mode` (cpu, nvenc, qsv, amf, mf), `--profile`, `--level`,
`--audio-codec`, `--audio-bitrate`, `--sample-rate`, `--channels`, `--audio-bits`, `--quality` (draft, preview, good,
best), `--audio-only`, `--video-only`.

Range: `--range START END`, `--start`, `--end`, `--length` (seconds, `hh:mm:ss.sss`, timecode `hh:mm:ss;ff` or frames
like `300f`), `--region NAME` (or `#2`), `--loop-region A,B`. Render As options: `--stretch`, `--use-rotation`,
`--markers`, `--loudness-log`, `--summary`, `--project-path`. Others: `--threads N`, `--plugin-path DIR` (repeatable;
OpenFX, VST and VST3 plug-ins in DIR for this render only), `--no-scan`, `--strict` (stop if media or effects are
missing instead of rendering without them), `--vegas-fx`, `--efx`, `-v`, `--log FILE`.

Progress and messages go to stderr; stdout gets only the result line. Ctrl+C stops and deletes the half-made file.
Exit codes: 0 rendered, 1 render failed, 2 bad command line, 3 input could not be opened or nothing to render, 4
`--strict` and something is missing, 5 output exists (`-y` replaces it), 130 stopped with Ctrl+C.

**Profiling.** `reno_cli --profile project.veg [--range START END] [--scale 0.5] [--threads N]` renders the range
frame by frame as the preview does and prints average, median and worst frame time, then where the time went: per
plug-in (own time, calls, ms per call, wait for its turn if not thread safe, preview or project size, thread safety,
identifier), media decoding per file (reads, seeks, time), and compositing. See `src/engine/profile.h` and
`tests/cli/profile_test.sh`.

**Plug-in checker.** `reno_cli check` tries every OpenFX, VST and DirectX plug-in on the machine, each in a process of
its own, and writes `plugin_report.txt` and `.json` (`--out-dir DIR`, `--no-video`, `--no-audio`, `--no-dx`).
`reno.exe` runs `reno_cli ofxprobe`, `audioprobe` and `dxprobe` on each new plug-in file.

**VEGAS effect comparison.** `reno_cli vegasfx [--out-dir DIR] [--size WxH] [--frames N] [--only TEXT]
[--plugin-path DIR] [--quick] [--max-params N] [--gpu on|off] [--stubs] [--all] [--timeout SECONDS]` renders test
pictures through each real VEGAS plug-in (in a process of its own) and through Reno's built-in version at the
defaults, at each setting's smallest, middle and largest values, and with the bundle's presets, then compares. It writes
`vegasfx_report.txt` and `.json` and PNGs of the worst cases. The difference is the mean absolute difference per
pixel and channel, 0 to 255: under 0.25 is "same picture", under 2 "close", under 8 "differs visibly". The code is
`src/ofx/fxcompare.*`; `--params-only` compares every built-in's parameter list with the `Contents/Resources/*.xml`
of VEGAS's OFX bundles.

## Testing

Everything is tested on Linux first; the Windows programs are cross-compiled and then tried by hand. Test programs are
standalone `main` programs; see the README for how they are built. Notes by area:

- **`.veg` files** (`tests/vegas`): reading is checked against real projects and against thousands of damaged copies
  (under ASan/UBSan); writing is checked by round trip (an unchanged project must come out byte for byte the same),
  by edits of every kind, and by comparing every record Reno makes with the ones VEGAS made. `vegaudit` reports which
  bytes of a project the reader never looked at.
- **Against VEGAS itself**: `tests/compare` builds a suite of about a hundred small projects that each try one thing
  (cuts, fades of every curve, transitions, pan/crop, track motion, every compositing mode, rates, reverse, loops,
  envelopes, generated media, stock effects, nested projects, odd frame sizes and rates), renders them with Reno
  (`run_reno.sh`) and compares with VEGAS's renders (`compare.py`: PSNR and SSIM per frame, timing and sound offsets,
  worst frames side by side as a web page). `tools/vegas/Reno Bridge.cs` is a VEGAS script that does the VEGAS side
  unattended; see `tools/vegas/BRIDGE.txt`.
- **Plug-in hosts**: `tests/ofx/run_tests.sh` builds the OpenFX host test and a test plug-in and runs the suite plain,
  under ASan/UBSan and under TSan. VEGAS's own OpenFX plug-ins can be run through Reno's Windows host under Wine
  (`tests/vegas/vegofx_wine.sh`, `vegaudiofx_wine.sh`). `tests/vst3` checks `vst3_abi.h` against Steinberg's headers
  and the host against SDK sample plug-ins; `tests/vst32` runs the bridge with a Linux test plug-in.
- **Interface** (`tests/ui`): drives the real windows through Qt's offscreen platform with synthetic mouse and key
  events and compares screenshots. `monkey_test` performs random valid actions with a fixed seed and checks that
  undo/redo and save/load stay consistent after every step.
- **Media** (`tests/media`): `formats_test` makes sample files of every kind FFmpeg can encode and checks size, rate,
  length, channels, picture and sound content, seeking and transparency; `vegasfiles_test` checks peak and proxy files
  against samples made by VEGAS (a folder named by `RENO_VEGAS_FILES`; not in the repository).
- **Performance** (`tests/perf`, parts of `tests/ui`): benchmarks and tests that fail if the interface thread stalls or
  playback drops frames, including a synthetic project generator (`gen_big.cpp`).

### Test data

The repository holds no sample projects or media. Tests that need them read these environment variables (a test's
own command-line argument wins) and print a `SKIP` line, which does not count as a failure, when the data is missing:

| variable | what it names |
|---|---|
| `RENO_SAMPLES` | folder of sample VEGAS projects (`.veg`, searched recursively) and their media; tests that look at "the samples" run over every `.veg` in it |
| `RENO_SAMPLE_VEG` | one particular `.veg` file for the tests that look at a single project (`cookie_paste_test`, `vegmatrix_diff_test`, `open_media_test`, `zoombar_test`, `scrollzoom_test`, `load_lag_test`, `shortclips_test`, `builtins_test`, `reno_cli_test.sh`); without it each picks a suitable project from `RENO_SAMPLES`, and the test skips when there is none |
| `RENO_MATRIX` | a folder written by the Settings Matrix script (`manifest.txt` and the `.veg` files, in VEGAS's own saves) for `vegmatrix_write_test` |
| `RENO_CMP` | folder the VEGAS bridge scripts (`tools/vegas/bridge_scripts`) wrote: `info` (`real_<project>.vegas.reno`), `loud` (`loud_sweep.veg`), `pitchtests`, `shorttests`, `vegas` (VEGAS's own renders: `nofx_<project>.wav` with `<project>` the name of a `.veg` under this folder, spaces as `_`, and `nofx_loud_sweep.wav`) |
| `RENO_TEST_PLUGIN` | the test OpenFX plug-in file (`effects.ofx`) for `tests/ofx` and `tests/vegas/vegofxstate` |
| `RENO_TEST_VST_DIR` | folder with test VST2 plug-ins (`vegofxstate`) |
| `RENO_EFX_REF` | text dump of the real eFX plug-ins' settings (`efx_test`'s sweep) |
| `RENO_EFFECT_REF` | folder written by `tools/scripts/Reno Effect Reference.cs` (`tools/effectref_compare.py`) |
| `RENO_VEGAS_DIR` | a VEGAS Pro 18 program folder (`ScriptPortal.Vegas.dll`, `Script Menu`, `OFX Video Plug-Ins`) |
| `RENO_VEGAS_FILES` | peak files (.sfk) and an audio proxy (.sfap0) made by VEGAS, with their .wav, named `peaks_a.wav`/`peaks_a.sfk`, `peaks_b.wav`/`peaks_b.sfk` and `nested.veg.sfap0`, for `vegasfiles_test` and `peakfiles_test` |
| `RENO_WINFORMS_DIR`, `RENO_USER_SCRIPTS` | a real `System.Windows.Forms.dll`, and a folder with your own VEGAS scripts (`tests/scripting`, `tests/compare/bridge_test.sh`) |
| `RENO_RENDER`, `RENO_MAKESUITE` | `reno_cli` and `makesuite` for `tests/compare` (default: from the path) |
| `RENO_WINENV` | script setting `WINFLAGS`/`WINLINK` of a Windows cross compiler (the `*_wine.sh` tests) |
| `QT_DIR`, `FFMPEG_INCLUDE`, `BASE`, `UIBUILD` | Qt install, FFmpeg headers, built base library and build helper for the `tests/ui` and `tests/vst32` scripts |

The VEGAS bridge scripts (`tools/vegas/bridge_scripts/*.cs`) use the bridge folder `RENO_BRIDGE_DIR` (default
`Documents\Reno Bridge`, as `Reno Bridge.cs`) and a clip of your own with speech, `speech.mp4`, in `RENO_TEST_MEDIA`
(default `<bridge folder>\media`). `makesuite` takes the bridge folder as VEGAS sees it from `--win-root`.
Scratch files go to the system temp folder.

## Known limits

- VEGAS's pre-OpenFX video plug-ins (its "Video Plug-Ins" folder) use a private Sony interface and cannot be hosted.
  A few of the most common ones are mapped to built-ins (see above).
- VEGAS's own Titles & Text cannot run: it needs a .NET object created through a COM object of the host
  (`OfxPropHostIUnknown`). Reno's own Titles & Text is used instead.
- VEGAS's HWnd overlays (`OfxHWndOverlayInteractSuite`) are not drawn. Ordinary OpenFX overlays are, following
  Pan/Crop (for pre-pan/crop effects), Track Motion and Parent Motion.
- VEGAS's OpenCL suite is not provided, so effects that have an OpenCL path render on the CPU.
- `OfxPropVegasTimeCode` is the frame's timeline time for effects on the timeline; elsewhere (thumbnails, the
  generator preview) it is the effect's own time. In VEGAS it is a separate, unrounded field of the render request
  (timeline ticks / 10^7 x frame rate) while `OfxPropTime` is rounded to half frames; what the request holds outside
  the timeline could not be read from VEGAS's code, so that case is a guess.
- Compositing plug-ins cannot be hosted. The Custom compositing mode is kept in a `.veg` but shown as Source Alpha.
- VST3 effects are not saved into `.veg` files. 32-bit DirectX plug-ins cannot be loaded by a 64-bit program.
- Nested projects' sound: the proxies Reno renders differ from VEGAS's (Reno's AAC sound starts 2112 samples later;
  time-stretched events do not sound alike; VEGAS's DirectX effects exist only on Windows).
- Peak files are written in 16 bits: VEGAS's own code accepts only 8 and 16 bits, so there is no 24/32-bit form to
  write. The `.sfk1`/`.sfap1` files of second and later audio streams are not used: Reno plays one audio stream per
  file, and no sample shows how VEGAS names them.
  Files of more than two channels get no `.sfk`: no multichannel `.sfk` made by VEGAS has been seen, so its layout there
  is unconfirmed (the code reads and writes the channels interleaved block by block, as for two).
- élastique is zplane's. Reno's stand-ins keep pitch and length as exactly but do not sound identical; their
  pitch-shifted sound lines up with VEGAS's only up to about +-4 semitones (Efficient), +-2 (Soloist) and +-8 (Pro).
- 5.1: buses stay stereo (the manual says only the Surround Master takes six channels: a bus's own pan puts its
  stereo sound into the master through the surround panner); only the master's plug-ins take six channels. Playback
  sends six channels to one sound card. The pan types (Add Channels, Balance, Constant Power, Film), Center (a gain
  on the centre channel, -Inf to +12 dB) and Smoothness (the path's curve; a key's own, the keyframe type) follow
  the manual's description, not VEGAS's code: the default pan type in 5.1 is Balance (0 dB), Film's speaker angles
  (C 0, R 30, Rs 110, Ls 250, L 330 degrees) are a guess. No real multichannel plug-in, card or 5.1 file has been
  tried; the tests use stand-ins.
- 3D track motion: the camera is VEGAS's (read from its `tCAMERASETTINGS::Init` and the hidden `dDefaultFieldOfView`
  setting: a 50 degree vertical field of view, so 1.0723 frame heights away), but what the compositor does with the
  card beyond that (clipping planes, filtering) is Reno's. VEGAS has no 3D Shadow or Glow (converting to 3D asks, then
  deletes the 2D Shadow and Glow keyframes; Reno does the same). The Perspective pane is turned with Alt+drag or the
  middle button (not found in VEGAS's manual: Reno's own way); snapping (F8) goes to the same grid as the flat window.
  Edit in Object Space and Scale About Center are still 2D only.
- Pre-pan/crop effects get the media at its own size, told the project is that size (X/Y settings scaled to it as in a
  smaller preview); whether VEGAS tells them the media's or the project's size is not known.
- Mixing Console: the master's faders split into left and right (the L/R button; the Surround Master's per-channel
  trims are in the project and the mix but have no console faders). Input bus strips have no send rows (the sends
  are in the model and the mix), and the Preview bus strip has a fader, mute and meters but no effects. Input busses
  sound in playback and Real-Time Render, not in a normal render. Live sound is read from the sound card in blocks
  the mix takes ahead of the sound, so monitoring and input busses are heard late (about the mix-ahead time) and
  can drop out; a lower mix-ahead time is not tried. No hardware outputs for busses.
- Recording: each armed track records from its own Record Input (a sound card input and mono channel or stereo
  pair, or an input bus's sound, taken before its fader); tracks on the same input share a file. A card is opened
  with the channels it has and the interface Qt reports; no interface with more than two channels has been tried
  (the tests use a stand-in card). Text to Speech and MIDI timecode/clock need Windows (SAPI 5, winmm).
- MPEG-1 (Render As > MPEG-1: Video CD NTSC/PAL, 352x240/288 and larger templates, Layer II sound) is Reno's own list of
  templates built on FFmpeg's mpeg1video and mp2 encoders; VEGAS Pro 23's exact template list is not known here.
- Not done from VEGAS Pro 23's additions: CD extraction. (Input busses, punch-in and take recording, MPEG-1 and smart
  rendering are done: see Recording, the Mixing Console and Smart rendering.)
- Windows-only code (DirectX hosting, the .NET scripting host, SAPI, MIDI timecode, the 32-bit bridge's real use) is
  built but can only be exercised on Windows.
