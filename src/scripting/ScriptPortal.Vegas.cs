// Reno's ScriptPortal.Vegas: the classes of VEGAS Pro's .NET scripting API, for running VEGAS scripts in Reno.
//
// VEGAS scripts (.cs, .js, .vb) are written against VEGAS's assembly ScriptPortal.Vegas. This file is Reno's version
// of it, with the same classes, properties and methods, compiled once with the .NET Framework's csc.exe. Calls reach
// Reno through the C functions of src/engine/scriptapi.h.
//
// Unsupported members throw a NotSupportedException naming the member.
//
// Must stay C# 5 (csc.exe of .NET Framework 4): no $"" strings, ?. , nameof, => members or auto-property
// initialisers. The numbers in class Abi must match scriptapi.h.
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

[assembly: AssemblyTitle("ScriptPortal.Vegas (Reno)")]
[assembly: AssemblyVersion("18.0.0.0")]

#pragma warning disable 67      // events no one raises (VEGAS's notifications: Reno has none while a script runs)
#pragma warning disable 660, 661    // == without Equals (they are defined on the base class)

namespace ScriptPortal.Vegas
{
    // ------------------------------------------------------------------------------------------------ the C interface
    internal static class Abi
    {
        // kinds
        public const int RK_NONE = 0, RK_APP = 1, RK_TRACK = 2, RK_EVENT = 3, RK_TAKE = 4, RK_MEDIA = 5, RK_STREAM = 6, RK_MARKER = 7,
            RK_FX = 8, RK_PLUGIN = 9, RK_ENVELOPE = 10, RK_ENVPOINT = 11, RK_GROUP = 12, RK_PANCROP = 13, RK_BUS = 14, RK_MOTION = 15,
            RK_OFXPARAM = 16, RK_OFXKEY = 17;
        // properties
        public const int RP_VERSION = 1, RP_APP_NAME = 2, RP_INSTALL_DIR = 3, RP_APPDATA_DIR = 4, RP_TEMP_DIR = 5, RP_SCRIPT_FILE = 6,
            RP_SCRIPT_ARGS = 7, RP_PROJECT_PATH = 8, RP_CURSOR = 9, RP_SEL_START = 10, RP_SEL_LENGTH = 11, RP_LOOP_MODE = 12,
            RP_IS_MODIFIED = 13, RP_PROJECT_LENGTH = 14, RP_WIDTH = 15, RP_HEIGHT = 16, RP_FRAME_RATE = 17, RP_FIELD_ORDER = 18,
            RP_PIXEL_ASPECT = 19, RP_PIXEL_FORMAT = 20, RP_RENDER_QUALITY = 21, RP_MOTION_BLUR = 22, RP_DEINTERLACE = 23,
            RP_OUTPUT_ROTATION = 24, RP_SAMPLE_RATE = 25, RP_BIT_DEPTH = 26, RP_BUS_MODE = 27, RP_RULER_FORMAT = 28, RP_RULER_START = 29,
            RP_TEMPO = 30, RP_BEATS_PER_MEASURE = 31, RP_TITLE = 32, RP_ARTIST = 33, RP_ENGINEER = 34, RP_COPYRIGHT = 35, RP_COMMENTS = 36,
            RP_OUTPUT_360 = 400,
            RP_BYPASS_FX = 37, RP_IS_PLAYING = 38, RP_UNDO_LABEL = 39, RP_UNDO_CANCEL = 40, RP_MAIN_WINDOW = 41,
            RP_NAME = 50, RP_INDEX = 51, RP_MEDIA_TYPE = 52, RP_MUTE = 53, RP_SOLO = 54, RP_SELECTED = 55, RP_START = 56, RP_LENGTH = 57,
            RP_LEVEL = 58, RP_PAN = 59, RP_MEDIA = 60, RP_PATH = 61, RP_COMMENT = 62, RP_IS_VALID = 63,
            RP_COMPOSITE_MODE = 70, RP_PARENT_COMPOSITE_MODE = 71, RP_NESTING_LEVEL = 72, RP_TOP_FADE_COLOR = 73, RP_BOTTOM_FADE_COLOR = 74,
            RP_TRACK_BUS = 75,
            RP_END = 80, RP_RATE = 81, RP_LOCKED = 82, RP_LOOP = 83, RP_TRACK = 84, RP_GROUP = 85, RP_ACTIVE_TAKE = 86, RP_FADE_IN = 87,
            RP_FADE_OUT = 88, RP_FADE_IN_CURVE = 89, RP_FADE_OUT_CURVE = 90, RP_MAINTAIN_ASPECT = 91, RP_REDUCE_INTERLACE = 92,
            RP_RESAMPLE_MODE = 93, RP_UNDERSAMPLE = 94, RP_NORMALIZE = 95, RP_NORMALIZE_GAIN = 96, RP_INVERT_PHASE = 97, RP_CHANNELS = 98,
            RP_STRETCH_METHOD = 99, RP_PITCH_LOCK = 100, RP_PITCH_SEMIS = 101, RP_CLASSIC_ATTR = 102, RP_ELASTIQUE_ATTR = 103,
            RP_FORMANT_LOCK = 104, RP_FORMANT_SHIFT = 105, RP_TRANSITION = 106, RP_MEDIA_WIDTH = 107, RP_MEDIA_HEIGHT = 108, RP_REVERSE = 109,
            RP_OFFSET = 120, RP_IS_ACTIVE = 121, RP_STREAM = 122, RP_AVAILABLE_LENGTH = 123,
            RP_HAS_VIDEO = 130, RP_HAS_AUDIO = 131, RP_IS_GENERATED = 132, RP_IS_SUBCLIP = 133, RP_IS_OFFLINE = 134, RP_USE_COUNT = 135,
            RP_MEDIA_ID = 136, RP_GENERATOR = 137, RP_SUBCLIP_START = 138, RP_IS_REVERSED = 139, RP_STREAM_WIDTH = 140, RP_STREAM_HEIGHT = 141,
            RP_STREAM_FPS = 142, RP_STREAM_CHANNELS = 143, RP_STREAM_SAMPLE_RATE = 144, RP_STREAM_BITS = 145, RP_STREAM_FORMAT = 146,
            RP_STREAM_FIELD_ORDER = 147, RP_STREAM_PIXEL_ASPECT = 148,
            RP_MARKER_KIND = 150, RP_COMMAND_TYPE = 151,
            RP_BYPASS = 160, RP_BEFORE_PANCROP = 161, RP_PLUGIN = 162, RP_IS_OFX = 163, RP_GROUP_NAME = 164, RP_UNIQUE_ID = 165,
            RP_CLASS_ID = 166, RP_PLUGIN_TYPE = 167, RP_IS_VIDEO = 168, RP_IS_AUDIO = 169,
            RP_ENV_TYPE = 180, RP_ENV_MIN = 181, RP_ENV_MAX = 182, RP_ENV_NEUTRAL = 183, RP_X = 184, RP_Y = 185, RP_CURVE = 186,
            RP_KEY_TIME = 200, RP_KEY_X = 201, RP_KEY_Y = 202, RP_KEY_W = 203, RP_KEY_H = 204, RP_KEY_ROTATION = 205, RP_KEY_TYPE = 206,
            RP_KEY_SMOOTHNESS = 207,
            RP_OFX_TYPE = 220, RP_OFX_LABEL = 221, RP_OFX_HINT = 222, RP_OFX_PARENT = 223, RP_OFX_ENABLED = 224, RP_OFX_SECRET = 225,
            RP_OFX_CAN_ANIMATE = 226, RP_OFX_PERSIST = 227, RP_OFX_ANIMATED = 228, RP_OFX_DIM = 229, RP_OFX_CHOICES = 230, RP_OFX_TEXT = 231,
            RP_OFX_INTERP = 232, RP_OFX_EFFECT = 233, RP_OFX_PLUGIN_LABEL = 234, RP_OFX_PLUGIN_GROUP = 235, RP_OFX_PLUGIN_PATH = 236,
            RP_OFX_CURRENT_TIME = 237, RP_FADE_IN_RECIP_CURVE = 238;
        // lists
        public const int RL_TRACKS = 1, RL_MEDIA = 2, RL_MARKERS = 3, RL_REGIONS = 4, RL_COMMAND_MARKERS = 5, RL_CD_TRACKS = 6,
            RL_CD_INDICES = 7, RL_GROUPS = 8, RL_VIDEO_FX = 9, RL_AUDIO_FX = 10, RL_TRANSITIONS = 11, RL_GENERATORS = 12, RL_BUSES = 13,
            RL_EVENTS = 20, RL_EFFECTS = 21, RL_ENVELOPES = 22, RL_TAKES = 23, RL_STREAMS = 24, RL_POINTS = 25, RL_PANCROP = 26,
            RL_MOTION = 27, RL_MASTER_EFFECTS = 28, RL_MASTER_ENVELOPES = 29, RL_SELECTED_MEDIA = 30, RL_SPECIAL_BUSES = 31,
            RL_OFX_PARAMS = 32, RL_OFX_KEYS = 33;
        // operations
        public const int RO_ADD_TRACK = 1, RO_REMOVE_TRACK = 2, RO_IMPORT_MEDIA = 3, RO_FIND_MEDIA = 4, RO_REMOVE_MEDIA = 5,
            RO_ADD_SUBCLIP = 6, RO_ADD_MARKER = 7, RO_REMOVE_MARKER = 8, RO_NEW_GROUP = 9, RO_REMOVE_GROUP = 10, RO_NEW_PROJECT = 11,
            RO_SAVE_PROJECT = 12, RO_OPEN_PROJECT = 13, RO_LOG = 14, RO_INSERT_TIME = 15, RO_REMOVE_UNUSED_MEDIA = 16,
            RO_GENERATED_MEDIA = 17, RO_MOVE_TRACK = 18, RO_UPDATE_UI = 19,
            RO_ADD_EVENT = 30, RO_REMOVE_EVENT = 31, RO_ADD_ENVELOPE = 32, RO_REMOVE_ENVELOPE = 33, RO_ADD_FX = 34, RO_REMOVE_FX = 35,
            RO_ADD_MOTION_KEY = 36, RO_REMOVE_MOTION_KEY = 37, RO_ADD_MASTER_FX = 38, RO_ADD_MASTER_ENVELOPE = 39,
            RO_SPLIT = 50, RO_COPY = 51, RO_ADD_TAKE = 52, RO_REMOVE_TAKE = 53, RO_ADJUST_START_LENGTH = 54, RO_ADJUST_RATE = 55,
            RO_ADD_PANCROP_KEY = 56, RO_REMOVE_PANCROP_KEY = 57, RO_SET_TRANSITION = 58, RO_CLEAR_TAKES = 59, RO_CLEAR_PANCROP = 60,
            RO_GROUP_ADD = 70, RO_GROUP_REMOVE = 71,
            RO_ADD_POINT = 80, RO_REMOVE_POINT = 81, RO_CLEAR_POINTS = 82, RO_VALUE_AT = 83,
            RO_OFX_GET = 90, RO_OFX_SET = 91, RO_OFX_SET_AT = 92, RO_OFX_REMOVE_KEY = 93;
    }

    // The connection to Reno for one script run, and the objects handed to the script (one C# object per Reno object,
    // so == works as in VEGAS).
    internal static class Reno
    {
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int GetIFn(IntPtr ctx, ulong h, int prop, out long v);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int GetDFn(IntPtr ctx, ulong h, int prop, out double v);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int GetSFn(IntPtr ctx, ulong h, int prop, [In, Out] byte[] buf, int cap, out int len);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int SetIFn(IntPtr ctx, ulong h, int prop, long v);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int SetDFn(IntPtr ctx, ulong h, int prop, double v);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int SetSFn(IntPtr ctx, ulong h, int prop, byte[] v);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int ListFn(IntPtr ctx, ulong h, int which, [In, Out] ulong[] outp, int cap, out int count);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int CallFn(IntPtr ctx, ulong h, int op, long[] ia, int ni, double[] da, int nd, byte[] s, out ulong o, out double d);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int ErrorFn(IntPtr ctx, [In, Out] byte[] buf, int cap, out int len);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int KindFn(IntPtr ctx, ulong h);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void FinishFn(IntPtr ctx, int failed, byte[] msg);

        static IntPtr ctx;
        static GetIFn getI; static GetDFn getD; static GetSFn getS; static SetIFn setI; static SetDFn setD; static SetSFn setS;
        static ListFn list; static CallFn call; static ErrorFn error; static KindFn kind; static FinishFn finish;
        static Dictionary<ulong, object> objects = new Dictionary<ulong, object>();
        internal static Vegas App;
        internal static double FrameRate = 30;      // what Timecode's frames mean (the project's frame rate)

        public static bool Connected { get { return ctx != IntPtr.Zero; } }

        public static void Init(IntPtr api)
        {
            if (api == IntPtr.Zero) throw new ArgumentException("no Reno interface");
            int version = Marshal.ReadInt32(api, 0);
            if (version != 1) throw new InvalidOperationException("this ScriptPortal.Vegas does not match this Reno (interface version " + version + ")");
            int p = 8;
            ctx = Marshal.ReadIntPtr(api, p); p += IntPtr.Size;
            getI = Fn<GetIFn>(api, ref p); getD = Fn<GetDFn>(api, ref p); getS = Fn<GetSFn>(api, ref p);
            setI = Fn<SetIFn>(api, ref p); setD = Fn<SetDFn>(api, ref p); setS = Fn<SetSFn>(api, ref p);
            list = Fn<ListFn>(api, ref p); call = Fn<CallFn>(api, ref p); error = Fn<ErrorFn>(api, ref p);
            kind = Fn<KindFn>(api, ref p); finish = Fn<FinishFn>(api, ref p);
            objects = new Dictionary<ulong, object>();
            CustomDataContainer.Reset();
            App = new Vegas();
            objects[1] = App;
            FrameRate = GD(1, Abi.RP_FRAME_RATE, "Project.Video.FrameRate");
        }

        static T Fn<T>(IntPtr api, ref int p) where T : class
        {
            IntPtr f = Marshal.ReadIntPtr(api, p);
            p += IntPtr.Size;
            return (T)(object)Marshal.GetDelegateForFunctionPointer(f, typeof(T));
        }

        public static void Finish(bool failed, string message)
        {
            if (finish != null) finish(ctx, failed ? 1 : 0, Z(message));
        }

        static byte[] Z(string s)
        {
            byte[] b = Encoding.UTF8.GetBytes(s ?? "");
            byte[] z = new byte[b.Length + 1];
            Array.Copy(b, z, b.Length);
            return z;
        }

        static string LastError()
        {
            byte[] buf = new byte[512];
            int len;
            error(ctx, buf, buf.Length, out len);
            if (len > buf.Length) { buf = new byte[len]; error(ctx, buf, buf.Length, out len); }
            return Encoding.UTF8.GetString(buf, 0, Math.Min(len, buf.Length));
        }

        static void Check(int r, string member)
        {
            if (r == 0) return;
            if (r == 2) throw new NotSupportedException(member + " is not supported by Reno.");
            throw new InvalidOperationException(member + ": " + LastError());
        }

        static void Need()
        {
            if (ctx == IntPtr.Zero) throw new InvalidOperationException("ScriptPortal.Vegas (Reno): not running inside Reno.");
        }

        public static long GI(ulong h, int prop, string member) { Need(); long v; Check(getI(ctx, h, prop, out v), member); return v; }
        public static double GD(ulong h, int prop, string member) { Need(); double v; Check(getD(ctx, h, prop, out v), member); return v; }
        public static string GS(ulong h, int prop, string member)
        {
            Need();
            byte[] buf = new byte[256];
            int len;
            Check(getS(ctx, h, prop, buf, buf.Length, out len), member);
            if (len > buf.Length) { buf = new byte[len]; Check(getS(ctx, h, prop, buf, buf.Length, out len), member); }
            return Encoding.UTF8.GetString(buf, 0, Math.Min(len, buf.Length));
        }
        public static void SI(ulong h, int prop, long v, string member) { Need(); Check(setI(ctx, h, prop, v), member); }
        public static void SD(ulong h, int prop, double v, string member) { Need(); Check(setD(ctx, h, prop, v), member); }
        public static void SS(ulong h, int prop, string v, string member) { Need(); Check(setS(ctx, h, prop, Z(v)), member); }
        public static bool TryGI(ulong h, int prop, out long v) { v = 0; return ctx != IntPtr.Zero && getI(ctx, h, prop, out v) == 0; }

        public static ulong[] List(ulong h, int which, string member)
        {
            Need();
            ulong[] buf = new ulong[64];
            int n;
            Check(list(ctx, h, which, buf, buf.Length, out n), member);
            if (n > buf.Length) { buf = new ulong[n]; Check(list(ctx, h, which, buf, buf.Length, out n), member); }
            ulong[] r = new ulong[n];
            Array.Copy(buf, r, n);
            return r;
        }

        public static ulong Call(ulong h, int op, long[] ia, double[] da, string s, string member)
        {
            double d;
            return Call(h, op, ia, da, s, member, out d);
        }

        public static ulong Call(ulong h, int op, long[] ia, double[] da, string s, string member, out double dout)
        {
            Need();
            ia = ia ?? new long[0];
            da = da ?? new double[0];
            ulong o;
            Check(call(ctx, h, op, ia, ia.Length, da, da.Length, s == null ? null : Z(s), out o, out dout), member);
            return o;
        }

        public static int Kind(ulong h) { return ctx == IntPtr.Zero ? 0 : kind(ctx, h); }

        // The script's object for a Reno object (made the first time).
        public static object Wrap(ulong h)
        {
            if (h == 0) return null;
            object o;
            if (objects.TryGetValue(h, out o)) return o;
            switch (Kind(h))
            {
            case Abi.RK_TRACK: o = GI(h, Abi.RP_MEDIA_TYPE, "Track.MediaType") == 2 ? (object)new AudioTrack(h) : new VideoTrack(h); break;
            case Abi.RK_EVENT: o = GI(h, Abi.RP_MEDIA_TYPE, "TrackEvent.MediaType") == 2 ? (object)new AudioEvent(h) : new VideoEvent(h); break;
            case Abi.RK_TAKE: o = new Take(h); break;
            case Abi.RK_MEDIA: o = GI(h, Abi.RP_IS_SUBCLIP, "Media.IsSubclip") != 0 ? new Subclip(h) : new Media(h); break;
            case Abi.RK_STREAM: o = GI(h, Abi.RP_MEDIA_TYPE, "MediaStream.MediaType") == 2 ? (object)new AudioStream(h) : new VideoStream(h); break;
            case Abi.RK_MARKER:
                switch (GI(h, Abi.RP_MARKER_KIND, "Marker"))
                {
                case 1: o = new Region(h); break;
                case 2: o = new CommandMarker(h); break;
                case 3: o = new CDRegion(h); break;
                case 4: o = new CDMarker(h); break;
                default: o = new Marker(h); break;
                }
                break;
            case Abi.RK_FX: o = new Effect(h); break;
            case Abi.RK_PLUGIN: o = new PlugInNode(h); break;
            case Abi.RK_ENVELOPE: o = new Envelope(h); break;
            case Abi.RK_ENVPOINT: o = new EnvelopePoint(h); break;
            case Abi.RK_GROUP: o = new TrackEventGroup(h); break;
            case Abi.RK_PANCROP: o = new VideoMotionKeyframe(h); break;
            case Abi.RK_MOTION: o = new TrackMotionKeyframe(h); break;
            case Abi.RK_OFXPARAM: o = OFXParameter.Make(h); break;
            case Abi.RK_OFXKEY: o = OFXParameter.MakeKey(h); break;
            case Abi.RK_BUS: o = GI(h, Abi.RP_MEDIA_TYPE, "BusTrack.MediaType") == 3 ? (object)new VideoBusTrack(h) : new AudioBusTrack(h); break;
            default: throw new InvalidOperationException("Reno gave an object of an unknown kind");
            }
            objects[h] = o;
            return o;
        }

        public static T W<T>(ulong h) where T : class { return Wrap(h) as T; }

        public static List<T> WrapAll<T>(ulong[] hs) where T : class
        {
            List<T> r = new List<T>(hs.Length);
            foreach (ulong h in hs) { T t = W<T>(h); if (t != null) r.Add(t); }
            return r;
        }

        // A script's own new object (new VideoEvent(...) added to a track...) becomes the object for that Reno object.
        public static void Adopt(object o, ulong h)
        {
            RenoObject r = o as RenoObject;
            if (r != null) r.H = h;
            objects[h] = o;
        }

        public static void Log(string s)
        {
            if (ctx != IntPtr.Zero) Call(1, Abi.RO_LOG, null, null, s, "Vegas.DebugOut");
        }
    }

    // Base of every object made by Reno: its handle (0 until a script's new object is added to the project) and the
    // settings given to it before that.
    public abstract partial class RenoObject
    {
        internal ulong H;
        internal Dictionary<int, object> pending;

        protected RenoObject() { }
        internal RenoObject(ulong h) { H = h; }

        internal string Member(string name) { return GetType().Name + "." + name; }

        internal long GI(int prop, [CallerMemberName] string m = "")
        {
            if (H == 0) { object v; return pending != null && pending.TryGetValue(prop, out v) ? Convert.ToInt64(v) : 0; }
            return Reno.GI(H, prop, Member(m));
        }
        internal double GD(int prop, [CallerMemberName] string m = "")
        {
            if (H == 0) { object v; return pending != null && pending.TryGetValue(prop, out v) ? Convert.ToDouble(v) : 0; }
            return Reno.GD(H, prop, Member(m));
        }
        internal string GS(int prop, [CallerMemberName] string m = "")
        {
            if (H == 0) { object v; return pending != null && pending.TryGetValue(prop, out v) ? (string)v : ""; }
            return Reno.GS(H, prop, Member(m));
        }
        internal bool GB(int prop, [CallerMemberName] string m = "") { return GI(prop, m) != 0; }
        internal Timecode GT(int prop, [CallerMemberName] string m = "") { return Timecode.FromSeconds(GD(prop, m)); }

        internal void SI(int prop, long v, [CallerMemberName] string m = "")
        {
            if (H == 0) { Pend(prop, v); return; }
            Reno.SI(H, prop, v, Member(m));
        }
        internal void SD(int prop, double v, [CallerMemberName] string m = "")
        {
            if (H == 0) { Pend(prop, v); return; }
            Reno.SD(H, prop, v, Member(m));
        }
        internal void SS(int prop, string v, [CallerMemberName] string m = "")
        {
            if (H == 0) { Pend(prop, v ?? ""); return; }
            Reno.SS(H, prop, v ?? "", Member(m));
        }
        internal void SB(int prop, bool v, [CallerMemberName] string m = "") { SI(prop, v ? 1 : 0, m); }
        internal void ST(int prop, Timecode v, [CallerMemberName] string m = "")
        {
            if (v == null) throw new ArgumentNullException(Member(m));
            SD(prop, v.ToSeconds(), m);
        }
        internal ulong[] L(int which, [CallerMemberName] string m = "")
        {
            if (H == 0) return new ulong[0];
            return Reno.List(H, which, Member(m));
        }
        internal ulong Op(int op, long[] ia, double[] da, string s, [CallerMemberName] string m = "")
        {
            NeedAdded(m);
            return Reno.Call(H, op, ia, da, s, Member(m));
        }
        internal void NeedAdded(string m)
        {
            if (H == 0) throw new InvalidOperationException(Member(m) + ": the object has not been added to the project yet.");
        }
        internal Exception NotSupported(string m) { return new NotSupportedException(Member(m) + " is not supported by Reno."); }

        void Pend(int prop, object v)
        {
            if (pending == null) pending = new Dictionary<int, object>();
            pending[prop] = v;
        }

        // Called when the object has been added to the project: the settings it was given go to Reno.
        internal virtual void Attached(ulong h)
        {
            Reno.Adopt(this, h);
            if (pending == null) return;
            Dictionary<int, object> p = pending;
            pending = null;
            foreach (KeyValuePair<int, object> kv in p)
            {
                if (kv.Value is string) Reno.SS(H, kv.Key, (string)kv.Value, Member("(settings)"));
                else if (kv.Value is double) Reno.SD(H, kv.Key, (double)kv.Value, Member("(settings)"));
                else Reno.SI(H, kv.Key, Convert.ToInt64(kv.Value), Member("(settings)"));
            }
        }

        public bool IsValid()
        {
            if (H == 0) return false;
            long v;
            return Reno.TryGI(H, Abi.RP_IS_VALID, out v) && v != 0;
        }

        public override bool Equals(object obj)
        {
            RenoObject o = obj as RenoObject;
            if (ReferenceEquals(o, null)) return false;
            if (ReferenceEquals(o, this)) return true;
            return H != 0 && o.H == H;
        }
        public override int GetHashCode() { return H != 0 ? H.GetHashCode() : base.GetHashCode(); }
        public static bool operator ==(RenoObject a, RenoObject b)
        {
            if (ReferenceEquals(a, b)) return true;
            if (ReferenceEquals(a, null) || ReferenceEquals(b, null)) return false;
            return a.Equals(b);
        }
        public static bool operator !=(RenoObject a, RenoObject b) { return !(a == b); }
    }

    // ------------------------------------------------------------------------------------------------ lists
    // VEGAS's BaseList<T>: a list of the project's objects, read from Reno each time it is used.
    public abstract partial class BaseList<T> : IList, IList<T> where T : class
    {
        internal abstract List<T> Items();
        internal virtual void AddItem(T item) { throw new NotSupportedException(GetType().Name + ".Add is not supported by Reno."); }
        internal virtual bool RemoveItem(T item) { throw new NotSupportedException(GetType().Name + ".Remove is not supported by Reno."); }

        public virtual bool IsFixedSize { get { return false; } }
        public virtual bool IsReadOnly { get { return false; } }
        public int Count { get { return Items().Count; } }
        public bool IsSynchronized { get { return false; } }
        public object SyncRoot { get { return this; } }
        public T this[int index]
        {
            get
            {
                List<T> l = Items();
                if (index < 0 || index >= l.Count) throw new ArgumentOutOfRangeException("index", GetType().Name + ": no item " + index + " (there are " + l.Count + ")");
                return l[index];
            }
            set { throw new NotSupportedException(GetType().Name + "[index] = ... is not supported by Reno."); }
        }
        object IList.this[int index] { get { return this[index]; } set { this[index] = (T)value; } }

        public virtual void CopyTo(Array array, int index) { ((ICollection)Items()).CopyTo(array, index); }
        public virtual void CopyTo(T[] array, int index) { Items().CopyTo(array, index); }
        // (a copy of the list as it is now: a script may change the project while going through it)
        public virtual IEnumerator<T> GetEnumerator() { return Items().GetEnumerator(); }
        IEnumerator IEnumerable.GetEnumerator() { return GetEnumerator(); }
        public virtual int Add(object item) { AddItem((T)item); return IndexOf((T)item); }
        public virtual void Add(T item)
        {
            if (item == null) throw new ArgumentNullException("item");
            AddItem(item);
        }
        public virtual void Clear() { foreach (T t in Items()) RemoveItem(t); }
        public virtual bool Contains(object item) { return IndexOf(item) >= 0; }
        public virtual bool Contains(T item) { return IndexOf(item) >= 0; }
        public virtual int IndexOf(object item) { return IndexOf(item as T); }
        public virtual int IndexOf(T item)
        {
            if (item == null) return -1;
            List<T> l = Items();
            for (int k = 0; k < l.Count; ++k) if (l[k].Equals(item)) return k;
            return -1;
        }
        public virtual void Insert(int index, object item) { Insert(index, (T)item); }
        public virtual void Insert(int index, T item) { Add(item); }
        public virtual bool Remove(T item) { return item != null && RemoveItem(item); }
        public virtual void Remove(object item) { Remove(item as T); }
        public virtual void RemoveAt(int index) { Remove(this[index]); }
    }

    // A list of Reno objects of one owner.
    public abstract partial class HandleList<T> : BaseList<T> where T : class
    {
        internal RenoObject owner;
        internal int which;
        internal HandleList(RenoObject owner, int which) { this.owner = owner; this.which = which; }
        internal override List<T> Items() { return Reno.WrapAll<T>(owner.L(which, GetType().Name)); }
    }

    // ------------------------------------------------------------------------------------------------ Timecode
    // A time, kept as VEGAS does in units of 100 ns ("nanos"). Frames are counted at the project's frame rate.
    public partial class Timecode : IComparable
    {
        long nanos;
        const double NanosPerSecond = 10000000.0;

        public Timecode() { }
        public Timecode(double milliseconds) { nanos = (long)Math.Round(milliseconds * 10000.0); }
        public Timecode(string timestamp) { nanos = Parse(timestamp, RulerFormat.Unknown).nanos; }

        public long Nanos { get { return nanos; } set { nanos = value; } }
        public double FrameRate { get { return Reno.FrameRate > 0 ? Reno.FrameRate : 30; } }
        public long FrameCount
        {
            get { return (long)Math.Floor(ToSeconds() * FrameRate + 1e-6); }
            set { nanos = (long)Math.Round(value / FrameRate * NanosPerSecond); }
        }

        internal double ToSeconds() { return nanos / NanosPerSecond; }
        public double ToMilliseconds() { return nanos / 10000.0; }

        public static Timecode FromNanos(long nanos) { Timecode t = new Timecode(); t.nanos = nanos; return t; }
        public static Timecode FromSeconds(double seconds) { return FromNanos((long)Math.Round(seconds * NanosPerSecond)); }
        public static Timecode FromMilliseconds(double milliseconds) { return new Timecode(milliseconds); }
        public static Timecode FromFrames(long frames) { Timecode t = new Timecode(); t.FrameCount = frames; return t; }
        public static Timecode FromString(string timestamp) { return Parse(timestamp, RulerFormat.Unknown); }
        public static Timecode FromPositionString(string timestamp) { return Parse(timestamp, RulerFormat.Unknown); }
        public static Timecode FromString(string timestamp, RulerFormat format) { return Parse(timestamp, format); }
        public static Timecode FromPositionString(string timestamp, RulerFormat format) { return Parse(timestamp, format); }
        public static Timecode FromString(string timestamp, RulerFormat format, bool isPosition) { return Parse(timestamp, format); }

        // "hh:mm:ss;ff" / "hh:mm:ss:ff" (frames), "hh:mm:ss.fff" (time), "12.5" (seconds), "123" (frames), with the
        // format choosing when it is given
        static Timecode Parse(string s, RulerFormat format)
        {
            if (s == null) throw new ArgumentNullException("timestamp");
            s = s.Trim();
            CultureInfo inv = CultureInfo.InvariantCulture;
            double fps = Reno.FrameRate > 0 ? Reno.FrameRate : 30;
            try
            {
                if (format == RulerFormat.Seconds) return FromSeconds(double.Parse(s, inv));
                if (format == RulerFormat.AbsoluteFrames) return FromFrames(long.Parse(s, inv));
                if (format == RulerFormat.Samples) return FromSeconds(double.Parse(s, inv) / 48000.0);
                if (format == RulerFormat.Nanoseconds) return FromNanos(long.Parse(s, inv) / 100);
                if (format == RulerFormat.Microseconds) return FromNanos(long.Parse(s, inv) * 10);
                string[] parts = s.Split(':', ';');
                if (parts.Length == 1)
                {
                    if (format == RulerFormat.Unknown && s.IndexOf('.') < 0) return FromFrames(long.Parse(s, inv));
                    return FromSeconds(double.Parse(s, inv));
                }
                bool frames = s.IndexOf(';') >= 0 || (parts.Length == 4) ||
                              (format != RulerFormat.Unknown && format != RulerFormat.Time && format != RulerFormat.Seconds && s.IndexOf('.') < 0);
                double total = 0;
                int n = frames ? parts.Length - 1 : parts.Length;
                for (int k = 0; k < n; ++k) total = total * 60 + double.Parse(parts[k], inv);
                if (frames)
                {
                    long ff = long.Parse(parts[parts.Length - 1], inv);
                    long fr = (long)Math.Round(fps);
                    long whole = (long)Math.Round(total);
                    long count = whole * fr + ff;
                    if (s.IndexOf(';') >= 0 && Math.Abs(fps * 1001 / 1000 - fr) < 0.01 && (fr == 30 || fr == 60))
                    {
                        // SMPTE drop frame: two frame numbers (four at 60) are skipped each minute but every tenth
                        long drop = fr / 15;
                        long minutes = whole / 60;
                        count -= drop * (minutes - minutes / 10);
                    }
                    return FromFrames(count);
                }
                return FromSeconds(total);
            }
            catch (FormatException)
            {
                throw new FormatException("Timecode: \"" + s + "\" is not a time Reno can read");
            }
        }

        public override string ToString() { return ToString(RulerFormat.Unknown); }
        public string ToPositionString() { return ToString(RulerFormat.Unknown); }
        public string ToPositionString(RulerFormat format) { return ToString(format); }
        public string ToString(RulerFormat format, bool isPosition) { return ToString(format); }
        public string ToString(RulerFormat format)
        {
            CultureInfo inv = CultureInfo.InvariantCulture;
            double secs = ToSeconds();
            string sign = secs < 0 ? "-" : "";
            secs = Math.Abs(secs);
            double fps = FrameRate;
            switch (format)
            {
            case RulerFormat.Seconds: return (nanos / NanosPerSecond).ToString("0.000", inv);
            case RulerFormat.AbsoluteFrames: return FrameCount.ToString(inv);
            case RulerFormat.Samples: return ((long)Math.Round(nanos / NanosPerSecond * 48000)).ToString(inv);
            case RulerFormat.Nanoseconds: return (nanos * 100).ToString(inv);
            case RulerFormat.Microseconds: return (nanos / 10).ToString(inv);
            case RulerFormat.Time:
            {
                long ms = (long)Math.Round(secs * 1000);
                return sign + string.Format(inv, "{0:00}:{1:00}:{2:00}.{3:000}", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000);
            }
            default:
            {
                // hours:minutes:seconds;frames, frames numbered as SMPTE drop frame at 29.97 and 59.94
                long fr = (long)Math.Round(fps);
                long count = (long)Math.Floor(secs * fps + 1e-6);
                bool dropFrame = Math.Abs(fps * 1001 / 1000 - fr) < 0.01 && (fr == 30 || fr == 60);
                if (dropFrame)
                {
                    long drop = fr / 15, perMin = fr * 60 - drop, per10 = fr * 600 - drop * 9;
                    long d = count / per10, m = count % per10;
                    count += drop * 9 * d + (m > drop ? drop * ((m - drop) / perMin) : 0);
                }
                if (fr < 1) fr = 1;
                long f = count % fr, s = count / fr;
                return sign + string.Format(inv, "{0:00}:{1:00}:{2:00}{3}{4:00}", s / 3600, s / 60 % 60, s % 60, dropFrame ? ";" : ":", f);
            }
            }
        }

        public int CompareTo(object other)
        {
            Timecode t = other as Timecode;
            if (t == null) return 1;
            return nanos.CompareTo(t.nanos);
        }
        protected bool MembersEqual(Timecode o) { return nanos == o.nanos; }
        public override bool Equals(object obj) { Timecode t = obj as Timecode; return !ReferenceEquals(t, null) && t.nanos == nanos; }
        public override int GetHashCode() { return nanos.GetHashCode(); }
        public static bool operator ==(Timecode a, Timecode b)
        {
            if (ReferenceEquals(a, b)) return true;
            if (ReferenceEquals(a, null) || ReferenceEquals(b, null)) return false;
            return a.nanos == b.nanos;
        }
        public static bool operator !=(Timecode a, Timecode b) { return !(a == b); }
        public static bool operator <(Timecode a, Timecode b) { return a.nanos < b.nanos; }
        public static bool operator >(Timecode a, Timecode b) { return a.nanos > b.nanos; }
        public static bool operator <=(Timecode a, Timecode b) { return a.nanos <= b.nanos; }
        public static bool operator >=(Timecode a, Timecode b) { return a.nanos >= b.nanos; }
        public static Timecode operator +(Timecode a, Timecode b) { return FromNanos(a.nanos + b.nanos); }
        public static Timecode operator -(Timecode a, Timecode b) { return FromNanos(a.nanos - b.nanos); }
        public static Timecode operator ++(Timecode v) { Timecode t = FromNanos(v.nanos); t.FrameCount = v.FrameCount + 1; return t; }
        public static Timecode operator --(Timecode v) { Timecode t = FromNanos(v.nanos); t.FrameCount = v.FrameCount - 1; return t; }
    }

    // VEGAS's ProjectTimecode: Timecode made with a given project's frame rate (the project is Reno's one project).
    public partial class ProjectTimecode
    {
        public ProjectTimecode() { }
        public static Timecode FromString(Project project, string timestamp) { return Timecode.FromString(timestamp); }
        public static Timecode FromPositionString(Project project, string timestamp) { return Timecode.FromString(timestamp); }
        public static Timecode FromString(Project project, string timestamp, RulerFormat format) { return Timecode.FromString(timestamp, format); }
        public static Timecode FromPositionString(Project project, string timestamp, RulerFormat format) { return Timecode.FromString(timestamp, format); }
        public static Timecode FromString(Project project, string timestamp, RulerFormat format, bool isPosition) { return Timecode.FromString(timestamp, format); }
        public static Timecode FromMilliseconds(Project project, double milliseconds) { return Timecode.FromMilliseconds(milliseconds); }
        public static Timecode FromSeconds(Project project, double seconds) { return Timecode.FromSeconds(seconds); }
        public static Timecode FromNanos(Project project, long nanos) { return Timecode.FromNanos(nanos); }
        public static Timecode FromPositionNanos(Project project, long nanos) { return Timecode.FromNanos(nanos); }
        public static Timecode FromFrames(Project project, long frames) { return Timecode.FromFrames(frames); }
    }

    // ------------------------------------------------------------------------------------------------ the application

    // VEGAS's Vegas object: what a script's FromVegas is given (and JScript's and VB's global "Vegas").
    public partial class Vegas : RenoObject
    {
        TransportControl transport;
        Project project;

        internal Vegas() : base(1) { }

        public string LongAppName { get { return GS(Abi.RP_APP_NAME) + " " + Version; } }
        public string ShortAppName { get { return GS(Abi.RP_APP_NAME); } }
        public string Version { get { return GS(Abi.RP_VERSION); } }
        public string TemporaryFilesPath { get { return GS(Abi.RP_TEMP_DIR); } }
        public string ApplicationDataPath { get { return GS(Abi.RP_APPDATA_DIR); } }
        public string InstallationDirectory { get { return GS(Abi.RP_INSTALL_DIR); } }
        public CultureInfo AppCultureInfo { get { return CultureInfo.CurrentUICulture; } }
        public IWin32Window MainWindow { get { return new Win32Window(new IntPtr(GI(Abi.RP_MAIN_WINDOW))); } }
        public IWin32Window TimelineWindow { get { return MainWindow; } }
        public Project Project { get { if (project == null) project = new Project(this); return project; } }
        public TransportControl Transport { get { if (transport == null) transport = new TransportControl(); return transport; } }
        public PlugInNode PlugIns { get { return PlugInNode.Root(); } }
        public PlugInNode VideoFX { get { return PlugInNode.Container("Video FX", Abi.RL_VIDEO_FX); } }
        public PlugInNode AudioFX { get { return PlugInNode.Container("Audio FX", Abi.RL_AUDIO_FX); } }
        public PlugInNode Transitions { get { return PlugInNode.Container("Transitions", Abi.RL_TRANSITIONS); } }
        public PlugInNode Generators { get { return PlugInNode.Container("Media Generators", Abi.RL_GENERATORS); } }
        public PlugInNode VideoCompositeFX { get { return PlugInNode.Container("Compositors", -1); } }
        public Timecode Cursor { get { return GT(Abi.RP_CURSOR); } set { ST(Abi.RP_CURSOR, value); } }
        public Timecode SelectionStart { get { return GT(Abi.RP_SEL_START); } set { ST(Abi.RP_SEL_START, value); } }
        public Timecode SelectionLength { get { return GT(Abi.RP_SEL_LENGTH); } set { ST(Abi.RP_SEL_LENGTH, value); } }
        public bool LoopPlayback { get { return GB(Abi.RP_LOOP_MODE); } set { SB(Abi.RP_LOOP_MODE, value); } }
        public bool LoopMode { get { return GB(Abi.RP_LOOP_MODE); } set { SB(Abi.RP_LOOP_MODE, value); } }
        public bool BypassAllAudioFX { get { return GB(Abi.RP_BYPASS_FX); } set { SB(Abi.RP_BYPASS_FX, value); } }
        public bool IsPreviewing { get { return false; } }
        public bool ResumePlaybackOnScriptExit { set { } }
        public bool UnloadScriptDomainOnScriptExit { set { } }

        public event EventHandler ProjectOpening, ProjectOpened, ProjectClosed, RenderStarted, RenderProgress, RenderFinished, AppInitialized,
            AppActivated, AppDeactivate, ProjectSaving, ProjectSaved, MissingMedia, TrackCountChanged, TrackStateChanged, TrackEventCountChanged,
            TrackEventStateChanged, TrackEventTimeChanged, MarkersChanged, MediaPoolChanged, RulerFormatChanged, PlaybackStarted, PlaybackStopped,
            ProjectPropsChanged, TrackEventDataChanged, PreviewStopped;

        public static string LRZ(string rezName) { return rezName; }
        public string GetApplicationDataPath(Environment.SpecialFolder baseFolder) { return Path.Combine(Environment.GetFolderPath(baseFolder), "Reno"); }

        // The project is shown when the script ends: nothing to do now.
        public void UpdateUI() { Op(Abi.RO_UPDATE_UI, null, null, null); }
        public void WaitForIdle() { }
        public void ResetFPU() { }
        public void DebugOut(string sz) { Reno.Log(sz); }
        public void DebugClear() { }
        public void LogEvent(string str) { Reno.Log(str); }
        public void ShowError(string msg) { ShowError(msg, null); }
        public void ShowError(Exception e) { ShowError(e.Message, e.ToString()); }
        public void ShowError(string msg, string details)
        {
            Reno.Log("Error: " + msg + (string.IsNullOrEmpty(details) ? "" : "\n" + details));
            MessageBox.Show(MainWindow, msg + (string.IsNullOrEmpty(details) ? "" : "\n\n" + details), ShortAppName, MessageBoxButtons.OK, MessageBoxIcon.Error);
        }

        public bool NewProject() { return NewProject(true, false); }
        public bool NewProject(bool promptSave, bool showDialog)
        {
            Op(Abi.RO_NEW_PROJECT, null, null, null);
            project = null;
            Reno.FrameRate = GD(Abi.RP_FRAME_RATE);
            return true;
        }
        public Project CreateEmptyProject() { throw NotSupported("CreateEmptyProject"); }
        public Project ReadProject(string path) { throw NotSupported("ReadProject"); }
        public bool OpenProject() { throw NotSupported("OpenProject"); }
        public bool OpenProject(string fileName)
        {
            Op(Abi.RO_OPEN_PROJECT, null, null, fileName);
            project = null;
            Reno.FrameRate = GD(Abi.RP_FRAME_RATE);
            return true;
        }
        public bool SaveProject() { Op(Abi.RO_SAVE_PROJECT, null, null, ""); return true; }
        public bool SaveProject(string fileName) { Op(Abi.RO_SAVE_PROJECT, null, null, fileName); return true; }
        public bool SaveProjectDialog(string fileName) { throw NotSupported("SaveProjectDialog"); }
        public void OpenFile(string fileName) { throw NotSupported("OpenFile"); }
        public void OpenFile(string fileName, bool mediaPoolOnly) { if (mediaPoolOnly) Project.MediaPool.AddMedia(fileName); else throw NotSupported("OpenFile"); }
        public void ImportFile(string fileName) { Project.MediaPool.AddMedia(fileName); }
        public void ImportFile(string fileName, bool mediaPoolOnly) { Project.MediaPool.AddMedia(fileName); }
        public void AddMedia(string filename) { Project.MediaPool.AddMedia(filename); }
        public DialogResult OpenFileDialog() { throw NotSupported("OpenFileDialog"); }
        public DialogResult ImportFileDialog() { throw NotSupported("ImportFileDialog"); }
        public string MediaInfo(string filePath) { throw NotSupported("MediaInfo"); }
        public void PreviewStart(string filePath) { throw NotSupported("PreviewStart"); }
        public void PreviewStop() { }
        public void Exit() { throw NotSupported("Exit"); }
        public void InvokeCommand(string section, string name) { throw NotSupported("InvokeCommand"); }
        public void RunScriptFile(string scriptFile) { throw NotSupported("RunScriptFile"); }
        public void RunScriptFile(string scriptFile, string scriptArgs) { throw NotSupported("RunScriptFile"); }
        public RenderStatus Render(string outputFile, RenderTemplate renderTemplate) { throw NotSupported("Render"); }
        public RenderStatus Render(string outputFile, RenderTemplate renderTemplate, Timecode start, Timecode length) { throw NotSupported("Render"); }
        public RenderStatus SaveSnapshot(string outputFile, ImageFileFormat format) { throw NotSupported("SaveSnapshot"); }
        public RenderStatus SaveSnapshot(string outputFile, ImageFileFormat format, Timecode seekTime) { throw NotSupported("SaveSnapshot"); }
        public Renderers Renderers { get { return new Renderers(); } }
        public IFileUtilities FileUtilities { get { return new FileUtilities(); } }
        public void CancelAsynchronousTasks() { }
        public int UIScale(int scale) { return scale; }
        public double UIScaleFactor() { return 1.0; }
        public string GetRecentProjectPath(int idx) { return null; }
    }

    // An owner window for the script's dialogs (Reno's main window).
    public partial class Win32Window : IWin32Window
    {
        IntPtr hwnd;
        public Win32Window(IntPtr hwnd) { this.hwnd = hwnd; }
        public IntPtr Handle { get { return hwnd; } }
    }

    // VEGAS's Script: the script being run.
    public partial class Script
    {
        public Script() { }
        public static Vegas Application { get { return Reno.App; } }
        public static string File { get { return Reno.GS(1, Abi.RP_SCRIPT_FILE, "Script.File"); } }
        public static string RawArgs { get { return Reno.GS(1, Abi.RP_SCRIPT_ARGS, "Script.RawArgs"); } }
        public static string Name { get { return Path.GetFileNameWithoutExtension(File); } }
        public static string Directory { get { return Path.GetDirectoryName(File); } }
        public static ScriptArgs Args { get { ScriptArgs a = new ScriptArgs(); a.Parse(RawArgs); return a; } }
        public static System.Xml.XmlDocument Settings { get { return null; } }
    }

    public partial class ScriptArg
    {
        public string Name, Value;
        public static char[] InvalidCharacters = new char[] { '&', '=' };
        public static char[] SeparatorCharacters = new char[] { '=' };
        public ScriptArg() { }
        public ScriptArg(string szArg)
        {
            int eq = (szArg ?? "").IndexOf('=');
            Name = eq < 0 ? szArg : szArg.Substring(0, eq);
            Value = eq < 0 ? "" : szArg.Substring(eq + 1);
        }
        public static bool IsValidNameString(string val) { return val != null && val.IndexOfAny(InvalidCharacters) < 0; }
        public static bool IsValidValueString(string val) { return val != null && val.IndexOf('&') < 0; }
        public override string ToString() { return Name + "=" + Value; }
    }

    // A script's arguments: "name=value&name2=value2".
    public partial class ScriptArgs : CollectionBase
    {
        public static char[] SeparatorCharacters = new char[] { '&' };
        public ScriptArgs() { }
        internal void Parse(string raw)
        {
            if (string.IsNullOrEmpty(raw)) return;
            foreach (string part in raw.Split(SeparatorCharacters, StringSplitOptions.RemoveEmptyEntries)) List.Add(new ScriptArg(part));
        }
        public ScriptArg this[int index] { get { return (ScriptArg)List[index]; } set { List[index] = value; } }
        public void Add(ScriptArg aArg) { List.Add(aArg); }
        public void Remove(int index) { List.RemoveAt(index); }
        ScriptArg Find(string key, bool ignoreCase)
        {
            foreach (ScriptArg a in List)
                if (string.Compare(a.Name, key, ignoreCase ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal) == 0) return a;
            return null;
        }
        public bool Exists(string sKey) { return Find(sKey, true) != null; }
        public bool Exists(string sKey, bool fIgnoreCase) { return Find(sKey, fIgnoreCase) != null; }
        public string ValueOf(string sKey) { ScriptArg a = Find(sKey, true); return a == null ? null : a.Value; }
        public string ValueOf(string sKey, bool fIgnoreCase) { ScriptArg a = Find(sKey, fIgnoreCase); return a == null ? null : a.Value; }
        public bool AsBool(string sKey)
        {
            string v = ValueOf(sKey);
            return v != null && (v == "1" || string.Compare(v, "true", StringComparison.OrdinalIgnoreCase) == 0 || string.Compare(v, "yes", StringComparison.OrdinalIgnoreCase) == 0);
        }
        public int AsInt(string sKey) { return AsInt(sKey, 0); }
        public int AsInt(string sKey, int iDefault) { int r; return int.TryParse(ValueOf(sKey), out r) ? r : iDefault; }
        public long AsInt64(string sKey) { return AsInt64(sKey, 0); }
        public long AsInt64(string sKey, long llDefault) { long r; return long.TryParse(ValueOf(sKey), out r) ? r : llDefault; }
        public Timecode AsTimecode(string sKey) { return AsTimecode(sKey, new Timecode()); }
        public Timecode AsTimecode(string sKey, Timecode tcDefault) { return AsTimecode(sKey, tcDefault, RulerFormat.Unknown); }
        public Timecode AsTimecode(string sKey, Timecode tcDefault, RulerFormat fmt)
        {
            string v = ValueOf(sKey);
            if (v == null) return tcDefault;
            try { return Timecode.FromString(v, fmt); } catch (FormatException) { return tcDefault; }
        }
        public override string ToString()
        {
            StringBuilder b = new StringBuilder();
            foreach (ScriptArg a in List) { if (b.Length > 0) b.Append('&'); b.Append(a.ToString()); }
            return b.ToString();
        }
    }

    // VEGAS's UndoBlock: the script's changes as one named undo step. In Reno the whole run of a script is one undo
    // step; the first block's label names it. Cancel = true leaves the project as it was before the script.
    public partial class UndoBlock : IDisposable
    {
        bool cancel;
        public UndoBlock(string label) { if (!string.IsNullOrEmpty(label)) Reno.SS(1, Abi.RP_UNDO_LABEL, label, "UndoBlock"); }
        public UndoBlock(Project project, string label) : this(label) { }
        public bool Cancel { get { return cancel; } set { cancel = value; Reno.SI(1, Abi.RP_UNDO_CANCEL, value ? 1 : 0, "UndoBlock.Cancel"); } }
        public virtual void Dispose() { }
    }

    // VEGAS's Transport: the cursor and the time selection (Reno's loop region).
    public partial class TransportControl
    {
        internal TransportControl() { }
        Vegas V { get { return Reno.App; } }
        public bool IsPlaying { get { return false; } }
        public bool IsRecording { get { return false; } }
        public bool IsRecordMonitoring { get { return false; } }
        public Timecode CursorPosition { get { return V.Cursor; } set { V.Cursor = value; } }
        public Timecode PlayCursorPosition { get { return V.Cursor; } }
        public Timecode SelectionStart { get { return V.SelectionStart; } set { V.SelectionStart = value; } }
        public Timecode SelectionLength { get { return V.SelectionLength; } set { V.SelectionLength = value; } }
        public Timecode LoopRegionStart { get { return V.SelectionStart; } set { V.SelectionStart = value; } }
        public Timecode LoopRegionLength { get { return V.SelectionLength; } set { V.SelectionLength = value; } }
        public bool LoopMode { get { return V.LoopMode; } set { V.LoopMode = value; } }
        public void Play() { throw new NotSupportedException("TransportControl.Play is not supported by Reno (scripts run while playback is stopped)."); }
        public void PlayFromStart() { throw new NotSupportedException("TransportControl.PlayFromStart is not supported by Reno."); }
        public void Stop() { }
        public void Pause() { }
        public void ViewCursor(bool centered) { }
        public void ZoomSelection() { }
        public IDisposable Suspend() { return new UndoBlock(null); }
        public IDisposable SuspendForFX() { return new UndoBlock(null); }
    }

    // ------------------------------------------------------------------------------------------------ the project
    public partial class Project : RenoObject, IDisposable
    {
        internal Project(Vegas app) : base(1) { }
        public Project() : base(1) { throw new NotSupportedException("new Project() is not supported by Reno: use Vegas.NewProject() (Reno has one project at a time)."); }
        public Project(bool promptSave, bool showDialog) : this() { }

        public static Project ActiveProject { get { return Reno.App.Project; } }
        public Tracks Tracks { get { return new Tracks(this); } }
        public BusTracks BusTracks { get { return new BusTracks(this); } }
        public AudioBusTrack MasterBus { get { return Reno.W<AudioBusTrack>(Reno.List(1, Abi.RL_SPECIAL_BUSES, "Project.MasterBus")[0]); } }
        public VideoBusTrack VideoBus { get { return Reno.W<VideoBusTrack>(Reno.List(1, Abi.RL_SPECIAL_BUSES, "Project.VideoBus")[1]); } }
        public MediaPool MediaPool { get { return new MediaPool(); } }
        public MarkerList Markers { get { return new MarkerList(this); } }
        public RegionList Regions { get { return new RegionList(this); } }
        public CommandMarkerList CommandMarkers { get { return new CommandMarkerList(this); } }
        public CDMarkerList CDIndices { get { return new CDMarkerList(this); } }
        public CDRegionList CDTracks { get { return new CDRegionList(this); } }
        public TrackEventGroups TrackEventGroups { get { return new TrackEventGroups(this); } }
        public TrackEventGroups Groups { get { return new TrackEventGroups(this); } }
        public ProjectVideoProperties Video { get { return new ProjectVideoProperties(); } }
        public ProjectAudioProperties Audio { get { return new ProjectAudioProperties(); } }
        public RulerProperties Ruler { get { return new RulerProperties(); } }
        public SummaryProperties Summary { get { return new SummaryProperties(); } }
        public Timecode Length { get { return GT(Abi.RP_PROJECT_LENGTH); } }
        public bool IsModified { get { return GB(Abi.RP_IS_MODIFIED); } }
        public bool IsUntitled { get { return string.IsNullOrEmpty(FilePath); } }
        public string FilePath { get { string p = GS(Abi.RP_PROJECT_PATH); return p.Length == 0 ? null : p; } }
        public TrackGroups TrackGroups { get { return new TrackGroups(); } }
        public PreviewVideoProperties Preview { get { return new PreviewVideoProperties(); } }
        public AudioCDProperties AudioCD { get { return new AudioCDProperties(); } }
        public CustomDataContainer CustomData { get { return CustomDataContainer.Of(1); } }
        public string RenderPath { get { return ""; } set { } }
        public string DefaultRenderPath { get { return ""; } }

        public virtual void Dispose() { }
        public VideoTrack AddVideoTrack() { VideoTrack t = new VideoTrack(); Tracks.Add(t); return t; }
        public AudioTrack AddAudioTrack() { AudioTrack t = new AudioTrack(); Tracks.Add(t); return t; }
        public void Undo() { throw NotSupported("Undo"); }
        public void Redo() { throw NotSupported("Redo"); }
        public void InsertTime() { InsertTime(Reno.App.SelectionStart, Reno.App.SelectionLength); }
        public void InsertTime(Timecode start, Timecode length) { Op(Abi.RO_INSERT_TIME, null, new double[] { start.ToSeconds(), length.ToSeconds() }, null); }
        public void InsertTime(Timecode start, Timecode length, bool ignoreSelection) { InsertTime(start, length); }
        public bool SaveProject(string fileName) { return Reno.App.SaveProject(fileName); }
        public bool MatchProjectSettingsWithMedia(Media media) { return Video.MatchProjectSettingsWithMedia(media); }
    }

    public abstract partial class VideoProperties
    {
        protected static long I(int p, string m) { return Reno.GI(1, p, m); }
        protected static double D(int p, string m) { return Reno.GD(1, p, m); }
        public int Width { get { return (int)I(Abi.RP_WIDTH, "VideoProperties.Width"); } set { Reno.SI(1, Abi.RP_WIDTH, value, "VideoProperties.Width"); } }
        public int Height { get { return (int)I(Abi.RP_HEIGHT, "VideoProperties.Height"); } set { Reno.SI(1, Abi.RP_HEIGHT, value, "VideoProperties.Height"); } }
        public double FrameRate
        {
            get { return D(Abi.RP_FRAME_RATE, "VideoProperties.FrameRate"); }
            set { Reno.SD(1, Abi.RP_FRAME_RATE, value, "VideoProperties.FrameRate"); Reno.FrameRate = FrameRate; }
        }
        public VideoFieldOrder FieldOrder
        {
            get { return (VideoFieldOrder)I(Abi.RP_FIELD_ORDER, "VideoProperties.FieldOrder"); }
            set { Reno.SI(1, Abi.RP_FIELD_ORDER, (long)value, "VideoProperties.FieldOrder"); }
        }
        public double PixelAspectRatio
        {
            get { return D(Abi.RP_PIXEL_ASPECT, "VideoProperties.PixelAspectRatio"); }
            set { Reno.SD(1, Abi.RP_PIXEL_ASPECT, value, "VideoProperties.PixelAspectRatio"); }
        }
    }

    public partial class ProjectVideoProperties : VideoProperties
    {
        internal ProjectVideoProperties() { }
        public VideoOutputRotation OutputRotation
        {
            get
            {
                long d = I(Abi.RP_OUTPUT_ROTATION, "ProjectVideoProperties.OutputRotation");
                return d == 90 ? VideoOutputRotation.QuarterTurnClockwise : d == 180 ? VideoOutputRotation.Inverted : d == 270 ? VideoOutputRotation.QuarterTurnCounterclockwise : VideoOutputRotation.None;
            }
            set
            {
                long d = value == VideoOutputRotation.QuarterTurnClockwise ? 90 : value == VideoOutputRotation.Inverted ? 180 : value == VideoOutputRotation.QuarterTurnCounterclockwise ? 270 : 0;
                Reno.SI(1, Abi.RP_OUTPUT_ROTATION, d, "ProjectVideoProperties.OutputRotation");
            }
        }
        public VideoRenderQuality RenderQuality
        {
            get { return (VideoRenderQuality)I(Abi.RP_RENDER_QUALITY, "ProjectVideoProperties.RenderQuality"); }
            set { Reno.SI(1, Abi.RP_RENDER_QUALITY, (long)value, "ProjectVideoProperties.RenderQuality"); }
        }
        public MotionBlurType MotionBlurType
        {
            get { return (MotionBlurType)I(Abi.RP_MOTION_BLUR, "ProjectVideoProperties.MotionBlurType"); }
            set { Reno.SI(1, Abi.RP_MOTION_BLUR, (long)value, "ProjectVideoProperties.MotionBlurType"); }
        }
        public VideoDeinterlaceMethod DeinterlaceMethod
        {
            get { return (VideoDeinterlaceMethod)I(Abi.RP_DEINTERLACE, "ProjectVideoProperties.DeinterlaceMethod"); }
            set { Reno.SI(1, Abi.RP_DEINTERLACE, (long)value, "ProjectVideoProperties.DeinterlaceMethod"); }
        }
        public PixelFormat PixelFormat
        {
            get { return (PixelFormat)I(Abi.RP_PIXEL_FORMAT, "ProjectVideoProperties.PixelFormat"); }
            set { Reno.SI(1, Abi.RP_PIXEL_FORMAT, (long)value, "ProjectVideoProperties.PixelFormat"); }
        }
        public T360Output T360Output
        {
            get { return (T360Output)I(Abi.RP_OUTPUT_360, "ProjectVideoProperties.T360Output"); }
            set { Reno.SI(1, Abi.RP_OUTPUT_360, (long)value, "ProjectVideoProperties.T360Output"); }
        }
        public string PrerenderedFilesFolder { get { return ""; } set { throw new NotSupportedException("ProjectVideoProperties.PrerenderedFilesFolder is not supported by Reno."); } }
        public bool MatchProjectSettingsWithMedia(Media media)
        {
            if (media == null) return false;
            VideoStream vs = media.GetVideoStreamByIndex(0);
            if (vs == null) return false;
            Width = vs.Width;
            Height = vs.Height;
            if (vs.FrameRate > 0) FrameRate = vs.FrameRate;
            FieldOrder = vs.FieldOrder;
            return true;
        }
        public bool MatchProjectSettingsWithFile(string filePath) { return MatchProjectSettingsWithMedia(Reno.App.Project.MediaPool.AddMedia(filePath)); }
    }

    public partial class AudioProperties
    {
        public uint SampleRate
        {
            get { return (uint)Reno.GI(1, Abi.RP_SAMPLE_RATE, "AudioProperties.SampleRate"); }
            set { Reno.SI(1, Abi.RP_SAMPLE_RATE, value, "AudioProperties.SampleRate"); }
        }
        public uint BitDepth
        {
            get { return (uint)Reno.GI(1, Abi.RP_BIT_DEPTH, "AudioProperties.BitDepth"); }
            set { Reno.SI(1, Abi.RP_BIT_DEPTH, value, "AudioProperties.BitDepth"); }
        }
    }

    public partial class ProjectAudioProperties : AudioProperties
    {
        internal ProjectAudioProperties() { }
        public AudioBusMode MasterBusMode
        {
            get { return (AudioBusMode)Reno.GI(1, Abi.RP_BUS_MODE, "ProjectAudioProperties.MasterBusMode"); }
            set { Reno.SI(1, Abi.RP_BUS_MODE, (long)value, "ProjectAudioProperties.MasterBusMode"); }
        }
        public string RecordedFilesFolder { get { return ""; } set { throw new NotSupportedException("ProjectAudioProperties.RecordedFilesFolder is not supported by Reno."); } }
        public bool MatchProjectSettingsWithMedia(Media media)
        {
            if (media == null) return false;
            AudioStream a = media.GetAudioStreamByIndex(0);
            if (a == null) return false;
            SampleRate = a.SampleRate;
            return true;
        }
    }

    public partial class RulerProperties
    {
        internal RulerProperties() { }
        public RulerFormat Format
        {
            get { return (RulerFormat)Reno.GI(1, Abi.RP_RULER_FORMAT, "RulerProperties.Format"); }
            set { Reno.SI(1, Abi.RP_RULER_FORMAT, (long)value, "RulerProperties.Format"); }
        }
        public Timecode StartTime
        {
            get { return Timecode.FromSeconds(Reno.GD(1, Abi.RP_RULER_START, "RulerProperties.StartTime")); }
            set { Reno.SD(1, Abi.RP_RULER_START, value.ToSeconds(), "RulerProperties.StartTime"); }
        }
        public double BeatsPerMinute
        {
            get { return Reno.GD(1, Abi.RP_TEMPO, "RulerProperties.BeatsPerMinute"); }
            set { Reno.SD(1, Abi.RP_TEMPO, value, "RulerProperties.BeatsPerMinute"); }
        }
        public uint BeatsPerMeasure
        {
            get { return (uint)Reno.GI(1, Abi.RP_BEATS_PER_MEASURE, "RulerProperties.BeatsPerMeasure"); }
            set { Reno.SI(1, Abi.RP_BEATS_PER_MEASURE, value, "RulerProperties.BeatsPerMeasure"); }
        }
    }

    public partial class SummaryProperties
    {
        internal SummaryProperties() { }
        static string G(int p, string m) { return Reno.GS(1, p, m); }
        static void S(int p, string v, string m) { Reno.SS(1, p, v, m); }
        public string Title { get { return G(Abi.RP_TITLE, "SummaryProperties.Title"); } set { S(Abi.RP_TITLE, value, "SummaryProperties.Title"); } }
        public string Artist { get { return G(Abi.RP_ARTIST, "SummaryProperties.Artist"); } set { S(Abi.RP_ARTIST, value, "SummaryProperties.Artist"); } }
        public string Engineer { get { return G(Abi.RP_ENGINEER, "SummaryProperties.Engineer"); } set { S(Abi.RP_ENGINEER, value, "SummaryProperties.Engineer"); } }
        public string Copyright { get { return G(Abi.RP_COPYRIGHT, "SummaryProperties.Copyright"); } set { S(Abi.RP_COPYRIGHT, value, "SummaryProperties.Copyright"); } }
        public string Comments { get { return G(Abi.RP_COMMENTS, "SummaryProperties.Comments"); } set { S(Abi.RP_COMMENTS, value, "SummaryProperties.Comments"); } }
        public string GetItem(string labelOrFcc)
        {
            switch ((labelOrFcc ?? "").ToUpperInvariant())
            {
            case "INAM": case "TITLE": return Title;
            case "IART": case "ARTIST": return Artist;
            case "IENG": case "ENGINEER": return Engineer;
            case "ICOP": case "COPYRIGHT": return Copyright;
            case "ICMT": case "COMMENTS": return Comments;
            }
            throw new NotSupportedException("SummaryProperties.GetItem(\"" + labelOrFcc + "\") is not supported by Reno.");
        }
    }

    // ------------------------------------------------------------------------------------------------ tracks
    public partial interface IEffectOwner { }
    public partial interface IEnvelopeOwner { }

    public abstract partial class Track : RenoObject, IEffectOwner, IEnvelopeOwner
    {
        internal int newIndex = -1;         // a script's new track: where it goes
        internal Track(ulong h) : base(h) { }
        protected Track(int index, string name) { newIndex = index; if (name != null) SS(Abi.RP_NAME, name, "Name"); }

        public virtual MediaType MediaType { get { return IsAudio() ? MediaType.Audio : MediaType.Video; } }
        public TrackEvents Events { get { return new TrackEvents(this); } }
        public Project Project { get { return Reno.App.Project; } }
        public Envelopes Envelopes { get { return new Envelopes(this); } }
        public Effects Effects { get { return new Effects(this); } }
        public string Name { get { return GS(Abi.RP_NAME); } set { SS(Abi.RP_NAME, value); } }
        public Timecode Length { get { return GT(Abi.RP_LENGTH); } }
        public int Index { get { return H == 0 ? newIndex : (int)GI(Abi.RP_INDEX); } }
        public int DisplayIndex { get { return Index; } }
        public bool Solo { get { return GB(Abi.RP_SOLO); } set { SB(Abi.RP_SOLO, value); } }
        public bool Mute { get { return GB(Abi.RP_MUTE); } set { SB(Abi.RP_MUTE, value); } }
        public bool Selected { get { return GB(Abi.RP_SELECTED); } set { SB(Abi.RP_SELECTED, value); } }
        public virtual BusTrack BusTrack { get { throw NotSupported("BusTrack"); } set { throw NotSupported("BusTrack"); } }
        public CustomDataContainer CustomData { get { return CustomDataContainer.Of(H); } }
        public abstract bool IsAudio();
        public abstract bool IsVideo();
        public virtual bool CanAddEnvelope(EnvelopeType type)
        {
            if (IsAudio()) return type == EnvelopeType.Volume || type == EnvelopeType.Pan || type == EnvelopeType.Mute || (type >= EnvelopeType.BusA && type <= EnvelopeType.BusZ);
            return type == EnvelopeType.Composite || type == EnvelopeType.FadeToColor || type == EnvelopeType.Mute;
        }
    }

    public partial class VideoTrack : Track
    {
        internal VideoTrack(ulong h) : base(h) { }
        public VideoTrack() : base(-1, null) { }
        public VideoTrack(int index) : base(index, null) { }
        public VideoTrack(int index, string name) : base(index, name) { }
        public VideoTrack(Project project, int index, string name) : base(index, name) { }

        public override MediaType MediaType { get { return MediaType.Video; } }
        public override bool IsAudio() { return false; }
        public override bool IsVideo() { return true; }
        public float CompositeLevel { get { return (float)GD(Abi.RP_LEVEL); } set { SD(Abi.RP_LEVEL, value); } }
        public CompositeMode CompositeMode { get { return (CompositeMode)GI(Abi.RP_COMPOSITE_MODE); } set { SI(Abi.RP_COMPOSITE_MODE, (long)value); } }
        public CompositeMode ParentCompositeMode { get { return (CompositeMode)GI(Abi.RP_PARENT_COMPOSITE_MODE); } set { SI(Abi.RP_PARENT_COMPOSITE_MODE, (long)value); } }
        public int CompositeNestingLevel { get { return (int)GI(Abi.RP_NESTING_LEVEL); } set { SI(Abi.RP_NESTING_LEVEL, value); } }
        public bool IsCompositingChild { get { return CompositeNestingLevel > 0; } }
        public bool IsCompositingParent
        {
            get
            {
                if (H == 0) return false;
                List<Track> all = new Tracks(Project).Items();
                int i = Index;
                return i + 1 < all.Count && all[i + 1] is VideoTrack && ((VideoTrack)all[i + 1]).CompositeNestingLevel > CompositeNestingLevel;
            }
        }
        public bool SetCompositeMode(CompositeMode mode, bool showWarningDialog) { CompositeMode = mode; return true; }
        public bool SetParentCompositeMode(CompositeMode mode, bool showWarningDialog) { ParentCompositeMode = mode; return true; }
        public VideoColor TopFadeColor { get { return new VideoColor((uint)GI(Abi.RP_TOP_FADE_COLOR)); } set { SI(Abi.RP_TOP_FADE_COLOR, value.ToArgb()); } }
        public VideoColor BottomFadeColor { get { return new VideoColor((uint)GI(Abi.RP_BOTTOM_FADE_COLOR)); } set { SI(Abi.RP_BOTTOM_FADE_COLOR, value.ToArgb()); } }
        public Effect CompositeModeEffect { get { throw NotSupported("CompositeModeEffect"); } set { throw NotSupported("CompositeModeEffect"); } }
        public Effect ParentCompositeModeEffect { get { throw NotSupported("ParentCompositeModeEffect"); } set { throw NotSupported("ParentCompositeModeEffect"); } }
        public bool BypassMotionBlur { get { return false; } set { throw NotSupported("BypassMotionBlur"); } }
        public TrackMotion TrackMotion { get { return new TrackMotion(this); } }
        public TrackMotion ParentTrackMotion { get { throw NotSupported("ParentTrackMotion"); } }
        public VideoEvent AddVideoEvent() { return AddVideoEvent(Reno.App.Cursor, Timecode.FromSeconds(1)); }
        public VideoEvent AddVideoEvent(Timecode start, Timecode length)
        {
            VideoEvent e = new VideoEvent(start, length);
            Events.Add(e);
            return e;
        }
        public override bool CanAddEnvelope(EnvelopeType type) { return base.CanAddEnvelope(type); }
    }

    public partial class AudioTrack : Track
    {
        internal AudioTrack(ulong h) : base(h) { }
        public AudioTrack() : base(-1, null) { }
        public AudioTrack(int index) : base(index, null) { }
        public AudioTrack(int index, string name) : base(index, name) { }
        public AudioTrack(Project project, int index, string name) : base(index, name) { }

        public override MediaType MediaType { get { return MediaType.Audio; } }
        public override bool IsAudio() { return true; }
        public override bool IsVideo() { return false; }
        public float Volume { get { return (float)GD(Abi.RP_LEVEL); } set { SD(Abi.RP_LEVEL, value); } }
        public float PanX { get { return (float)GD(Abi.RP_PAN); } set { SD(Abi.RP_PAN, value); } }
        public float AutomatedPanX { get { return PanX; } set { PanX = value; } }
        public float PanY { get { return 0; } set { throw NotSupported("PanY"); } }
        public float PanCenter { get { return 0; } set { throw NotSupported("PanCenter"); } }
        public bool InvertPhase { get { return false; } set { throw NotSupported("InvertPhase"); } }
        public bool ArmRecord { get { return false; } set { throw NotSupported("ArmRecord"); } }
        public override BusTrack BusTrack
        {
            get { ulong b = (ulong)GI(Abi.RP_TRACK_BUS); return b == 0 ? (BusTrack)Reno.App.Project.MasterBus : Reno.W<BusTrack>(b); }
            set { SI(Abi.RP_TRACK_BUS, value == null ? 0 : (long)value.H); }
        }
        public AudioEvent AddAudioEvent() { return AddAudioEvent(Reno.App.Cursor, Timecode.FromSeconds(1)); }
        public AudioEvent AddAudioEvent(Timecode start, Timecode length)
        {
            AudioEvent e = new AudioEvent(start, length);
            Events.Add(e);
            return e;
        }
        public override bool CanAddEnvelope(EnvelopeType type) { return base.CanAddEnvelope(type); }
    }

    public partial class Tracks : BaseList<Track>
    {
        internal Tracks(Project p) { }
        internal override List<Track> Items() { return Reno.WrapAll<Track>(Reno.List(1, Abi.RL_TRACKS, "Tracks")); }
        internal override void AddItem(Track t)
        {
            if (t.H != 0) throw new InvalidOperationException("Tracks.Add: the track is already in the project");
            ulong h = Reno.Call(1, Abi.RO_ADD_TRACK, new long[] { t.IsAudio() ? 2 : 3, t.newIndex }, null, "", "Tracks.Add");
            t.Attached(h);
        }
        internal override bool RemoveItem(Track t)
        {
            if (t.H == 0) return false;
            Reno.Call(1, Abi.RO_REMOVE_TRACK, new long[] { (long)t.H }, null, null, "Tracks.Remove");
            return true;
        }
        public override void Insert(int index, Track item) { item.newIndex = index; Add(item); }
    }

    // ------------------------------------------------------------------------------------------------ events
    public abstract partial class TrackEvent : RenoObject
    {
        internal TrackEvent(ulong h) : base(h) { }
        protected TrackEvent(Timecode start, Timecode length, string name)
        {
            if (start != null) SD(Abi.RP_START, start.ToSeconds(), "Start");
            SD(Abi.RP_LENGTH, length != null ? length.ToSeconds() : 1.0, "Length");
            if (name != null) SS(Abi.RP_NAME, name, "Name");
        }

        public Project Project { get { return Reno.App.Project; } }
        public virtual MediaType MediaType { get { return IsAudio() ? MediaType.Audio : MediaType.Video; } }
        public Track Track
        {
            get { return H == 0 ? null : Reno.W<Track>((ulong)GI(Abi.RP_TRACK)); }
            set { NeedAdded("Track"); SI(Abi.RP_TRACK, value == null ? 0 : (long)value.H); }
        }
        public Takes Takes { get { return new Takes(this); } }
        public Take ActiveTake
        {
            get { return H == 0 ? null : Reno.W<Take>((ulong)GI(Abi.RP_ACTIVE_TAKE)); }
            set { NeedAdded("ActiveTake"); SI(Abi.RP_ACTIVE_TAKE, value == null ? 0 : (long)value.H); }
        }
        public Fade FadeIn { get { return new Fade(this, true); } }
        public Fade FadeOut { get { return new Fade(this, false); } }
        public int Index { get { return H == 0 ? -1 : (int)GI(Abi.RP_INDEX); } }
        public string Name { get { return GS(Abi.RP_NAME); } set { SS(Abi.RP_NAME, value); } }
        public Timecode Start { get { return GT(Abi.RP_START); } set { ST(Abi.RP_START, value); } }
        public Timecode Length { get { return GT(Abi.RP_LENGTH); } set { ST(Abi.RP_LENGTH, value); } }
        public Timecode End
        {
            get { return H == 0 ? Start + Length : GT(Abi.RP_END); }
            set { if (H == 0) Length = value - Start; else ST(Abi.RP_END, value); }
        }
        public double PlaybackRate
        {
            get { return H == 0 && (pending == null || !pending.ContainsKey(Abi.RP_RATE)) ? 1.0 : GD(Abi.RP_RATE); }
            set { SD(Abi.RP_RATE, value); }
        }
        public bool Mute { get { return GB(Abi.RP_MUTE); } set { SB(Abi.RP_MUTE, value); } }
        public bool Locked { get { return GB(Abi.RP_LOCKED); } set { SB(Abi.RP_LOCKED, value); } }
        public bool Loop { get { return GB(Abi.RP_LOOP); } set { SB(Abi.RP_LOOP, value); } }
        public bool Selected { get { return GB(Abi.RP_SELECTED); } set { SB(Abi.RP_SELECTED, value); } }
        public bool IsGrouped { get { return H != 0 && GI(Abi.RP_GROUP) != 0; } }
        public TrackEventGroup Group { get { return H == 0 ? null : Reno.W<TrackEventGroup>((ulong)GI(Abi.RP_GROUP)); } }
        public Timecode SyncOffset { get { throw NotSupported("SyncOffset"); } }
        public TrackEvent SyncEvent { get { throw NotSupported("SyncEvent"); } }
        public Timecode SnapOffset { get { return new Timecode(); } set { throw NotSupported("SnapOffset"); } }
        public CustomDataContainer CustomData { get { return CustomDataContainer.Of(H); } }
        public int SourceStreamIndex { get { return 0; } }
        public abstract bool IsAudio();
        public abstract bool IsVideo();

        public Take AddTake(MediaStream mediaStream) { return AddTake(mediaStream, true, null); }
        public Take AddTake(MediaStream mediaStream, bool makeActive) { return AddTake(mediaStream, makeActive, null); }
        public Take AddTake(MediaStream mediaStream, bool makeActive, string name)
        {
            if (mediaStream == null) throw new ArgumentNullException("mediaStream");
            NeedAdded("AddTake");
            return Reno.W<Take>(Op(Abi.RO_ADD_TAKE, new long[] { (long)mediaStream.H, makeActive ? 1 : 0 }, null, name ?? ""));
        }
        public void AdjustStartLength(Timecode start, Timecode length, bool adjustTakes)
        {
            Op(Abi.RO_ADJUST_START_LENGTH, new long[] { adjustTakes ? 1 : 0 }, new double[] { start.ToSeconds(), length.ToSeconds() }, null);
        }
        public TrackEvent Split(Timecode offset) { return Reno.W<TrackEvent>(Op(Abi.RO_SPLIT, null, new double[] { offset.ToSeconds() }, null)); }
        public TrackEvent Copy(Track destination, Timecode startTime)
        {
            if (destination == null) throw new ArgumentNullException("destination");
            return Reno.W<TrackEvent>(Op(Abi.RO_COPY, new long[] { (long)destination.H }, new double[] { startTime.ToSeconds() }, null));
        }
        public void AdjustPlaybackRate(double rate, bool adjustStart) { Op(Abi.RO_ADJUST_RATE, new long[] { adjustStart ? 1 : 0 }, new double[] { rate }, null); }
        public void OpenVideoEffectUI() { throw NotSupported("OpenVideoEffectUI"); }
        public void OpenMediaGeneratorUI() { throw NotSupported("OpenMediaGeneratorUI"); }
        public void SyncByMove(bool selected) { throw NotSupported("SyncByMove"); }
        public void SyncBySlip(bool selected) { throw NotSupported("SyncBySlip"); }

        // Reno's: the event plays its media backwards (VEGAS scripts reverse an event with a reversed Subclip take)
        public bool RenoReverse { get { return GB(Abi.RP_REVERSE); } set { SB(Abi.RP_REVERSE, value); } }
    }

    public partial class VideoEvent : TrackEvent, IEffectOwner, IEnvelopeOwner
    {
        internal VideoEvent(ulong h) : base(h) { }
        public VideoEvent() : base(null, null, null) { }
        public VideoEvent(Timecode start) : base(start, null, null) { }
        public VideoEvent(Timecode start, Timecode length) : base(start, length, null) { }
        public VideoEvent(Timecode start, Timecode length, string name) : base(start, length, name) { }
        public VideoEvent(Project project, Timecode start, Timecode length, string name) : base(start, length, name) { }

        public override MediaType MediaType { get { return MediaType.Video; } }
        public override bool IsAudio() { return false; }
        public override bool IsVideo() { return true; }
        public Envelopes Envelopes { get { return new Envelopes(this); } }
        public Effects Effects { get { return new Effects(this); } }
        public double UnderSampleRate { get { return H == 0 ? 1.0 : GD(Abi.RP_UNDERSAMPLE); } set { SD(Abi.RP_UNDERSAMPLE, value); } }
        public bool MaintainAspectRatio { get { return H == 0 || GB(Abi.RP_MAINTAIN_ASPECT); } set { SB(Abi.RP_MAINTAIN_ASPECT, value); } }
        public bool ReduceInterlace { get { return GB(Abi.RP_REDUCE_INTERLACE); } set { SB(Abi.RP_REDUCE_INTERLACE, value); } }
        public VideoResampleMode ResampleMode { get { return (VideoResampleMode)GI(Abi.RP_RESAMPLE_MODE); } set { SI(Abi.RP_RESAMPLE_MODE, (long)value); } }
        public VideoMotion VideoMotion { get { return new VideoMotion(this); } }
        public virtual bool CanAddEnvelope(EnvelopeType type)
        {
            return type == EnvelopeType.Composite || type == EnvelopeType.Velocity || type == EnvelopeType.TransitionProgress;
        }
    }

    public partial class AudioEvent : TrackEvent, IEffectOwner
    {
        internal AudioEvent(ulong h) : base(h) { }
        public AudioEvent() : base(null, null, null) { }
        public AudioEvent(Timecode start) : base(start, null, null) { }
        public AudioEvent(Timecode start, Timecode length) : base(start, length, null) { }
        public AudioEvent(Timecode start, Timecode length, string name) : base(start, length, name) { }
        public AudioEvent(Project project, Timecode start, Timecode length, string name) : base(start, length, name) { }

        public override MediaType MediaType { get { return MediaType.Audio; } }
        public override bool IsAudio() { return true; }
        public override bool IsVideo() { return false; }
        public Effects Effects { get { return new Effects(this); } }
        public bool Normalize { get { return GB(Abi.RP_NORMALIZE); } set { SB(Abi.RP_NORMALIZE, value); } }
        public double NormalizeGain { get { return H == 0 ? 1.0 : GD(Abi.RP_NORMALIZE_GAIN); } set { SD(Abi.RP_NORMALIZE_GAIN, value); } }
        public bool InvertPhase { get { return GB(Abi.RP_INVERT_PHASE); } set { SB(Abi.RP_INVERT_PHASE, value); } }
        public ChannelRemapping Channels { get { return H == 0 ? ChannelRemapping.None : (ChannelRemapping)GI(Abi.RP_CHANNELS); } set { SI(Abi.RP_CHANNELS, (long)value); } }
        public TimeStretchPitchShift Method { get { return (TimeStretchPitchShift)GI(Abi.RP_STRETCH_METHOD); } set { SI(Abi.RP_STRETCH_METHOD, (long)value); } }
        public bool PitchLock { get { return GB(Abi.RP_PITCH_LOCK); } set { SB(Abi.RP_PITCH_LOCK, value); } }
        public double PitchSemis { get { return GD(Abi.RP_PITCH_SEMIS); } set { SD(Abi.RP_PITCH_SEMIS, value); } }
        public ClassicStretchAttributes ClassicAttribute { get { return (ClassicStretchAttributes)GI(Abi.RP_CLASSIC_ATTR); } set { SI(Abi.RP_CLASSIC_ATTR, (long)value); } }
        public ElastiqueStretchAttributes ElastiqueAttribute { get { return (ElastiqueStretchAttributes)GI(Abi.RP_ELASTIQUE_ATTR); } set { SI(Abi.RP_ELASTIQUE_ATTR, (long)value); } }
        public bool FormantLock { get { return GB(Abi.RP_FORMANT_LOCK); } set { SB(Abi.RP_FORMANT_LOCK, value); } }
        public double FormantShift { get { return GD(Abi.RP_FORMANT_SHIFT); } set { SD(Abi.RP_FORMANT_SHIFT, value); } }
        public void SetNormalize(bool normalize, double gain) { Normalize = normalize; if (normalize) NormalizeGain = gain; }
        public double RecalculateNorm() { Normalize = false; Normalize = true; return NormalizeGain; }
    }

    public partial class TrackEvents : BaseList<TrackEvent>
    {
        Track track;
        internal TrackEvents(Track t) { track = t; }
        internal override List<TrackEvent> Items() { return Reno.WrapAll<TrackEvent>(track.L(Abi.RL_EVENTS, "Events")); }
        internal override void AddItem(TrackEvent e)
        {
            track.NeedAdded("Events.Add");
            if (e.H != 0) throw new InvalidOperationException("TrackEvents.Add: the event is already on a track (use TrackEvent.Copy or TrackEvent.Track)");
            if (e.IsAudio() != track.IsAudio()) throw new InvalidOperationException("TrackEvents.Add: " + (e.IsAudio() ? "an audio event cannot go on a video track" : "a video event cannot go on an audio track"));
            double start = e.Start.ToSeconds(), length = e.Length.ToSeconds();
            ulong h = Reno.Call(track.H, Abi.RO_ADD_EVENT, null, new double[] { start, length }, "", "TrackEvents.Add");
            e.Attached(h);
        }
        internal override bool RemoveItem(TrackEvent e)
        {
            if (e.H == 0) return false;
            Reno.Call(track.H, Abi.RO_REMOVE_EVENT, new long[] { (long)e.H }, null, null, "TrackEvents.Remove");
            return true;
        }
    }

    // An event's fade-in or fade-out. VEGAS keeps the event's level as FadeIn.Gain.
    public partial class Fade : IEffectOwner, IEnvelopeOwner
    {
        TrackEvent ev;
        bool fadeIn;
        internal Fade(TrackEvent e, bool fadeIn) { ev = e; this.fadeIn = fadeIn; }
        public Project Project { get { return Reno.App.Project; } }
        public Timecode Length
        {
            get { return ev.GT(fadeIn ? Abi.RP_FADE_IN : Abi.RP_FADE_OUT, "Length"); }
            set { ev.ST(fadeIn ? Abi.RP_FADE_IN : Abi.RP_FADE_OUT, value, "Length"); }
        }
        public CurveType Curve
        {
            get { long c = ev.GI(fadeIn ? Abi.RP_FADE_IN_CURVE : Abi.RP_FADE_OUT_CURVE, "Curve"); return ev.H == 0 && c == 0 ? CurveType.Linear : (CurveType)c; }
            set { ev.SI(fadeIn ? Abi.RP_FADE_IN_CURVE : Abi.RP_FADE_OUT_CURVE, (long)value, "Curve"); }
        }
        // The curve of a cross-fade's other half: on a fade-in, how the earlier event fades out under this one.
        // On a fade-out this is the mirror of Curve, as for a new event in VEGAS.
        public CurveType ReciprocalCurve
        {
            get
            {
                if (!fadeIn) { CurveType c = Curve; return c == CurveType.Fast ? CurveType.Slow : c == CurveType.Slow ? CurveType.Fast : c; }
                long v = ev.GI(Abi.RP_FADE_IN_RECIP_CURVE, "ReciprocalCurve");
                return ev.H == 0 && v == 0 ? CurveType.Slow : (CurveType)v;
            }
            set { if (fadeIn) ev.SI(Abi.RP_FADE_IN_RECIP_CURVE, (long)value, "ReciprocalCurve"); }
        }
        public float Gain
        {
            get { return ev.H == 0 && (ev.pending == null || !ev.pending.ContainsKey(Abi.RP_LEVEL)) ? 1f : (float)ev.GD(Abi.RP_LEVEL, "Gain"); }
            set { ev.SD(Abi.RP_LEVEL, value, "Gain"); }
        }
        public object Transition
        {
            get { return fadeIn && ev.H != 0 && ev.IsVideo() ? Reno.W<Effect>((ulong)ev.GI(Abi.RP_TRANSITION, "Transition")) : null; }
            set
            {
                if (!fadeIn) throw new NotSupportedException("Fade.Transition on a fade-out is not supported by Reno (the transition belongs to the next event's fade-in).");
                Effect fx = value as Effect;
                PlugInNode p = value as PlugInNode;
                if (fx != null) p = fx.PlugIn;
                ulong h = ev.Op(Abi.RO_SET_TRANSITION, new long[] { p == null ? 0 : (long)p.H }, null, null, "Transition");
                if (fx != null && h != 0) fx.Attached(h);
            }
        }
        public bool RemoveTransition() { Transition = null; return true; }
        public Envelopes Envelopes { get { throw new NotSupportedException("Fade.Envelopes is not supported by Reno."); } }
        public virtual bool CanAddEnvelope(EnvelopeType type) { return false; }
    }

    // ------------------------------------------------------------------------------------------------ takes
    public partial class Take : RenoObject
    {
        internal MediaStream newStream;     // a script's new take
        internal bool newActive = true;
        internal Take(ulong h) : base(h) { }
        public Take(MediaStream mediaStream) : this(mediaStream, true, null) { }
        public Take(MediaStream mediaStream, bool makeActive) : this(mediaStream, makeActive, null) { }
        public Take(MediaStream mediaStream, bool makeActive, string name)
        {
            if (mediaStream == null) throw new ArgumentNullException("mediaStream");
            newStream = mediaStream;
            newActive = makeActive;
            if (name != null) SS(Abi.RP_NAME, name, "Name");
        }
        public static Take CreateInstance(Project project, MediaStream mediaStream) { return new Take(mediaStream); }
        public static Take CreateInstance(Project project, MediaStream mediaStream, bool makeActive) { return new Take(mediaStream, makeActive); }
        public static Take CreateInstance(Project project, MediaStream mediaStream, bool makeActive, string name) { return new Take(mediaStream, makeActive, name); }

        public int Index { get { return H == 0 ? -1 : (int)GI(Abi.RP_INDEX); } }
        public Media Media { get { return H == 0 ? (newStream == null ? null : newStream.Parent) : Reno.W<Media>((ulong)GI(Abi.RP_MEDIA)); } }
        public MediaStream MediaStream { get { return H == 0 ? newStream : Reno.W<MediaStream>((ulong)GI(Abi.RP_STREAM)); } }
        public string MediaPath { get { Media m = Media; return m == null ? null : m.FilePath; } }
        public string Name { get { return GS(Abi.RP_NAME); } set { SS(Abi.RP_NAME, value); } }
        public int StreamIndex { get { return 0; } }
        public Timecode Offset { get { return GT(Abi.RP_OFFSET); } set { ST(Abi.RP_OFFSET, value); } }
        public Timecode Length { get { return GT(Abi.RP_LENGTH); } }
        public Timecode AvailableLength { get { return GT(Abi.RP_AVAILABLE_LENGTH); } }
        public bool IsActive { get { return H != 0 && GB(Abi.RP_IS_ACTIVE); } }
        public int FirstAudioChannelIndex { get { return 0; } }
        public int AudioChannelCount { get { AudioStream a = MediaStream as AudioStream; return a == null ? 0 : a.Channels; } }
        public void SetAudioChannels(int firstChannelIndex, int channelCount) { throw NotSupported("SetAudioChannels"); }
    }

    public partial class Takes : BaseList<Take>
    {
        TrackEvent ev;
        internal Takes(TrackEvent e) { ev = e; }
        internal override List<Take> Items() { return Reno.WrapAll<Take>(ev.L(Abi.RL_TAKES, "Takes")); }
        internal override void AddItem(Take t)
        {
            if (t.H != 0) throw new InvalidOperationException("Takes.Add: the take is already an event's take");
            if (t.newStream == null) throw new ArgumentException("Takes.Add: the take has no media stream");
            string name = t.pending != null && t.pending.ContainsKey(Abi.RP_NAME) ? (string)t.pending[Abi.RP_NAME] : "";
            ulong h = ev.Op(Abi.RO_ADD_TAKE, new long[] { (long)t.newStream.H, t.newActive ? 1 : 0 }, null, name, "Takes.Add");
            t.pending = null;
            t.Attached(h);
        }
        internal override bool RemoveItem(Take t)
        {
            if (t.H == 0) return false;
            ev.Op(Abi.RO_REMOVE_TAKE, new long[] { (long)t.H }, null, null, "Takes.Remove");
            return true;
        }
        public override void Clear()
        {
            // (removing the active take last leaves an empty event, as in VEGAS)
            ev.Op(Abi.RO_CLEAR_TAKES, null, null, null, "Takes.Clear");
            Take a = ev.ActiveTake;
            if (a != null) RemoveItem(a);
        }
    }

    // ------------------------------------------------------------------------------------------------ media
    public partial interface IMediaBinNode { }

    public partial class Media : RenoObject, IMediaBinNode, IEffectOwner
    {
        internal Media(ulong h) : base(h) { }
        protected Media() { }
        // VEGAS adds the file to the project's media at once
        public Media(string path) : base(0) { Attached(Reno.Call(1, Abi.RO_IMPORT_MEDIA, null, null, path, "new Media")); }
        public Media(PlugInNode generator) : this(generator, null) { }
        public Media(PlugInNode generator, string presetName)
        {
            if (generator == null) throw new ArgumentNullException("generator");
            Attached(Reno.Call(1, Abi.RO_GENERATED_MEDIA, new long[] { (long)generator.H }, null, presetName ?? "", "new Media"));
        }
        public static Media CreateInstance(Project project, string path) { return new Media(path); }
        public static Media CreateInstance(Project project, PlugInNode generator) { return new Media(generator); }
        public static Media CreateInstance(Project project, PlugInNode generator, string presetName) { return new Media(generator, presetName); }

        public MediaBinNodeType NodeType { get { return MediaBinNodeType.MediaRef; } }
        public uint NodeID { get { return MediaID; } }
        public uint MediaID { get { return (uint)GI(Abi.RP_MEDIA_ID); } }
        public MediaStreams Streams { get { return new MediaStreams(this); } }
        public Effects Effects { get { throw NotSupported("Effects"); } }
        public Project Project { get { return Reno.App.Project; } }
        public string FilePath { get { return GS(Abi.RP_PATH); } }
        public Effect Generator { get { return Reno.W<Effect>((ulong)GI(Abi.RP_GENERATOR)); } }
        public string KeyString { get { return FilePath; } }
        public bool UseCustomTimecode { get { return false; } set { throw NotSupported("UseCustomTimecode"); } }
        public Timecode TimecodeIn { get { return new Timecode(); } set { throw NotSupported("TimecodeIn"); } }
        public Timecode TimecodeOut { get { return Length; } set { throw NotSupported("TimecodeOut"); } }
        public Timecode Length { get { return GT(Abi.RP_LENGTH); } set { throw NotSupported("Length"); } }
        public RulerFormat RulerFormat { get { return RulerFormat.Unknown; } set { } }
        public string Comment { get { return GS(Abi.RP_COMMENT); } set { SS(Abi.RP_COMMENT, value); } }
        public string Title { get { return GS(Abi.RP_NAME); } }
        public string TapeName { get { return ""; } set { throw NotSupported("TapeName"); } }
        public int UseCount { get { return (int)GI(Abi.RP_USE_COUNT); } }
        // (Reno does not read the markers inside media files: the lists are empty)
        public MediaMarkerList Markers { get { return new MediaMarkerList(); } }
        public MediaRegionList Regions { get { return new MediaRegionList(); } }
        public MediaCommandMarkerList CommandMarkers { get { return new MediaCommandMarkerList(); } }
        public CustomDataContainer CustomData { get { return CustomDataContainer.Of(H); } }
        public bool IsOffline() { return GB(Abi.RP_IS_OFFLINE); }
        public bool CanRecapture() { return false; }
        public bool IsGenerated() { return GB(Abi.RP_IS_GENERATED); }
        public bool IsImageSequence() { return false; }
        public bool IsSubclip() { return GB(Abi.RP_IS_SUBCLIP); }
        public bool HasAudio() { return GB(Abi.RP_HAS_AUDIO); }
        public bool HasVideo() { return GB(Abi.RP_HAS_VIDEO); }
        public int StreamCount(MediaType type) { int n = 0; foreach (MediaStream s in Streams) if (s.MediaType == type) ++n; return n; }
        public VideoStream GetVideoStreamByIndex(int index) { return Streams.GetItemByMediaType(MediaType.Video, index) as VideoStream; }
        public AudioStream GetAudioStreamByIndex(int index) { return Streams.GetItemByMediaType(MediaType.Audio, index) as AudioStream; }
        public MediaStream CreateOfflineStream(MediaType type) { throw NotSupported("CreateOfflineStream"); }
        public void ReplaceWith(Media other) { throw NotSupported("ReplaceWith"); }
    }

    // A part of a file as media of its own (VEGAS's subclips); a reversed one plays backwards.
    public partial class Subclip : Media
    {
        internal Subclip(ulong h) : base(h) { }
        public Subclip(string path, Timecode start, Timecode length, bool reverse, string displayName)
        {
            Attached(Reno.Call(1, Abi.RO_ADD_SUBCLIP, new long[] { reverse ? 1 : 0 }, new double[] { start.ToSeconds(), length.ToSeconds() },
                               (path ?? "") + "\n" + (displayName ?? ""), "new Subclip"));
        }
        public Subclip(Project project, string path, Timecode start, Timecode length, bool reverse, string displayName)
            : this(path, start, length, reverse, displayName) { }
        public Media ParentMedia { get { return Reno.W<Media>(Reno.Call(1, Abi.RO_FIND_MEDIA, null, null, FilePath, "Subclip.ParentMedia")); } }
        public Timecode Start { get { return GT(Abi.RP_SUBCLIP_START); } }
        public bool IsReversed { get { return GB(Abi.RP_IS_REVERSED); } }
    }

    // The project's media: a dictionary from file names to Media.
    public partial class MediaPool : IDictionary
    {
        internal MediaPool() { }
        List<Media> All() { return Reno.WrapAll<Media>(Reno.List(1, Abi.RL_MEDIA, "MediaPool")); }
        public int Count { get { return All().Count; } }
        public bool IsSynchronized { get { return false; } }
        public object SyncRoot { get { return this; } }
        public bool IsFixedSize { get { return false; } }
        public bool IsReadOnly { get { return false; } }
        public object this[object key] { get { return Find(key as string); } set { throw new NotSupportedException("MediaPool[key] = ... is not supported by Reno."); } }
        public ICollection Keys { get { List<string> k = new List<string>(); foreach (Media m in All()) k.Add(m.KeyString); return k; } }
        public ICollection Values { get { return All(); } }
        public MediaBin RootMediaBin { get { throw new NotSupportedException("MediaPool.RootMediaBin is not supported by Reno."); } }
        public void CopyTo(Array array, int index) { ((ICollection)All()).CopyTo(array, index); }
        public void Add(object key, object value) { throw new NotSupportedException("MediaPool.Add is not supported by Reno: use AddMedia."); }
        public Media AddMedia(string path) { return Reno.W<Media>(Reno.Call(1, Abi.RO_IMPORT_MEDIA, null, null, path, "MediaPool.AddMedia")); }
        public Media AddMedia(string path, bool addToSelectedBin) { return AddMedia(path); }
        public Media AddImageSequence(string path, int imageCount, double fps) { throw new NotSupportedException("MediaPool.AddImageSequence is not supported by Reno."); }
        public void Clear() { RemoveUnusedMedia(); }
        public bool Contains(object key) { return Find(key as string) != null; }
        public void Remove(object key)
        {
            Media m = key as Media ?? Find(key as string);
            if (m != null) Reno.Call(1, Abi.RO_REMOVE_MEDIA, new long[] { (long)m.H }, null, null, "MediaPool.Remove");
        }
        public void RemoveUnusedMedia() { Reno.Call(1, Abi.RO_REMOVE_UNUSED_MEDIA, null, null, null, "MediaPool.RemoveUnusedMedia"); }
        public Media Find(string path) { return path == null ? null : Reno.W<Media>(Reno.Call(1, Abi.RO_FIND_MEDIA, null, null, path, "MediaPool.Find")); }
        public Media[] GetSelectedMedia() { return Reno.WrapAll<Media>(Reno.List(1, Abi.RL_SELECTED_MEDIA, "MediaPool.GetSelectedMedia")).ToArray(); }
        public void OpenAllMedia() { }
        public IDictionaryEnumerator GetEnumerator() { return new MediaPoolEnumerator(All()); }
        IEnumerator IEnumerable.GetEnumerator() { return GetEnumerator(); }
    }

    public partial class MediaPoolEnumerator : IDictionaryEnumerator
    {
        List<Media> items;
        int at = -1;
        internal MediaPoolEnumerator(List<Media> items) { this.items = items; }
        public object Current { get { return items[at]; } }
        public DictionaryEntry Entry { get { return new DictionaryEntry(items[at].KeyString, items[at]); } }
        public object Key { get { return items[at].KeyString; } }
        public object Value { get { return items[at]; } }
        public bool MoveNext() { return ++at < items.Count; }
        public void Reset() { at = -1; }
    }

    public abstract partial class MediaStream : RenoObject
    {
        internal MediaStream(ulong h) : base(h) { }
        public Media Parent { get { return Reno.W<Media>((ulong)GI(Abi.RP_MEDIA)); } }
        public virtual MediaType MediaType { get { return (MediaType)GI(Abi.RP_MEDIA_TYPE); } }
        public int Index { get { return (int)GI(Abi.RP_INDEX); } }
        public Timecode Offset { get { return new Timecode(); } }
        public Timecode Length { get { return GT(Abi.RP_LENGTH); } }
        public string FormatFourCCString { get { return GS(Abi.RP_STREAM_FORMAT); } }
        public string StreamInfoText { get { return GS(Abi.RP_STREAM_FORMAT); } }
        public bool IsOffline { get { return GB(Abi.RP_IS_OFFLINE); } }
    }

    public partial class VideoStream : MediaStream
    {
        internal VideoStream(ulong h) : base(h) { }
        public override MediaType MediaType { get { return MediaType.Video; } }
        public int Width { get { return (int)GI(Abi.RP_STREAM_WIDTH); } }
        public int Height { get { return (int)GI(Abi.RP_STREAM_HEIGHT); } }
        public System.Drawing.Size Size { get { return new System.Drawing.Size(Width, Height); } set { throw NotSupported("Size"); } }
        public double FrameRate { get { return GD(Abi.RP_STREAM_FPS); } }
        public VideoFieldOrder FieldOrder { get { return (VideoFieldOrder)GI(Abi.RP_STREAM_FIELD_ORDER); } set { throw NotSupported("FieldOrder"); } }
        public double PixelAspectRatio { get { return GD(Abi.RP_STREAM_PIXEL_ASPECT); } set { throw NotSupported("PixelAspectRatio"); } }
        public int ColorDepth { get { return (int)GI(Abi.RP_STREAM_BITS); } }
        // (Reno takes a picture's alpha channel from the file itself; what a script sets here is kept for it to read)
        VideoAlphaType alpha = VideoAlphaType.Undefined;
        public VideoAlphaType AlphaChannel { get { return alpha; } set { alpha = value; } }
        public long AverageDataRate { get { return 0; } }
        public string Format { get { return GS(Abi.RP_STREAM_FORMAT); } }
        public VideoColor BackgroundColor { get { return new VideoColor(0, 0, 0, 0); } set { throw NotSupported("BackgroundColor"); } }
        public VideoOutputRotation Rotation { get { return VideoOutputRotation.None; } set { throw NotSupported("Rotation"); } }
    }

    public partial class AudioStream : MediaStream
    {
        internal AudioStream(ulong h) : base(h) { }
        public override MediaType MediaType { get { return MediaType.Audio; } }
        public uint SampleRate { get { return (uint)GI(Abi.RP_STREAM_SAMPLE_RATE); } }
        public uint BitDepth { get { return (uint)GI(Abi.RP_STREAM_BITS); } }
        public int Channels { get { return (int)GI(Abi.RP_STREAM_CHANNELS); } }
        public string Format { get { return GS(Abi.RP_STREAM_FORMAT); } }
        public long AverageDataRate { get { return 0; } }
    }

    public partial class MediaStreams : BaseList<MediaStream>
    {
        Media media;
        internal MediaStreams(Media m) { media = m; }
        public override bool IsFixedSize { get { return true; } }
        public override bool IsReadOnly { get { return true; } }
        internal override List<MediaStream> Items() { return Reno.WrapAll<MediaStream>(media.L(Abi.RL_STREAMS, "Streams")); }
        public MediaStream GetItemByMediaType(MediaType type, int index)
        {
            int n = 0;
            foreach (MediaStream s in Items())
                if (s.MediaType == type && n++ == index) return s;
            return null;
        }
    }

    // ------------------------------------------------------------------------------------------------ effects
    public partial class Effect : RenoObject
    {
        internal PlugInNode newPlugIn;      // a script's new effect
        internal Effect(ulong h) : base(h) { }
        public Effect(PlugInNode plugIn)
        {
            if (plugIn == null) throw new ArgumentNullException("plugIn");
            newPlugIn = plugIn;
        }
        public int Index { get { return H == 0 ? -1 : (int)GI(Abi.RP_INDEX); } }
        public PlugInNode PlugIn { get { return H == 0 ? newPlugIn : Reno.W<PlugInNode>((ulong)GI(Abi.RP_PLUGIN)); } }
        public string Description { get { return H == 0 ? newPlugIn.Name : GS(Abi.RP_NAME); } }
        public bool Bypass { get { return GB(Abi.RP_BYPASS); } set { SB(Abi.RP_BYPASS, value); } }
        public bool ApplyBeforePanCrop { get { return GB(Abi.RP_BEFORE_PANCROP); } set { SB(Abi.RP_BEFORE_PANCROP, value); } }
        public bool IsOFX { get { return H == 0 ? newPlugIn.IsOFX : GB(Abi.RP_IS_OFX); } }
        public bool ActiveInteract { get { return false; } }
        public bool ForceShowingOverlayInteract { get { return false; } set { } }
        public Keyframes Keyframes { get { throw NotSupported("Keyframes"); } }
        public EffectPresets Presets { get { return new EffectPresets(); } }
        public string Preset { set { throw new NotSupportedException("Effect.Preset is not supported by Reno (the effect keeps its default settings): \"" + value + "\""); } }
        public EffectPreset CurrentPreset { get { return null; } set { throw NotSupported("CurrentPreset"); } }
        public OFXEffect OFXEffect { get { if (!IsOFX) throw new InvalidOperationException("Effect.OFXEffect: " + Description + " is not an OpenFX effect"); NeedAdded("OFXEffect"); return new OFXEffect(this); } }
    }

    // ------------------------------------------------------------------------------------------------ OpenFX parameters
    // VEGAS's OFXEffect: an OpenFX effect's parameters. Keyframe times are from the start of what the effect is on.
    public abstract partial class OFXBase
    {
        internal OFXBase() { }
        public abstract OFXEffectType EffectType { get; }
        public abstract string Label { get; }
        public abstract string Grouping { get; }
        public abstract string PlugInPath { get; }
        public Version Version { get { return new Version(1, 0); } }
        public bool Equals(OFXBase that) { return Equals((object)that); }
    }

    // An installed OpenFX plug-in (PlugInNode.OFXPlugIn).
    public partial class OFXPlugIn : OFXBase
    {
        PlugInNode node;
        internal OFXPlugIn(PlugInNode n) { node = n; }
        public override OFXEffectType EffectType
        {
            get
            {
                long t = Reno.GI(node.H, Abi.RP_PLUGIN_TYPE, "OFXPlugIn.EffectType");
                return t == (long)PlugInType.VideoTransition ? OFXEffectType.Transition : t == (long)PlugInType.VideoGenerator ? OFXEffectType.Generator : OFXEffectType.Filter;
            }
        }
        public override string Label { get { return node.Name; } }
        public override string Grouping { get { return node.Group; } }
        public override string PlugInPath { get { return ""; } }
        public bool Equals(OFXPlugIn that) { return !ReferenceEquals(that, null) && that.node == node; }
        public override bool Equals(object that) { return Equals(that as OFXPlugIn); }
        public override int GetHashCode() { return node.GetHashCode(); }
        public static bool operator ==(OFXPlugIn a, OFXPlugIn b) { return ReferenceEquals(a, b) || (!ReferenceEquals(a, null) && a.Equals(b)); }
        public static bool operator !=(OFXPlugIn a, OFXPlugIn b) { return !(a == b); }
    }

    // An OpenFX effect in the project (Effect.OFXEffect).
    public partial class OFXEffect : OFXBase
    {
        internal Effect effect;
        internal OFXEffect(Effect e) { effect = e; }
        public override OFXEffectType EffectType
        {
            get
            {
                PlugInNode n = effect.PlugIn;
                if (n == null) return OFXEffectType.Filter;
                return new OFXPlugIn(n).EffectType;
            }
        }
        public override string Label { get { return Reno.GS(effect.H, Abi.RP_OFX_PLUGIN_LABEL, "OFXEffect.Label"); } }
        public override string Grouping { get { return Reno.GS(effect.H, Abi.RP_OFX_PLUGIN_GROUP, "OFXEffect.Grouping"); } }
        public override string PlugInPath { get { return Reno.GS(effect.H, Abi.RP_OFX_PLUGIN_PATH, "OFXEffect.PlugInPath"); } }
        public OFXParameters Parameters { get { return new OFXParameters(this); } }
        public OFXParameter this[string name] { get { return FindParameterByName(name); } }
        public OFXParameter FindParameterByName(string name)
        {
            foreach (OFXParameter p in Parameters) if (p.Name == name) return p;
            return null;
        }
        // where a Value set on an animated parameter goes (VEGAS: the cursor's place in the effect)
        public Timecode CurrentTime
        {
            get { return Timecode.FromSeconds(Reno.GD(effect.H, Abi.RP_OFX_CURRENT_TIME, "OFXEffect.CurrentTime")); }
            set { Reno.SD(effect.H, Abi.RP_OFX_CURRENT_TIME, value == null ? 0 : value.ToSeconds(), "OFXEffect.CurrentTime"); }
        }
        public void AllParametersChanged() { }
        public bool Equals(OFXEffect that) { return !ReferenceEquals(that, null) && that.effect == effect; }
        public override bool Equals(object that) { return Equals(that as OFXEffect); }
        public override int GetHashCode() { return effect.GetHashCode(); }
        public static bool operator ==(OFXEffect a, OFXEffect b) { return ReferenceEquals(a, b) || (!ReferenceEquals(a, null) && a.Equals(b)); }
        public static bool operator !=(OFXEffect a, OFXEffect b) { return !(a == b); }
    }

    public partial class OFXParameters : BaseList<OFXParameter>
    {
        OFXEffect fx;
        internal OFXParameters(OFXEffect fx) { this.fx = fx; }
        public override bool IsReadOnly { get { return true; } }
        internal override List<OFXParameter> Items() { return Reno.WrapAll<OFXParameter>(Reno.List(fx.effect.H, Abi.RL_OFX_PARAMS, "OFXEffect.Parameters")); }
    }

    // A parameter: pages, groups and buttons are plain OFXParameters; the others are OFXDoubleParameter, OFXChoiceParameter...
    public partial class OFXParameter : RenoObject
    {
        public const bool DISABLE_MEDIA_CLOSE = true;
        internal OFXParameter(ulong h) : base(h) { }
        internal static OFXParameter Make(ulong h)
        {
            switch ((OFXParameterType)Reno.GI(h, Abi.RP_OFX_TYPE, "OFXParameter.ParameterType"))
            {
            case OFXParameterType.Boolean: return new OFXBooleanParameter(h);
            case OFXParameterType.Choice: return new OFXChoiceParameter(h);
            case OFXParameterType.RGB: return new OFXRGBParameter(h);
            case OFXParameterType.RGBA: return new OFXRGBAParameter(h);
            case OFXParameterType.Integer: return new OFXIntegerParameter(h);
            case OFXParameterType.Integer2D: return new OFXInteger2DParameter(h);
            case OFXParameterType.Integer3D: return new OFXInteger3DParameter(h);
            case OFXParameterType.Double: return new OFXDoubleParameter(h);
            case OFXParameterType.Double2D: return new OFXDouble2DParameter(h);
            case OFXParameterType.Double3D: return new OFXDouble3DParameter(h);
            case OFXParameterType.String: return new OFXStringParameter(h);
            case OFXParameterType.Custom: return new OFXCustomParameter(h);
            default: return new OFXParameter(h);
            }
        }
        internal static OFXKeyframe MakeKey(ulong h)
        {
            switch ((OFXParameterType)Reno.GI(h, Abi.RP_OFX_TYPE, "OFXKeyframe"))
            {
            case OFXParameterType.Boolean: return new OFXBooleanKeyframe(h);
            case OFXParameterType.Choice: return new OFXChoiceKeyframe(h);
            case OFXParameterType.RGB: return new OFXRGBKeyframe(h);
            case OFXParameterType.RGBA: return new OFXRGBAKeyframe(h);
            case OFXParameterType.Integer: return new OFXIntegerKeyframe(h);
            case OFXParameterType.Integer2D: return new OFXInteger2DKeyframe(h);
            case OFXParameterType.Integer3D: return new OFXInteger3DKeyframe(h);
            case OFXParameterType.Double: return new OFXDoubleKeyframe(h);
            case OFXParameterType.Double2D: return new OFXDouble2DKeyframe(h);
            case OFXParameterType.Double3D: return new OFXDouble3DKeyframe(h);
            case OFXParameterType.String: return new OFXStringKeyframe(h);
            default: return new OFXCustomKeyframe(h);
            }
        }
        public int Index { get { return (int)GI(Abi.RP_INDEX); } }
        public OFXParameterType ParameterType { get { return (OFXParameterType)GI(Abi.RP_OFX_TYPE); } }
        public string Name { get { return GS(Abi.RP_NAME); } }
        public string Definition { get { throw NotSupported("Definition"); } }
        public string Label { get { return GS(Abi.RP_OFX_LABEL); } }
        public string Hint { get { return GS(Abi.RP_OFX_HINT); } }
        public string ParentName { get { return GS(Abi.RP_OFX_PARENT); } }
        public bool Enabled { get { return GB(Abi.RP_OFX_ENABLED); } }
        public bool Secret { get { return GB(Abi.RP_OFX_SECRET); } }
        public bool Subservient { get { return false; } }
        public bool CanAnimate { get { return GB(Abi.RP_OFX_CAN_ANIMATE); } }
        public bool IsAnimated { get { return GB(Abi.RP_OFX_ANIMATED); } set { SB(Abi.RP_OFX_ANIMATED, value); } }
        public bool Persisted { get { return GB(Abi.RP_OFX_PERSIST); } }
        public void ParameterChanged() { }
        public void ParameterChanged(bool fDisableClose) { }
        public bool Equals(OFXParameter that) { return Equals((object)that); }

        // the numbers of the value (Reno's side): which = 0 the value, 1 default, 2 min, 3 max, 4 display min, 5 display max
        internal double[] Numbers(int which, int count, string member)
        {
            double[] x = new double[count];
            for (int k = 0; k < count; ++k)
            {
                double d;
                Reno.Call(H, Abi.RO_OFX_GET, new long[] { k, which }, null, null, member, out d);
                x[k] = d;
            }
            return x;
        }
        internal double[] NumbersAt(Timecode time, int count, string member)
        {
            double[] x = new double[count];
            for (int k = 0; k < count; ++k)
            {
                double d;
                Reno.Call(H, Abi.RO_OFX_GET, new long[] { k, 6 }, new double[] { time == null ? 0 : time.ToSeconds() }, null, member, out d);
                x[k] = d;
            }
            return x;
        }
        internal void SetNumbers(double[] x, string member) { Reno.Call(H, Abi.RO_OFX_SET, null, x, null, member); }
        internal ulong SetNumbersAt(Timecode time, double[] x, string member)
        {
            double[] a = new double[x.Length + 1];
            a[0] = time == null ? 0 : time.ToSeconds();
            Array.Copy(x, 0, a, 1, x.Length);
            return Reno.Call(H, Abi.RO_OFX_SET_AT, null, a, null, member);
        }
    }

    // A parameter whose value is a T (VEGAS's OFXParameter<T, K>): the conversions to and from Reno's numbers are the subclass's.
    public abstract partial class OFXParameter<T, K> : OFXParameter where K : OFXKeyframe<T>
    {
        internal OFXParameter(ulong h) : base(h) { }
        internal virtual int Count { get { return 1; } }
        internal virtual T From(double[] x) { throw new NotSupportedException(); }
        internal virtual double[] To(T v) { throw new NotSupportedException(); }
        internal virtual bool IsText { get { return false; } }
        internal virtual T FromText(string s) { return default(T); }
        internal virtual string ToText(T v) { return ""; }

        public T Default { get { if (IsText) throw NotSupported("Default"); return From(Numbers(1, Count, Member("Default"))); } }
        public T Value
        {
            get { return IsText ? FromText(GS(Abi.RP_OFX_TEXT)) : From(Numbers(0, Count, Member("Value"))); }
            set { if (IsText) SS(Abi.RP_OFX_TEXT, ToText(value)); else SetNumbers(To(value), Member("Value")); }
        }
        public OFXKeyframes<T, K> Keyframes { get { return new OFXKeyframes<T, K>(this); } }
        public virtual T GetValueAtTime(Timecode time)
        {
            if (IsText) throw NotSupported("GetValueAtTime");
            return From(NumbersAt(time, Count, Member("GetValueAtTime")));
        }
        public virtual void SetValueAtTime(Timecode time, T value)
        {
            if (IsText) Reno.Call(H, Abi.RO_OFX_SET_AT, null, new double[] { time == null ? 0 : time.ToSeconds() }, ToText(value), Member("SetValueAtTime"));
            else SetNumbersAt(time, To(value), Member("SetValueAtTime"));
        }
    }

    public abstract partial class OFXRangeParameter<T, K> : OFXParameter<T, K> where K : OFXKeyframe<T>
    {
        internal OFXRangeParameter(ulong h) : base(h) { }
        public T Min { get { return From(Numbers(2, Count, Member("Min"))); } }
        public T Max { get { return From(Numbers(3, Count, Member("Max"))); } }
        public T DisplayMin { get { return From(Numbers(4, Count, Member("DisplayMin"))); } }
        public T DisplayMax { get { return From(Numbers(5, Count, Member("DisplayMax"))); } }
    }

    public partial class OFXBooleanParameter : OFXParameter<bool, OFXBooleanKeyframe>
    {
        internal OFXBooleanParameter(ulong h) : base(h) { }
        internal override bool From(double[] x) { return x[0] != 0; }
        internal override double[] To(bool v) { return new double[] { v ? 1 : 0 }; }
    }
    public partial class OFXChoiceParameter : OFXParameter<OFXChoice, OFXChoiceKeyframe>
    {
        internal OFXChoiceParameter(ulong h) : base(h) { }
        public OFXChoice[] Choices
        {
            get
            {
                string all = GS(Abi.RP_OFX_CHOICES);
                string[] names = all.Length == 0 ? new string[0] : all.Split('\n');
                OFXChoice[] r = new OFXChoice[names.Length];
                for (int k = 0; k < names.Length; ++k) r[k] = new OFXChoice(k, names[k]);
                return r;
            }
        }
        internal override OFXChoice From(double[] x)
        {
            int i = (int)Math.Round(x[0]);
            OFXChoice[] c = Choices;
            return new OFXChoice(i, i >= 0 && i < c.Length ? c[i].Name : "");
        }
        internal override double[] To(OFXChoice v)
        {
            if (v == null) throw new ArgumentNullException("value");
            return new double[] { v.Index };
        }
    }
    public partial class OFXRGBParameter : OFXParameter<OFXColor, OFXRGBKeyframe>
    {
        internal OFXRGBParameter(ulong h) : base(h) { }
        internal override int Count { get { return 3; } }
        internal override OFXColor From(double[] x) { return new OFXColor(x[0], x[1], x[2]); }
        internal override double[] To(OFXColor v) { return new double[] { v.R, v.G, v.B }; }
    }
    public partial class OFXRGBAParameter : OFXParameter<OFXColor, OFXRGBAKeyframe>
    {
        internal OFXRGBAParameter(ulong h) : base(h) { }
        internal override int Count { get { return 4; } }
        internal override OFXColor From(double[] x) { return new OFXColor(x[0], x[1], x[2], x[3]); }
        internal override double[] To(OFXColor v) { return new double[] { v.R, v.G, v.B, v.A }; }
    }
    public partial class OFXIntegerParameter : OFXRangeParameter<int, OFXIntegerKeyframe>
    {
        internal OFXIntegerParameter(ulong h) : base(h) { }
        internal override int From(double[] x) { return (int)Math.Round(x[0]); }
        internal override double[] To(int v) { return new double[] { v }; }
    }
    public partial class OFXInteger2DParameter : OFXRangeParameter<OFXInteger2D, OFXInteger2DKeyframe>
    {
        internal OFXInteger2DParameter(ulong h) : base(h) { }
        internal override int Count { get { return 2; } }
        internal override OFXInteger2D From(double[] x) { OFXInteger2D v; v.X = (int)Math.Round(x[0]); v.Y = (int)Math.Round(x[1]); return v; }
        internal override double[] To(OFXInteger2D v) { return new double[] { v.X, v.Y }; }
    }
    public partial class OFXInteger3DParameter : OFXRangeParameter<OFXInteger3D, OFXInteger3DKeyframe>
    {
        internal OFXInteger3DParameter(ulong h) : base(h) { }
        internal override int Count { get { return 3; } }
        internal override OFXInteger3D From(double[] x)
        {
            OFXInteger3D v;
            v.X = (int)Math.Round(x[0]); v.Y = (int)Math.Round(x[1]); v.Z = (int)Math.Round(x[2]);
            return v;
        }
        internal override double[] To(OFXInteger3D v) { return new double[] { v.X, v.Y, v.Z }; }
    }
    public partial class OFXDoubleParameter : OFXRangeParameter<double, OFXDoubleKeyframe>
    {
        internal OFXDoubleParameter(ulong h) : base(h) { }
        internal override double From(double[] x) { return x[0]; }
        internal override double[] To(double v) { return new double[] { v }; }
    }
    public partial class OFXDouble2DParameter : OFXRangeParameter<OFXDouble2D, OFXDouble2DKeyframe>
    {
        internal OFXDouble2DParameter(ulong h) : base(h) { }
        internal override int Count { get { return 2; } }
        internal override OFXDouble2D From(double[] x) { OFXDouble2D v; v.X = x[0]; v.Y = x[1]; return v; }
        internal override double[] To(OFXDouble2D v) { return new double[] { v.X, v.Y }; }
    }
    public partial class OFXDouble3DParameter : OFXRangeParameter<OFXDouble3D, OFXDouble3DKeyframe>
    {
        internal OFXDouble3DParameter(ulong h) : base(h) { }
        internal override int Count { get { return 3; } }
        internal override OFXDouble3D From(double[] x) { OFXDouble3D v; v.X = x[0]; v.Y = x[1]; v.Z = x[2]; return v; }
        internal override double[] To(OFXDouble3D v) { return new double[] { v.X, v.Y, v.Z }; }
    }
    public partial class OFXStringParameter : OFXParameter<string, OFXStringKeyframe>
    {
        internal OFXStringParameter(ulong h) : base(h) { }
        internal override bool IsText { get { return true; } }
        internal override string FromText(string s) { return s; }
        internal override string ToText(string v) { return v ?? ""; }
    }
    public partial class OFXCustomParameter : OFXParameter<string, OFXCustomKeyframe>
    {
        internal OFXCustomParameter(ulong h) : base(h) { }
        internal override bool IsText { get { return true; } }
        internal override string FromText(string s) { return s; }
        internal override string ToText(string v) { return v ?? ""; }
    }

    // A parameter's keyframes, by time; new ones are made with SetValueAtTime.
    public partial class OFXKeyframes<T, K> : BaseList<K> where K : OFXKeyframe<T>
    {
        OFXParameter<T, K> param;
        internal OFXKeyframes(OFXParameter<T, K> p) { param = p; }
        internal override List<K> Items() { return Reno.WrapAll<K>(Reno.List(param.H, Abi.RL_OFX_KEYS, "OFXParameter.Keyframes")); }
        internal override bool RemoveItem(K item)
        {
            if (item == null || item.H == 0) return false;
            Reno.Call(param.H, Abi.RO_OFX_REMOVE_KEY, new long[] { (long)item.H }, null, null, "OFXKeyframes.Remove");
            return true;
        }
    }

    public partial class OFXKeyframe : RenoObject
    {
        internal OFXKeyframe(ulong h) : base(h) { }
        public Timecode Time { get { return GT(Abi.RP_KEY_TIME); } set { ST(Abi.RP_KEY_TIME, value); } }
        public OFXInterpolationType Interpolation { get { return (OFXInterpolationType)GI(Abi.RP_OFX_INTERP); } set { SI(Abi.RP_OFX_INTERP, (long)value); } }
        internal double Number(int k)
        {
            double d;
            Reno.Call(H, Abi.RO_OFX_GET, new long[] { k, 0 }, null, null, Member("Value"), out d);
            return d;
        }
        internal double[] Numbers(int n) { double[] x = new double[n]; for (int k = 0; k < n; ++k) x[k] = Number(k); return x; }
        internal void SetNumbers(double[] x) { Reno.Call(H, Abi.RO_OFX_SET, null, x, null, Member("Value")); }
    }
    public abstract partial class OFXKeyframe<T> : OFXKeyframe
    {
        internal OFXKeyframe(ulong h) : base(h) { }
        public abstract T Value { get; set; }
    }
    public partial class OFXBooleanKeyframe : OFXKeyframe<bool>
    {
        internal OFXBooleanKeyframe(ulong h) : base(h) { }
        public override bool Value { get { return Number(0) != 0; } set { SetNumbers(new double[] { value ? 1 : 0 }); } }
    }
    public partial class OFXChoiceKeyframe : OFXKeyframe<OFXChoice>
    {
        internal OFXChoiceKeyframe(ulong h) : base(h) { }
        public override OFXChoice Value
        {
            get { return new OFXChoice((int)Math.Round(Number(0)), ""); }
            set { if (value == null) throw new ArgumentNullException("value"); SetNumbers(new double[] { value.Index }); }
        }
    }
    public partial class OFXRGBKeyframe : OFXKeyframe<OFXColor>
    {
        internal OFXRGBKeyframe(ulong h) : base(h) { }
        public override OFXColor Value { get { double[] x = Numbers(3); return new OFXColor(x[0], x[1], x[2]); } set { SetNumbers(new double[] { value.R, value.G, value.B }); } }
    }
    public partial class OFXRGBAKeyframe : OFXKeyframe<OFXColor>
    {
        internal OFXRGBAKeyframe(ulong h) : base(h) { }
        public override OFXColor Value
        {
            get { double[] x = Numbers(4); return new OFXColor(x[0], x[1], x[2], x[3]); }
            set { SetNumbers(new double[] { value.R, value.G, value.B, value.A }); }
        }
    }
    public partial class OFXIntegerKeyframe : OFXKeyframe<int>
    {
        internal OFXIntegerKeyframe(ulong h) : base(h) { }
        public override int Value { get { return (int)Math.Round(Number(0)); } set { SetNumbers(new double[] { value }); } }
    }
    public partial class OFXInteger2DKeyframe : OFXKeyframe<OFXInteger2D>
    {
        internal OFXInteger2DKeyframe(ulong h) : base(h) { }
        public override OFXInteger2D Value
        {
            get { OFXInteger2D v; v.X = (int)Math.Round(Number(0)); v.Y = (int)Math.Round(Number(1)); return v; }
            set { SetNumbers(new double[] { value.X, value.Y }); }
        }
    }
    public partial class OFXInteger3DKeyframe : OFXKeyframe<OFXInteger3D>
    {
        internal OFXInteger3DKeyframe(ulong h) : base(h) { }
        public override OFXInteger3D Value
        {
            get { OFXInteger3D v; v.X = (int)Math.Round(Number(0)); v.Y = (int)Math.Round(Number(1)); v.Z = (int)Math.Round(Number(2)); return v; }
            set { SetNumbers(new double[] { value.X, value.Y, value.Z }); }
        }
    }
    public partial class OFXDoubleKeyframe : OFXKeyframe<double>
    {
        internal OFXDoubleKeyframe(ulong h) : base(h) { }
        public override double Value { get { return Number(0); } set { SetNumbers(new double[] { value }); } }
        public OFXControlPoint PrevControlPoint { get { throw NotSupported("PrevControlPoint"); } set { throw NotSupported("PrevControlPoint"); } }
        public OFXControlPoint NextControlPoint { get { throw NotSupported("NextControlPoint"); } set { throw NotSupported("NextControlPoint"); } }
    }
    public partial class OFXDouble2DKeyframe : OFXKeyframe<OFXDouble2D>
    {
        internal OFXDouble2DKeyframe(ulong h) : base(h) { }
        public override OFXDouble2D Value { get { OFXDouble2D v; v.X = Number(0); v.Y = Number(1); return v; } set { SetNumbers(new double[] { value.X, value.Y }); } }
        public OFXControlPoint PrevControlPointX { get { throw NotSupported("PrevControlPointX"); } set { throw NotSupported("PrevControlPointX"); } }
        public OFXControlPoint NextControlPointX { get { throw NotSupported("NextControlPointX"); } set { throw NotSupported("NextControlPointX"); } }
        public OFXControlPoint PrevControlPointY { get { throw NotSupported("PrevControlPointY"); } set { throw NotSupported("PrevControlPointY"); } }
        public OFXControlPoint NextControlPointY { get { throw NotSupported("NextControlPointY"); } set { throw NotSupported("NextControlPointY"); } }
    }
    public partial class OFXDouble3DKeyframe : OFXKeyframe<OFXDouble3D>
    {
        internal OFXDouble3DKeyframe(ulong h) : base(h) { }
        public override OFXDouble3D Value
        {
            get { OFXDouble3D v; v.X = Number(0); v.Y = Number(1); v.Z = Number(2); return v; }
            set { SetNumbers(new double[] { value.X, value.Y, value.Z }); }
        }
        public OFXControlPoint PrevControlPointX { get { throw NotSupported("PrevControlPointX"); } set { throw NotSupported("PrevControlPointX"); } }
        public OFXControlPoint NextControlPointX { get { throw NotSupported("NextControlPointX"); } set { throw NotSupported("NextControlPointX"); } }
        public OFXControlPoint PrevControlPointY { get { throw NotSupported("PrevControlPointY"); } set { throw NotSupported("PrevControlPointY"); } }
        public OFXControlPoint NextControlPointY { get { throw NotSupported("NextControlPointY"); } set { throw NotSupported("NextControlPointY"); } }
        public OFXControlPoint PrevControlPointZ { get { throw NotSupported("PrevControlPointZ"); } set { throw NotSupported("PrevControlPointZ"); } }
        public OFXControlPoint NextControlPointZ { get { throw NotSupported("NextControlPointZ"); } set { throw NotSupported("NextControlPointZ"); } }
    }
    public partial class OFXStringKeyframe : OFXKeyframe<string>
    {
        internal OFXStringKeyframe(ulong h) : base(h) { }
        public override string Value { get { return GS(Abi.RP_OFX_TEXT); } set { SS(Abi.RP_OFX_TEXT, value ?? ""); } }
    }
    public partial class OFXCustomKeyframe : OFXKeyframe<string>
    {
        internal OFXCustomKeyframe(ulong h) : base(h) { }
        public override string Value { get { return GS(Abi.RP_OFX_TEXT); } set { SS(Abi.RP_OFX_TEXT, value ?? ""); } }
    }

    public partial struct OFXControlPoint
    {
        public Timecode Time;
        public double Value;
        public OFXControlPoint(Timecode time, double val) { Time = time; Value = val; }
    }
    public partial struct OFXColor
    {
        public double R, G, B, A;
        public OFXColor(double r, double g, double b) { R = r; G = g; B = b; A = 1; }
        public OFXColor(double r, double g, double b, double a) { R = r; G = g; B = b; A = a; }
        public bool Equals(OFXColor that) { return R == that.R && G == that.G && B == that.B && A == that.A; }
        public override bool Equals(object that) { return that is OFXColor && Equals((OFXColor)that); }
        public override int GetHashCode() { return R.GetHashCode() ^ (G.GetHashCode() * 3) ^ (B.GetHashCode() * 7) ^ (A.GetHashCode() * 11); }
        public static bool operator ==(OFXColor a, OFXColor b) { return a.Equals(b); }
        public static bool operator !=(OFXColor a, OFXColor b) { return !a.Equals(b); }
    }
    public partial struct OFXInteger2D { public int X, Y; }
    public partial struct OFXInteger3D { public int X, Y, Z; }
    public partial struct OFXDouble2D { public double X, Y; }
    public partial struct OFXDouble3D { public double X, Y, Z; }
    public partial class OFXChoice
    {
        public string Name;
        public int Index;
        internal OFXChoice(int index, string name) { Index = index; Name = name; }
        public override string ToString() { return Name; }
    }

    public partial class EffectPreset
    {
        internal EffectPreset() { }
        public string Name { get { return ""; } }
        public int Index { get { return -1; } }
        public override string ToString() { return Name; }
    }

    public partial class EffectPresets : BaseList<EffectPreset>
    {
        internal EffectPresets() { }
        public override bool IsReadOnly { get { return true; } }
        internal override List<EffectPreset> Items() { return new List<EffectPreset>(); }
    }

    public partial class Keyframes : BaseList<Keyframe>
    {
        internal Keyframes() { }
        internal override List<Keyframe> Items() { return new List<Keyframe>(); }
    }

    public partial class Keyframe
    {
        public Keyframe(Timecode position) { throw new NotSupportedException("Keyframe (effect keyframes) is not supported by Reno."); }
        public Timecode Position { get { return null; } set { } }
        public VideoKeyframeType Type { get { return VideoKeyframeType.Linear; } set { } }
    }

    // The effects of a track, an event, a bus or the video output.
    public partial class Effects : BaseList<Effect>
    {
        RenoObject owner;
        bool master;
        internal Effects(RenoObject owner) { this.owner = owner; }
        internal Effects(RenoObject owner, bool master) { this.owner = owner; this.master = master; }
        internal override List<Effect> Items() { return Reno.WrapAll<Effect>(owner.L(master ? Abi.RL_MASTER_EFFECTS : Abi.RL_EFFECTS, "Effects")); }
        void Put(Effect e, int index)
        {
            if (e.H != 0) throw new InvalidOperationException("Effects.Add: the effect is already in a chain");
            ulong h = owner.Op(master ? Abi.RO_ADD_MASTER_FX : Abi.RO_ADD_FX, new long[] { (long)e.newPlugIn.H, index }, null, null, "Effects.Add");
            e.Attached(h);
        }
        internal override void AddItem(Effect e) { Put(e, -1); }
        public override void Insert(int index, Effect item) { Put(item, index); }
        internal override bool RemoveItem(Effect e)
        {
            if (e.H == 0) return false;
            owner.Op(Abi.RO_REMOVE_FX, new long[] { (long)e.H }, null, null, "Effects.Remove");
            return true;
        }
        public Effect AddEffect(PlugInNode plugIn) { Effect e = new Effect(plugIn); Add(e); return e; }
    }

    // An installed plug-in, or a folder of them (Vegas.VideoFX, AudioFX, Transitions, Generators).
    public partial class PlugInNode : BaseList<PlugInNode>
    {
        internal ulong H;
        string containerName;
        int containerList;          // RL_...; -1 an empty folder; 0 the root of all four
        internal PlugInNode(ulong h) { H = h; }
        PlugInNode(string name, int list) { containerName = name; containerList = list; }
        internal static PlugInNode Container(string name, int list) { return new PlugInNode(name, list); }
        internal static PlugInNode Root() { return new PlugInNode("Plug-Ins", 0); }

        bool IsFolder { get { return H == 0; } }
        string S(int p, string m) { return IsFolder ? "" : Reno.GS(H, p, "PlugInNode." + m); }
        long I(int p, string m) { return IsFolder ? 0 : Reno.GI(H, p, "PlugInNode." + m); }

        internal override List<PlugInNode> Items()
        {
            if (!IsFolder) return new List<PlugInNode>();
            if (containerList == 0)
                return new List<PlugInNode> { Container("Video FX", Abi.RL_VIDEO_FX), Container("Audio FX", Abi.RL_AUDIO_FX),
                                              Container("Transitions", Abi.RL_TRANSITIONS), Container("Media Generators", Abi.RL_GENERATORS) };
            if (containerList < 0) return new List<PlugInNode>();
            return Reno.WrapAll<PlugInNode>(Reno.List(1, containerList, "PlugInNode"));
        }
        public override bool IsFixedSize { get { return true; } }
        public override bool IsReadOnly { get { return true; } }
        public string Name { get { return IsFolder ? containerName : S(Abi.RP_NAME, "Name"); } }
        public string Info { get { return Name; } }
        public string Group { get { return S(Abi.RP_GROUP_NAME, "Group"); } }
        public string UniqueID { get { return S(Abi.RP_UNIQUE_ID, "UniqueID"); } }
        public Guid ClassID { get { return IsFolder ? Guid.Empty : new Guid(S(Abi.RP_CLASS_ID, "ClassID")); } }
        public bool IsVideo { get { return IsFolder ? containerList != Abi.RL_AUDIO_FX : I(Abi.RP_IS_VIDEO, "IsVideo") != 0; } }
        public bool IsAudio { get { return IsFolder ? containerList == Abi.RL_AUDIO_FX : I(Abi.RP_IS_AUDIO, "IsAudio") != 0; } }
        public bool IsContainer { get { return IsFolder; } }
        public bool CanCreateSubnodes { get { return false; } }
        public bool IsDisabled { get { return false; } }
        public bool IsPackage { get { return false; } }
        public bool IsAutomatable { get { return IsAudio; } }
        public bool IsOFX { get { return !IsFolder && I(Abi.RP_IS_OFX, "IsOFX") != 0; } }
        public OFXPlugIn OFXPlugIn { get { return IsOFX ? new OFXPlugIn(this) : null; } }
        public EffectPresets Presets { get { return new EffectPresets(); } }
        public PlugInNode GetChild(int index) { return this[index]; }

        // VEGAS's own effects are called "VEGAS Gaussian Blur" (older scripts: "Sony Gaussian Blur"); Reno's list may
        // name them without the maker, so a name is also tried without it.
        static string Bare(string n)
        {
            n = (n ?? "").Trim();
            foreach (string p in new string[] { "VEGAS ", "Sony ", "MAGIX ", "Magix " })
                if (n.StartsWith(p, StringComparison.OrdinalIgnoreCase)) return n.Substring(p.Length).Trim();
            return n;
        }
        public PlugInNode GetChildByName(string val)
        {
            List<PlugInNode> all = Items();
            foreach (PlugInNode n in all) if (string.Equals(n.Name, val, StringComparison.OrdinalIgnoreCase)) return n;
            foreach (PlugInNode n in all) if (string.Equals(Bare(n.Name), Bare(val), StringComparison.OrdinalIgnoreCase)) return n;
            foreach (PlugInNode n in all) { PlugInNode c = n.IsContainer ? n.GetChildByName(val) : null; if (c != null) return c; }
            return null;
        }
        public PlugInNode FindChildByName(string name) { return GetChildByName(name); }
        public PlugInNode GetChildByClassID(Guid val)
        {
            foreach (PlugInNode n in Items())
            {
                if (!n.IsContainer && n.ClassID == val) return n;
                PlugInNode c = n.IsContainer ? n.GetChildByClassID(val) : null;
                if (c != null) return c;
            }
            return null;
        }
        public PlugInNode FindChildByClassID(Guid classID) { return GetChildByClassID(classID); }
        public PlugInNode GetChildByUniqueID(string val)
        {
            foreach (PlugInNode n in Items())
            {
                if (!n.IsContainer && string.Equals(BareId(n.UniqueID), BareId(val), StringComparison.OrdinalIgnoreCase)) return n;
                PlugInNode c = n.IsContainer ? n.GetChildByUniqueID(val) : null;
                if (c != null) return c;
            }
            return null;
        }
        // VEGAS names OpenFX plug-ins "{Svfx:<identifier>}"
        static string BareId(string id)
        {
            id = id ?? "";
            if (id.StartsWith("{Svfx:", StringComparison.OrdinalIgnoreCase) && id.EndsWith("}")) return id.Substring(6, id.Length - 7);
            return id;
        }
        public PlugInNode FindChildByUniqueID(string uniqueID) { return GetChildByUniqueID(uniqueID); }
        public bool CanAnimate() { return false; }

        public override bool Equals(object obj)
        {
            PlugInNode o = obj as PlugInNode;
            if (ReferenceEquals(o, null)) return false;
            if (IsFolder || o.IsFolder) return IsFolder && o.IsFolder && containerList == o.containerList;
            return H == o.H;
        }
        public override int GetHashCode() { return IsFolder ? containerList : H.GetHashCode(); }
        public static bool operator ==(PlugInNode a, PlugInNode b)
        {
            if (ReferenceEquals(a, b)) return true;
            if (ReferenceEquals(a, null) || ReferenceEquals(b, null)) return false;
            return a.Equals(b);
        }
        public static bool operator !=(PlugInNode a, PlugInNode b) { return !(a == b); }
        public override string ToString() { return Name; }
    }

    // ------------------------------------------------------------------------------------------------ envelopes
    public partial class Envelope : RenoObject
    {
        internal EnvelopeType newType;
        internal Envelope(ulong h) : base(h) { }
        public Envelope(EnvelopeType type) { newType = type; }
        public EnvelopeType Type { get { return H == 0 ? newType : (EnvelopeType)GI(Abi.RP_ENV_TYPE); } }
        public string Name { get { return H == 0 ? newType.ToString() : GS(Abi.RP_NAME); } }
        public int Index { get { return H == 0 ? -1 : (int)GI(Abi.RP_INDEX); } }
        public double Min { get { return GD(Abi.RP_ENV_MIN); } }
        public double Max { get { return GD(Abi.RP_ENV_MAX); } }
        public double Neutral { get { return GD(Abi.RP_ENV_NEUTRAL); } }
        public EnvelopePoints Points { get { return new EnvelopePoints(this); } }
        public double ValueAt(Timecode position)
        {
            double v;
            NeedAdded("ValueAt");
            Reno.Call(H, Abi.RO_VALUE_AT, null, new double[] { position.ToSeconds() }, null, "Envelope.ValueAt", out v);
            return v;
        }
    }

    public partial class Envelopes : BaseList<Envelope>
    {
        RenoObject owner;
        bool master;
        internal Envelopes(RenoObject owner) { this.owner = owner; }
        internal Envelopes(RenoObject owner, bool master) { this.owner = owner; this.master = master; }
        internal override List<Envelope> Items() { return Reno.WrapAll<Envelope>(owner.L(master ? Abi.RL_MASTER_ENVELOPES : Abi.RL_ENVELOPES, "Envelopes")); }
        internal override void AddItem(Envelope e)
        {
            if (e.H != 0) throw new InvalidOperationException("Envelopes.Add: the envelope already belongs to something");
            if (FindByType(e.newType) != null) throw new InvalidOperationException("Envelopes.Add: there is already a " + e.newType + " envelope");
            ulong h = owner.Op(master ? Abi.RO_ADD_MASTER_ENVELOPE : Abi.RO_ADD_ENVELOPE, new long[] { (long)e.newType }, null, null, "Envelopes.Add");
            e.Attached(h);
        }
        internal override bool RemoveItem(Envelope e)
        {
            if (e.H == 0) return false;
            owner.Op(Abi.RO_REMOVE_ENVELOPE, new long[] { (long)e.H }, null, null, "Envelopes.Remove");
            return true;
        }
        public bool HasEnvelope(EnvelopeType type) { return FindByType(type) != null; }
        public Envelope FindByType(EnvelopeType type)
        {
            foreach (Envelope e in Items()) if (e.Type == type) return e;
            return null;
        }
    }

    public partial class EnvelopePoint : RenoObject
    {
        internal EnvelopePoint(ulong h) : base(h) { }
        public EnvelopePoint(Timecode x, double y) : this(x, y, CurveType.Linear) { }
        public EnvelopePoint(Timecode x, double y, CurveType curveType)
        {
            SD(Abi.RP_X, x == null ? 0 : x.ToSeconds(), "X");
            SD(Abi.RP_Y, y, "Y");
            SI(Abi.RP_CURVE, (long)curveType, "Curve");
        }
        public EnvelopeType EnvelopeType { get { return (EnvelopeType)GI(Abi.RP_ENV_TYPE); } }
        public int Index { get { return H == 0 ? -1 : (int)GI(Abi.RP_INDEX); } }
        public Timecode X { get { return GT(Abi.RP_X); } set { ST(Abi.RP_X, value); } }
        public double Y { get { return GD(Abi.RP_Y); } set { SD(Abi.RP_Y, value); } }
        public CurveType Curve { get { return (CurveType)GI(Abi.RP_CURVE); } set { SI(Abi.RP_CURVE, (long)value); } }
        public static Timecode MaxTransitionProgress { get { return Timecode.FromSeconds(1); } }
    }

    public partial class EnvelopePoints : BaseList<EnvelopePoint>
    {
        Envelope env;
        internal EnvelopePoints(Envelope e) { env = e; }
        internal override List<EnvelopePoint> Items() { return Reno.WrapAll<EnvelopePoint>(env.L(Abi.RL_POINTS, "Points")); }
        internal override void AddItem(EnvelopePoint p)
        {
            if (p.H != 0) throw new InvalidOperationException("EnvelopePoints.Add: the point is already in an envelope");
            double x = p.X.ToSeconds(), y = p.Y;
            long curve = p.pending != null && p.pending.ContainsKey(Abi.RP_CURVE) ? Convert.ToInt64(p.pending[Abi.RP_CURVE]) : (long)CurveType.Linear;
            ulong h = env.Op(Abi.RO_ADD_POINT, new long[] { curve }, new double[] { x, y }, null, "Points.Add");
            p.pending = null;
            p.Attached(h);
        }
        internal override bool RemoveItem(EnvelopePoint p)
        {
            if (p.H == 0) return false;
            env.Op(Abi.RO_REMOVE_POINT, new long[] { (long)p.H }, null, null, "Points.Remove");
            return true;
        }
        public override void Clear() { env.Op(Abi.RO_CLEAR_POINTS, null, null, null, "Points.Clear"); }
        public EnvelopePoint GetPointAtX(Timecode x)
        {
            foreach (EnvelopePoint p in Items()) if (p.X == x) return p;
            return null;
        }
    }

    // ------------------------------------------------------------------------------------------------ markers
    public partial class Marker : RenoObject
    {
        internal virtual int MarkerKind { get { return 0; } }
        internal Marker(ulong h) : base(h) { }
        public Marker() : this(new Timecode(), null) { }
        public Marker(Timecode position) : this(position, null) { }
        public Marker(Timecode position, string label)
        {
            SD(Abi.RP_START, position == null ? 0 : position.ToSeconds(), "Position");
            if (label != null) SS(Abi.RP_NAME, label, "Label");
        }
        public int Index { get { return H == 0 ? -1 : (int)GI(Abi.RP_INDEX); } }
        public Timecode Position { get { return GT(Abi.RP_START); } set { ST(Abi.RP_START, value); } }
        public virtual string Label { get { return GS(Abi.RP_NAME); } set { SS(Abi.RP_NAME, value); } }

        internal void AddTo(int kind, string command)
        {
            if (H != 0) throw new InvalidOperationException("Markers.Add: the marker is already in the project");
            double len = pending != null && pending.ContainsKey(Abi.RP_LENGTH) ? Convert.ToDouble(pending[Abi.RP_LENGTH]) : 0;
            string label = pending != null && pending.ContainsKey(Abi.RP_NAME) ? (string)pending[Abi.RP_NAME] : "";
            ulong h = Reno.Call(1, Abi.RO_ADD_MARKER, new long[] { kind }, new double[] { Position.ToSeconds(), len },
                                command != null ? command + "\n" + label : label, GetType().Name + "s.Add");
            pending = null;
            Attached(h);
        }
    }

    public partial class Region : Marker
    {
        internal override int MarkerKind { get { return 1; } }
        internal Region(ulong h) : base(h) { }
        public Region() : this(new Timecode(), new Timecode(), null) { }
        public Region(Timecode position, Timecode length) : this(position, length, null) { }
        public Region(Timecode position, Timecode length, string label) : base(position, label)
        {
            SD(Abi.RP_LENGTH, length == null ? 0 : length.ToSeconds(), "Length");
        }
        public Timecode Length { get { return GT(Abi.RP_LENGTH); } set { ST(Abi.RP_LENGTH, value); } }
        public Timecode End { get { return Position + Length; } set { Length = value - Position; } }
    }

    public partial class CDMarker : Marker
    {
        internal override int MarkerKind { get { return 4; } }
        internal CDMarker(ulong h) : base(h) { }
        public CDMarker() : base(new Timecode(), null) { }
        public CDMarker(Timecode position) : base(position, null) { }
        public CDMarker(Timecode position, string label) : base(position, label) { }
    }

    public partial class CDRegion : Region
    {
        internal override int MarkerKind { get { return 3; } }
        internal CDRegion(ulong h) : base(h) { }
        public CDRegion() : base(new Timecode(), new Timecode(), null) { }
        public CDRegion(Timecode position, Timecode length) : base(position, length, null) { }
        public CDRegion(Timecode position, Timecode length, string label) : base(position, length, label) { }
    }

    public partial class MarkerCommandType
    {
        string cmd;
        public MarkerCommandType(string cmd) { this.cmd = cmd; }
        public override string ToString() { return cmd; }
        public override bool Equals(object obj) { MarkerCommandType o = obj as MarkerCommandType; return o != null && string.Equals(o.cmd, cmd, StringComparison.OrdinalIgnoreCase); }
        public override int GetHashCode() { return (cmd ?? "").ToUpperInvariant().GetHashCode(); }
        public static MarkerCommandType URL = new MarkerCommandType("URL"), Text = new MarkerCommandType("TEXT"),
            WMClosedCaption = new MarkerCommandType("WMClosedCaption"), WMWMTextBodytext = new MarkerCommandType("WMTextBodyText"),
            WMTextHeadline = new MarkerCommandType("WMTextHeadline"), Title = new MarkerCommandType("Title"), Author = new MarkerCommandType("Author"),
            Copyright = new MarkerCommandType("Copyright"), HotSpotPlay = new MarkerCommandType("HotSpotPlay"),
            HotSpotBrowse = new MarkerCommandType("HotSpotBrowse"), HotSpotSeek = new MarkerCommandType("HotSpotSeek"),
            ScottEOM = new MarkerCommandType("ScottEOM"), ScottCueIn = new MarkerCommandType("ScottCueIn"), CEA608CC1 = new MarkerCommandType("CEA608CC1"),
            CEA608CC2 = new MarkerCommandType("CEA608CC2"), CEA608CC3 = new MarkerCommandType("CEA608CC3"), CEA608CC4 = new MarkerCommandType("CEA608CC4");
    }

    // A command marker: its command, and its parameter (Reno keeps the parameter as the marker's name).
    public partial class CommandMarker : Marker
    {
        internal string newCommand = "TEXT";
        internal override int MarkerKind { get { return 2; } }
        internal CommandMarker(ulong h) : base(h) { }
        public CommandMarker() : base(new Timecode(), null) { }
        public CommandMarker(Timecode position) : base(position, null) { }
        public CommandMarker(Timecode position, MarkerCommandType cmd, string param) : base(position, param) { newCommand = cmd == null ? "TEXT" : cmd.ToString(); }
        public CommandMarker(Timecode position, MarkerCommandType cmd, string param, string comment) : this(position, cmd, param) { }
        public MarkerCommandType CommandType { get { return new MarkerCommandType(H == 0 ? newCommand : GS(Abi.RP_COMMAND_TYPE)); } }
        public string CommandParameter { get { return Label; } set { Label = value; } }
        public string Comment { get { return ""; } set { throw NotSupported("Comment"); } }
        public void SetCommand(MarkerCommandType cmdType, string param)
        {
            if (H == 0) newCommand = cmdType.ToString(); else SS(Abi.RP_COMMAND_TYPE, cmdType.ToString(), "SetCommand");
            Label = param;
        }
    }

    public abstract partial class BaseMarkerList<T> : BaseList<T> where T : Marker
    {
        internal int kind, list;
        internal BaseMarkerList(int kind, int list) { this.kind = kind; this.list = list; }
        internal override List<T> Items() { return Reno.WrapAll<T>(Reno.List(1, list, GetType().Name)); }
        internal override void AddItem(T m)
        {
            if (m.MarkerKind != kind) throw new ArgumentException(GetType().Name + ".Add: not a " + typeof(T).Name);
            CommandMarker c = m as CommandMarker;
            m.AddTo(kind, c != null ? c.newCommand : null);
        }
        internal override bool RemoveItem(T m)
        {
            if (m.H == 0) return false;
            Reno.Call(1, Abi.RO_REMOVE_MARKER, new long[] { (long)m.H }, null, null, GetType().Name + ".Remove");
            return true;
        }
    }
    public partial class MarkerList : BaseMarkerList<Marker> { public MarkerList() : base(0, Abi.RL_MARKERS) { } internal MarkerList(Project p) : this() { } }
    public partial class RegionList : BaseMarkerList<Region> { public RegionList() : base(1, Abi.RL_REGIONS) { } internal RegionList(Project p) : this() { } }
    public partial class CommandMarkerList : BaseMarkerList<CommandMarker> { public CommandMarkerList() : base(2, Abi.RL_COMMAND_MARKERS) { } internal CommandMarkerList(Project p) : this() { } }
    public partial class CDRegionList : BaseMarkerList<CDRegion> { public CDRegionList() : base(3, Abi.RL_CD_TRACKS) { } internal CDRegionList(Project p) : this() { } }
    public partial class CDMarkerList : BaseMarkerList<CDMarker> { public CDMarkerList() : base(4, Abi.RL_CD_INDICES) { } internal CDMarkerList(Project p) : this() { } }

    // ------------------------------------------------------------------------------------------------ groups
    // Events that move together. A new group gets its events once it is in Project.Groups.
    public partial class TrackEventGroup : BaseList<TrackEvent>
    {
        internal ulong H;
        List<TrackEvent> waiting = new List<TrackEvent>();
        internal TrackEventGroup(ulong h) { H = h; }
        public TrackEventGroup() { }
        public TrackEventGroup(Project project) { }
        internal override List<TrackEvent> Items()
        {
            if (H == 0) return new List<TrackEvent>(waiting);
            return Reno.WrapAll<TrackEvent>(Reno.List(H, Abi.RL_EVENTS, "TrackEventGroup"));
        }
        internal override void AddItem(TrackEvent e)
        {
            if (H == 0) { waiting.Add(e); return; }
            e.NeedAdded("Group.Add");
            Reno.Call(H, Abi.RO_GROUP_ADD, new long[] { (long)e.H }, null, null, "TrackEventGroup.Add");
        }
        internal override bool RemoveItem(TrackEvent e)
        {
            if (H == 0) return waiting.Remove(e);
            Reno.Call(H, Abi.RO_GROUP_REMOVE, new long[] { (long)e.H }, null, null, "TrackEventGroup.Remove");
            return true;
        }
        internal void Attached(ulong h)
        {
            H = h;
            Reno.Adopt(this, h);
            foreach (TrackEvent e in waiting) AddItem(e);
            waiting.Clear();
        }
        public bool IsValid() { long v; return H != 0 && Reno.TryGI(H, Abi.RP_IS_VALID, out v) && v != 0; }
        public override bool Equals(object obj) { TrackEventGroup g = obj as TrackEventGroup; return !ReferenceEquals(g, null) && (ReferenceEquals(g, this) || (H != 0 && g.H == H)); }
        public override int GetHashCode() { return H != 0 ? H.GetHashCode() : base.GetHashCode(); }
        public static bool operator ==(TrackEventGroup a, TrackEventGroup b)
        {
            if (ReferenceEquals(a, b)) return true;
            if (ReferenceEquals(a, null) || ReferenceEquals(b, null)) return false;
            return a.Equals(b);
        }
        public static bool operator !=(TrackEventGroup a, TrackEventGroup b) { return !(a == b); }
    }

    public partial class TrackEventGroups : BaseList<TrackEventGroup>
    {
        internal TrackEventGroups(Project p) { }
        internal override List<TrackEventGroup> Items() { return Reno.WrapAll<TrackEventGroup>(Reno.List(1, Abi.RL_GROUPS, "TrackEventGroups")); }
        internal override void AddItem(TrackEventGroup g)
        {
            if (g.H != 0) throw new InvalidOperationException("TrackEventGroups.Add: the group is already in the project");
            g.Attached(Reno.Call(1, Abi.RO_NEW_GROUP, null, null, null, "TrackEventGroups.Add"));
        }
        internal override bool RemoveItem(TrackEventGroup g)
        {
            if (g.H == 0) return false;
            Reno.Call(1, Abi.RO_REMOVE_GROUP, new long[] { (long)g.H }, null, null, "TrackEventGroups.Remove");
            return true;
        }
    }

    // ------------------------------------------------------------------------------------------------ pan/crop
    public partial class VideoMotionVertex
    {
        public float X { get; set; }
        public float Y { get; set; }
        public VideoMotionVertex(float x, float y) { X = x; Y = y; }
        public override string ToString() { return "{" + X.ToString(CultureInfo.InvariantCulture) + ", " + Y.ToString(CultureInfo.InvariantCulture) + "}"; }
    }

    public partial class VideoMotionBounds
    {
        public VideoMotionVertex TopLeft { get; set; }
        public VideoMotionVertex TopRight { get; set; }
        public VideoMotionVertex BottomRight { get; set; }
        public VideoMotionVertex BottomLeft { get; set; }
        public VideoMotionBounds(VideoMotionVertex topLeft, VideoMotionVertex topRight, VideoMotionVertex bottomRight, VideoMotionVertex bottomLeft)
        {
            TopLeft = topLeft; TopRight = topRight; BottomRight = bottomRight; BottomLeft = bottomLeft;
        }
        public VideoMotionBounds(float topLeftX, float topLeftY, float topRightX, float topRightY, float bottomRightX, float bottomRightY, float bottomLeftX, float bottomLeftY)
            : this(new VideoMotionVertex(topLeftX, topLeftY), new VideoMotionVertex(topRightX, topRightY),
                   new VideoMotionVertex(bottomRightX, bottomRightY), new VideoMotionVertex(bottomLeftX, bottomLeftY)) { }
    }

    // An event's pan/crop (VEGAS's VideoMotion): its keys, in the pixels of the event's media.
    public partial class VideoMotion
    {
        VideoEvent ev;
        internal VideoMotion(VideoEvent e) { ev = e; }
        public VideoMotionKeyframes Keyframes { get { return new VideoMotionKeyframes(ev); } }
        public bool ScaleToFill { get { return false; } set { throw new NotSupportedException("VideoMotion.ScaleToFill is not supported by Reno."); } }
    }

    // A pan/crop key. Reno stores centre and size as fractions of the media and the turn in degrees clockwise;
    // VEGAS uses pixels and radians anticlockwise.
    public partial class VideoMotionKeyframe : RenoObject
    {
        double nx = 0.5, ny = 0.5, nw = 1, nh = 1, nrot = 0, ntime = 0;     // (a script's new key, before it is added: in pixels once known)
        bool newHasBox;
        int newType;
        internal VideoMotionKeyframe(ulong h) : base(h) { }
        public VideoMotionKeyframe(Timecode position) { ntime = position == null ? 0 : position.ToSeconds(); }
        public VideoMotionKeyframe(Project project, Timecode position) : this(position) { }

        double MW { get { return H == 0 ? 0 : Math.Max(1, ownerGI(Abi.RP_MEDIA_WIDTH)); } }
        double MH { get { return H == 0 ? 0 : Math.Max(1, ownerGI(Abi.RP_MEDIA_HEIGHT)); } }
        internal VideoEvent owner;
        long ownerGI(int p) { return owner.GI(p, "VideoMotionKeyframe"); }

        public Timecode Position
        {
            get { return H == 0 ? Timecode.FromSeconds(ntime) : GT(Abi.RP_KEY_TIME); }
            set { if (H == 0) ntime = value.ToSeconds(); else ST(Abi.RP_KEY_TIME, value); }
        }
        public float Smoothness { get { return 0; } set { } }
        public VideoKeyframeType Type
        {
            get { return H == 0 ? (VideoKeyframeType)newType : (VideoKeyframeType)GI(Abi.RP_KEY_TYPE); }
            set { if (H == 0) newType = (int)value; else SI(Abi.RP_KEY_TYPE, (long)value); }
        }
        // the box in pixels: centre, size, turn (radians, anticlockwise)
        void Get(out double cx, out double cy, out double w, out double h, out double rot)
        {
            if (H == 0) { cx = nx; cy = ny; w = nw; h = nh; rot = nrot; return; }
            cx = GD(Abi.RP_KEY_X, "Center") * MW;
            cy = GD(Abi.RP_KEY_Y, "Center") * MH;
            w = GD(Abi.RP_KEY_W, "Bounds") * MW;
            h = GD(Abi.RP_KEY_H, "Bounds") * MH;
            rot = -GD(Abi.RP_KEY_ROTATION, "Rotation") * Math.PI / 180.0;
        }
        void Set(double cx, double cy, double w, double h, double rot)
        {
            if (H == 0) { nx = cx; ny = cy; nw = w; nh = h; nrot = rot; newHasBox = true; return; }
            SD(Abi.RP_KEY_X, cx / MW, "Center");
            SD(Abi.RP_KEY_Y, cy / MH, "Center");
            SD(Abi.RP_KEY_W, Math.Max(1e-6, w / MW), "Bounds");
            SD(Abi.RP_KEY_H, Math.Max(1e-6, h / MH), "Bounds");
            SD(Abi.RP_KEY_ROTATION, -rot * 180.0 / Math.PI, "Rotation");
        }
        public VideoMotionVertex Center
        {
            get { double cx, cy, w, h, r; Get(out cx, out cy, out w, out h, out r); return new VideoMotionVertex((float)cx, (float)cy); }
            set { double cx, cy, w, h, r; Get(out cx, out cy, out w, out h, out r); Set(value.X, value.Y, w, h, r); }
        }
        public double Rotation
        {
            get { double cx, cy, w, h, r; Get(out cx, out cy, out w, out h, out r); return r; }
            set { double cx, cy, w, h, r; Get(out cx, out cy, out w, out h, out r); Set(cx, cy, w, h, value); }
        }
        public VideoMotionBounds Bounds
        {
            get
            {
                double cx, cy, w, h, r;
                Get(out cx, out cy, out w, out h, out r);
                // (y grows downwards: an anticlockwise turn on screen is a negative angle in these coordinates)
                double c = Math.Cos(-r), s = Math.Sin(-r);
                Func<double, double, VideoMotionVertex> at = (dx, dy) => new VideoMotionVertex((float)(cx + dx * c - dy * s), (float)(cy + dx * s + dy * c));
                return new VideoMotionBounds(at(-w / 2, -h / 2), at(w / 2, -h / 2), at(w / 2, h / 2), at(-w / 2, h / 2));
            }
            set
            {
                if (value == null) throw new ArgumentNullException("Bounds");
                double cx = (value.TopLeft.X + value.TopRight.X + value.BottomRight.X + value.BottomLeft.X) / 4.0;
                double cy = (value.TopLeft.Y + value.TopRight.Y + value.BottomRight.Y + value.BottomLeft.Y) / 4.0;
                double ux = value.TopRight.X - value.TopLeft.X, uy = value.TopRight.Y - value.TopLeft.Y;
                double vx = value.BottomLeft.X - value.TopLeft.X, vy = value.BottomLeft.Y - value.TopLeft.Y;
                double w = Math.Sqrt(ux * ux + uy * uy), h = Math.Sqrt(vx * vx + vy * vy);
                if (w <= 0 || h <= 0) throw new ArgumentException("VideoMotionKeyframe.Bounds: the box has no size");
                Set(cx, cy, w, h, -Math.Atan2(uy, ux));
            }
        }
        public VideoMotionVertex TopLeft { get { return Bounds.TopLeft; } }
        public VideoMotionVertex TopRight { get { return Bounds.TopRight; } }
        public VideoMotionVertex BottomRight { get { return Bounds.BottomRight; } }
        public VideoMotionVertex BottomLeft { get { return Bounds.BottomLeft; } }
        public void MoveBy(VideoMotionVertex delta)
        {
            double cx, cy, w, h, r;
            Get(out cx, out cy, out w, out h, out r);
            Set(cx + delta.X, cy + delta.Y, w, h, r);
        }
        public void ScaleBy(VideoMotionVertex delta)
        {
            double cx, cy, w, h, r;
            Get(out cx, out cy, out w, out h, out r);
            Set(cx, cy, w * delta.X, h * delta.Y, r);
        }
        public void RotateBy(double delta) { Rotation = Rotation + delta; }

        internal void AttachTo(VideoEvent ev, ulong h)
        {
            owner = ev;
            Reno.Adopt(this, h);
            if (newHasBox) Set(nx, ny, nw, nh, nrot);
            if (newType != 0) SI(Abi.RP_KEY_TYPE, newType, "Type");
        }
    }

    public partial class VideoMotionKeyframes : BaseList<VideoMotionKeyframe>
    {
        VideoEvent ev;
        internal VideoMotionKeyframes(VideoEvent e) { ev = e; }
        internal override List<VideoMotionKeyframe> Items()
        {
            List<VideoMotionKeyframe> l = Reno.WrapAll<VideoMotionKeyframe>(ev.L(Abi.RL_PANCROP, "VideoMotion.Keyframes"));
            foreach (VideoMotionKeyframe k in l) k.owner = ev;
            return l;
        }
        internal override void AddItem(VideoMotionKeyframe k)
        {
            if (k.H != 0) throw new InvalidOperationException("VideoMotionKeyframes.Add: the key is already an event's");
            // (if a key is there already - the first key, at 0, always is - the new one becomes it, with its settings)
            ulong h = ev.Op(Abi.RO_ADD_PANCROP_KEY, null, new double[] { k.Position.ToSeconds() }, null, "VideoMotion.Keyframes.Add");
            k.AttachTo(ev, h);
        }
        internal override bool RemoveItem(VideoMotionKeyframe k)
        {
            if (k.H == 0) return false;
            ev.Op(Abi.RO_REMOVE_PANCROP_KEY, new long[] { (long)k.H }, null, null, "VideoMotion.Keyframes.Remove");
            return true;
        }
        public override void Clear() { ev.Op(Abi.RO_CLEAR_PANCROP, null, null, null, "VideoMotion.Keyframes.Clear"); }
    }

    // ------------------------------------------------------------------------------------------------ track motion
    // A video track's track motion. Reno stores centre and size as fractions of the frame and the turn in degrees
    // clockwise; VEGAS uses pixels from the frame's centre (y up) and radians anticlockwise. The conversions are not
    // checked against VEGAS.
    public partial class TrackMotion
    {
        VideoTrack track;
        internal TrackMotion(VideoTrack t) { track = t; }
        public bool HasMotionData { get { return MotionKeyframes.Count > 0; } }
        public TrackMotionKeyframeList MotionKeyframes { get { return new TrackMotionKeyframeList(track); } }
        public TrackMotionKeyframe InsertMotionKeyframe(Timecode position)
        {
            return Reno.W<TrackMotionKeyframe>(track.Op(Abi.RO_ADD_MOTION_KEY, null, new double[] { position.ToSeconds() }, null, "InsertMotionKeyframe"));
        }
        public bool HasShadowData { get { return false; } }
        public bool ShadowEnabled { get { return false; } set { throw new NotSupportedException("TrackMotion.ShadowEnabled is not supported by Reno."); } }
        public bool HasGlowData { get { return false; } }
        public bool GlowEnabled { get { return false; } set { throw new NotSupportedException("TrackMotion.GlowEnabled is not supported by Reno."); } }
        public TrackShadowKeyframeList ShadowKeyframes { get { return new TrackShadowKeyframeList(); } }
        public TrackGlowKeyframeList GlowKeyframes { get { return new TrackGlowKeyframeList(); } }
    }

    public abstract partial class BaseTrackMotionKeyframe : RenoObject
    {
        internal BaseTrackMotionKeyframe(ulong h) : base(h) { }
        protected BaseTrackMotionKeyframe() { }
        static double FW { get { return Reno.GI(1, Abi.RP_WIDTH, "TrackMotionKeyframe"); } }
        static double FH { get { return Reno.GI(1, Abi.RP_HEIGHT, "TrackMotionKeyframe"); } }
        public int Index { get { return (int)GI(Abi.RP_INDEX); } }
        public Timecode Position { get { return GT(Abi.RP_KEY_TIME); } set { ST(Abi.RP_KEY_TIME, value); } }
        public double Smoothness { get { return 0; } set { } }
        public VideoKeyframeType Type { get { return (VideoKeyframeType)GI(Abi.RP_KEY_TYPE); } set { SI(Abi.RP_KEY_TYPE, (long)value); } }
        public bool Selected { get { return false; } set { } }
        public double PositionX { get { return (GD(Abi.RP_KEY_X) - 0.5) * FW; } set { SD(Abi.RP_KEY_X, value / FW + 0.5); } }
        public double PositionY { get { return (0.5 - GD(Abi.RP_KEY_Y)) * FH; } set { SD(Abi.RP_KEY_Y, 0.5 - value / FH); } }
        public double Width { get { return GD(Abi.RP_KEY_W) * FW; } set { SD(Abi.RP_KEY_W, value / FW); } }
        public double Height { get { return GD(Abi.RP_KEY_H) * FH; } set { SD(Abi.RP_KEY_H, value / FH); } }
        public double RotationZ { get { return -GD(Abi.RP_KEY_ROTATION) * Math.PI / 180.0; } set { SD(Abi.RP_KEY_ROTATION, -value * 180.0 / Math.PI); } }
        public double RotationOffsetX { get { return 0; } set { throw NotSupported("RotationOffsetX"); } }
        public double RotationOffsetY { get { return 0; } set { throw NotSupported("RotationOffsetY"); } }
        public double OrientationZ { get { return 0; } set { throw NotSupported("OrientationZ"); } }
    }

    public partial class TrackMotionKeyframe : BaseTrackMotionKeyframe
    {
        internal TrackMotionKeyframe(ulong h) : base(h) { }
        public TrackMotionKeyframe() { throw new NotSupportedException("new TrackMotionKeyframe() is not supported by Reno: use TrackMotion.InsertMotionKeyframe."); }
        public double PositionZ { get { return 0; } set { throw NotSupported("PositionZ"); } }
        public double Depth { get { return 0; } set { throw NotSupported("Depth"); } }
        public double RotationX { get { return 0; } set { throw NotSupported("RotationX"); } }
        public double RotationY { get { return 0; } set { throw NotSupported("RotationY"); } }
        public double RotationOffsetZ { get { return 0; } set { throw NotSupported("RotationOffsetZ"); } }
        public double OrientationX { get { return 0; } set { throw NotSupported("OrientationX"); } }
        public double OrientationY { get { return 0; } set { throw NotSupported("OrientationY"); } }
    }

    public partial class TrackMotionKeyframeList : BaseList<TrackMotionKeyframe>
    {
        VideoTrack track;
        internal TrackMotionKeyframeList(VideoTrack t) { track = t; }
        internal override List<TrackMotionKeyframe> Items() { return Reno.WrapAll<TrackMotionKeyframe>(track.L(Abi.RL_MOTION, "MotionKeyframes")); }
        internal override bool RemoveItem(TrackMotionKeyframe k)
        {
            track.Op(Abi.RO_REMOVE_MOTION_KEY, new long[] { (long)k.H }, null, null, "MotionKeyframes.Remove");
            return true;
        }
    }

    // ------------------------------------------------------------------------------------------------ buses
    public abstract partial class BusTrack : RenoObject, IEffectOwner, IEnvelopeOwner
    {
        internal bool special;      // the master or the video output
        internal BusTrack(ulong h) : base(h) { }
        public abstract MediaType MediaType { get; }
        public int Index { get { return (int)GI(Abi.RP_INDEX); } }
        public string Name { get { return GS(Abi.RP_NAME); } }
        public string Description { get { return Name; } set { throw NotSupported("Description"); } }
        public Project Project { get { return Reno.App.Project; } }
        public Envelopes Envelopes { get { return new Envelopes(this); } }
        public Effects Effects { get { return new Effects(this); } }
        public abstract bool IsAudio();
        public abstract bool IsVideo();
        public virtual bool IsMaster() { return special || Index < 0; }
        public virtual bool CanAddEnvelope(EnvelopeType type) { return IsAudio() ? (type == EnvelopeType.Volume || type == EnvelopeType.Mute || (!special && type == EnvelopeType.Pan)) : (type == EnvelopeType.FadeToColor || type == EnvelopeType.Mute); }
    }

    public partial class AudioBusTrack : BusTrack
    {
        internal AudioBusTrack(ulong h) : base(h) { }
        public AudioBusTrack() : base(0) { throw new NotSupportedException("new AudioBusTrack() is not supported by Reno (add buses in the Mixing Console)."); }
        public AudioBusTrack(Project project) : this() { }
        public override MediaType MediaType { get { return MediaType.Audio; } }
        public override bool IsAudio() { return true; }
        public override bool IsVideo() { return false; }
        public bool Solo { get { return GB(Abi.RP_SOLO); } set { SB(Abi.RP_SOLO, value); } }
        public bool Mute { get { return GB(Abi.RP_MUTE); } set { SB(Abi.RP_MUTE, value); } }
    }

    public partial class VideoBusTrack : BusTrack
    {
        internal VideoBusTrack(ulong h) : base(h) { special = true; }
        public override MediaType MediaType { get { return MediaType.Video; } }
        public override bool IsAudio() { return false; }
        public override bool IsVideo() { return true; }
        public bool Bypass { get { return false; } set { throw NotSupported("Bypass"); } }
        public VideoColor TopFadeColor { get { throw NotSupported("TopFadeColor"); } set { throw NotSupported("TopFadeColor"); } }
        public VideoColor BottomFadeColor { get { throw NotSupported("BottomFadeColor"); } set { throw NotSupported("BottomFadeColor"); } }
    }

    public partial class BusTracks : BaseList<BusTrack>
    {
        internal BusTracks(Project p) { }
        internal override List<BusTrack> Items() { return Reno.WrapAll<BusTrack>(Reno.List(1, Abi.RL_BUSES, "BusTracks")); }
    }

    // ------------------------------------------------------------------------------------------------ colours
    public partial class VideoColor
    {
        public float Red { get; set; }
        public float Green { get; set; }
        public float Blue { get; set; }
        public float Alpha { get; set; }
        static byte B8(float f) { return (byte)Math.Max(0, Math.Min(255, Math.Round(f * 255))); }
        public byte R { get { return B8(Red); } set { Red = value / 255f; } }
        public byte G { get { return B8(Green); } set { Green = value / 255f; } }
        public byte B { get { return B8(Blue); } set { Blue = value / 255f; } }
        public byte A { get { return B8(Alpha); } set { Alpha = value / 255f; } }
        public VideoColor() { Alpha = 1; }
        public VideoColor(float r, float g, float b, float a) { Red = r; Green = g; Blue = b; Alpha = a; }
        public VideoColor(float r, float g, float b) : this(r, g, b, 1f) { }
        public VideoColor(uint argb) { A = (byte)(argb >> 24); R = (byte)(argb >> 16); G = (byte)(argb >> 8); B = (byte)argb; }
        public VideoColor(byte r, byte g, byte b, byte a) { R = r; G = g; B = b; A = a; }
        public VideoColor(byte r, byte g, byte b) : this(r, g, b, (byte)255) { }
        public static VideoColor FromByteValues(byte r, byte g, byte b, byte a) { return new VideoColor(r, g, b, a); }
        public static VideoColor FromByteValues(byte r, byte g, byte b) { return new VideoColor(r, g, b); }
        public uint ToArgb() { return ((uint)A << 24) | ((uint)R << 16) | ((uint)G << 8) | B; }
        public override bool Equals(object obj) { VideoColor o = obj as VideoColor; return !ReferenceEquals(o, null) && o.ToArgb() == ToArgb(); }
        public override int GetHashCode() { return (int)ToArgb(); }
        public static bool operator ==(VideoColor a, VideoColor b) { return ReferenceEquals(a, b) || (!ReferenceEquals(a, null) && a.Equals(b)); }
        public static bool operator !=(VideoColor a, VideoColor b) { return !(a == b); }
        public override string ToString() { return string.Format(CultureInfo.InvariantCulture, "R={0} G={1} B={2} A={3}", R, G, B, A); }
    }

    // ------------------------------------------------------------------------------------------------ rendering (not in Reno's scripts)
    public partial class Renderer { internal Renderer() { } public string FileTypeName { get { return ""; } } public string Name { get { return ""; } } public RenderTemplates Templates { get { return new RenderTemplates(); } } }
    public partial class Renderers : BaseList<Renderer> { internal Renderers() { } internal override List<Renderer> Items() { return new List<Renderer>(); } }
    public partial class RenderTemplate { internal RenderTemplate() { } public string Name { get { return ""; } } }
    public partial class RenderTemplates : BaseList<RenderTemplate> { internal RenderTemplates() { } internal override List<RenderTemplate> Items() { return new List<RenderTemplate>(); } }
    public partial class RenderArgs
    {
        public RenderArgs() { throw new NotSupportedException("RenderArgs (rendering from a script) is not supported by Reno: use File > Render As."); }
    }

    // ------------------------------------------------------------------------------------------------ smaller parts
    // VEGAS's CustomData: kept while the script runs, not saved in the project file.
    public partial class CustomDataContainer
    {
        static Dictionary<ulong, CustomDataContainer> all = new Dictionary<ulong, CustomDataContainer>();
        Dictionary<Guid, object> data = new Dictionary<Guid, object>();
        internal CustomDataContainer() { }
        internal static void Reset() { all = new Dictionary<ulong, CustomDataContainer>(); }
        internal static CustomDataContainer Of(ulong owner)
        {
            CustomDataContainer c;
            if (!all.TryGetValue(owner, out c)) { c = new CustomDataContainer(); if (owner != 0) all[owner] = c; }
            return c;
        }
        public object GetObject(Guid dataID) { object o; return data.TryGetValue(dataID, out o) ? o : null; }
        public void SetObject(Guid dataID, object obj) { data[dataID] = obj; }
        public byte[] GetBytes(Guid dataID) { return GetObject(dataID) as byte[]; }
        public void SetBytes(Guid dataID, byte[] data) { SetObject(dataID, data == null ? null : (byte[])data.Clone()); }
        public void SetBytes(Guid dataID, byte[] data, int index, int count)
        {
            byte[] b = new byte[count];
            Array.Copy(data, index, b, 0, count);
            SetObject(dataID, b);
        }
        public bool GetStream(Guid dataID, Stream dataStream)
        {
            byte[] b = GetBytes(dataID);
            if (b == null) return false;
            dataStream.Write(b, 0, b.Length);
            return true;
        }
        public void SetStream(Guid dataID, Stream dataStream, int cbData)
        {
            byte[] b = new byte[cbData];
            int n = 0;
            while (n < cbData) { int r = dataStream.Read(b, n, cbData - n); if (r <= 0) break; n += r; }
            SetObject(dataID, b);
        }
        public void DeleteData(Guid dataID) { data.Remove(dataID); }
    }

    // VEGAS's file and folder dialogs for scripts (Vegas.FileUtilities), with Windows Forms' own.
    public partial interface IFileUtilities
    {
        void SelectFileDlg(IntPtr hwndParent, string title, string filter, string defaultFile, out string file, ref int filterIndex);
        void CreateFileDlg(IntPtr hwndParent, string title, string filter, string defaultFile, out string file, ref int filterIndex);
        void SelectDirectoryDlg(IntPtr hwndParent, string dlgTitle, string defaultDir, bool useNetworkScope, out string result);
        uint GetDiskFreeSpace(string filePath, uint divisor);
        uint GetDiskTotalSpace(string filePath, uint divisor);
    }

    internal class FileUtilities : IFileUtilities
    {
        // (VEGAS's filters separate with '|' as Windows Forms' do; a trailing '|' is dropped)
        static string Filter(string f) { f = f ?? ""; while (f.EndsWith("|")) f = f.Substring(0, f.Length - 1); return f.Length == 0 ? "All files (*.*)|*.*" : f; }
        static void Ask(FileDialog d, string title, string filter, string defaultFile, out string file, ref int filterIndex)
        {
            d.Title = title;
            d.Filter = Filter(filter);
            d.FilterIndex = Math.Max(1, filterIndex);
            if (!string.IsNullOrEmpty(defaultFile))
            {
                if (Directory.Exists(defaultFile)) d.InitialDirectory = defaultFile;
                else { d.FileName = Path.GetFileName(defaultFile); string dir = Path.GetDirectoryName(defaultFile); if (!string.IsNullOrEmpty(dir)) d.InitialDirectory = dir; }
            }
            file = null;
            if (d.ShowDialog(Reno.App.MainWindow) == DialogResult.OK) { file = d.FileName; filterIndex = d.FilterIndex; }
        }
        public void SelectFileDlg(IntPtr hwndParent, string title, string filter, string defaultFile, out string file, ref int filterIndex)
        {
            Ask(new OpenFileDialog(), title, filter, defaultFile, out file, ref filterIndex);
        }
        public void CreateFileDlg(IntPtr hwndParent, string title, string filter, string defaultFile, out string file, ref int filterIndex)
        {
            Ask(new SaveFileDialog(), title, filter, defaultFile, out file, ref filterIndex);
        }
        public void SelectDirectoryDlg(IntPtr hwndParent, string dlgTitle, string defaultDir, bool useNetworkScope, out string result)
        {
            FolderBrowserDialog d = new FolderBrowserDialog();
            d.Description = dlgTitle;
            if (!string.IsNullOrEmpty(defaultDir) && Directory.Exists(defaultDir)) d.SelectedPath = defaultDir;
            result = d.ShowDialog(Reno.App.MainWindow) == DialogResult.OK ? d.SelectedPath : null;
        }
        static DriveInfo Drive(string filePath) { return new DriveInfo(Path.GetPathRoot(Path.GetFullPath(filePath))); }
        public uint GetDiskFreeSpace(string filePath, uint divisor) { return (uint)Math.Min(uint.MaxValue, Drive(filePath).AvailableFreeSpace / Math.Max(1u, divisor)); }
        public uint GetDiskTotalSpace(string filePath, uint divisor) { return (uint)Math.Min(uint.MaxValue, Drive(filePath).TotalSize / Math.Max(1u, divisor)); }
    }

    // Markers inside media files (VEGAS's Media.Markers): Reno does not read them, so the lists are empty.
    public partial class MediaMarker : Marker { public MediaMarker() { } }
    public partial class MediaRegion : Region { public MediaRegion() { } }
    public partial class MediaCommandMarker : CommandMarker { public MediaCommandMarker() { } }
    public partial class BaseMediaMarkerList<T> : BaseMarkerList<T> where T : Marker
    {
        public BaseMediaMarkerList() : base(-1, -1) { }
        internal override List<T> Items() { return new List<T>(); }
        internal override void AddItem(T m) { throw new NotSupportedException(GetType().Name + ".Add (markers in media files) is not supported by Reno."); }
    }
    public partial class MediaMarkerList : BaseMediaMarkerList<MediaMarker> { }
    public partial class MediaRegionList : BaseMediaMarkerList<MediaRegion> { }
    public partial class MediaCommandMarkerList : BaseMediaMarkerList<MediaCommandMarker> { }

    // Track groups (VEGAS's TrackGroups): Reno has none.
    public partial class TrackGroup : BaseList<Track>
    {
        internal TrackGroup() { }
        internal override List<Track> Items() { return new List<Track>(); }
    }
    public partial class TrackGroups : BaseList<TrackGroup>
    {
        internal TrackGroups() { }
        internal override List<TrackGroup> Items() { return new List<TrackGroup>(); }
    }

    // A media bin (VEGAS's Project Media bins): Reno has none.
    public partial class MediaBin : BaseList<IMediaBinNode>
    {
        internal MediaBin() { }
        internal override List<IMediaBinNode> Items() { throw new NotSupportedException("MediaBin is not supported by Reno."); }
    }

    // The preview's video settings and the audio CD settings: not in Reno (their members are in ScriptPortal.Vegas.Unsupported.cs).
    public partial class PreviewVideoProperties : VideoProperties { internal PreviewVideoProperties() { } }
    public partial class AudioCDProperties { internal AudioCDProperties() { } }

    // A track's shadow and glow (VEGAS's 2D shadow and glow of track motion): Reno has none, so the lists are empty.
    public partial class TrackShadowKeyframe : BaseTrackMotionKeyframe { internal TrackShadowKeyframe() { } }
    public partial class TrackGlowKeyframe : BaseTrackMotionKeyframe { internal TrackGlowKeyframe() { } }
    public partial class BaseTrackMotionKeyframeList<T> : BaseList<T> where T : BaseTrackMotionKeyframe
    {
        internal BaseTrackMotionKeyframeList() { }
        internal override List<T> Items() { return new List<T>(); }
    }
    public partial class TrackShadowKeyframeList : BaseTrackMotionKeyframeList<TrackShadowKeyframe> { internal TrackShadowKeyframeList() { } }
    public partial class TrackGlowKeyframeList : BaseTrackMotionKeyframeList<TrackGlowKeyframe> { internal TrackGlowKeyframeList() { } }

    // ------------------------------------------------------------------------------------------------ VEGAS's enumerations
    // (every public enumeration of VEGAS Pro 18's ScriptPortal.Vegas, with its names and numbers)
    public enum ScriptEngineType
    {
        Unknown = 0,
        CSharp = 1,
        JScript = 2,
        VBScript = 3,
        Precompiled = 4,
        JScript_VSA = 5,
        VBScript_VSA = 6
    }

    public enum RenderStatus
    {
        Unknown = -1,
        Queued = 0,
        NothingToDo = 1,
        Rendering = 2,
        Stitching = 3,
        Complete = 4,
        Canceled = 5,
        Failed = 6,
        Quit = 7
    }

    public enum StreamType
    {
        Unknown = 0,
        Audio = 1,
        Video = 2,
        Midi = 4,
        Text = 8,
        VegPrj = 16,
        VegEDL = 32,
        DV = 8
    }

    public enum PlugInType
    {
        None = 0,
        Root = 1,
        AudioAll = 2,
        AudioSf = 3,
        Audio3rdParty = 4,
        AudioAutomatable = 5,
        AudioTrackOptimized = 6,
        AudioPackage = 7,
        VideoRoot = 8,
        VideoFilter = 9,
        VideoFilterPackage = 10,
        VideoTransition = 11,
        VideoTransitionPackage = 12,
        VideoGenerator = 13,
        VideoGeneratorPackage = 14,
        Video2to1 = 15
    }

    public enum OFXEffectType
    {
        Unknown = 0,
        Compositor = 1,
        General = 2,
        Filter = 3,
        Generator = 4,
        Transition = 5,
        Retimer = 6,
        Paint = 7
    }

    public enum OFXParameterType
    {
        Unknown = 0,
        Page = 1,
        Group = 2,
        PushButton = 3,
        Boolean = 4,
        Choice = 5,
        RGB = 6,
        RGBA = 7,
        Integer = 8,
        Integer2D = 9,
        Integer3D = 10,
        Double = 11,
        Double2D = 12,
        Double3D = 13,
        String = 14,
        Custom = 15
    }

    public enum OFXInterpolationType
    {
        Unknown = 0,
        Linear = 1,
        Fast = 2,
        Slow = 3,
        Smooth = 4,
        Sharp = 5,
        Hold = 6,
        Manual = 7,
        Split = 8
    }

    public enum OFXControlPointType
    {
        Unknown = 0,
        Prev = 1,
        Next = 2
    }

    public enum MarkerType
    {
        Unknown = 0,
        Marker = 1,
        Region = 2,
        CDMarker = 3,
        CDRegion = 4,
        CommandMarker = 5,
        MediaMarker = 6,
        MediaRegion = 7,
        MediaCommandMarker = 8
    }

    public enum MediaType
    {
        Unknown = 0,
        Stream = 1,
        Audio = 2,
        Video = 3,
        Midi = 4,
        Text = 5,
        Max = 6
    }

    public enum MediaBinNodeType
    {
        Unknown = 0,
        Bin = 1,
        MediaRef = 2
    }

    public enum MediaFileType
    {
        Unknown = 0,
        MixedEssence = 1,
        VideoEssence = 2,
        AudioEssence = 3,
        VideoProxy = 4,
        AudioProxy = 5,
        VegasMetadata = 6,
        OtherMetadata = 7
    }

    public enum VideoPropertiesType
    {
        None = 0,
        Project = 1,
        Preview = 2,
        Stream = 3,
        Event = 4
    }

    public enum VMKeyframeType
    {
        None = 1,
        Position = 0,
        Mask = 1,
        Count = 2
    }

    public enum VideoResampleMode
    {
        Smart = 0,
        Force = 1,
        Disable = 2
    }

    public enum ChannelRemapping
    {
        None = 1000,
        DisableLeft = 1001,
        DisableRight = 1002,
        MuteLeft = 1003,
        MuteRight = 1004,
        Mono = 1005,
        Swap = 1006
    }

    public enum TimeStretchPitchShift
    {
        Classic = 0,
        AcidStyle = 1,
        Elastique = 2,
        None = 3
    }

    public enum ClassicStretchAttributes
    {
        A01 = 0,
        A02 = 1,
        A03 = 2,
        A04 = 3,
        A05 = 4,
        A06 = 5,
        A07 = 6,
        A08 = 7,
        A09 = 8,
        A10 = 9,
        A11 = 10,
        A12 = 11,
        A13 = 12,
        A14 = 13,
        A15 = 14,
        A16 = 15,
        A17 = 16,
        A18 = 17,
        A19 = 18
    }

    public enum ElastiqueStretchAttributes
    {
        Pro = 0,
        Efficient = 1,
        Soloist_Monophonic = 2,
        Soloist_Speech = 3
    }

    public enum NormalizeFlags
    {
        Norm = 1,
        Gain = 2,
        None = 0
    }

    public enum BusType
    {
        Unknown = 0,
        SubBus = 1,
        FXBus = 2,
        VideoBus = 3,
        All = 4
    }

    public enum CompositeMode
    {
        Invalid = 0,
        Custom = 1,
        SrcAlpha = 2,
        SrcAlpha3D = 3,
        Add = 4,
        Subtract = 5,
        Multiply = 6,
        Cut = 7,
        Screen = 8,
        Overlay = 9,
        HardLight = 10,
        Dodge = 11,
        Burn = 12,
        Darken = 13,
        Lighten = 14,
        Difference = 15,
        DifferenceSquared = 16
    }

    public enum VideoKeyframeType
    {
        Linear = 0,
        Hold = 1,
        Slow = 2,
        Fast = 3,
        Smooth = 4,
        Sharp = 5
    }

    public enum TrackMotionType
    {
        Motion = 0,
        Shadow = 1,
        Glow = 2,
        ParentMotion = 3,
        ParentShadow = 4,
        ParentGlow = 5,
        Invalid = 6
    }

    public enum AutomationControlAutomationState
    {
        Off = 0,
        Reading = 1,
        Armed = 2,
        Writing = 3
    }

    public enum AutomationWritingMode
    {
        Touch = 0,
        Latch = 1
    }

    public enum AutomationReadWriteState
    {
        Off = 0,
        Read = 1,
        Write = 2
    }

    public enum OwnerType
    {
        None = 0,
        Event = 1,
        Track = 2,
        TrackMask = 3,
        Project = 4,
        Surface = 5,
        Media = 6,
        EventFadeIn = 7,
        EventFadeOut = 8,
        Track2To1 = 9,
        ParentTrack = 10,
        ParentTrack2To1 = 11
    }

    public enum VideoFieldOrder
    {
        Unknown = -1,
        ProgressiveScan = 0,
        UpperFieldFirst = 1,
        LowerFieldFirst = 2
    }

    public enum VideoOutputRotation
    {
        None = 0,
        QuarterTurnClockwise = 1,
        Inverted = 2,
        QuarterTurnCounterclockwise = 3
    }

    public enum PixelFormat
    {
        Int8Bit = 0,
        Float32Bit = 1,
        Float32BitFullRange = 2
    }

    public enum T360Output
    {
        On = 0,
        Off = 1
    }

    public enum HDRMode
    {
        HDR10 = 0,
        HLG = 1,
        Off = 2
    }

    public enum Stereo3DOutputMode
    {
        Off = 0,
        SideBySideHalf = 1,
        SideBySideFull = 2,
        TopBottomHalf = 3,
        TopBottomFull = 4,
        AnaglyphicRedCyan = 5,
        AnaglyphicAmberBlue = 6,
        AnaglyphicGreenMagenta = 7,
        LineAlternate = 8,
        Checkerboard = 9,
        LeftOnly = 10,
        RightOnly = 11,
        Blend = 12,
        Difference = 13,
        LeftAndRight = 14
    }

    public enum Stereo3DInputMode
    {
        Off = 0,
        PairWithNextStream = 1,
        SideBySideHalf = 2,
        SideBySideFull = 3,
        TopBottomHalf = 4,
        TopBottomFull = 5,
        LineAlternate = 6
    }

    public enum RulerFormat
    {
        Unknown = 0,
        Samples = 1,
        Time = 2,
        Seconds = 3,
        TimeAndFrames = 4,
        AbsoluteFrames = 5,
        MeasuresAndBeats = 6,
        FeetAndFrames16mm = 7,
        FeetAndFrames35mm = 8,
        SmpteFilmSyncIVTC = 9,
        SmpteFilmSync = 10,
        SmpteEBU = 11,
        SmpteNonDrop = 12,
        SmpteDrop = 13,
        Smpte30 = 14,
        AudioCDTime = 15,
        Nanoseconds = 16,
        Microseconds = 17
    }

    public enum CurveType
    {
        Invalid = -10,
        Sharp = -4,
        Slow = -2,
        None = 0,
        Linear = 1,
        Fast = 2,
        Smooth = 4
    }

    public enum BeatValue
    {
        Invalid = 0,
        Whole = 1,
        Half = 2,
        Quarter = 4,
        Eighth = 8,
        Sixteenth = 16,
        ThirtySecond = 32
    }

    public enum AudioBusMode
    {
        Internal_00 = 0,
        Stereo = 1,
        Surround = 2,
        Internal03 = 3,
        Internal04 = 4,
        Internal05 = 5,
        Internal06 = 6,
        Undefined = 7
    }

    public enum AudioResampleQuality
    {
        Preview = 0,
        Good = 1,
        Best = 2
    }

    public enum VideoRenderQuality
    {
        Draft = 1,
        Preview = 2,
        Good = 3,
        Best = 4
    }

    public enum VideoDeinterlaceMethod
    {
        None = 0,
        BlendFields = 1,
        InterpolateFields = 2
    }

    public enum MotionBlurType
    {
        Unknown = -1,
        Gaussian = 0,
        Pyramid = 1,
        Box = 2,
        AsymmetricGaussian = 3,
        AsymmetricPyramid = 4,
        AsymmetricBox = 5
    }

    public enum ImageFileFormat
    {
        Unknown = 0,
        PNG = 1,
        JPEG = 2
    }

    public enum VideoAlphaType
    {
        Undefined = -1,
        None = 0,
        Straight = 1,
        Premultiplied = 2,
        PremultipliedDirty = 3
    }

    public enum PanType
    {
        Add = 0,
        ConstantPower = 1,
        Balance = 2,
        Notch3Db = 3,
        Notch6Db = 4,
        Film = 5,
        Unknown = 666
    }

    public enum LowPassFilterQuality
    {
        Preview = 0,
        Good = 1,
        Best = 2
    }

    public enum VideoPreviewSize
    {
        Full = 0,
        Auto = 1,
        PreserveAspect_NOT_USED = 2,
        Half = 3,
        Quarter = 4
    }

    public enum ProjectSaveType
    {
        Unknown = -1,
        Project_Default = 0,
        Project_Reference = 1,
        Project_AAF = 2,
        Project_AvidAAF = 3,
        Project_Archive = 4,
        Render_Wave = 5,
        Render_Vfw = 6
    }

    public enum EnvelopeType
    {
        Unknown = -1,
        Volume = 0,
        Pan = 1,
        BusA = 3,
        BusB = 4,
        BusC = 5,
        BusD = 6,
        BusE = 7,
        BusF = 8,
        BusG = 9,
        BusH = 10,
        BusI = 11,
        BusJ = 12,
        BusK = 13,
        BusL = 14,
        BusM = 15,
        BusN = 16,
        BusO = 17,
        BusP = 18,
        BusQ = 19,
        BusR = 20,
        BusS = 21,
        BusT = 22,
        BusU = 23,
        BusV = 24,
        BusW = 25,
        BusX = 26,
        BusY = 27,
        BusZ = 28,
        Composite = 29,
        FadeToColor = 30,
        Mute = 31,
        MotionBlurLength = 32,
        VideoSupersampling = 33,
        FX1 = 64,
        FX2 = 65,
        FX3 = 66,
        FX4 = 67,
        FX5 = 68,
        FX6 = 69,
        FX7 = 70,
        FX8 = 71,
        FX9 = 72,
        FX10 = 73,
        FX11 = 74,
        FX12 = 75,
        FX13 = 76,
        FX14 = 77,
        FX15 = 78,
        FX16 = 79,
        FX17 = 80,
        FX18 = 81,
        FX19 = 82,
        FX20 = 83,
        FX21 = 84,
        FX22 = 85,
        FX23 = 86,
        FX24 = 87,
        FX25 = 88,
        FX26 = 89,
        FX27 = 90,
        FX28 = 91,
        FX29 = 92,
        FX30 = 93,
        FX31 = 94,
        FX32 = 95,
        PanY = 96,
        PanSmoothness = 97,
        PanCenter = 101,
        TransitionProgress = 200,
        Fade = 201,
        Velocity = 202,
        AutomationMin = 301,
        AutomationMax = 400
    }

    // ------------------------------------------------------------------------------------------------ running a script
    // Runs a script: Reno starts the .NET Framework in its own process and calls RenoHost.Run with the address of its
    // function table; Run compiles the script, finds its entry point, runs it and reports back.
    public static partial class RenoHost
    {
        static bool resolving, styled;

        // The entry point Reno calls (ICLRRuntimeHost::ExecuteInDefaultAppDomain): "api=<hex address>".
        public static int Run(string arg)
        {
            IntPtr api = IntPtr.Zero;
            foreach (string part in (arg ?? "").Split(';'))
                if (part.StartsWith("api=", StringComparison.Ordinal))
                    api = new IntPtr(long.Parse(part.Substring(4), NumberStyles.HexNumber, CultureInfo.InvariantCulture));
            try
            {
                Reno.Init(api);
            }
            catch (Exception)
            {
                return 2;     // (no Reno to tell)
            }
            // the script's Windows Forms dialogs look as in VEGAS (Windows' own control styles), once
            if (!styled)
            {
                styled = true;
                try { Application.EnableVisualStyles(); } catch (Exception) { }
            }
            try
            {
                RunFile(Script.File);
                Reno.Finish(false, "");
                return 0;
            }
            catch (Exception e)
            {
                Reno.Finish(true, Describe(e));
                return 1;
            }
        }

        // For tests: connects to Reno's table of functions without running anything.
        public static void Connect(IntPtr api) { Reno.Init(api); }
        public static void Finish(bool failed, string message) { Reno.Finish(failed, message); }

        // What went wrong, for the user: the script's own exception (not the reflection wrapping around it).
        public static string Describe(Exception e)
        {
            while (e is TargetInvocationException && e.InnerException != null) e = e.InnerException;
            return e.GetType().Name == "ScriptCompileException" ? e.Message : e.GetType().FullName + ": " + e.Message + "\n\n" + e.StackTrace;
        }

        static void HookResolve()
        {
            if (resolving) return;
            resolving = true;
            // scripts compiled against VEGAS's own assembly (or Sony's older one) use this one
            AppDomain.CurrentDomain.AssemblyResolve += delegate(object sender, ResolveEventArgs args)
            {
                string n = new AssemblyName(args.Name).Name;
                if (n == "ScriptPortal.Vegas" || n == "Sony.Vegas" || n == "SonicFoundry.Vegas") return typeof(Vegas).Assembly;
                return null;
            };
        }

        // Scripts written for Sony's VEGAS (up to 13) name the API Sony.Vegas: the same classes.
        public static string Modernise(string source)
        {
            return source.Replace("Sony.Vegas", "ScriptPortal.Vegas").Replace("SonicFoundry.Vegas", "ScriptPortal.Vegas");
        }

        public static void RunFile(string path)
        {
            HookResolve();
            if (string.IsNullOrEmpty(path) || !File.Exists(path)) throw new FileNotFoundException("The script file was not found: " + path);
            string ext = Path.GetExtension(path).ToLowerInvariant();
            Assembly asm;
            if (ext == ".dll") asm = Assembly.LoadFrom(path);
            else asm = Compile(path, ext);
            RunAssembly(asm);
        }

        internal class ScriptCompileException : Exception
        {
            public ScriptCompileException(string message) : base(message) { }
        }

        static Assembly Compile(string path, string ext)
        {
#if RENO_NO_CODEDOM
            throw new NotSupportedException("compiling scripts needs the .NET Framework");
#else
            string language = ext == ".cs" ? "CSharp" : ext == ".vb" ? "VisualBasic" : (ext == ".js" || ext == ".jscript") ? "JScript" : null;
            if (language == null) throw new NotSupportedException("Reno runs scripts written in C# (.cs), JScript (.js) and Visual Basic (.vb), and compiled scripts (.dll).");
            string source = Modernise(File.ReadAllText(path));
            // JScript and VB scripts use a global "Vegas"; it is given its value before the script runs
            if (language == "JScript") source = source + "\r\nvar Vegas : ScriptPortal.Vegas.Vegas;\r\n";
            if (language == "VisualBasic") source = source + "\r\nPublic Module RenoVegasGlobal\r\n    Public Vegas As ScriptPortal.Vegas.Vegas\r\nEnd Module\r\n";
            System.CodeDom.Compiler.CodeDomProvider provider = System.CodeDom.Compiler.CodeDomProvider.CreateProvider(language);
            System.CodeDom.Compiler.CompilerParameters p = new System.CodeDom.Compiler.CompilerParameters();
            p.GenerateInMemory = true;
            p.GenerateExecutable = false;
            p.IncludeDebugInformation = false;
            foreach (string r in new string[] { "System.dll", "System.Core.dll", "System.Data.dll", "System.Drawing.dll", "System.Windows.Forms.dll", "System.Xml.dll" })
                p.ReferencedAssemblies.Add(r);
            if (language == "VisualBasic") p.ReferencedAssemblies.Add("Microsoft.VisualBasic.dll");
            if (language == "JScript") p.ReferencedAssemblies.Add("Microsoft.JScript.dll");
            p.ReferencedAssemblies.Add(typeof(Vegas).Assembly.Location);
            // VEGAS's "<script>.config" (or "<script>.xml"): <ScriptSettings><AssemblyReference>name.dll</AssemblyReference>...
            foreach (string cfg in new string[] { path + ".config", Path.ChangeExtension(path, ".xml") })
            {
                if (!File.Exists(cfg)) continue;
                try
                {
                    System.Xml.XmlDocument doc = new System.Xml.XmlDocument();
                    doc.Load(cfg);
                    foreach (System.Xml.XmlNode n in doc.GetElementsByTagName("AssemblyReference"))
                    {
                        string r = n.InnerText.Trim();
                        if (r.Length == 0) continue;
                        string local = Path.Combine(Path.GetDirectoryName(path), r);
                        p.ReferencedAssemblies.Add(File.Exists(local) ? local : r);
                    }
                }
                catch (System.Xml.XmlException) { }
            }
            System.CodeDom.Compiler.CompilerResults res = provider.CompileAssemblyFromSource(p, source);
            if (res.Errors.HasErrors)
            {
                StringBuilder b = new StringBuilder("The script could not be compiled:\n");
                int n = 0;
                foreach (System.CodeDom.Compiler.CompilerError err in res.Errors)
                {
                    if (err.IsWarning) continue;
                    if (++n > 12) { b.Append("..."); break; }
                    b.AppendFormat(CultureInfo.InvariantCulture, "\nline {0}: {1} ({2})", err.Line, err.ErrorText, err.ErrorNumber);
                }
                throw new ScriptCompileException(b.ToString());
            }
            return res.CompiledAssembly;
#endif
        }

        // Runs a compiled script, as VEGAS looks for its entry point: a class with FromVegas(Vegas) (C#, VB, JScript
        // classes), else JScript's global code, else a VB module's Main().
        public static void RunAssembly(Assembly asm)
        {
            HookResolve();
            Type[] types;
            try { types = asm.GetTypes(); }
            catch (ReflectionTypeLoadException e) { types = e.Types; }
            Type entry = null;
            MethodInfo from = null;
            foreach (Type t in types)
            {
                if (t == null) continue;
                MethodInfo m = t.GetMethod("FromVegas", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static, null,
                                           new Type[] { typeof(Vegas) }, null);
                if (m == null) continue;
                if (entry == null || t.Name == "EntryPoint") { entry = t; from = m; }
            }
            SetGlobals(types);
            if (from != null)
            {
                object target = from.IsStatic ? null : Activator.CreateInstance(entry, true);
                from.Invoke(target, new object[] { Reno.App });
                return;
            }
            // JScript .NET: the script's top-level code is the "Global Code" method of the global scope class
            foreach (Type t in types)
            {
                if (t == null || t.BaseType == null || t.BaseType.FullName != "Microsoft.JScript.GlobalScope") continue;
                MethodInfo g = t.GetMethod("Global Code", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);
                if (g == null) continue;
                object scope = MakeJScriptScope(t);
                SetGlobal(t, scope);
                g.Invoke(scope, null);
                return;
            }
            foreach (Type t in types)
            {
                if (t == null) continue;
                MethodInfo main = t.GetMethod("Main", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static, null, Type.EmptyTypes, null);
                if (main != null) { main.Invoke(null, null); return; }
            }
            throw new ScriptCompileException("The script has no entry point: Reno (like VEGAS) runs a class with a method FromVegas(Vegas vegas), " +
                                             "JScript's top-level code, or a VB module's Sub Main().");
        }

        // VB's RenoVegasGlobal.Vegas (and any static "Vegas" field of the right type)
        static void SetGlobals(Type[] types)
        {
            foreach (Type t in types)
            {
                if (t == null) continue;
                SetGlobal(t, null);
            }
        }

        static void SetGlobal(Type t, object instance)
        {
            FieldInfo f = t.GetField("Vegas", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance);
            if (f == null || f.FieldType != typeof(Vegas)) return;
            if (f.IsStatic) f.SetValue(null, Reno.App);
            else if (instance != null) f.SetValue(instance, Reno.App);
        }

        static object MakeJScriptScope(Type t)
        {
            // new <GlobalScope class>(VsaEngine.CreateEngine().GetGlobalScope()) - what JScript .NET's own host does
            Assembly js = t.BaseType.Assembly;
            Type engineType = js.GetType("Microsoft.JScript.Vsa.VsaEngine");
            object engine = engineType.GetMethod("CreateEngine", BindingFlags.Public | BindingFlags.Static).Invoke(null, null);
            object global = engineType.GetMethod("GetGlobalScope").Invoke(engine, null);
            foreach (ConstructorInfo c in t.GetConstructors(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance))
                if (c.GetParameters().Length == 1) return c.Invoke(new object[] { global });
            return Activator.CreateInstance(t, true);
        }
    }
}
