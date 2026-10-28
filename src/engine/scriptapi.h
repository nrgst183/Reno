/* C interface between VEGAS scripts and Reno (Tools > Scripting > Run Script).
 *
 * Scripts are compiled against src/scripting/ScriptPortal.Vegas.cs, which mirrors VEGAS's scripting API; its calls
 * land here and are served by a ScriptSession (scriptsession.h) working on a copy of the project.
 *
 * Every object a script sees is a handle: a number that stays the same for the whole run. Handle 1 is the
 * application. Properties are read/written by property number, lists with 'list', everything else is an operation
 * number given to 'call'.
 *
 * Every function returns 0 (done), 1 (failed, see 'error') or 2 (does not apply to this kind of object).
 * Strings are UTF-8, times are seconds. The numbers are repeated in the C# class Abi; keep this file plain C. */
#ifndef RENO_SCRIPTAPI_H
#define RENO_SCRIPTAPI_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RENO_SCRIPT_API_VERSION 1

enum RenoScriptStatus { RS_OK = 0, RS_FAILED = 1, RS_UNSUPPORTED = 2 };

/* What a handle is (the 'kind' function). */
enum RenoScriptKind {
    RK_NONE = 0,
    RK_APP = 1,         /* VEGAS's Vegas object, its Project and Transport (handle 1) */
    RK_TRACK = 2,
    RK_EVENT = 3,
    RK_TAKE = 4,        /* an event's take, by its place among the event's takes */
    RK_MEDIA = 5,
    RK_STREAM = 6,      /* a media item's video or audio stream */
    RK_MARKER = 7,      /* markers, regions, command markers, CD track regions and indexes */
    RK_FX = 8,          /* an effect in a chain (or a transition, or a generator) */
    RK_PLUGIN = 9,      /* an installed plug-in (VEGAS's PlugInNode) */
    RK_ENVELOPE = 10,
    RK_ENVPOINT = 11,
    RK_GROUP = 12,      /* a group of events (VEGAS's TrackEventGroup) */
    RK_PANCROP = 13,    /* a key of an event's pan/crop (VEGAS's VideoMotionKeyframe) */
    RK_BUS = 14,        /* an audio bus (VEGAS's AudioBusTrack) */
    RK_MOTION = 15,     /* a key of a video track's track motion (VEGAS's TrackMotionKeyframe) */
    RK_OFXPARAM = 16,   /* a parameter of an OpenFX effect (VEGAS's OFXParameter) */
    RK_OFXKEY = 17,     /* a keyframe of one (VEGAS's OFXKeyframe) */
    RK_TRACKGROUP = 18, /* a group of tracks (VEGAS's TrackGroup; Track::trackGroup) */
    RK_BIN = 19,        /* a media bin (VEGAS's MediaBin); the root is the Project Media folder */
    RK_PRESET = 20,     /* an OpenFX effect preset (VEGAS's EffectPreset) of a plug-in */
    RK_RENDERER = 21,   /* a Render As format (VEGAS's Renderer) */
    RK_RTEMPLATE = 22   /* one of its templates (VEGAS's RenderTemplate) */
};

/* Properties. Types: I integer, D number, S string, H handle (as an integer). Which kinds have which is in scriptsession.cpp. */
enum RenoScriptProp {
    /* the application and its project (RK_APP) */
    RP_VERSION = 1,             /* S */
    RP_APP_NAME = 2,            /* S */
    RP_INSTALL_DIR = 3,         /* S */
    RP_APPDATA_DIR = 4,         /* S */
    RP_TEMP_DIR = 5,            /* S */
    RP_SCRIPT_FILE = 6,         /* S the script being run */
    RP_SCRIPT_ARGS = 7,         /* S */
    RP_PROJECT_PATH = 8,        /* S (empty: untitled) */
    RP_CURSOR = 9,              /* D */
    RP_SEL_START = 10,          /* D the time selection (= the loop region) */
    RP_SEL_LENGTH = 11,         /* D */
    RP_LOOP_MODE = 12,          /* I */
    RP_IS_MODIFIED = 13,        /* I */
    RP_PROJECT_LENGTH = 14,     /* D */
    RP_WIDTH = 15,              /* I */
    RP_HEIGHT = 16,             /* I */
    RP_FRAME_RATE = 17,         /* D */
    RP_FIELD_ORDER = 18,        /* I VEGAS's VideoFieldOrder */
    RP_PIXEL_ASPECT = 19,       /* D */
    RP_PIXEL_FORMAT = 20,       /* I VEGAS's PixelFormat */
    RP_RENDER_QUALITY = 21,     /* I VEGAS's VideoRenderQuality */
    RP_MOTION_BLUR = 22,        /* I VEGAS's MotionBlurType */
    RP_DEINTERLACE = 23,        /* I VEGAS's VideoDeinterlaceMethod */
    RP_OUTPUT_ROTATION = 24,    /* I degrees */
    RP_SAMPLE_RATE = 25,        /* I */
    RP_BIT_DEPTH = 26,          /* I */
    RP_BUS_MODE = 27,           /* I VEGAS's AudioBusMode */
    RP_RULER_FORMAT = 28,       /* I VEGAS's RulerFormat */
    RP_RULER_START = 29,        /* D */
    RP_TEMPO = 30,              /* D */
    RP_BEATS_PER_MEASURE = 31,  /* I */
    RP_TITLE = 32,              /* S project summary */
    RP_ARTIST = 33,             /* S */
    RP_ENGINEER = 34,           /* S */
    RP_COPYRIGHT = 35,          /* S */
    RP_COMMENTS = 36,           /* S */
    RP_BYPASS_FX = 37,          /* I */
    RP_IS_PLAYING = 38,         /* I (always 0 while a script runs) */
    RP_UNDO_LABEL = 39,         /* S the name of the run's undo step (VEGAS's UndoBlock label) */
    RP_UNDO_CANCEL = 40,        /* I 1: the run's changes are dropped (VEGAS's UndoBlock.Cancel) */
    RP_MAIN_WINDOW = 41,        /* I the main window (Windows: its HWND) for the script's dialogs */

    /* shared by several kinds */
    RP_NAME = 50,               /* S track, event, take, media, plug-in, effect, envelope; marker label */
    RP_INDEX = 51,              /* I place in its list */
    RP_MEDIA_TYPE = 52,         /* I VEGAS's MediaType: 2 audio, 3 video (track, event, stream) */
    RP_MUTE = 53,               /* I track, event, bus */
    RP_SOLO = 54,               /* I track, bus */
    RP_SELECTED = 55,           /* I track, event */
    RP_START = 56,              /* D event, marker (position) */
    RP_LENGTH = 57,             /* D event, marker (region), take, media, stream; track: where its last event ends */
    RP_LEVEL = 58,              /* D track (volume, composite level), event (VEGAS's FadeIn.Gain), bus */
    RP_PAN = 59,                /* D audio track (PanX), bus */
    RP_MEDIA = 60,              /* H take, stream, event (its active take's) */
    RP_PATH = 61,               /* S media file, take (MediaPath) */
    RP_COMMENT = 62,            /* S media */
    RP_IS_VALID = 63,           /* I the object still exists */

    /* tracks */
    RP_COMPOSITE_MODE = 70,     /* I VEGAS's CompositeMode */
    RP_PARENT_COMPOSITE_MODE = 71, /* I */
    RP_NESTING_LEVEL = 72,      /* I */
    RP_TOP_FADE_COLOR = 73,     /* I ARGB */
    RP_BOTTOM_FADE_COLOR = 74,  /* I ARGB */
    RP_TRACK_BUS = 75,          /* H the bus its sound goes to (0: the master) */

    /* events */
    RP_END = 80,                /* D */
    RP_RATE = 81,               /* D playback rate */
    RP_LOCKED = 82,             /* I */
    RP_LOOP = 83,               /* I */
    RP_TRACK = 84,              /* H (set: moves the event to that track) */
    RP_GROUP = 85,              /* H (0: not grouped) */
    RP_ACTIVE_TAKE = 86,        /* H */
    RP_FADE_IN = 87,            /* D length */
    RP_FADE_OUT = 88,           /* D */
    RP_FADE_IN_CURVE = 89,      /* I VEGAS's CurveType */
    RP_FADE_OUT_CURVE = 90,     /* I */
    RP_MAINTAIN_ASPECT = 91,    /* I video */
    RP_REDUCE_INTERLACE = 92,   /* I video */
    RP_RESAMPLE_MODE = 93,      /* I video: VEGAS's VideoResampleMode */
    RP_UNDERSAMPLE = 94,        /* D video */
    RP_NORMALIZE = 95,          /* I audio */
    RP_NORMALIZE_GAIN = 96,     /* D audio */
    RP_INVERT_PHASE = 97,       /* I audio */
    RP_CHANNELS = 98,           /* I audio: VEGAS's ChannelRemapping */
    RP_STRETCH_METHOD = 99,     /* I audio: VEGAS's TimeStretchPitchShift */
    RP_PITCH_LOCK = 100,        /* I audio: the pitch follows the rate (VEGAS's "Lock to stretch") */
    RP_PITCH_SEMIS = 101,       /* D audio */
    RP_CLASSIC_ATTR = 102,      /* I audio: VEGAS's ClassicStretchAttributes */
    RP_ELASTIQUE_ATTR = 103,    /* I audio: VEGAS's ElastiqueStretchAttributes */
    RP_FORMANT_LOCK = 104,      /* I audio: preserve formants */
    RP_FORMANT_SHIFT = 105,     /* D audio */
    RP_TRANSITION = 106,        /* H the transition on its fade-in (0: a plain cross-fade) */
    RP_MEDIA_WIDTH = 107,       /* I video: its active media's picture size (pan/crop is in its pixels) */
    RP_MEDIA_HEIGHT = 108,      /* I */
    RP_REVERSE = 109,           /* I Reno: the event plays backwards (a VEGAS script reverses an event with a reversed subclip) */

    /* takes */
    RP_OFFSET = 120,            /* D how far into the media */
    RP_IS_ACTIVE = 121,         /* I */
    RP_STREAM = 122,            /* H */
    RP_AVAILABLE_LENGTH = 123,  /* D */

    /* media and streams */
    RP_HAS_VIDEO = 130,         /* I */
    RP_HAS_AUDIO = 131,         /* I */
    RP_IS_GENERATED = 132,      /* I */
    RP_IS_SUBCLIP = 133,        /* I */
    RP_IS_OFFLINE = 134,        /* I */
    RP_USE_COUNT = 135,         /* I */
    RP_MEDIA_ID = 136,          /* I */
    RP_GENERATOR = 137,         /* H the generator effect of generated media */
    RP_SUBCLIP_START = 138,     /* D */
    RP_IS_REVERSED = 139,       /* I a reversed subclip */
    RP_STREAM_WIDTH = 140,      /* I */
    RP_STREAM_HEIGHT = 141,     /* I */
    RP_STREAM_FPS = 142,        /* D */
    RP_STREAM_CHANNELS = 143,   /* I */
    RP_STREAM_SAMPLE_RATE = 144, /* I */
    RP_STREAM_BITS = 145,       /* I */
    RP_STREAM_FORMAT = 146,     /* S the codec */
    RP_STREAM_FIELD_ORDER = 147, /* I */
    RP_STREAM_PIXEL_ASPECT = 148, /* D */

    /* markers */
    RP_MARKER_KIND = 150,       /* I 0 marker, 1 region, 2 command marker, 3 CD track region, 4 CD index */
    RP_COMMAND_TYPE = 151,      /* S command markers: the command ("URL", "TEXT"...) */

    /* effects and plug-ins */
    RP_BYPASS = 160,            /* I */
    RP_BEFORE_PANCROP = 161,    /* I */
    RP_PLUGIN = 162,            /* H the effect's plug-in (0 if it is not installed) */
    RP_IS_OFX = 163,            /* I */
    RP_GROUP_NAME = 164,        /* S plug-in */
    RP_UNIQUE_ID = 165,         /* S plug-in: the OpenFX identifier or audio plug-in key */
    RP_CLASS_ID = 166,          /* S plug-in: a GUID ("{...}"): the DirectX CLSID or VST3 class, else made from the unique id */
    RP_PLUGIN_TYPE = 167,       /* I VEGAS's PlugInType */
    RP_IS_VIDEO = 168,          /* I */
    RP_IS_AUDIO = 169,          /* I */

    /* envelopes and their points */
    RP_ENV_TYPE = 180,          /* I VEGAS's EnvelopeType */
    RP_ENV_MIN = 181,           /* D */
    RP_ENV_MAX = 182,           /* D */
    RP_ENV_NEUTRAL = 183,       /* D */
    RP_X = 184,                 /* D point: time (from the event's start for an event's envelope) */
    RP_Y = 185,                 /* D point: value */
    RP_CURVE = 186,             /* I point: VEGAS's CurveType */

    /* pan/crop and track motion keys (RK_PANCROP, RK_MOTION) */
    RP_KEY_TIME = 200,          /* D from the event's start (pan/crop) or on the timeline (track motion) */
    RP_KEY_X = 201,             /* D pan/crop: centre in the media as fractions of its size; motion: centre in the frame, fractions */
    RP_KEY_Y = 202,             /* D */
    RP_KEY_W = 203,             /* D size, fractions */
    RP_KEY_H = 204,             /* D */
    RP_KEY_ROTATION = 205,      /* D degrees, clockwise (Reno's) */
    RP_KEY_TYPE = 206,          /* I VEGAS's VideoKeyframeType */
    RP_KEY_SMOOTHNESS = 207,    /* D */
    /* 3D track motion (RK_MOTION; used where the track's compositing mode is 3D Source Alpha); distances are fractions of
     * the frame's width, angles degrees (Reno's MotionKey::z, d, rotX, rotY, offX/Y/Z, oriX/Y/Z) */
    RP_KEY_Z = 210,             /* D */
    RP_KEY_DEPTH = 211,         /* D */
    RP_KEY_ROT_X = 212,         /* D */
    RP_KEY_ROT_Y = 213,         /* D */
    RP_KEY_OFF_X = 214,         /* D rotation offset */
    RP_KEY_OFF_Y = 215,         /* D */
    RP_KEY_OFF_Z = 216,         /* D */
    RP_KEY_ORI_X = 217,         /* D orientation */
    RP_KEY_ORI_Y = 218,         /* D */
    RP_KEY_ORI_Z = 219,         /* D */

    /* OpenFX parameters (RK_OFXPARAM) and keyframes (RK_OFXKEY); values go through RO_OFX_GET/SET. */
    RP_OFX_TYPE = 220,          /* I VEGAS's OFXParameterType */
    RP_OFX_LABEL = 221,         /* S */
    RP_OFX_HINT = 222,          /* S */
    RP_OFX_PARENT = 223,        /* S */
    RP_OFX_ENABLED = 224,       /* I */
    RP_OFX_SECRET = 225,        /* I */
    RP_OFX_CAN_ANIMATE = 226,   /* I */
    RP_OFX_PERSIST = 227,       /* I */
    RP_OFX_ANIMATED = 228,      /* I (set 0: its keyframes are removed) */
    RP_OFX_DIM = 229,           /* I how many numbers its value has (0: text) */
    RP_OFX_CHOICES = 230,       /* S a choice parameter's choices, one per line */
    RP_OFX_TEXT = 231,          /* S a text parameter's value (a keyframe's text) */
    RP_OFX_INTERP = 232,        /* I keyframe: VEGAS's OFXInterpolationType */
    RP_OFX_EFFECT = 233,        /* H the effect a parameter belongs to */
    RP_OFX_PLUGIN_LABEL = 234,  /* S effect: its plug-in's label */
    RP_OFX_PLUGIN_GROUP = 235,  /* S */
    RP_OFX_PLUGIN_PATH = 236,   /* S */
    RP_OFX_CURRENT_TIME = 237,  /* D effect: VEGAS's OFXEffect.CurrentTime (what a Value set on an animated parameter changes) */
    RP_FADE_IN_RECIP_CURVE = 238,   /* I event: VEGAS's FadeIn.ReciprocalCurve (the earlier event's side of a cross-fade) */
    RP_OUTPUT_360 = 400,            /* I project: VEGAS's T360Output (0 On, 1 Off): Project Properties' 360° output */
    RP_HDR_MODE = 401,              /* I project: VEGAS's HDRMode (0 HDR10, 1 HLG, 2 Off) */
    RP_STEREO_MODE = 402,           /* I project: VEGAS's Stereo3DOutputMode (14, LeftAndRight, is not offered by Reno) */
    RP_STEREO_SWAP = 403,           /* I project: swap left and right (of the current mode) */
    RP_STEREO_CROSSTALK = 404,      /* D project: crosstalk cancelation, 0..1 (of the current mode) */
    RP_STEREO_CROSSTALK_RENDER = 405, /* I project: crosstalk cancelation is included in renders */
    RP_TRACK_GROUP = 410,           /* H track: its track group (0: none) */
    RP_PARENT = 411,                /* H bin: the bin it is in (0: the root has none) */
    RP_MEDIA_BIN = 412,             /* H media: the bin it is in (the root bin if none) */
    /* Render As formats (RK_RENDERER; RP_NAME is the format's name) and templates (RK_RTEMPLATE) */
    RP_FILE_EXT = 420,              /* S renderer, template: the file name extension, without the dot */
    RP_DESCRIPTION = 421,           /* S renderer (its blurb), template */
    RP_RT_HAS_VIDEO = 422,          /* I renderer, template */
    RP_RT_HAS_AUDIO = 423,          /* I */
    RP_RT_WIDTH = 424,              /* I template: 0 is the project's */
    RP_RT_HEIGHT = 425,             /* I */
    RP_RT_FPS = 426,                /* D */
    RP_RT_SAMPLE_RATE = 427,        /* I */
    RP_RT_CHANNELS = 428,           /* I */
    RP_RT_BITS = 429,               /* I */
    RP_RT_VIDEO_KBPS = 430,         /* I */
    RP_RT_AUDIO_KBPS = 431,         /* I */
    RP_RT_RENDERER = 432,           /* H template: its format */
    RP_RT_PAR = 433                 /* D template: pixel aspect ratio (0: the project's) */
};

/* Lists ('list'). */
enum RenoScriptList {
    RL_TRACKS = 1,              /* app */
    RL_MEDIA = 2,               /* app: the project's media */
    RL_MARKERS = 3,             /* app: markers (RK_MARKER kind 0) */
    RL_REGIONS = 4,             /* app */
    RL_COMMAND_MARKERS = 5,     /* app */
    RL_CD_TRACKS = 6,           /* app */
    RL_CD_INDICES = 7,          /* app */
    RL_GROUPS = 8,              /* app */
    RL_VIDEO_FX = 9,            /* app: installed plug-ins */
    RL_AUDIO_FX = 10,           /* app */
    RL_TRANSITIONS = 11,        /* app */
    RL_GENERATORS = 12,         /* app */
    RL_BUSES = 13,              /* app */
    RL_EVENTS = 20,             /* track, group */
    RL_EFFECTS = 21,            /* track, event, bus; app: the video output's (VideoBus); RL_MASTER_EFFECTS the master's */
    RL_ENVELOPES = 22,          /* track, event, bus; app: the video output's */
    RL_TAKES = 23,              /* event */
    RL_STREAMS = 24,            /* media */
    RL_POINTS = 25,             /* envelope */
    RL_PANCROP = 26,            /* event (always at least one key, as in VEGAS) */
    RL_MOTION = 27,             /* video track */
    RL_MASTER_EFFECTS = 28,     /* app */
    RL_MASTER_ENVELOPES = 29,   /* app */
    RL_SELECTED_MEDIA = 30,     /* app */
    RL_SPECIAL_BUSES = 31,      /* app: the master bus and the video output (RK_BUS) */
    RL_OFX_PARAMS = 32,         /* effect: its OpenFX parameters (fails for an effect whose plug-in cannot be loaded) */
    RL_OFX_KEYS = 33,           /* OpenFX parameter: its keyframes, by time */
    RL_TRACK_GROUPS = 44,       /* app */
    RL_GROUP_TRACKS = 45,       /* track group: its tracks */
    RL_BIN_BINS = 46,           /* bin: the bins in it */
    RL_BIN_MEDIA = 47,          /* bin: the media in it */
    RL_PRESETS = 48,            /* plug-in or effect: its presets (OpenFX effects; none for audio effects), in VEGAS's order */
    RL_RENDERERS = 49,          /* app: Render As's formats */
    RL_RENDER_TEMPLATES = 50    /* renderer: its templates */
};

/* Operations ('call'): arguments are ia (integers), da (numbers), s (string); results are out (handle) and dout (number). */
enum RenoScriptOp {
    RO_ADD_TRACK = 1,           /* app: ia[0] 2 audio / 3 video, ia[1] index (-1 at the end); s name -> track */
    RO_REMOVE_TRACK = 2,        /* app: ia[0] track */
    RO_IMPORT_MEDIA = 3,        /* app: s path -> media (the one already there if the file is in the project) */
    RO_FIND_MEDIA = 4,          /* app: s path -> media or 0 */
    RO_REMOVE_MEDIA = 5,        /* app: ia[0] media (fails if events use it) */
    RO_ADD_SUBCLIP = 6,         /* app: s "path\nname", da[0] start, da[1] length, ia[0] reverse -> media */
    RO_ADD_MARKER = 7,          /* app: ia[0] kind (RP_MARKER_KIND), da[0] position, da[1] length; s "label" or "command\nlabel" -> marker */
    RO_REMOVE_MARKER = 8,       /* app: ia[0] marker */
    RO_NEW_GROUP = 9,           /* app -> group (empty until events are added) */
    RO_REMOVE_GROUP = 10,       /* app: ia[0] group (its events stay, ungrouped) */
    RO_NEW_PROJECT = 11,        /* app: an empty project with the default settings */
    RO_SAVE_PROJECT = 12,       /* app: s path (empty: where it is) */
    RO_OPEN_PROJECT = 13,       /* app: s path */
    RO_LOG = 14,                /* app: s a line for the script's output (VEGAS's DebugOut) */
    RO_INSERT_TIME = 15,        /* app: da[0] at, da[1] length */
    RO_REMOVE_UNUSED_MEDIA = 16, /* app */
    RO_GENERATED_MEDIA = 17,    /* app: ia[0] generator plug-in; s preset name -> media */
    RO_MOVE_TRACK = 18,         /* app: ia[0] track, ia[1] new index */
    RO_UPDATE_UI = 19,          /* app: nothing to do while a script runs (the project is shown when it ends) */

    RO_ADD_EVENT = 30,          /* track: da[0] start, da[1] length; s name -> event */
    RO_REMOVE_EVENT = 31,       /* track: ia[0] event */
    RO_ADD_ENVELOPE = 32,       /* track, event, bus, app (video output), RO_ADD_MASTER_ENVELOPE: ia[0] VEGAS's EnvelopeType -> envelope */
    RO_REMOVE_ENVELOPE = 33,    /* the same: ia[0] envelope */
    RO_ADD_FX = 34,             /* track, event, bus, app (video output); ia[0] plug-in, ia[1] index (-1: at the end) -> effect */
    RO_REMOVE_FX = 35,          /* the same: ia[0] effect */
    RO_ADD_MOTION_KEY = 36,     /* video track: da[0] time -> key (the key there if there is one) */
    RO_REMOVE_MOTION_KEY = 37,  /* video track: ia[0] key */
    RO_ADD_MASTER_FX = 38,      /* app: the master's chain, as RO_ADD_FX */
    RO_ADD_MASTER_ENVELOPE = 39, /* app */

    RO_SPLIT = 50,              /* event: da[0] offset from its start -> the new (right) event */
    RO_COPY = 51,               /* event: ia[0] track, da[0] start -> the copy */
    RO_ADD_TAKE = 52,           /* event: ia[0] stream, ia[1] make it active; s name -> take */
    RO_REMOVE_TAKE = 53,        /* event: ia[0] take */
    RO_ADJUST_START_LENGTH = 54, /* event: da[0] start, da[1] length, ia[0] adjust takes (keep the media where it is on the timeline) */
    RO_ADJUST_RATE = 55,        /* event: da[0] rate, ia[0] adjust start */
    RO_ADD_PANCROP_KEY = 56,    /* event: da[0] time -> key (the key there if there is one) */
    RO_REMOVE_PANCROP_KEY = 57, /* event: ia[0] key */
    RO_SET_TRANSITION = 58,     /* event: ia[0] plug-in (0: back to a cross-fade) -> the transition effect */
    RO_CLEAR_TAKES = 59,        /* event: all but the active take */
    RO_CLEAR_PANCROP = 60,      /* event */

    RO_GROUP_ADD = 70,          /* group: ia[0] event */
    RO_GROUP_REMOVE = 71,       /* group: ia[0] event */

    RO_ADD_POINT = 80,          /* envelope: da[0] x, da[1] y, ia[0] VEGAS's CurveType -> point (an existing point at x is changed) */
    RO_REMOVE_POINT = 81,       /* envelope: ia[0] point */
    RO_CLEAR_POINTS = 82,       /* envelope */
    RO_VALUE_AT = 83,           /* envelope: da[0] x -> dout[0] value */

    RO_OFX_GET = 90,            /* OpenFX parameter or keyframe: ia[0] which number (0..3), ia[1] what (0 the value, 1 default, 2 min,
                                 * 3 max, 4 display min, 5 display max, 6 the value at da[0] seconds) -> dout[0] */
    RO_OFX_SET = 91,            /* parameter: da[0..n-1] the numbers (an animated one: its value at the effect's current time);
                                 * keyframe: its numbers */
    RO_OFX_SET_AT = 92,         /* parameter: da[0] seconds, da[1..] the numbers (or s, text) -> the keyframe there (made if none) */
    RO_OFX_REMOVE_KEY = 93,     /* parameter: ia[0] keyframe */

    RO_GROUP_SELECTED_TRACKS = 100, /* app: the selected tracks (two or more) into a new track group -> group */
    RO_TRACKGROUP_ADD = 101,    /* track group: ia[0] track (taken out of its other group) */
    RO_ROOT_BIN = 102,          /* app -> the Project Media root bin */
    RO_NEW_BIN = 103,           /* bin: s name -> the new bin inside it (the one there if the name is taken) */
    RO_BIN_ADD_MEDIA = 104,     /* bin: ia[0] media (moved into it) */
    RO_RENDER = 106,            /* app: renders the project with the template ia[0] to the file s; ia[1] save markers in the file, ia[2] use the
                                 * project's output rotation; da[0] start, da[1] length (negative: to the end) -> dout[0] VEGAS's RenderStatus */
    RO_APPLY_PRESET = 105       /* effect: ia[0] preset, or s the preset's name (the settings it holds are set; the rest stay) */
};

/* The functions Reno gives the script's side. 'ctx' is passed back to every one. */
typedef struct RenoScriptApi {
    int32_t version;            /* RENO_SCRIPT_API_VERSION */
    int32_t size;               /* sizeof(RenoScriptApi) */
    void* ctx;
    int32_t (*getI)(void* ctx, uint64_t h, int32_t prop, int64_t* out);
    int32_t (*getD)(void* ctx, uint64_t h, int32_t prop, double* out);
    /* writes up to 'cap' bytes (no terminating zero needed) and the full length to *len: call again with a bigger
     * buffer if *len > cap */
    int32_t (*getS)(void* ctx, uint64_t h, int32_t prop, char* buf, int32_t cap, int32_t* len);
    int32_t (*setI)(void* ctx, uint64_t h, int32_t prop, int64_t v);
    int32_t (*setD)(void* ctx, uint64_t h, int32_t prop, double v);
    int32_t (*setS)(void* ctx, uint64_t h, int32_t prop, const char* v);
    /* the handles in a list: up to 'cap' into 'out', the number there are into *count (call again if more) */
    int32_t (*list)(void* ctx, uint64_t h, int32_t which, uint64_t* out, int32_t cap, int32_t* count);
    int32_t (*call)(void* ctx, uint64_t h, int32_t op, const int64_t* ia, int32_t ni, const double* da, int32_t nd, const char* s,
                    uint64_t* out, double* dout);
    /* the last failure's message (as getS) */
    int32_t (*error)(void* ctx, char* buf, int32_t cap, int32_t* len);
    /* what kind of object a handle is (RenoScriptKind; RK_NONE for no such handle) */
    int32_t (*kind)(void* ctx, uint64_t h);
    /* the script's side reports how the run ended: 0 finished, 1 failed with 'message' (compile errors, an exception) */
    void (*finish)(void* ctx, int32_t failed, const char* message);
} RenoScriptApi;

#ifdef __cplusplus
}
#endif
#endif
