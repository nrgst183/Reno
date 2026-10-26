// Export to Reno: run inside VEGAS Pro (Tools > Scripting > Run Script...) with a project open.
// Writes the project as a Reno project file (.reno) next to the .veg, using VEGAS's own reading of it.
// tests/vegas/compare.cpp checks Reno's .veg reader against this output.
// The file is Reno's Project::toJson (src/engine/project.cpp) plus "vegas" entries with VEGAS's raw values, which Reno
// ignores. Where Reno has no equivalent (changing envelopes, track motion it cannot draw ...), the value is made the way
// Reno's .veg importer makes it, so the two can be compared field by field.
// C# 5 against ScriptPortal.Vegas. Members that are not on every class or version are read by reflection; a missing one
// leaves Reno's default and is listed under "problems" at the end of the file.

using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text;
using System.Windows.Forms;
using ScriptPortal.Vegas;

public class EntryPoint
{
    public void FromVegas(Vegas vegas)
    {
        try
        {
            RenoExporter ex = new RenoExporter(vegas);
            string written = ex.Run();
            if (written != null)
                MessageBox.Show("Written:\n" + written + (ex.Problems.Count > 0 ? "\n\nNot everything could be read; see the \"problems\" list at the end of the file." : ""),
                                "Export to Reno");
        }
        catch (Exception e)
        {
            MessageBox.Show("Export to Reno stopped:\n" + e.ToString(), "Export to Reno");
        }
    }
}

// ---------------------------------------------------------------------------------------------- JSON
// A small JSON writer: objects keep the order their keys were added in.
public class JObj
{
    public List<string> Keys = new List<string>();
    public List<object> Values = new List<object>();
    public JObj Add(string key, object value) { Keys.Add(key); Values.Add(value); return this; }
    public object Get(string key) { int i = Keys.IndexOf(key); return i < 0 ? null : Values[i]; }
    public void Set(string key, object value) { int i = Keys.IndexOf(key); if (i < 0) Add(key, value); else Values[i] = value; }
}

public class JArr
{
    public List<object> Items = new List<object>();
    public JArr Add(object value) { Items.Add(value); return this; }
    public int Count { get { return Items.Count; } }
}

public static class JWrite
{
    public static string Write(object v)
    {
        StringBuilder sb = new StringBuilder();
        Put(sb, v, 0);
        sb.Append('\n');
        return sb.ToString();
    }

    static void Indent(StringBuilder sb, int depth)
    {
        sb.Append('\n');
        sb.Append(' ', depth);
    }

    static void Put(StringBuilder sb, object v, int depth)
    {
        if (v == null) { sb.Append("null"); return; }
        if (v is string) { Str(sb, (string)v); return; }
        if (v is bool) { sb.Append((bool)v ? "true" : "false"); return; }
        if (v is JObj)
        {
            JObj o = (JObj)v;
            sb.Append('{');
            for (int i = 0; i < o.Keys.Count; i++)
            {
                if (i > 0) sb.Append(',');
                Indent(sb, depth + 1);
                Str(sb, o.Keys[i]);
                sb.Append(": ");
                Put(sb, o.Values[i], depth + 1);
            }
            if (o.Keys.Count > 0) Indent(sb, depth);
            sb.Append('}');
            return;
        }
        if (v is JArr)
        {
            JArr a = (JArr)v;
            sb.Append('[');
            for (int i = 0; i < a.Items.Count; i++)
            {
                if (i > 0) sb.Append(',');
                Indent(sb, depth + 1);
                Put(sb, a.Items[i], depth + 1);
            }
            if (a.Items.Count > 0) Indent(sb, depth);
            sb.Append(']');
            return;
        }
        double d = Conv.ToDouble(v, double.NaN);
        if (double.IsNaN(d) || double.IsInfinity(d)) { sb.Append('0'); return; }
        if (d == Math.Floor(d) && Math.Abs(d) < 1e15) sb.Append(((long)d).ToString(CultureInfo.InvariantCulture));
        else sb.Append(d.ToString("R", CultureInfo.InvariantCulture));
    }

    static void Str(StringBuilder sb, string s)
    {
        sb.Append('"');
        foreach (char c in s)
        {
            switch (c)
            {
                case '"': sb.Append("\\\""); break;
                case '\\': sb.Append("\\\\"); break;
                case '\n': sb.Append("\\n"); break;
                case '\r': sb.Append("\\r"); break;
                case '\t': sb.Append("\\t"); break;
                default:
                    if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4"));
                    else sb.Append(c);
                    break;
            }
        }
        sb.Append('"');
    }
}

// ---------------------------------------------------------------------------------------------- reflection
public static class Conv
{
    // A public property's value, or null when the object does not have it (or reading it fails). When a class
    // declares a property again over its base class's (VEGAS's OFX parameters do that with Value), the most
    // derived one is read.
    public static object Prop(object o, string name)
    {
        if (o == null) return null;
        try
        {
            PropertyInfo best = null;
            foreach (PropertyInfo pi in o.GetType().GetProperties(BindingFlags.Public | BindingFlags.Instance))
            {
                if (pi.Name != name || !pi.CanRead || pi.GetIndexParameters().Length > 0) continue;
                if (best == null || pi.DeclaringType.IsSubclassOf(best.DeclaringType)) best = pi;
            }
            return best == null ? null : best.GetValue(o, null);
        }
        catch { return null; }
    }

    // A public field's value (VEGAS's value structures, such as OFXColor, have fields), or null.
    public static object Field(object o, string name)
    {
        if (o == null) return null;
        try
        {
            FieldInfo f = o.GetType().GetField(name, BindingFlags.Public | BindingFlags.Instance);
            return f == null ? null : f.GetValue(o);
        }
        catch { return null; }
    }

    public static object PropOrField(object o, string name)
    {
        object v = Prop(o, name);
        return v ?? Field(o, name);
    }

    public static double ToDouble(object v, double def)
    {
        if (v == null) return def;
        if (v is double) return (double)v;
        if (v is float) return (float)v;
        if (v is int) return (int)v;
        if (v is uint) return (uint)v;
        if (v is long) return (long)v;
        if (v is ulong) return (ulong)v;
        if (v is short) return (short)v;
        if (v is ushort) return (ushort)v;
        if (v is byte) return (byte)v;
        if (v is decimal) return (double)(decimal)v;
        if (v is bool) return (bool)v ? 1 : 0;
        if (v is Timecode) return ((Timecode)v).ToMilliseconds() / 1000.0;
        if (v is Enum) return Convert.ToDouble(v, CultureInfo.InvariantCulture);
        return def;
    }

    public static bool ToBool(object v, bool def)
    {
        if (v == null) return def;
        if (v is bool) return (bool)v;
        return ToDouble(v, def ? 1 : 0) != 0;
    }

    public static string ToStr(object v)
    {
        return v == null ? "" : v.ToString();
    }

    public static double Seconds(Timecode t)
    {
        return t == null ? 0.0 : t.ToMilliseconds() / 1000.0;
    }

    public static double NumProp(object o, string name, double def) { return ToDouble(Prop(o, name), def); }
}

// ---------------------------------------------------------------------------------------------- the exporter
public class RenoExporter
{
    Vegas vegas;
    Project proj;
    long nextId = 1;
    double fps = 30;
    public List<string> Problems = new List<string>();
    Dictionary<string, long> fileMedia = new Dictionary<string, long>();
    Dictionary<Media, long> generatedMedia = new Dictionary<Media, long>();
    JArr mediaArr = new JArr();
    Dictionary<TrackEventGroup, long> groupIds = new Dictionary<TrackEventGroup, long>();
    Dictionary<TrackEventGroup, int> groupSizes = new Dictionary<TrackEventGroup, int>();
    Dictionary<BusTrack, long> busIds = new Dictionary<BusTrack, long>();

    public RenoExporter(Vegas v) { vegas = v; proj = v.Project; }

    long NewId() { return nextId++; }

    void Problem(string s)
    {
        if (!Problems.Contains(s)) Problems.Add(s);
    }

    // Where to write (Reno Bridge sets it); null: next to the .veg, or ask.
    public string Target;

    public string Run()
    {
        string target = null;
        if (!string.IsNullOrEmpty(Target)) target = Target;
        else if (!string.IsNullOrEmpty(proj.FilePath)) target = Path.ChangeExtension(proj.FilePath, ".reno");
        else
        {
            SaveFileDialog dlg = new SaveFileDialog();
            dlg.Filter = "Reno project (*.reno)|*.reno";
            dlg.Title = "Save the Reno project as";
            if (dlg.ShowDialog() != DialogResult.OK) return null;
            target = dlg.FileName;
        }

        // groups first: only groups of two or more events become Reno groups
        foreach (Track t in proj.Tracks)
            foreach (TrackEvent ev in t.Events)
            {
                TrackEventGroup g = ev.IsGrouped ? ev.Group : null;
                if (g == null) continue;
                int n;
                groupSizes.TryGetValue(g, out n);
                groupSizes[g] = n + 1;
            }

        JObj root = new JObj();
        root.Add("reno", 1);
        root.Add("settings", Settings());
        JArr buses = new JArr();
        JArr audioOut = new JArr(), videoOut = new JArr();
        JObj busInfo = new JObj();
        double masterVolume = 1.0;
        Buses(buses, audioOut, videoOut, busInfo, ref masterVolume);
        root.Add("masterVolume", masterVolume);
        root.Add("masterPan", 0.0);
        root.Add("masterMute", false);
        root.Add("buses", buses);
        root.Add("markers", Markers());
        JArr tracks = new JArr();
        int index = 0;
        foreach (Track t in proj.Tracks) tracks.Add(TrackJson(t, ++index));
        // media not used by any event is still in the project's media pool
        try
        {
            foreach (object o in proj.MediaPool)
            {
                Media m = o as Media;
                if (m == null && o is DictionaryEntry) m = ((DictionaryEntry)o).Value as Media;
                if (m != null) MediaIdOf(m);
            }
        }
        catch (Exception e) { Problem("the media pool could not be listed: " + e.Message); }
        root.Add("media", mediaArr);
        root.Add("tracks", tracks);
        root.Add("videoOut", videoOut);
        root.Add("audioOut", audioOut);
        root.Add("nextId", nextId);
        JObj info = new JObj();
        info.Add("exportedBy", "Export to Reno.cs");
        info.Add("vegasVersion", vegas.Version ?? "");
        info.Add("project", proj.FilePath ?? "");
        info.Add("fps", fps);
        info.Add("buses", busInfo);
        JArr probs = new JArr();
        foreach (string p in Problems) probs.Add(p);
        info.Add("problems", probs);
        root.Add("vegas", info);
        File.WriteAllText(target, JWrite.Write(root), new UTF8Encoding(false));
        return target;
    }

    // -------------------------------------------------------------------- project properties
    JObj Settings()
    {
        JObj s = new JObj();
        ProjectVideoProperties v = proj.Video;
        s.Add("width", v.Width);
        s.Add("height", v.Height);
        fps = v.FrameRate;
        long num, den;
        FrameRate(fps, out num, out den);
        s.Add("fpsNum", num);
        s.Add("fpsDen", den);
        s.Add("sampleRate", (long)proj.Audio.SampleRate);
        string fo = v.FieldOrder.ToString().ToLowerInvariant();
        s.Add("fieldOrder", fo.Contains("upper") ? "upper" : (fo.Contains("lower") ? "lower" : "none"));
        s.Add("pixelAspect", v.PixelAspectRatio);
        string pf = Conv.ToStr(Conv.Prop(v, "PixelFormat"));
        s.Add("pixelFormat", pf.Contains("32") ? "32bit" : "8bit");
        string rq = v.RenderQuality.ToString().ToLowerInvariant();
        s.Add("renderQuality", rq.Contains("draft") ? "draft" : rq.Contains("preview") ? "preview" : rq.Contains("best") ? "best" : "good");
        s.Add("resample", "smart");
        string mb = Conv.ToStr(Conv.Prop(v, "MotionBlurType")).ToLowerInvariant();
        s.Add("motionBlur", mb.Length > 0 && mb != "unknown" ? mb : "gaussian");
        string di = Conv.ToStr(Conv.Prop(v, "DeinterlaceMethod")).ToLowerInvariant();
        s.Add("deinterlace", di.Contains("blend") ? "blend" : di.Contains("interp") ? "interpolate" : "none");
        s.Add("audioBits", (long)proj.Audio.BitDepth);
        RulerProperties ruler = proj.Ruler;
        string rf = ruler.Format.ToString().ToLowerInvariant();
        s.Add("rulerFormat", rf == "seconds" ? "seconds" : rf.Contains("frame") && !rf.Contains("time") ? "frames" : rf == "samples" ? "samples" :
                             rf.Contains("measures") ? "measures" : "timecode");
        s.Add("rulerStart", Conv.Seconds(ruler.StartTime));
        s.Add("tempo", ruler.BeatsPerMinute);
        s.Add("beatsPerMeasure", (long)ruler.BeatsPerMeasure);
        SummaryProperties sum = proj.Summary;
        s.Add("title", sum.Title ?? "");
        s.Add("artist", sum.Artist ?? "");
        s.Add("engineer", sum.Engineer ?? "");
        s.Add("copyright", sum.Copyright ?? "");
        s.Add("comments", sum.Comments ?? "");
        // VEGAS's own values, for working out where the .veg keeps them
        JObj raw = new JObj();
        raw.Add("fieldOrder", v.FieldOrder.ToString());
        raw.Add("pixelFormat", pf);
        raw.Add("renderQuality", v.RenderQuality.ToString());
        raw.Add("motionBlur", Conv.ToStr(Conv.Prop(v, "MotionBlurType")));
        raw.Add("deinterlace", Conv.ToStr(Conv.Prop(v, "DeinterlaceMethod")));
        raw.Add("rulerFormat", ruler.Format.ToString());
        raw.Add("masterBusMode", proj.Audio.MasterBusMode.ToString());
        s.Add("vegas", raw);
        return s;
    }

    // Reno keeps the frame rate as a fraction; NTSC rates are exact fractions of 1001.
    static void FrameRate(double f, out long num, out long den)
    {
        double[] ntsc = { 24000.0 / 1001, 30000.0 / 1001, 60000.0 / 1001, 120000.0 / 1001 };
        foreach (double n in ntsc)
            if (Math.Abs(f - n) < 1e-4) { num = (long)Math.Round(n * 1001); den = 1001; return; }
        if (Math.Abs(f - Math.Round(f)) < 1e-6) { num = (long)Math.Round(f); den = 1; return; }
        num = (long)Math.Round(f * 1000); den = 1000;
    }

    // -------------------------------------------------------------------- buses and markers
    void Buses(JArr buses, JArr audioOut, JArr videoOut, JObj busInfo, ref double masterVolume)
    {
        // the master and the video output are asked for by name; BusTracks may or may not list them too
        AudioBusTrack master = proj.MasterBus;
        if (master != null)
        {
            bool known;
            masterVolume = BusLevel(master, out known);
            busInfo.Add("masterLevelKnown", known);
            foreach (Effect fx in master.Effects) { JObj j = FxJson(fx, "the master bus"); if (j != null) audioOut.Add(j); }
        }
        VideoBusTrack videoBus = proj.VideoBus;
        if (videoBus != null)
            foreach (Effect fx in videoBus.Effects) { JObj j = FxJson(fx, "the video output"); if (j != null) videoOut.Add(j); }
        foreach (BusTrack b in proj.BusTracks)
        {
            if (b == null || b.IsVideo() || b.IsMaster()) continue;
            bool known;
            double level = BusLevel(b, out known);
            JObj jb = new JObj();
            long id = NewId();
            string name = b.Name ?? "";
            busIds[b] = id;
            jb.Add("id", id);
            jb.Add("name", name);
            jb.Add("level", level);
            jb.Add("pan", 0.0);
            jb.Add("mute", Conv.ToBool(Conv.Prop(b, "Mute"), false));
            jb.Add("solo", Conv.ToBool(Conv.Prop(b, "Solo"), false));
            jb.Add("output", 0);
            JArr fxs = new JArr();
            foreach (Effect fx in b.Effects) { JObj j = FxJson(fx, "bus " + name); if (j != null) fxs.Add(j); }
            jb.Add("fx", fxs);
            jb.Add("vegas", new JObj().Add("levelKnown", known).Add("index", b.Index).Add("description", b.Description ?? ""));
            buses.Add(jb);
        }
    }

    // A bus's fader is not in VEGAS 18's scripting; a "Volume" property is used where a version has one, else
    // the level of its volume envelope where it has one, else 1 (and 'known' is false, so that compare.cpp
    // does not hold it against the importer).
    double BusLevel(BusTrack b, out bool known)
    {
        known = true;
        object vol = Conv.Prop(b, "Volume");
        if (vol != null) return Conv.ToDouble(vol, 1.0);
        try
        {
            Envelope e = b.Envelopes.FindByType(EnvelopeType.Volume);
            if (e != null && e.Points.Count == 1) return e.Points[0].Y;
        }
        catch { }
        known = false;
        return 1.0;
    }

    JArr Markers()
    {
        JArr a = new JArr();
        foreach (Marker m in proj.Markers)
        {
            JObj j = new JObj();
            j.Add("id", NewId());
            j.Add("time", Conv.Seconds(m.Position));
            j.Add("length", 0.0);
            j.Add("name", m.Label ?? "");
            j.Add("region", false);
            a.Add(j);
        }
        foreach (Region r in proj.Regions)
        {
            JObj j = new JObj();
            j.Add("id", NewId());
            j.Add("time", Conv.Seconds(r.Position));
            j.Add("length", Conv.Seconds(r.Length));
            j.Add("name", r.Label ?? "");
            j.Add("region", true);
            a.Add(j);
        }
        return a;
    }

    // -------------------------------------------------------------------- media
    static bool IsStill(string path)
    {
        string[] exts = { ".jpg", ".jpeg", ".png", ".bmp", ".tif", ".tiff", ".tga", ".psd", ".dds", ".webp", ".heic", ".jfif", ".exr", ".dib", ".pcx", ".gif" };
        string p = (path ?? "").ToLowerInvariant();
        foreach (string e in exts) if (p.EndsWith(e)) return true;
        return false;
    }

    // "META:\SubClip\{C:\a\b.mp4} (2)[start][length][reversed]" (100 ns units), as VEGAS names subclips.
    static bool ParseSubclip(string s, out string file, out double start, out double len, out bool rev)
    {
        file = null; start = 0; len = 0; rev = false;
        const string pre = "META:\\SubClip\\{";
        if (s == null || !s.StartsWith(pre, StringComparison.Ordinal)) return false;
        long[] nums = new long[3];
        int end = s.Length;
        for (int i = 2; i >= 0; i--)
        {
            if (end == 0 || s[end - 1] != ']') return false;
            int open = s.LastIndexOf('[', end - 1);
            if (open < 0) return false;
            if (!long.TryParse(s.Substring(open + 1, end - 1 - open - 1), NumberStyles.None, CultureInfo.InvariantCulture, out nums[i])) return false;
            end = open;
        }
        if (end < 3) return false;
        int close = s.LastIndexOf("} (", end - 1, StringComparison.Ordinal);
        if (close < pre.Length) return false;
        file = s.Substring(pre.Length, close - pre.Length);
        start = nums[0] / 1e7;
        len = nums[1] / 1e7;
        rev = nums[2] != 0;
        return true;
    }

    class MediaRef
    {
        public long Id;
        public bool Subclip;
        public double Start, Length;
        public bool Reversed;
    }

    MediaRef MediaIdOf(Media m)
    {
        MediaRef r = new MediaRef();
        if (m == null) return r;
        if (m.IsGenerated())
        {
            long gid;
            if (generatedMedia.TryGetValue(m, out gid)) { r.Id = gid; return r; }
            gid = NewId();
            generatedMedia[m] = gid;
            JObj j = new JObj();
            j.Add("id", gid);
            j.Add("path", "");
            Effect g = m.Generator;
            string gname = g != null && g.PlugIn != null ? g.PlugIn.Name : "generated media";
            j.Add("name", gname);
            j.Add("generated", true);
            JObj slot = g != null ? FxJson(g, "generated media '" + gname + "'") : null;
            j.Add("generator", slot ?? new JObj().Add("id", NewId()).Add("kind", "ofx").Add("name", gname).Add("plugin", "").Add("bypass", false).Add("state", null));
            JObj info = InfoOf(m, "");
            info.Set("video", true);
            info.Set("duration", 0.0);
            j.Add("info", info);
            mediaArr.Add(j);
            r.Id = gid;
            return r;
        }
        string path = m.FilePath ?? "";
        string key = m.KeyString ?? "";
        string file;
        double ss, sl;
        bool rev;
        Media infoFrom = m;
        Subclip sub = m as Subclip;
        if (ParseSubclip(key, out file, out ss, out sl, out rev) || ParseSubclip(path, out file, out ss, out sl, out rev))
        {
            r.Subclip = true; r.Start = ss; r.Length = sl; r.Reversed = rev;
            path = file;
            if (sub != null && sub.ParentMedia != null) infoFrom = sub.ParentMedia;
        }
        else if (sub != null)
        {
            // no META name to read: ask the subclip itself
            if (sub.ParentMedia != null) { path = sub.ParentMedia.FilePath ?? path; infoFrom = sub.ParentMedia; }
            r.Subclip = true;
            r.Start = Conv.Seconds(sub.Start);
            r.Length = Conv.Seconds(sub.Length);
            r.Reversed = sub.IsReversed;
        }
        string norm = path.ToLowerInvariant().Replace('/', '\\');
        long id;
        if (!fileMedia.TryGetValue(norm, out id))
        {
            id = NewId();
            fileMedia[norm] = id;
            JObj j = new JObj();
            j.Add("id", id);
            j.Add("path", path);
            j.Add("name", Path.GetFileName(path));
            j.Add("generated", false);
            j.Add("info", InfoOf(infoFrom, path));
            mediaArr.Add(j);
        }
        r.Id = id;
        return r;
    }

    JObj InfoOf(Media m, string path)
    {
        JObj i = new JObj();
        bool still = IsStill(path);
        bool video = false, audio = false;
        int w = 0, h = 0, rate = 0, ch = 0;
        double vfps = 0;
        try { video = m.HasVideo(); audio = m.HasAudio(); } catch { }
        try
        {
            VideoStream vs = video ? m.GetVideoStreamByIndex(0) : null;
            if (vs != null) { w = vs.Width; h = vs.Height; vfps = vs.FrameRate; }
        }
        catch { }
        try
        {
            AudioStream a = audio ? m.GetAudioStreamByIndex(0) : null;
            if (a != null) { rate = (int)a.SampleRate; ch = a.Channels; }
        }
        catch { }
        i.Add("video", video || still);
        i.Add("audio", audio && !still);
        i.Add("still", still);
        i.Add("width", w);
        i.Add("height", h);
        i.Add("fps", still ? 0 : vfps);
        double len = 0;
        try { len = Conv.Seconds(m.Length); } catch { }
        i.Add("duration", still ? 0 : len);
        i.Add("sampleRate", rate);
        i.Add("channels", ch);
        return i;
    }

    // -------------------------------------------------------------------- effects
    // VEGAS's CLSID of a VST plug-in: the VST id's four bytes (backwards), 4 more, then this fixed tail - the way
    // Reno's importer tells VST plug-ins from DirectX ones, so the same is done here.
    static readonly byte[] VstTail = { 0x02, 0x69, 0x00, 0x07, 0xe9, 0x5d, 0x36, 0xfa };

    static bool IsVstClassId(Guid g, out uint vstId)
    {
        byte[] b = g.ToByteArray();
        vstId = ((uint)b[3] << 24) | ((uint)b[2] << 16) | ((uint)b[1] << 8) | b[0];
        for (int i = 0; i < 8; i++) if (b[8 + i] != VstTail[i]) return false;
        return true;
    }

    // One effect as a Reno effect slot. OpenFX effects carry their parameters in the shape Reno's OpenFX host
    // loads ({"plugin": id, "params": {name: {"v": [...], "k": [[frame, ...], ...]} or {"s": text}}}).
    JObj FxJson(Effect fx, string where)
    {
        if (fx == null || fx.PlugIn == null) return null;
        JObj j = new JObj();
        j.Add("id", NewId());
        PlugInNode p = fx.PlugIn;
        JObj extra = new JObj();
        extra.Add("uniqueId", p.UniqueID ?? "");
        extra.Add("classId", p.ClassID.ToString());
        extra.Add("name", p.Name ?? "");
        extra.Add("bypass", fx.Bypass);
        extra.Add("applyBeforePanCrop", Conv.ToBool(Conv.Prop(fx, "ApplyBeforePanCrop"), false));
        string preset = "";
        try { if (fx.CurrentPreset != null) preset = fx.CurrentPreset.Name ?? ""; } catch { }
        extra.Add("preset", preset);
        if (fx.IsOFX)
        {
            string uid = p.UniqueID ?? "";
            if (uid.StartsWith("{Svfx:", StringComparison.Ordinal)) uid = uid.Substring(6);
            if (uid.EndsWith("}", StringComparison.Ordinal)) uid = uid.Substring(0, uid.Length - 1);
            j.Add("kind", "ofx");
            j.Add("name", "");
            j.Add("plugin", uid);
            j.Add("bypass", fx.Bypass);
            JObj state = new JObj();
            state.Add("plugin", uid);
            state.Add("params", OfxParams(fx.OFXEffect, where + ", effect " + uid));
            j.Add("state", state);
        }
        else
        {
            j.Add("kind", "audio");
            j.Add("name", p.Name ?? "");
            JObj a = new JObj();
            uint vstId;
            bool vst = IsVstClassId(p.ClassID, out vstId);
            a.Add("format", vst ? "VST2" : "DX");
            a.Add("path", "");
            a.Add("uid", vst ? vstId.ToString(CultureInfo.InvariantCulture) : "dshow:{" + p.ClassID.ToString().ToUpperInvariant() + "}");
            a.Add("name", p.Name ?? "");
            a.Add("vendor", "");
            a.Add("inputs", 2);
            a.Add("outputs", 2);
            a.Add("editor", false);
            a.Add("instrument", false);
            j.Add("audio", a);
            j.Add("bypass", fx.Bypass);
            // a plug-in's saved state is not in VEGAS's scripting
            j.Add("state", null);
        }
        j.Add("vegas", extra);
        return j;
    }

    // How many numbers an OpenFX parameter type has; 0 for text, -1 for types that hold no value Reno keeps.
    static int Dims(string type)
    {
        switch (type)
        {
            case "Boolean": case "Choice": case "Integer": case "Double": return 1;
            case "Integer2D": case "Double2D": return 2;
            case "Integer3D": case "Double3D": case "RGB": return 3;
            case "RGBA": return 4;
            case "String": case "Custom": return 0;
            default: return -1;
        }
    }

    JObj OfxParams(OFXEffect ofx, string where)
    {
        JObj ps = new JObj();
        if (ofx == null) { Problem(where + ": its OpenFX parameters could not be listed"); return ps; }
        foreach (OFXParameter p in ofx.Parameters)
        {
            string name = p.Name ?? "";
            string type = p.ParameterType.ToString();
            int dims = Dims(type);
            if (name.Length == 0 || dims < 0) continue;
            if (!Conv.ToBool(Conv.Prop(p, "Persisted"), true)) continue;      // Reno keeps what is saved, as VEGAS does
            object value = Conv.Prop(p, "Value");
            JObj e = new JObj();
            if (dims == 0)
            {
                e.Add("s", value as string ?? "");
            }
            else
            {
                JArr v = ValueArray(value, dims);
                if (v == null) { Problem(where + ": parameter " + name + " (" + type + ") could not be read"); continue; }
                e.Add("v", v);
                if (p.IsAnimated)
                {
                    IEnumerable keys = Conv.Prop(p, "Keyframes") as IEnumerable;
                    if (keys != null)
                    {
                        JArr ks = new JArr();
                        foreach (object k in keys)
                        {
                            Timecode t = Conv.Prop(k, "Time") as Timecode;
                            JArr kv = ValueArray(Conv.Prop(k, "Value"), dims);
                            if (t == null || kv == null) continue;
                            JArr key = new JArr();
                            key.Add(Conv.Seconds(t) * fps);     // Reno keeps keyframe times in frames
                            foreach (object x in kv.Items) key.Add(x);
                            ks.Add(key);
                        }
                        if (ks.Count > 0) e.Add("k", ks);
                    }
                }
            }
            ps.Add(name, e);
        }
        return ps;
    }

    // An OpenFX value as numbers: a number, a boolean, a choice (its index), or a structure with R/G/B/A or
    // X/Y/Z members; 'dims' of them.
    static JArr ValueArray(object v, int dims)
    {
        if (v == null) return null;
        JArr a = new JArr();
        if (v is bool || v is double || v is float || v is int || v is long || v is uint || v is short) { a.Add(Conv.ToDouble(v, 0)); return a; }
        object index = Conv.PropOrField(v, "Index");
        if (index != null && dims == 1) { a.Add(Conv.ToDouble(index, 0)); return a; }
        string[][] shapes = { new string[] { "R", "G", "B", "A" }, new string[] { "X", "Y", "Z" } };
        foreach (string[] shape in shapes)
        {
            JArr t = new JArr();
            for (int i = 0; i < shape.Length && t.Count < dims; i++)
            {
                object x = Conv.PropOrField(v, shape[i]);
                if (x == null) break;
                t.Add(Conv.ToDouble(x, 0));
            }
            if (t.Count == dims) return t;
        }
        return null;
    }

    // -------------------------------------------------------------------- envelopes
    // The value an envelope holds all along, or NaN if it changes.
    static double ConstantValue(Envelope e)
    {
        if (e == null || e.Points.Count == 0) return double.NaN;
        double v = e.Points[0].Y;
        foreach (EnvelopePoint p in e.Points) if (Math.Abs(p.Y - v) > 1e-6) return double.NaN;
        return v;
    }

    // The average of an envelope over [0, length] (straight lines between its points; a point whose curve is
    // None holds its value).
    static double AverageEnvelope(Envelope e, double length)
    {
        if (e.Points.Count == 0 || length <= 0) return 1.0;
        double area = 0;
        int n = e.Points.Count;
        for (int i = 0; i < n; i++)
        {
            EnvelopePoint p = e.Points[i];
            double t0 = Math.Min(length, Math.Max(0, Conv.Seconds(p.X)));
            double t1 = i + 1 < n ? Math.Min(length, Math.Max(0, Conv.Seconds(e.Points[i + 1].X))) : length;
            double v1 = (i + 1 < n && p.Curve != CurveType.None) ? e.Points[i + 1].Y : p.Y;
            if (t1 > t0) area += (t1 - t0) * 0.5 * (p.Y + v1);
        }
        double first = Conv.Seconds(e.Points[0].X);
        if (first > 0) area += Math.Min(length, first) * e.Points[0].Y;
        return area / length;
    }

    // -------------------------------------------------------------------- tracks
    static string CompositeName(CompositeMode m)
    {
        switch (m)
        {
            case CompositeMode.Add: return "add";
            case CompositeMode.Subtract: return "subtract";
            case CompositeMode.Multiply: return "multiply";
            case CompositeMode.Cut: return "cut";
            case CompositeMode.Screen: return "screen";
            case CompositeMode.Overlay: return "overlay";
            case CompositeMode.HardLight: return "hardlight";
            case CompositeMode.Dodge: return "dodge";
            case CompositeMode.Burn: return "burn";
            case CompositeMode.Darken: return "darken";
            case CompositeMode.Lighten: return "lighten";
            case CompositeMode.Difference: return "difference";
            case CompositeMode.DifferenceSquared: return "diffsquared";
            default: return "srcalpha";        // Source Alpha, 3D Source Alpha (drawn flat) and Custom
        }
    }

    JObj TrackJson(Track t, int index)
    {
        JObj j = new JObj();
        string label = "track " + index;
        j.Add("id", NewId());
        j.Add("type", t.IsVideo() ? "video" : "audio");
        j.Add("name", t.Name ?? "");
        j.Add("mute", t.Mute);
        j.Add("solo", t.Solo);
        JObj extra = new JObj();
        double level = 1.0, pan = 0;
        long output = 0;
        string composite = "srcalpha";
        JArr motion = new JArr();
        if (t.IsAudio())
        {
            AudioTrack at = (AudioTrack)t;
            level = at.Volume;
            pan = at.PanX;
            BusTrack bus = at.BusTrack;
            if (bus != null && !bus.IsMaster())
            {
                long bid;
                if (busIds.TryGetValue(bus, out bid)) output = bid;
                extra.Add("bus", bus.Name ?? "");
            }
        }
        else
        {
            VideoTrack vt = (VideoTrack)t;
            level = vt.CompositeLevel;
            extra.Add("compositeMode", vt.CompositeMode.ToString());
            extra.Add("compositeModeNumber", (int)vt.CompositeMode);
            try
            {
                Effect ce = vt.CompositeModeEffect;
                if (ce != null && ce.PlugIn != null)
                {
                    extra.Add("compositeEffectClassId", ce.PlugIn.ClassID.ToString());
                    extra.Add("compositeEffectName", ce.PlugIn.Name ?? "");
                }
            }
            catch { }
            composite = CompositeName(vt.CompositeMode);
            TrackMotion tm = vt.TrackMotion;
            if (tm != null)
            {
                double pw = proj.Video.Width, ph = proj.Video.Height;
                JArr raw = new JArr();
                foreach (TrackMotionKeyframe k in tm.MotionKeyframes)
                {
                    double px = k.PositionX, py = k.PositionY, kw = k.Width, kh = k.Height, rz = k.RotationZ;
                    double tsec = Conv.Seconds(k.Position);
                    raw.Add(new JArr().Add(tsec).Add(px).Add(py).Add(kw).Add(kh).Add(rz).Add(k.Smoothness).Add(k.Type.ToString())
                                      .Add(k.PositionZ).Add(k.RotationX).Add(k.RotationY).Add(k.RotationOffsetX).Add(k.RotationOffsetY)
                                      .Add(k.OrientationX).Add(k.OrientationY).Add(k.OrientationZ).Add(k.Depth));
                    // VEGAS: position of the centre from the frame's centre in pixels, y up (an assumption that
                    // compare.cpp's track motion table shows the truth of); Reno: fractions of the frame from the
                    // top left, y down
                    JArr mk = new JArr();
                    mk.Add(tsec).Add(0.5 + px / pw).Add(0.5 - py / ph).Add(kw / pw).Add(kh / ph).Add(-rz * 180.0 / Math.PI).Add(0);
                    motion.Add(mk);
                }
                extra.Add("trackMotionRaw", raw);
                extra.Add("shadow", tm.ShadowEnabled);
                extra.Add("glow", tm.GlowEnabled);
                // a single key that leaves the picture where it is is no motion at all
                if (motion.Count == 1)
                {
                    JArr k0 = (JArr)motion.Items[0];
                    if (Math.Abs((double)k0.Items[1] - 0.5) < 1e-6 && Math.Abs((double)k0.Items[2] - 0.5) < 1e-6 &&
                        Math.Abs((double)k0.Items[3] - 1) < 1e-6 && Math.Abs((double)k0.Items[4] - 1) < 1e-6 && Math.Abs((double)k0.Items[5]) < 1e-9)
                        motion = new JArr();
                }
            }
        }
        // envelopes that hold one value are folded into the setting they scale, as Reno's importer does
        try
        {
            foreach (Envelope e in t.Envelopes)
            {
                double c = ConstantValue(e);
                bool isLevel = (t.IsAudio() && e.Type == EnvelopeType.Volume) || (t.IsVideo() && e.Type == EnvelopeType.Composite);
                if (isLevel && !double.IsNaN(c) && c >= 0) level *= c;
                else if (e.Type == EnvelopeType.Pan && !double.IsNaN(c) && Math.Abs(pan) < 1e-9) pan = c;
                else Problem(label + ": its " + e.Type.ToString() + " envelope is not carried to Reno");
            }
        }
        catch { Problem(label + ": its envelopes could not be listed"); }
        j.Add("level", level);
        j.Add("pan", pan);
        JArr fxs = new JArr();
        try
        {
            foreach (Effect fx in t.Effects) { JObj f = FxJson(fx, label); if (f != null) fxs.Add(f); }
        }
        catch { Problem(label + ": its effects could not be listed"); }
        j.Add("fx", fxs);
        j.Add("output", output);
        j.Add("sends", new JArr());
        if (motion.Count > 0) j.Add("motion", motion);
        j.Add("composite", composite);
        j.Add("color", 0);
        j.Add("height", 0);
        JArr evs = new JArr();
        foreach (TrackEvent ev in t.Events)
        {
            JObj e = EventJson(ev, t.IsVideo(), label);
            if (e != null) evs.Add(e);
        }
        j.Add("events", evs);
        j.Add("vegas", extra);
        return j;
    }

    // -------------------------------------------------------------------- events
    // A value VEGAS's scripting may fail to give: its default then, and its name noted.
    static T Safe<T>(Func<T> get, T fallback, string name, List<string> failed)
    {
        try { return get(); }
        catch (Exception) { failed.Add(name); return fallback; }
    }

    JObj EventJson(TrackEvent ev, bool video, string label)
    {
        double start = Conv.Seconds(ev.Start), length = Conv.Seconds(ev.Length);
        string where = label + ", event at " + start.ToString("0.000", CultureInfo.InvariantCulture) + " s";
        Take take = ev.ActiveTake;
        if (take == null || take.Media == null) { Problem(where + ": an empty event (no media) was left out"); return null; }
        JObj extra = new JObj();
        MediaRef mr = MediaIdOf(take.Media);
        double rate0 = ev.PlaybackRate;
        double takeOffset = Conv.Seconds(take.Offset);
        extra.Add("takeOffset", takeOffset);
        extra.Add("takeLength", Conv.Seconds(take.Length));
        extra.Add("takeAvailableLength", Conv.Seconds(take.AvailableLength));
        extra.Add("playbackRate", rate0);
        extra.Add("takes", ev.Takes.Count);
        extra.Add("activeTake", take.Index);
        extra.Add("mute", ev.Mute);
        extra.Add("loop", ev.Loop);
        extra.Add("locked", ev.Locked);
        extra.Add("selected", ev.Selected);
        extra.Add("fadeInCurve", ev.FadeIn.Curve.ToString());
        extra.Add("fadeOutCurve", ev.FadeOut.Curve.ToString());
        extra.Add("media", take.Media.FilePath ?? "");
        // velocity (video events): VEGAS multiplies the playback rate by it; Reno has one rate, so the
        // average over the event is used (it keeps where the event starts and ends in the media)
        double vel = 1.0, gainScale = 1.0;
        int velPoints = 0;
        try
        {
            Envelopes envs = Conv.Prop(ev, "Envelopes") as Envelopes;
            if (envs != null)
                foreach (Envelope e in envs)
                {
                    if (e.Type == EnvelopeType.Velocity)
                    {
                        velPoints = e.Points.Count;
                        vel = AverageEnvelope(e, length);
                        continue;
                    }
                    double c = ConstantValue(e);
                    if ((e.Type == EnvelopeType.Composite || e.Type == EnvelopeType.Volume) && !double.IsNaN(c) && c >= 0) gainScale *= c;
                    else Problem(where + ": a " + e.Type.ToString() + " envelope is not carried to Reno");
                }
        }
        catch { }
        extra.Add("velocityAverage", vel);
        extra.Add("velocityPoints", velPoints);
        double rate = rate0 * vel;

        // VEGAS keeps a take's offset in timeline time: the position in the media is the offset times the
        // event's own rate (velocity aside). compare.cpp's "Take offset" table confirms or corrects this.
        double mediaOffset = takeOffset * rate0;
        double offset = mediaOffset;
        bool reverse = false;
        if (mr.Subclip)
        {
            if (mr.Reversed) { offset = Math.Max(0, mr.Start + mr.Length - mediaOffset - length * rate); reverse = true; }
            else offset = mr.Start + mediaOffset;
        }

        JObj j = new JObj();
        j.Add("id", NewId());
        j.Add("media", mr.Id);
        j.Add("start", start);
        j.Add("length", length);
        j.Add("offset", offset);
        j.Add("rate", rate);
        j.Add("fadeIn", Conv.Seconds(ev.FadeIn.Length));
        j.Add("fadeOut", Conv.Seconds(ev.FadeOut.Length));
        double gain = ev.FadeIn.Gain * gainScale;
        double pitch = 0;
        bool keepPitch = false;
        if (!video)
        {
            AudioEvent ae = (AudioEvent)ev;
            // (VEGAS's scripting fails some of these - PitchSemis gives E_FAIL for some events - so each is read on its own and
            // a failure is noted in "unreadable" rather than ending the export)
            List<string> unreadable = new List<string>();
            bool normalize = Safe(() => ae.Normalize, false, "Normalize", unreadable);
            double normalizeGain = Safe(() => ae.NormalizeGain, 1.0, "NormalizeGain", unreadable);
            extra.Add("normalize", normalize);
            extra.Add("normalizeGain", normalizeGain);
            if (normalize) gain *= normalizeGain;
            TimeStretchPitchShift method = Safe(() => ae.Method, TimeStretchPitchShift.None, "Method", unreadable);
            double semis = Safe(() => ae.PitchSemis, 0.0, "PitchSemis", unreadable);
            bool locked = Safe(() => ae.PitchLock, false, "PitchLock", unreadable);
            extra.Add("pitchSemis", semis);
            extra.Add("pitchLock", locked);
            extra.Add("method", method.ToString());
            extra.Add("elastique", Safe(() => ae.ElastiqueAttribute.ToString(), "", "ElastiqueAttribute", unreadable));
            extra.Add("classic", Safe(() => ae.ClassicAttribute.ToString(), "", "ClassicAttribute", unreadable));
            extra.Add("formantShift", Safe(() => ae.FormantShift, 0.0, "FormantShift", unreadable));
            extra.Add("formantLock", Safe(() => ae.FormantLock, false, "FormantLock", unreadable));
            extra.Add("invertPhase", Safe(() => ae.InvertPhase, false, "InvertPhase", unreadable));
            extra.Add("channels", Safe(() => ae.Channels.ToString(), "", "Channels", unreadable));
            if (unreadable.Count > 0) extra.Add("unreadable", string.Join(",", unreadable.ToArray()));
            // With no stretch method the rate changes the pitch (as on tape). With one, VEGAS keeps the
            // pitch when the rate changes, unless the pitch is locked to the rate.
            if (method != TimeStretchPitchShift.None && !locked) { keepPitch = true; pitch = semis; }
        }
        j.Add("gain", gain);
        j.Add("mute", ev.Mute);
        j.Add("loop", ev.Loop);
        long group = 0;
        TrackEventGroup grp = ev.IsGrouped ? ev.Group : null;
        if (grp != null)
        {
            int n;
            if (groupSizes.TryGetValue(grp, out n) && n >= 2)
            {
                if (!groupIds.TryGetValue(grp, out group)) { group = NewId(); groupIds[grp] = group; }
            }
        }
        j.Add("group", group);
        j.Add("name", ev.Name ?? "");
        j.Add("reverse", reverse);
        j.Add("pitch", pitch);
        j.Add("keepPitch", keepPitch);
        bool keepAspect = true;
        JArr panCrop = new JArr();
        JArr fxs = new JArr();
        if (video)
        {
            VideoEvent ve = (VideoEvent)ev;
            keepAspect = ve.MaintainAspectRatio;
            extra.Add("maintainAspectRatio", keepAspect);
            extra.Add("reduceInterlace", ve.ReduceInterlace);
            extra.Add("resampleMode", ve.ResampleMode.ToString());
            panCrop = PanCrop(ve, take, extra, where);
            // the media's own effects come first, as Reno's importer puts them
            try
            {
                foreach (Effect fx in take.Media.Effects) { JObj f = FxJson(fx, where + " (media effect)"); if (f != null) fxs.Add(f); }
            }
            catch { }
            try
            {
                foreach (Effect fx in ve.Effects) { JObj f = FxJson(fx, where); if (f != null) fxs.Add(f); }
            }
            catch { Problem(where + ": its effects could not be listed"); }
        }
        else
        {
            try
            {
                foreach (Effect fx in ((AudioEvent)ev).Effects) { JObj f = FxJson(fx, where); if (f != null) fxs.Add(f); }
            }
            catch { Problem(where + ": its effects could not be listed"); }
        }
        j.Add("keepAspect", keepAspect);
        if (panCrop.Count > 0) j.Add("panCrop", panCrop);
        j.Add("fx", fxs);
        try
        {
            Effect tr = ev.FadeIn.Transition as Effect;
            if (tr != null)
            {
                JObj t = FxJson(tr, where + " (transition)");
                if (t != null) j.Add("transition", t);
            }
        }
        catch { }
        j.Add("vegas", extra);
        return j;
    }

    // VEGAS's key types as Reno's pan/crop curves: linear, hold, and smooth for the eased ones.
    static int CurveOf(VideoKeyframeType t)
    {
        if (t == VideoKeyframeType.Linear) return 0;
        if (t == VideoKeyframeType.Hold) return 1;
        return 2;
    }

    // Pan/crop keys as Reno's [time, centre x, centre y, width, height, rotation, curve]: fractions of the
    // media's picture, rotation in degrees clockwise. Worked out from each key's four corners, not from its
    // Rotation number, so that VEGAS's own sense of turning is what is written.
    JArr PanCrop(VideoEvent ve, Take take, JObj extra, string where)
    {
        JArr keys = new JArr();
        double mw = 0, mh = 0;
        try
        {
            VideoStream vs = take.MediaStream as VideoStream;
            if (vs != null) { mw = vs.Width; mh = vs.Height; }
        }
        catch { }
        extra.Add("mediaWidth", mw);
        extra.Add("mediaHeight", mh);
        if (mw <= 0 || mh <= 0) return keys;
        JArr raw = new JArr();
        double outAspect = proj.Video.Width * proj.Video.PixelAspectRatio / Math.Max(1, proj.Video.Height);
        foreach (VideoMotionKeyframe k in ve.VideoMotion.Keyframes)
        {
            VideoMotionBounds b = k.Bounds;
            double tlx = b.TopLeft.X, tly = b.TopLeft.Y, trx = b.TopRight.X, try_ = b.TopRight.Y;
            double brx = b.BottomRight.X, bry = b.BottomRight.Y, blx = b.BottomLeft.X, bly = b.BottomLeft.Y;
            double cx = (tlx + trx + brx + blx) / 4, cy = (tly + try_ + bry + bly) / 4;
            double w = Math.Sqrt((trx - tlx) * (trx - tlx) + (try_ - tly) * (try_ - tly));
            double h = Math.Sqrt((blx - tlx) * (blx - tlx) + (bly - tly) * (bly - tly));
            double angle = Math.Atan2(try_ - tly, trx - tlx) * 180.0 / Math.PI;     // y down: positive is clockwise
            double t = Conv.Seconds(k.Position);
            raw.Add(new JArr().Add(t).Add(tlx).Add(tly).Add(trx).Add(try_).Add(brx).Add(bry).Add(blx).Add(bly).Add(k.Rotation).Add(k.Center.X).Add(k.Center.Y).Add(k.Type.ToString()));
            keys.Add(new JArr().Add(t).Add(cx / mw).Add(cy / mh).Add(w / mw).Add(h / mh).Add(angle).Add(CurveOf(k.Type)));
        }
        extra.Add("panCropRaw", raw);
        // VEGAS's untouched pan/crop (one key: the whole picture widened or heightened to the project's
        // shape, centred, not turned) is what Reno shows without pan/crop
        if (keys.Count == 1)
        {
            JArr k0 = (JArr)keys.Items[0];
            double cx = (double)k0.Items[1], cy = (double)k0.Items[2], w = (double)k0.Items[3] * mw, h = (double)k0.Items[4] * mh, a = (double)k0.Items[5];
            bool centred = Math.Abs(cx - 0.5) < 1e-4 && Math.Abs(cy - 0.5) < 1e-4;
            bool fitted = (Math.Abs(w - mw) < 0.01 && h >= mh - 0.01) || (Math.Abs(h - mh) < 0.01 && w >= mw - 0.01);
            bool shaped = h > 0 && Math.Abs(w / h - outAspect) < 0.01;
            if (centred && fitted && shaped && Math.Abs(a) < 1e-6) return new JArr();
        }
        return keys;
    }
}
