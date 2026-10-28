// The project: media, tracks, events and effect chains. A plain value - copying a Project is how the editor takes
// snapshots for undo and for the threads that render.
#pragma once
#include "../core/base.h"
#include "../core/json.h"
#include "../core/cowvec.h"
#include "../media/media.h"
#include "../audiofx/audioplugin.h"

namespace reno {

// The records VEGAS wrote for an object in a .veg file, kept (shared, never changed) so that saving as .veg writes
// them back as they were and changes only what was edited. Null for anything made in Reno; not saved in Reno's own
// files.
struct VegOrigin;
using VegOriginPtr = std::shared_ptr<const VegOrigin>;

// One effect in a chain. The live plug-in instance belongs to FxRuntime and is found by 'id'; 'state' is what was last
// saved from it (FxRuntime::captureStates updates it before a project is written).
struct FxSlot {
    uint64_t id = 0;
    std::string kind;           // "ofx" (video) or "audio"
    std::string pluginId;       // ofx: the effect's identifier
    AudioPluginDesc audio;      // audio: which plug-in
    std::string name;           // as shown
    bool bypass = false;
    bool beforePanCrop = false; // event video effects: applied before Event Pan/Crop (VEGAS's pre-pan/crop effects)
    bool afterComposite = false; // track video effects: applied after the track is composited (VEGAS's Composite button in the chain), not before
    bool bypassAutomation = false;  // audio: VEGAS's Bypass FX Automation, the "fx" envelopes are not followed
    Json state;
    VegOriginPtr veg;
};
using FxChain = std::vector<FxSlot>;

// The shape of a fade or of an envelope's line from one point to the next (VEGAS's curve types).
enum Curve : int { CurveLinear = 0, CurveFast = 1, CurveSlow = 2, CurveSmooth = 3, CurveSharp = 4, CurveHold = 5 };
// How far along (0..1) a change shaped by 'curve' is when 'x' (0..1) of its time has passed.
double curveShape(int curve, double x);

// An envelope (VEGAS's envelopes): a setting that changes over time, drawn as a line over a track, event or bus track.
// Between two points the value follows the left point's curve; before the first point it is the first point's value and
// after the last it is the last's. With no points the setting is unchanged (valueAt gives 'whenEmpty').
//   kind          value                                           where
//   "volume"      gain factor (1 = 0 dB, 2 = +6 dB, 0 = silent)    audio tracks, audio events, buses, the master
//   "pan"         -1 left .. 1 right                               audio tracks, audio events, buses
//   "mute"        1 muted, 0 not (always held)                     tracks, buses, the master, the video output
//   "send"        gain factor of the send to bus 'target'          audio tracks
//   "composite"   opacity 0..1                                     video tracks, video events
//   "fadecolor"   -1 (the bottom colour) .. 0 (none) .. 1 (top)    video tracks, the video output
//   "velocity"    speed factor -1..10 (negative plays backwards)  video events
//   "transition"  how far the transition is, 0..1                 video events (their transition from the event before;
//                                                                 VEGAS's Transition Progress envelope)
//   "fx"          the parameter 'param' of effect 'target', 0..1   tracks, buses (automation)
//   "motionblur"  amount 0..1                                      the video output
//   "supersample" sub-frames per frame, 1..8                       the video output
// Times: seconds from the event's start for an event's envelope, timeline seconds for all others.
struct EnvPoint {
    double time = 0;
    double value = 0;
    int curve = CurveLinear;    // to the next point
};
struct Envelope {
    std::string kind;
    uint64_t target = 0;        // "send": the bus; "fx": the effect slot
    std::string param;          // "fx": the parameter's name
    bool visible = true;        // drawn on the timeline (hidden envelopes still apply)
    bool locked = false;        // its points move with events (false) / stay put (true) - VEGAS's "Lock Envelopes to Events"
    std::vector<EnvPoint> points;   // in time order
    VegOriginPtr veg;
    double valueAt(double t, double whenEmpty) const;
};
using Envelopes = std::vector<Envelope>;
// The envelope of this kind (and target) in the list, or null.
const Envelope* findEnvelope(const Envelopes& list, const std::string& kind, uint64_t target = 0);
Envelope* findEnvelope(Envelopes& list, const std::string& kind, uint64_t target = 0);
// The value of the envelope of this kind at t, or 'whenNone' if there is no such envelope (or it has no points).
double envelopeValue(const Envelopes& list, const std::string& kind, double t, double whenNone, uint64_t target = 0);
struct Track;
// A track's envelope of this kind that applies (null: none with points, or the track's automation is Off), and
// the value of that setting at t: the envelope's, else the track's automation value (Track::autoLevel, autoPan,
// autoFade, autoMute) - "volume", "composite", "pan", "fadecolor", "mute".
const Envelope* trackEnvelope(const Track& tr, const std::string& kind, uint64_t target = 0);
double trackAutomationValue(const Track& tr, const std::string& kind, double t);

struct MediaItem {
    uint64_t id = 0;
    std::string path;           // empty for generated media
    std::string name;
    MediaInfo info;             // as read when it was added
    bool offline = false;       // the file could not be opened
    bool generated = false;     // made by an OpenFX generator rather than read from a file
    FxSlot generator;           // (generated only)
    VegOriginPtr veg;
    // Project Media organising (VEGAS's media bins, tags, comments and subclips)
    std::string bin;            // media bin: "" is the Project Media root, "Interviews/Day 1" a bin inside a bin
    std::vector<std::string> tags;
    std::string comment;
    // A subclip (Create Subclip): part of the file, starting 'subclipStart' seconds in and 'subclipLength' long, as
    // media of its own. Length 0: the whole file (an ordinary media item).
    double subclipStart = 0, subclipLength = 0;
    // Tools > Video > Red Eye Reduction (stills): the corrected areas, in the frame as fractions of its width and
    // height from the top left.
    struct RedEyeArea { double x = 0, y = 0, w = 0, h = 0; };
    std::vector<RedEyeArea> redEye;
    // Markers and regions added in the Trimmer and saved with Save Markers/Regions (S); VEGAS keeps them in the media
    // file itself. Events show them with View > Event Media Markers, beside the markers the file holds
    // (MediaInfo::markers).
    struct Marker { double time = 0, length = 0; std::string name; };
    std::vector<Marker> markers;
    // An image sequence (File > Import > Image Sequence, VEGAS's "Open still image sequence"): 'path' is the first of the
    // numbered frames, read in order as a video at this frame rate (media.h: imageSequenceSpec). 0: not a sequence.
    double sequenceFps = 0;
    // Media Properties' Alpha channel (overrides what the file says about transparency): "" as the file says (VEGAS's
    // Undefined), "none" (opaque), "straight", "premultiplied" (colours already multiplied by alpha: VEGAS's Premultiplied
    // and Premultiplied (dirty)).
    std::string alphaMode;
    // Media Properties' Stereoscopic 3D mode: how the file holds the two eyes' frames.
    std::string stereoMode = "off";
    bool stereoSwap = false;
    uint64_t stereoPair = 0;
    // Media Properties' Color space (VEGAS's ACES color management; used only when the project's pixel format is 32-bit
    // floating point (full range) with a View transform): "" is Default (Rec. 709), or one of colormgmt.h's input color
    // spaces ("aces", "acescg", "slog1", "slog3", "logc"...).
    std::string colorSpace;
    // Media Properties' Field order, Pixel aspect ratio and Rotation (VEGAS's Media tab).
    std::string fieldOrderOverride;
    double pixelAspectOverride = 0;
    int rotationOverride = 0;
    // How much the frame's width is stretched when the pixel aspect ratio is overridden (1: not)
    double pixelFactor() const
    {
        return pixelAspectOverride > 0.01 && info.pixelAspect > 0.01 ? pixelAspectOverride / info.pixelAspect : 1.0;
    }
    // The size the media is shown at: info's width and height with the pixel aspect ratio and rotation overrides applied.
    void shownSize(double& w, double& h) const
    {
        w = info.width;
        h = info.height;
        const double f = pixelFactor();
        if (f > 1.0005) w *= f;
        else if (f < 0.9995) h /= f;
        if (rotationOverride == 90 || rotationOverride == 270) std::swap(w, h);
    }
    // The field order in effect (the override, or what the file says)
    const std::string& fieldOrder() const { return fieldOrderOverride.empty() ? info.fieldOrder : fieldOrderOverride; }
    // The path a reader opens for this media: 'path', or an image sequence's spec.
    std::string readPath() const;
};

enum class TrackType { Video, Audio };

// Pan/crop of an event (VEGAS's Event Pan/Crop): which part of the media is shown at a moment of the event.
struct PanCropKey {
    double time = 0;            // seconds from the event's start
    double cx = 0.5, cy = 0.5, w = 1.0, h = 1.0, rotation = 0;
    int curve = 0;              // to the next key: a KeyCurve (0 linear, 1 hold, 2 smooth, 3 fast, 4 slow, 5 sharp)
    double smoothness = 0;      // VEGAS's Smoothness (0..100): 0 moves the centre straight to the next key, more follows a
                                // curve through the keys before and after (see smoothPath)
    double pivotX = 0, pivotY = 0;  // VEGAS's Rotation X / Y Center: the point the box rotates about when its angle changes,
                                    // relative to the box's centre (fractions of the media's width and height; 0, 0 is the
                                    // centre). The box is drawn where cx, cy say (VEGAS keeps the rotated box's centre).
};

// The curve of a keyframe of Event Pan/Crop, its mask, Track Motion, 2D Shadow / Glow and surround panning (VEGAS's
// keyframe types).
enum KeyCurve : int { KeyLinear = 0, KeyHold = 1, KeySmooth = 2, KeyFast = 3, KeySlow = 4, KeySharp = 5 };
// How far along (0..1) a key's values are towards the next key's when 's' (0..1) of the time between has passed.
double keyCurveShape(int keyCurve, double s);
// VEGAS's VideoKeyframeType (0 linear, 1 hold, 2 slow, 3 fast, 4 smooth, 5 sharp) to a KeyCurve and back.
int keyCurveFromVegas(int vegasType);
int keyCurveToVegas(int keyCurve);
// VEGAS's keyframe Smoothness: the point a fraction 'f' of the way from key 1 to key 2 when the path goes through
// keys 0, 1, 2, 3 (keys 0 and 3 are the ones before and after; where there are none, the end keys themselves).
void smoothPath(double smoothness, double x0, double y0, double x1, double y1, double x2, double y2, double x3, double y3, double f,
                double& x, double& y);

// The mask of Event Pan/Crop (VEGAS's Mask switch in the Pan/Crop window): shapes (Bezier paths; rectangles and ovals
// are kept as Bezier paths too) that cut the event's frame.
struct MaskPoint {
    double x = 0.5, y = 0.5;            // the anchor
    double inX = 0, inY = 0;            // its tangent handles, from the anchor (0, 0: a corner, no curve)
    double outX = 0, outY = 0;
};
struct MaskPath {
    std::vector<MaskPoint> points;
    bool closed = true;                 // an open path (being drawn) masks nothing
    bool negative = false;              // VEGAS's Mode: Positive (shows what is inside) or Negative (what is outside)
    bool enabled = true;                // the path's on switch
    bool antiAlias = true;              // VEGAS's Anti alias
    int featherType = 2;                // VEGAS's Feather type: 0 None, 1 In, 2 Out, 3 Both
    double feather = 0;                 // VEGAS's Feather (%): 0..100, how soft the edge is (100: a quarter of the frame's shorter side)
    double opacity = 1.0;               // VEGAS's Opacity, 0..1
};
struct MaskKey {                        // the mask at a moment of the event (keyed like Pan/Crop's box, on its own row)
    double time = 0;                    // seconds from the event's start
    int curve = 0;                      // to the next key: 0 linear, 1 hold, 2 smooth
    std::vector<MaskPath> paths;
};
// The mask at time t (seconds from the event's start). Paths with the same number of points in both keys are
// interpolated; any other change happens at the next key.
std::vector<MaskPath> maskAt(const std::vector<MaskKey>& keys, double t);
// True if an event's mask cuts anything: its switch is on and it has a closed, enabled path of 3 or more points.
struct Event;
bool maskActive(const Event& e);

// Track motion (VEGAS's Track Motion): where the track's frame goes in the project frame.
struct MotionKey {
    double time = 0;            // seconds on the timeline
    double x = 0.5, y = 0.5, w = 1.0, h = 1.0, rotation = 0;
    int curve = 0;              // a KeyCurve
    double smoothness = 0;      // VEGAS's Smoothness 0..100 of the path of the centre (see smoothPath).
    // VEGAS's 3D Track Motion, used only where the compositing mode is 3D Source Alpha ("3dsrcalpha"). x, y, w, h and
    // 'rotation' (RotationZ) mean the same in 3D.
    //   z            distance towards the camera (negative: away) as a fraction of the frame's width
    //   d            depth: only scales the rotation offset's z (and a parent's children's z)
    //   rotX, rotY   degrees about the horizontal / vertical axis through the rotation centre (rotX > 0: top towards
    //                the camera, rotY > 0: right side away)
    //   oriX/Y/Z     VEGAS's Orientation: the frame's own rotation about its centre, applied before the rotation
    //   offX/Y/Z     rotation centre relative to the frame's centre (right, down, towards the camera; fractions of width)
    double z = 0, d = 1.0, rotX = 0, rotY = 0, oriX = 0, oriY = 0, oriZ = 0, offX = 0, offY = 0, offZ = 0;
    bool has3d() const
    {
        return z != 0 || d != 1.0 || rotX != 0 || rotY != 0 || oriX != 0 || oriY != 0 || oriZ != 0 || offX != 0 || offY != 0 || offZ != 0;
    }
};

// VEGAS's Track Motion 2D Shadow and 2D Glow: a blurred single-colour copy of the shape of the track's (moved) frame,
// laid under it.
struct GlowKey {
    double time = 0;
    double x = 0, y = 0, w = 1.0, h = 1.0;
    double blur = 0.2, intensity = 0.5;
    uint32_t color = 0xFF000000;
    int curve = 0;
};

// VEGAS's surround panning in a 5.1 project (Project Properties > Audio > Master bus mode: 5.1 Surround): where a
// track's or bus's sound is placed among the master's six channels (L R C LFE Ls Rs).
struct SurroundKey {                    // VEGAS's Surround Pan Keyframes: the pan point (and centre) at a moment
    double time = 0;                    // seconds on the timeline
    double x = 0, y = 0, center = 1.0;
    int curve = 0;                      // how the pan moves in time to the next key: a KeyCurve (VEGAS's keyframe types)
    double smoothness = -1;             // this key's Smoothness (the curve of the path to the next key); below 0: the panner's own
};
struct SurroundPan {
    double x = 0, y = 0;                // the pan point: x -1 left .. 1 right, y -1 rear .. 1 front; (0, 0) is the middle of the room
    double center = 1.0;                // VEGAS's Center fader: a gain on the centre channel (shown in dB; 1 = 0.0 dB, no gain; boosts and cuts)
    double smoothness = 0.0;            // VEGAS's Smoothness 0..1: how curved the path is between keyframes (0: a straight line, the
                                        // pan point moves at constant speed; more: a smooth curve through the keys)
    bool lfe = false;                   // VEGAS's Enable LFE: both sides mixed are also sent to the LFE channel at lfeLevel
    double lfeLevel = 1.0;
    std::string route = "surround";     // VEGAS's Surround Pan menu: "surround" (panned), "front", "center", "lfe", "rear" (only)
    std::string motion = "free";        // how the pan point is dragged: "free", "x" (left/right only), "y" (front/back only)
    unsigned speakerMutes = 0;          // speakers this sound is kept out of: bit 0 L, 1 R, 2 C, 3 LFE, 4 Ls, 5 Rs
    std::vector<SurroundKey> keys;      // empty: the point above, all the time; else the keys (x, y, center) win
    bool isDefault() const
    {
        return x == 0 && y == 0 && center == 1.0 && smoothness == 0.0 && !lfe && lfeLevel == 1.0 && route == "surround" && motion == "free" &&
               speakerMutes == 0 && keys.empty();
    }
};
Json surroundToJson(const SurroundPan& s);
SurroundPan surroundFromJson(const Json& j);

// An audio bus (VEGAS's Bus A, Bus B...): tracks can send their sound to it instead of to the master,
// and/or send a share of it there.
struct Bus {
    uint64_t id = 0;
    std::string name;
    double level = 1.0, pan = 0;
    bool mute = false, solo = false;
    uint64_t output = 0;        // where the bus goes: 0 the master, else another bus
    std::string panType;        // as Track::panType (the bus's pan law; in a 5.1 project also its surround panner's)
    FxChain fx;
    Envelopes envelopes;        // volume, pan, mute, fx (on its bus track)
    VegOriginPtr veg;
    SurroundPan surround;       // its place in a 5.1 project's master (see SurroundPan)
    // VEGAS's Assignable FX (an FX send): a bus that tracks only send to (never route their output to), named after
    // its effects and shown as an FX strip in the Mixing Console.
    bool assignable = false;
    // VEGAS's Pre/Post Fader Insert FX: false (default) puts the effects before the bus's volume and pan, true after
    // them (stereo projects; in 5.1 a bus going to the master keeps them before).
    bool fxPostFader = false;
    // VEGAS's Automation Settings of a bus (its bus track and Mixing Console strip), as for Track: with 'automation' on,
    // the strip's controls work on the envelopes. automationMode as in Track.
    bool automation = false;
    std::string automationMode = "read";
};

struct Send {
    uint64_t bus = 0;
    double level = 0;           // 0..
    bool preFader = false;      // taken before the track's volume and pan (true) or after (false)
};

// Where an audio track records from (VEGAS's Record Input button in the track header): a sound card input of one or
// two channels, or an input bus. The default records from the input chosen in Preferences, channels 1 and 2.
struct RecordInput {
    std::string device;         // the sound card input (Qt Multimedia's id); empty: the one chosen in Preferences
    int first = 0;              // its first channel, from 0
    int channels = 2;           // 1 records mono, 2 stereo (this and the next channel)
    uint64_t inputBus = 0;      // else record the sound of this input bus (after its effects), whatever the three above say
    // VEGAS's Input Monitoring: "off", "on" (hear the input through the track's effects and fader during recording) or
    // "auto" (the same, but only where the track would record into events when recording into selected events)
    std::string monitor = "off";
    bool isDefault() const { return device.empty() && first == 0 && channels == 2 && inputBus == 0 && monitor == "off"; }
    bool sameSource(const RecordInput& o) const
    {
        return inputBus ? inputBus == o.inputBus : (!o.inputBus && device == o.device && first == o.first && channels == o.channels);
    }
};
Json recordInputToJson(const RecordInput& r);
RecordInput recordInputFromJson(const Json& j);

// VEGAS Pro 23's input bus: sound from the sound card's inputs that is processed (insert effects, always before its
// fader) and mixed with the project, sent on to a bus or the master or to nowhere, or recorded by tracks (their Record
// Input). It sounds in playback and in Real-Time Render, not in a normal render.
struct InputBus {
    uint64_t id = 0;
    std::string name;           // empty: Input Bus A, B ...
    std::string device;         // the sound card input (Qt Multimedia's id); empty: the one chosen in Preferences
    int first = 0;              // its first channel, from 0
    int channels = 2;           // 1 or 2
    double level = 1.0, pan = 0;
    bool mute = false, solo = false;
    uint64_t output = 0;        // 0 the master, else a bus
    bool outputOff = false;     // VEGAS's Output Off: the bus is not heard (a track can still record or monitor it)
    FxChain fx;
    std::vector<Send> sends;    // to buses and assignable effects chains
    std::string panType;        // as Track::panType
    SurroundPan surround;       // its place in a 5.1 project's master
};

// Another take of an event (VEGAS's takes): media it can show instead of its own. T / Shift+T switch takes.
struct Take {
    uint64_t mediaId = 0;
    double offset = 0;
    std::string name;
    bool reverse = false;       // the take plays its media backwards (VEGAS: a reversed subclip)
};

struct Event {
    uint64_t id = 0;
    uint64_t mediaId = 0;
    double start = 0;           // where it begins on the timeline
    double length = 0;
    double offset = 0;          // how far into the media it begins
    double rate = 1.0;          // playback rate (2 = twice as fast)
    double fadeIn = 0, fadeOut = 0;
    double gain = 1.0;          // opacity (video) or volume (audio), 0..1 and beyond for audio
    bool mute = false;
    bool loop = true;           // past the end of the media: start again (false: hold the last frame / silence)
    uint64_t group = 0;         // events with the same non-zero group move together (a clip's frame and its sound)
    bool reverse = false;       // played backwards
    double pitch = 0;           // audio: pitch change in semitones (applied on top of what the rate does)
    bool keepPitch = false;     // audio: a changed rate does not change the pitch
    // audio: VEGAS's Time Stretch/Pitch Shift settings (Audio Event Properties). method: "elastique" (VEGAS 18's
    // default), "classic", "none" (resample: speed and pitch change together). mode: for elastique "pro", "efficient",
    // "soloist-mono", "soloist-speech"; for classic VEGAS's numbered presets "A01".."A19". formant: formant shift in semitones.
    std::string stretchMethod = "elastique";
    std::string stretchMode = "pro";
    double formant = 0;
    bool preserveFormants = false;  // audio: VEGAS's "Preserve formants" (élastique Pro / Soloist (Monophonic)); 'formant' applies with it
    bool invertPhase = false;       // audio: VEGAS's Invert Phase switch
    std::string name;
    FxChain fx;
    FxSlot transition;          // video: the OpenFX transition used where this event overlaps the one before it (kind empty = cross-fade)
    std::vector<PanCropKey> panCrop;    // video: empty = the whole frame, fitted
    bool keepAspect = true;     // video: pan/crop keeps the media's shape
    // video: Event Pan/Crop's "Stretch to fill frame" (with keepAspect): the box's frame covers the whole frame (what
    // goes past its edges is cut off) instead of fitting inside it with bars.
    bool stretchToFill = false;
    // video: Event Pan/Crop's mask (see MaskKey). maskOn is VEGAS's Mask check box; maskApplyToFx its Apply to FX:
    // the event's effects show only inside the mask and the frame is not cut (outside it the frame is without them).
    std::vector<MaskKey> mask;
    bool maskOn = false;
    bool maskApplyToFx = false;
    std::vector<Take> takes;    // the event's OTHER takes (mediaId/offset above are the active one's)
    int activeTake = 0;         // where the active take stands among all of them, in VEGAS's order: takes[0 .. activeTake)
                                // come before it and the rest after (T / Shift+T and the multicamera keys go by that order)
    bool locked = false;        // VEGAS's Lock: cannot be moved, trimmed or deleted
    int fadeInCurve = CurveLinear, fadeOutCurve = CurveLinear;  // the shape of the fades
    // Where this event cross-fades in over an earlier one: the shape the earlier one fades out by (VEGAS's
    // FadeIn.ReciprocalCurve - VEGAS keeps a cross-fade's two curves on the later event). -1: not given, the earlier
    // event's own fade-out curve.
    int fadeInRecipCurve = -1;
    std::string channels = "both";      // audio: both, left (left only, on both sides), right, swap, combine (mono mix)
    double undersample = 1.0;   // video: 1 = every frame; 0.5 = new frames at half the project's rate (VEGAS's undersample rate)
    // VEGAS's event switches (Edit > Switches):
    // audio: VEGAS's Auto Normalize switch (Switches > Auto Normalize; VEGAS 22): Normalize whose gain is
    // worked out again from what the event plays whenever it is trimmed or split (normalize is on with it)
    bool autoNormalize = false;
    bool normalize = false;     // audio: Normalize to the peak level set in Preferences
    double normalizeGain = 1.0; // gain Normalize applies; 'gain' and the envelopes apply on top
    bool reduceFlicker = false; // video: Reduce Interlace Flicker - a light vertical blur of the frame
    // video: VEGAS's resample switches (Edit > Switches).
    std::string resample = "project";
    // audio: which of a multichannel file's own channels the event plays.
    int mediaChannel = -1;
    int mediaChannels = 0;
    // VEGAS's event snap offset (the white triangle at the event's bottom left, dragged): the point,
    // in seconds from the event's start, that snaps when the event is moved (0: its start)
    double snapOffset = 0;
    // VEGAS Pro 23's adjustment event (Insert > Adjustment Event): an event with no media whose effects
    // work on the frame of the tracks below it for as long as it lasts (fades, opacity, mute, cross-fades as an
    // ordinary video event's); not kept in a .veg (VEGAS 18's has none).
    bool adjustment = false;
    // VEGAS's sync link: the event this one is linked to (0: none). A linked event moves along when the
    // event it is linked to moves, but not the other way round (a one-way group).
    uint64_t syncLink = 0;
    Envelopes envelopes;        // volume, pan (audio), composite, velocity (video)
    VegOriginPtr veg;

    double end() const { return start + length; }
};

struct Track {
    uint64_t id = 0;
    TrackType type = TrackType::Video;
    std::string name;
    bool mute = false, solo = false;
    double level = 1.0;         // opacity (video) or volume (audio)
    double pan = 0;             // audio: -1 left .. 1 right
    // audio: how the pan slider (and pan envelope) shares the sound between left and right (VEGAS's Pan Type, per
    // track): "" or "balance0" Balance (0 dB center), "balance3" / "balance6" Balance (-3 / -6 dB center), "add" Add
    // Channels, "power" Constant Power, "film" Film (in stereo the same as Constant Power).
    std::string panType;
    FxChain fx;
    RecordInput recordInput;    // audio: where it records from when armed
    // kept in order of start time. Copies of a track share the events until one changes them (CowVec: the editor copies
    // the project for every edit and undo step); read through a const reference where nothing is changed.
    CowVec<Event> events;
    uint64_t output = 0;        // audio: where the track's sound goes: 0 the master, else a bus id
    std::vector<Send> sends;    // audio: shares sent to buses as well
    std::vector<MotionKey> motion;      // video: empty = fills the frame
    std::string composite = "srcalpha"; // video: how it is laid over the tracks below (VEGAS's compositing modes)
    uint32_t color = 0;         // the colour of its header (0 = by position)
    int height = 0;             // in the timeline (0 = default)
    Envelopes envelopes;        // see Envelope for which kinds a track can have
    uint32_t fadeTop = 0xFFFFFFFF, fadeBottom = 0xFF000000;    // video: the colours of the "fadecolor" envelope (ARGB)
    uint64_t trackGroup = 0;    // tracks with the same non-zero id form a VEGAS track group (mute/solo/collapse together)
    bool adjustment = false;    // video: an adjustment track - its effects apply to the frame of the tracks below it
    int compositeLevel = 0;     // video: VEGAS's compositing parent/child; 1 = a child of the nearest track above at level 0
    // video, for a track that has compositing children (VEGAS's "Parent Composite Mode" and "Parent Motion"): how the
    // finished group (the children composited together, the parent's own frame laid over them with 'composite') is
    // laid over the tracks below, and where the whole group is moved to.
    std::string parentComposite = "srcalpha";
    std::vector<MotionKey> parentMotion;
    std::vector<GlowKey> shadow2d, glow2d;      // video: Track Motion's 2D Shadow and 2D Glow (empty: off)
    VegOriginPtr veg;
    SurroundPan surround;       // audio: its place in a 5.1 project's master (see SurroundPan)
    // VEGAS's Minimize Track Height (the track header's minimize button): the track is drawn at the smallest height,
    // and 'height' is the height it goes back to when restored.
    bool minimized = false;
    // video: VEGAS's Expand Track Layers (track header menu): the track's lane shows its events on three sub-lanes,
    // the A roll, the B roll (sequential events alternate between them) and the transition roll (where events
    // overlap).
    bool layersExpanded = false;
    // video: VEGAS's Bypass Motion Blur (the track header's button / hamburger menu): the track's frame is left out
    // of the output's motion blur and supersampling (it stays sharp).
    bool bypassMotionBlur = false;
    // audio: VEGAS's Invert Phase switch of the track (Switches > Invert Phase in the track header's menu): the track's
    // sound is turned upside down (multiplied by -1) before its volume and pan.
    bool invertPhase = false;
    // VEGAS's Automation Settings button and the track's automation mode.
    bool automation = false;
    std::string automationMode = "read";
    // the automation values (VEGAS's parameters behind the automation controls): what applies where the track has no
    // envelope of that kind, or in Off mode instead of it - volume (gain) / composite level, pan, mute, fade to colour
    double autoLevel = 1.0, autoPan = 0.0, autoFade = 0.0;
    bool autoMute = false;
    // the automation applies: in Off mode the envelopes are left out
    bool automationOff() const { return automation && automationMode == "off"; }
    // audio: VEGAS's Auto Ducking (track list > More > Auto Ducking Controls): a controller track lowers
    // its listener tracks' volume where it has sound - to 'duckFloor' dB under the sound's threshold, and by
    // 'duckPeak' (0..1) more with the sound's peaks (engine/automation.h: autoDuckGain).
    bool duckController = false, duckListener = false;
    double duckFloor = -12.0, duckPeak = 0.0;

    void sortEvents();
};

// VEGAS 18's Master Display (Project Properties > Video: a combo "Custom display" and a Customize button, available
// with an HDR mode): the display the HDR frame was graded on.
struct MasterDisplay {
    std::string preset = "custom";
    double redX = 0.708, redY = 0.292, greenX = 0.170, greenY = 0.797, blueX = 0.131, blueY = 0.046;   // Rec.2020 primaries
    double whiteX = 0.3127, whiteY = 0.3290;                                                           // D65
    double maxNits = 1000.0, minNits = 0.005;
    int maxCll = 0, maxFall = 0;                // content light levels in nits (0: not written)
    bool operator==(const MasterDisplay& o) const
    {
        return preset == o.preset && redX == o.redX && redY == o.redY && greenX == o.greenX && greenY == o.greenY && blueX == o.blueX &&
               blueY == o.blueY && whiteX == o.whiteX && whiteY == o.whiteY && maxNits == o.maxNits && minNits == o.minNits &&
               maxCll == o.maxCll && maxFall == o.maxFall;
    }
    bool operator!=(const MasterDisplay& o) const { return !(*this == o); }
};

struct ProjectSettings {
    int width = 1920, height = 1080;
    int fpsNum = 30, fpsDen = 1;
    int sampleRate = 48000;
    double fps() const { return fpsDen > 0 ? (double)fpsNum / fpsDen : 30.0; }
    // the rest of VEGAS's Project Properties
    std::string fieldOrder = "none";            // none, upper, lower
    double pixelAspect = 1.0;
    std::string pixelFormat = "8bit";           // 8bit, 32bit (float)
    std::string renderQuality = "good";         // draft, preview, good, best
    std::string resample = "smart";             // smart, force, optical, off (frame-rate resampling)
    std::string motionBlur = "gaussian";
    std::string deinterlace = "none";           // none, blend, interpolate
    int audioBits = 16;
    std::string rulerFormat = "timecode";       // timecode (Time & Frames), seconds, frames, samples, measures, and VEGAS's others: timefmt.h
    double rulerStart = 0;
    double tempo = 120.0;                       // for measures and beats
    int beatsPerMeasure = 4;
    std::string title, artist, engineer, copyright, comments;
    int outputRotation = 0;                     // VEGAS's Output rotation: 0, 90 (clockwise), 180, 270 (90 counter-clockwise);
                                                // Render As turns the frame by it when "Use project output rotation setting" is on
    // VEGAS's Master bus mode: 2 (Stereo) or 6 (5.1 Surround: the master has six channels L R C LFE Ls Rs, and tracks and
    // buses are placed in it by their surround panners; see SurroundPan and AudioEngine)
    int audioChannels = 2;
    bool surround() const { return audioChannels == 6; }
    // 5.1 only: VEGAS's "Enable low-pass filter on LFE", its cutoff (Hz) and quality ("best", "good", "fast")
    bool lfeLowpass = true;
    double lfeCutoff = 120.0;
    std::string lfeQuality = "best";
    // the rest of VEGAS 18's Project Properties, kept with the project (Reno's own files; a .veg does not
    // hold them yet). pixelRange: VEGAS 18's pixel formats are pixelFormat x pixelRange - "8-bit (full range)" (its
    // default), "Legacy 8-bit (video levels)", "32-bit floating point (video levels)" / "(full range)"; Reno decodes
    // to full range in all of them.
    std::string pixelRange = "full";            // full, video
    double compositingGamma = 2.222;            // 1.000 (Linear) or 2.222 (Video); VEGAS offers it with 32-bit full range
    std::string hdrMode = "off";                // off, hdr10, hlg
    MasterDisplay masterDisplay;                // Project Properties > Video > Master Display (used with an HDR mode)
    bool output360 = false;                     // VEGAS's 360° Output
    std::string acesVersion = "1.1";            // ACES version / color space / view transform / look modification transform
    std::string acesColorSpace = "aces2065-1";
    std::string viewTransform = "off";
    std::string lookTransform = "none";
    bool adjustSourceMedia = true;              // "Adjust source media to better match project or render settings"
    std::string resampleQuality = "best";       // Audio: "Resample and stretch quality" (draft, preview, good, best)
    int noteGetsBeat = 4;                       // Ruler: "Note that gets one beat"
    std::string cdUpc;                          // Audio CD: Universal Product Code / Media Catalog Number
    int cdFirstTrack = 1;                       // Audio CD: First track number on disc
    // VEGAS's Stereoscopic 3D mode (Project Properties > Video).
    std::string stereoMode = "off";
    bool stereoSwap = false;
    double crosstalk = 0;
    bool crosstalkInRender = true;
    bool stereo() const { return !stereoMode.empty() && stereoMode != "off"; }
    // VEGAS's 360° Output is 'output360' above; the color management (ACES) fields above are used by the
    // engine when the project is 32-bit floating point (full range) with compositing gamma 1.000 (Linear) or a view
    // transform (colormgmt.h linearLightProject)
};

struct Project {
    ProjectSettings settings;
    std::vector<MediaItem> media;
    std::vector<Track> tracks;
    FxChain videoOut;           // effects on the whole frame
    FxChain audioOut;           // effects on the whole mix (the master bus)
    double masterVolume = 1.0;
    // VEGAS's Master Bus output fader has a thumb for each side: masterVolume is the left one and this the right one;
    // below 0 the right one is the same (the faders are locked together, as they are by default).
    double masterVolumeR = -1;
    double masterRightVolume() const { return masterVolumeR >= 0 ? masterVolumeR : masterVolume; }
    // 5.1: the Surround Master's own fader for each channel (L R C LFE Ls Rs) besides the master volume; not automated
    double masterTrim[6] = {1, 1, 1, 1, 1, 1};
    double masterPan = 0;
    bool masterMute = false;
    bool masterFxPostFader = false;     // the master's effects after its volume (VEGAS's Post FX / Post Volume)
    std::vector<Bus> buses;
    std::vector<InputBus> inputBuses;   // VEGAS Pro 23's input busses (at most 26)
    Envelopes masterEnvelopes;          // the master audio bus track: volume, mute, fx
    Envelopes videoEnvelopes;           // the video bus track: fadecolor, mute, motionblur, supersample, fx
    uint32_t videoFadeTop = 0xFFFFFFFF, videoFadeBottom = 0xFF000000;
    struct Marker {
        uint64_t id = 0; double time = 0; double length = 0; std::string name; bool region = false; VegOriginPtr veg;
        std::string kind;       // "" a marker or region; "command" (VEGAS's command markers: 'command' + 'name' as the parameter), "cdtrack" (a CD track region), "cdindex"
        std::string command;
        std::string comment;    // a command marker's Comment (Command Properties; in a .veg its first text)
    };
    std::vector<Marker> markers;        // markers and regions
    // Project Media's bins: every user bin's path ("Interviews", "Interviews/Day 1"), kept even when empty, and the
    // smart bins (a name and the search they show).
    std::vector<std::string> mediaBins;
    std::vector<std::pair<std::string, std::string>> smartBins;
    uint64_t nextId = 1;
    VegOriginPtr veg;                   // the VEGAS project file it was read from (see VegOrigin)
    // Options > Automatic Crossfades (an option of the editor, not saved with the project: AppState keeps it set to
    // the option): where two events overlap on a track they cross-fade.
    bool autoCrossfades = true;
    // Options > Quickfade Audio Edits and Preferences > Editing's "Quick fade length for audio events" (an option of
    // the editor like autoCrossfades, not saved with the project): an audio event's start and end that have no fade
    // and no cross-fade get a short straight fade this long (seconds), so cuts do not click.
    double quickFade = 0;

    uint64_t newId() { return nextId++; }
    double length() const;                                  // where the last event ends
    const MediaItem* findMedia(uint64_t id) const;
    MediaItem* findMedia(uint64_t id);
    Track* findTrack(uint64_t id);
    Bus* findBus(uint64_t id);
    const Bus* findBus(uint64_t id) const;
    InputBus* findInputBus(uint64_t id);
    const InputBus* findInputBus(uint64_t id) const;
    // The event with this id, and the track it is on..
    Event* findEvent(uint64_t id, Track** track = nullptr);
    const Event* findEvent(uint64_t id, const Track** track = nullptr) const;
    struct EventIndex;
    mutable std::shared_ptr<const EventIndex> eventIndex_;      // (see findEvent)
    // The ids of all events (one pass; for code that looks at many ids)
    void collectEventIds(std::vector<uint64_t>& out) const;
    FxSlot* findFx(uint64_t slotId);
    // Every effect slot in the project (chains, transitions, generators).
    void forEachFx(const std::function<void(FxSlot&)>& fn);
    void forEachFx(const std::function<void(const FxSlot&)>& fn) const;
    bool anySolo(TrackType t) const;

    // Snaps a time to the nearest frame boundary.
    double quantize(double t) const;
    int64_t frameAt(double t) const;                        // the frame showing at time t
    double frameTime(int64_t frame) const;

    Json toJson() const;
    bool fromJson(const Json& j, std::string* error = nullptr);
    bool save(const std::string& path, std::string* error = nullptr) const;
    bool load(const std::string& path, std::string* error = nullptr);
};

// How much media time (before the event's rate) has gone by 'u' seconds into the event: u itself, or with a
// "velocity" envelope the area under it (speed x time, piece by piece along its curves; negative speed goes back).
double eventVelocityIntegral(const Event& e, double u);
// For an event at timeline time t: the time in its media, taking offset, rate, velocity and looping into account.
double eventMediaTime(const Event& e, const MediaItem* m, double t);
// VEGAS Pro 23's Slow Motion effect (an OpenFX slot of kSlowMotionId on a video event): the playback rate it multiplies the
// event's by (1 without it), and the resample mode it asks for ("force", "optical", "off"; empty without it).
extern const char* const kSlowMotionId;
extern const char* const kTimeWarpId;       // VEGAS Pro 18's version of the same effect (de.magix:TimeWarp), with its own names for the settings
double eventFxRate(const Event& e);
std::string eventFxResample(const Event& e);
// VEGAS's sample grid: the sample a timeline time falls on at 'rate' is the nearest one, so an event
// plays the samples [timeToSample(start), timeToSample(end)) and starts reading its media at its offset rounded to a
// sample of the media's own rate (mediaSampleTime) - both measured against the mixes VEGAS Pro 18 writes (.sfap0).
int64_t timeToSample(double t, int rate);
double mediaSampleTime(double seconds, const MediaItem& m);
// The event's fade level at timeline time t (0..1), from its fade-in and fade-out and their curves.
double eventFade(const Event& e, double t);
// A new event's fade curves as VEGAS gives them (and keeps in every sample project): Fast in and Slow out for sound
// (together a cross-fade that keeps the loudness), Smooth in and out for frames (their cross-fade adds up to 1).
void setDefaultFadeCurves(Event& e, TrackType type);
// The fade-in curve new events get (the fade-out gets its mirror: Slow for Fast...): VEGAS's Preferences > External
// Control & Automation "New audio event gain" / "New video event opacity", which the interface puts here.
struct DefaultFadeTypes {
    int audio = CurveFast, video = CurveSmooth;
};
DefaultFadeTypes& defaultFadeTypes();
// The left and right gains of a sound at 'level' panned to 'pan' (-1 .. 1) by pan type 'type' (Track::panType).
struct PanGains { float ll = 1, rl = 0, lr = 0, rr = 1; };
PanGains panGains(const std::string& type, double level, double pan);

}  // namespace reno
