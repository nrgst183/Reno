// Import Reno Project: run inside VEGAS Pro (Tools > Scripting > Run Script...). Asks for a .reno file and builds it in
// a new VEGAS project: properties, media (files and OpenFX generators), buses, tracks, events with takes, rates, fades,
// gain, pan/crop, effects with settings and keyframes, transitions, groups, track motion, markers and regions.
// One Undo takes it all back.
// What VEGAS's scripting cannot set is listed at the end and written next to the .reno as "<name> - import problems.txt":
// audio plug-in settings, bus and master levels, track heights and colours, and missing media (their events are left out).
// C# 5 against ScriptPortal.Vegas.

using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Windows.Forms;
using ScriptPortal.Vegas;

public class EntryPoint
{
    public void FromVegas(Vegas vegas)
    {
        try
        {
            OpenFileDialog dlg = new OpenFileDialog();
            dlg.Filter = "Reno project (*.reno)|*.reno|All files (*.*)|*.*";
            dlg.Title = "Import a Reno project";
            if (dlg.ShowDialog() != DialogResult.OK) return;
            object root = Json.Parse(File.ReadAllText(dlg.FileName, Encoding.UTF8));
            Dictionary<string, object> project = root as Dictionary<string, object>;
            if (project == null || !project.ContainsKey("tracks"))
            {
                MessageBox.Show("This is not a Reno project.", "Import Reno Project");
                return;
            }
            // a new project, so that nothing the user has open is changed (VEGAS asks to save the old one)
            if (!vegas.NewProject(true, false)) return;
            RenoImporter im = new RenoImporter(vegas, Path.GetDirectoryName(dlg.FileName));
            using (UndoBlock undo = new UndoBlock("Import Reno project"))
            {
                im.Run(project);
            }
            string report = "Imported " + Path.GetFileName(dlg.FileName) + ": " + im.Summary();
            if (im.Problems.Count > 0)
            {
                string txt = Path.Combine(Path.GetDirectoryName(dlg.FileName), Path.GetFileNameWithoutExtension(dlg.FileName) + " - import problems.txt");
                try { File.WriteAllLines(txt, im.Problems.ToArray(), new UTF8Encoding(false)); report += "\n\nNot everything could be brought over; the list is in\n" + txt; }
                catch { }
                int shown = Math.Min(15, im.Problems.Count);
                report += "\n\n" + string.Join("\n", im.Problems.GetRange(0, shown).ToArray()) + (im.Problems.Count > shown ? "\n..." : "");
            }
            MessageBox.Show(report, "Import Reno Project");
        }
        catch (Exception e)
        {
            MessageBox.Show("Import Reno Project stopped:\n" + e.ToString(), "Import Reno Project");
        }
    }
}

// ---------------------------------------------------------------------------------------------- JSON
// A small JSON reader: objects become Dictionary<string, object>, arrays List<object>, numbers double.
public class Json
{
    string s;
    int p;
    int depth;

    public static object Parse(string text)
    {
        Json j = new Json();
        j.s = text;
        j.p = 0;
        if (j.s.Length > 0 && j.s[0] == '﻿') j.p = 1;
        object v = j.Value();
        j.Ws();
        if (j.p != j.s.Length) throw new FormatException("text after the end of the JSON at " + j.p);
        return v;
    }

    void Ws() { while (p < s.Length && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) p++; }

    object Value()
    {
        Ws();
        if (p >= s.Length) throw new FormatException("the JSON ends early");
        char c = s[p];
        if (c == '{') return Obj();
        if (c == '[') return Arr();
        if (c == '"') return Str();
        if (Word("true")) return true;
        if (Word("false")) return false;
        if (Word("null")) return null;
        return Number();
    }

    bool Word(string w)
    {
        if (string.CompareOrdinal(s, p, w, 0, w.Length) != 0) return false;
        p += w.Length;
        return true;
    }

    Dictionary<string, object> Obj()
    {
        if (++depth > 200) throw new FormatException("the JSON is nested too deep");
        Dictionary<string, object> o = new Dictionary<string, object>();
        p++;
        Ws();
        if (p < s.Length && s[p] == '}') { p++; depth--; return o; }
        while (true)
        {
            Ws();
            if (p >= s.Length || s[p] != '"') throw new FormatException("a name was expected at " + p);
            string k = Str();
            Ws();
            if (p >= s.Length || s[p] != ':') throw new FormatException("':' was expected at " + p);
            p++;
            o[k] = Value();
            Ws();
            if (p < s.Length && s[p] == ',') { p++; continue; }
            if (p < s.Length && s[p] == '}') { p++; depth--; return o; }
            throw new FormatException("',' or '}' was expected at " + p);
        }
    }

    List<object> Arr()
    {
        if (++depth > 200) throw new FormatException("the JSON is nested too deep");
        List<object> a = new List<object>();
        p++;
        Ws();
        if (p < s.Length && s[p] == ']') { p++; depth--; return a; }
        while (true)
        {
            a.Add(Value());
            Ws();
            if (p < s.Length && s[p] == ',') { p++; continue; }
            if (p < s.Length && s[p] == ']') { p++; depth--; return a; }
            throw new FormatException("',' or ']' was expected at " + p);
        }
    }

    string Str()
    {
        StringBuilder sb = new StringBuilder();
        p++;
        while (p < s.Length)
        {
            char c = s[p++];
            if (c == '"') return sb.ToString();
            if (c != '\\') { sb.Append(c); continue; }
            if (p >= s.Length) break;
            char e = s[p++];
            switch (e)
            {
                case 'n': sb.Append('\n'); break;
                case 'r': sb.Append('\r'); break;
                case 't': sb.Append('\t'); break;
                case 'b': sb.Append('\b'); break;
                case 'f': sb.Append('\f'); break;
                case 'u':
                    if (p + 4 > s.Length) throw new FormatException("a \\u escape ends early");
                    sb.Append((char)int.Parse(s.Substring(p, 4), NumberStyles.HexNumber, CultureInfo.InvariantCulture));
                    p += 4;
                    break;
                default: sb.Append(e); break;
            }
        }
        throw new FormatException("a text does not end");
    }

    double Number()
    {
        int a = p;
        while (p < s.Length && "+-0123456789.eE".IndexOf(s[p]) >= 0) p++;
        double d;
        if (a == p || !double.TryParse(s.Substring(a, p - a), NumberStyles.Float, CultureInfo.InvariantCulture, out d))
            throw new FormatException("a value was expected at " + a);
        return d;
    }

    // helpers for reading what Parse gave
    public static object Get(object o, string key)
    {
        Dictionary<string, object> d = o as Dictionary<string, object>;
        object v;
        return d != null && d.TryGetValue(key, out v) ? v : null;
    }
    public static double Num(object o, string key, double def) { object v = Get(o, key); return v is double ? (double)v : def; }
    public static bool Bool(object o, string key, bool def) { object v = Get(o, key); return v is bool ? (bool)v : def; }
    public static string Text(object o, string key) { object v = Get(o, key); return v as string ?? ""; }
    public static long Id(object o, string key) { object v = Get(o, key); return v is double ? (long)(double)v : 0; }
    public static List<object> Items(object o, string key) { return Get(o, key) as List<object> ?? new List<object>(); }
    public static double At(object list, int i, double def)
    {
        List<object> l = list as List<object>;
        return l != null && i < l.Count && l[i] is double ? (double)l[i] : def;
    }
}

// ---------------------------------------------------------------------------------------------- the importer
public class RenoImporter
{
    Vegas vegas;
    Project proj;
    string renoDir;
    double fps = 30;
    public List<string> Problems = new List<string>();
    Dictionary<long, Media> media = new Dictionary<long, Media>();
    Dictionary<long, object> mediaJson = new Dictionary<long, object>();
    Dictionary<long, AudioBusTrack> buses = new Dictionary<long, AudioBusTrack>();
    Dictionary<long, TrackEventGroup> groups = new Dictionary<long, TrackEventGroup>();
    int tracksMade, eventsMade, effectsMade;

    List<KeyValuePair<VideoTrack, string>> parentModes = new List<KeyValuePair<VideoTrack, string>>();

    public RenoImporter(Vegas v, string dir) { vegas = v; proj = v.Project; renoDir = dir; }

    void Problem(string s) { if (!Problems.Contains(s)) Problems.Add(s); }

    public string Summary()
    {
        return tracksMade + " tracks, " + eventsMade + " events, " + effectsMade + " effects.";
    }

    public void Run(Dictionary<string, object> j)
    {
        Settings(Json.Get(j, "settings"));
        double master = Json.Num(j, "masterVolume", 1.0);
        if (Math.Abs(master - 1.0) > 1e-6)
            Problem("The master volume (" + Db(master) + ") could not be set: VEGAS's scripting has no master fader.");
        if (Json.Bool(j, "masterMute", false) || Math.Abs(Json.Num(j, "masterPan", 0)) > 1e-6)
            Problem("The master bus's mute and pan could not be set: VEGAS's scripting has no way to.");
        foreach (object b in Json.Items(j, "buses")) Bus(b);
        Chain(proj.MasterBus.Effects, Json.Items(j, "audioOut"), "the master bus", false);
        Chain(proj.VideoBus.Effects, Json.Items(j, "videoOut"), "the video output", true);
        foreach (object m in Json.Items(j, "media")) mediaJson[Json.Id(m, "id")] = m;
        foreach (object m in Json.Items(j, "media")) MediaOf(Json.Id(m, "id"));
        int index = 0;
        foreach (object t in Json.Items(j, "tracks")) TrackOf(t, index++);
        // a track is a compositing parent only once its children are there
        foreach (KeyValuePair<VideoTrack, string> pm in parentModes)
        {
            try { pm.Key.SetParentCompositeMode(CompositeOf(pm.Value), false); }
            catch (Exception e) { Problem("Track '" + pm.Key.Name + "': its Parent Composite Mode could not be set (" + e.Message + ")."); }
        }
        foreach (object m in Json.Items(j, "markers"))
        {
            Timecode at = Timecode.FromSeconds(Json.Num(m, "time", 0));
            string name = Json.Text(m, "name");
            double len = Json.Num(m, "length", 0);
            if (Json.Bool(m, "region", false) && len > 0) proj.Regions.Add(new Region(at, Timecode.FromSeconds(len), name));
            else proj.Markers.Add(new Marker(at, name));
        }
    }

    static string Db(double gain)
    {
        return gain <= 0 ? "-inf dB" : (20 * Math.Log10(gain)).ToString("0.0", CultureInfo.InvariantCulture) + " dB";
    }

    // -------------------------------------------------------------------- project properties
    void Settings(object s)
    {
        if (s == null) return;
        ProjectVideoProperties v = proj.Video;
        v.Width = (int)Json.Num(s, "width", 1920);
        v.Height = (int)Json.Num(s, "height", 1080);
        fps = Json.Num(s, "fpsNum", 30) / Math.Max(1, Json.Num(s, "fpsDen", 1));
        v.FrameRate = fps;
        v.PixelAspectRatio = Json.Num(s, "pixelAspect", 1.0);
        string fo = Json.Text(s, "fieldOrder");
        v.FieldOrder = fo == "upper" ? VideoFieldOrder.UpperFieldFirst : fo == "lower" ? VideoFieldOrder.LowerFieldFirst : VideoFieldOrder.ProgressiveScan;
        string rq = Json.Text(s, "renderQuality");
        v.RenderQuality = rq == "draft" ? VideoRenderQuality.Draft : rq == "preview" ? VideoRenderQuality.Preview : rq == "best" ? VideoRenderQuality.Best : VideoRenderQuality.Good;
        string di = Json.Text(s, "deinterlace");
        v.DeinterlaceMethod = di == "blend" ? VideoDeinterlaceMethod.BlendFields : di == "interpolate" ? VideoDeinterlaceMethod.InterpolateFields : VideoDeinterlaceMethod.None;
        v.PixelFormat = Json.Text(s, "pixelFormat") == "32bit" ? PixelFormat.Float32Bit : PixelFormat.Int8Bit;
        string mb = Json.Text(s, "motionBlur");
        foreach (MotionBlurType t in Enum.GetValues(typeof(MotionBlurType)))
            if (t.ToString().ToLowerInvariant() == mb) v.MotionBlurType = t;
        uint rate = (uint)Json.Num(s, "sampleRate", 48000);
        if (rate >= 8000) proj.Audio.SampleRate = rate;
        uint bits = (uint)Json.Num(s, "audioBits", 16);
        if (bits == 16 || bits == 24 || bits == 32) proj.Audio.BitDepth = bits;
        double tempo = Json.Num(s, "tempo", 120);
        if (tempo >= 10 && tempo <= 1000) proj.Ruler.BeatsPerMinute = tempo;
        uint bpm = (uint)Json.Num(s, "beatsPerMeasure", 4);
        if (bpm >= 1 && bpm <= 32) proj.Ruler.BeatsPerMeasure = bpm;
        proj.Summary.Title = Json.Text(s, "title");
        proj.Summary.Artist = Json.Text(s, "artist");
        proj.Summary.Engineer = Json.Text(s, "engineer");
        proj.Summary.Copyright = Json.Text(s, "copyright");
        proj.Summary.Comments = Json.Text(s, "comments");
    }

    // -------------------------------------------------------------------- buses
    void Bus(object b)
    {
        AudioBusTrack bus = proj.AddAudioBusTrack();
        buses[Json.Id(b, "id")] = bus;
        string name = Json.Text(b, "name");
        if (name.Length > 0 && name != bus.Name) bus.Description = name;
        bus.Mute = Json.Bool(b, "mute", false);
        bus.Solo = Json.Bool(b, "solo", false);
        double level = Json.Num(b, "level", 1.0);
        if (Math.Abs(level - 1.0) > 1e-6) Problem("Bus '" + name + "': its level (" + Db(level) + ") could not be set: VEGAS's scripting has no bus fader.");
        if (Json.Id(b, "output") != 0) Problem("Bus '" + name + "' goes to another bus in Reno; in VEGAS it goes to the master.");
        Chain(bus.Effects, Json.Items(b, "fx"), "bus '" + name + "'", false);
    }

    // -------------------------------------------------------------------- plug-ins
    // Every node below 'root', depth first (VEGAS files plug-ins in folders).
    static IEnumerable<PlugInNode> AllNodes(PlugInNode root)
    {
        if (root == null) yield break;
        foreach (PlugInNode n in root)
        {
            yield return n;
            if (n.IsContainer)
                foreach (PlugInNode k in AllNodes(n)) yield return k;
        }
    }

    // An OpenFX plug-in by its identifier ("com.vegascreativesoftware:sharpen"), as VEGAS names it ("{Svfx:...}").
    PlugInNode OfxNode(PlugInNode root, string id)
    {
        string want = "{Svfx:" + id + "}";
        foreach (PlugInNode n in AllNodes(root))
            if (!n.IsContainer && string.Equals(n.UniqueID, want, StringComparison.OrdinalIgnoreCase)) return n;
        return null;
    }

    static readonly byte[] VstTail = { 0x02, 0x69, 0x00, 0x07, 0xe9, 0x5d, 0x36, 0xfa };

    // An audio plug-in from Reno's description: DirectX by its CLSID ("dshow:{...}"), VST2 by its id (VEGAS's
    // CLSID for a VST plug-in starts with the id backwards and ends with a fixed tail), else by name.
    PlugInNode AudioNode(object desc)
    {
        string format = Json.Text(desc, "format"), uid = Json.Text(desc, "uid"), name = Json.Text(desc, "name");
        Guid dx = Guid.Empty;
        if (format == "DX" && uid.StartsWith("dshow:", StringComparison.Ordinal))
        {
            try { dx = new Guid(uid.Substring(6)); } catch { }
        }
        uint vstId = 0;
        bool vst = format == "VST2" && uint.TryParse(uid, NumberStyles.Integer, CultureInfo.InvariantCulture, out vstId);
        PlugInNode byName = null;
        foreach (PlugInNode n in AllNodes(vegas.AudioFX))
        {
            if (n.IsContainer) continue;
            Guid g = n.ClassID;
            if (dx != Guid.Empty && g == dx) return n;
            if (vst)
            {
                byte[] b = g.ToByteArray();
                uint id = ((uint)b[3] << 24) | ((uint)b[2] << 16) | ((uint)b[1] << 8) | b[0];
                bool tail = true;
                for (int i = 0; i < 8; i++) tail = tail && b[8 + i] == VstTail[i];
                if (tail && id == vstId) return n;
            }
            if (byName == null && name.Length > 0 && string.Equals(n.Name, name, StringComparison.OrdinalIgnoreCase)) byName = n;
        }
        return byName;
    }

    // A chain of Reno effect slots onto a VEGAS effect list.
    void Chain(Effects into, List<object> slots, string where, bool video)
    {
        foreach (object slot in slots)
        {
            string kind = Json.Text(slot, "kind");
            if (video != (kind == "ofx"))
            {
                Problem(where + ": a" + (video ? "n audio" : " video") + " effect is in the wrong place and was left out.");
                continue;
            }
            Effect fx = AddEffect(into, slot, where);
            if (fx != null) fx.Bypass = Json.Bool(slot, "bypass", false);
        }
    }

    Effect AddEffect(Effects into, object slot, string where)
    {
        string kind = Json.Text(slot, "kind");
        if (kind == "ofx")
        {
            string id = Json.Text(slot, "plugin");
            PlugInNode node = OfxNode(vegas.VideoFX, id);
            if (node == null) { Problem(where + ": the video effect '" + id + "' is not installed in VEGAS; it was left out."); return null; }
            Effect fx = into.AddEffect(node);
            effectsMade++;
            ApplyOfx(fx, Json.Get(slot, "state"), where);
            return fx;
        }
        object desc = Json.Get(slot, "audio");
        PlugInNode an = AudioNode(desc);
        string name = Json.Text(desc, "name");
        if (an == null) { Problem(where + ": the audio effect '" + name + "' was not found in VEGAS; it was left out."); return null; }
        Effect afx = into.AddEffect(an);
        effectsMade++;
        if (Json.Get(slot, "state") is string && Json.Text(slot, "state").Length > 0)
            Problem(where + ": the settings of audio effect '" + name + "' could not be brought over (VEGAS's scripting cannot load a plug-in's state); it has its defaults.");
        return afx;
    }

    // Reno's OpenFX state ({"params": {name: {"v": [...], "k": [[frame, ...]...]} or {"s": text}}}) onto an
    // effect VEGAS made.
    void ApplyOfx(Effect fx, object state, string where)
    {
        if (fx == null || !fx.IsOFX || state == null) return;
        OFXEffect ofx = fx.OFXEffect;
        Dictionary<string, object> ps = Json.Get(state, "params") as Dictionary<string, object>;
        if (ofx == null || ps == null) return;
        foreach (KeyValuePair<string, object> e in ps)
        {
            OFXParameter p = ofx.FindParameterByName(e.Key);
            if (p == null) { Problem(where + ": " + Json.Text(state, "plugin") + " has no setting '" + e.Key + "' in this VEGAS; it was skipped."); continue; }
            try
            {
                if (Json.Get(e.Value, "s") is string) { SetText(p, Json.Text(e.Value, "s")); continue; }
                List<object> v = Json.Items(e.Value, "v");
                List<object> keys = Json.Items(e.Value, "k");
                if (keys.Count == 0) { SetValue(p, v, 0, null); continue; }
                p.IsAnimated = true;
                foreach (object k in keys)
                {
                    List<object> kl = k as List<object>;
                    if (kl == null || kl.Count < 2) continue;
                    SetValue(p, kl, 1, Timecode.FromSeconds(Json.At(kl, 0, 0) / fps));
                }
            }
            catch (Exception ex)
            {
                Problem(where + ": setting '" + e.Key + "' could not be set (" + ex.Message + ").");
            }
        }
        ofx.AllParametersChanged();
    }

    static void SetText(OFXParameter p, string s)
    {
        OFXStringParameter sp = p as OFXStringParameter;
        if (sp != null) { sp.Value = s; return; }
        OFXCustomParameter cp = p as OFXCustomParameter;
        if (cp != null) cp.Value = s;
    }

    // The numbers from list[first...] as the parameter's value, at 'at' (a keyframe) or as its plain value.
    static void SetValue(OFXParameter p, List<object> list, int first, Timecode at)
    {
        double a = Json.At(list, first, 0), b = Json.At(list, first + 1, 0), c = Json.At(list, first + 2, 0), d = Json.At(list, first + 3, 1);
        switch (p.ParameterType)
        {
            case OFXParameterType.Double:
                {
                    OFXDoubleParameter q = (OFXDoubleParameter)p;
                    if (at == null) q.Value = a; else q.SetValueAtTime(at, a);
                    break;
                }
            case OFXParameterType.Integer:
                {
                    OFXIntegerParameter q = (OFXIntegerParameter)p;
                    if (at == null) q.Value = (int)Math.Round(a); else q.SetValueAtTime(at, (int)Math.Round(a));
                    break;
                }
            case OFXParameterType.Boolean:
                {
                    OFXBooleanParameter q = (OFXBooleanParameter)p;
                    if (at == null) q.Value = a != 0; else q.SetValueAtTime(at, a != 0);
                    break;
                }
            case OFXParameterType.Choice:
                {
                    OFXChoiceParameter q = (OFXChoiceParameter)p;
                    int i = (int)Math.Round(a);
                    if (q.Choices == null || i < 0 || i >= q.Choices.Length) break;
                    if (at == null) q.Value = q.Choices[i]; else q.SetValueAtTime(at, q.Choices[i]);
                    break;
                }
            case OFXParameterType.RGB:
                {
                    OFXRGBParameter q = (OFXRGBParameter)p;
                    OFXColor col = new OFXColor(a, b, c);
                    if (at == null) q.Value = col; else q.SetValueAtTime(at, col);
                    break;
                }
            case OFXParameterType.RGBA:
                {
                    OFXRGBAParameter q = (OFXRGBAParameter)p;
                    OFXColor col = new OFXColor(a, b, c, d);
                    if (at == null) q.Value = col; else q.SetValueAtTime(at, col);
                    break;
                }
            case OFXParameterType.Double2D:
                {
                    OFXDouble2DParameter q = (OFXDouble2DParameter)p;
                    OFXDouble2D v = new OFXDouble2D();
                    v.X = a; v.Y = b;
                    if (at == null) q.Value = v; else q.SetValueAtTime(at, v);
                    break;
                }
            case OFXParameterType.Double3D:
                {
                    OFXDouble3DParameter q = (OFXDouble3DParameter)p;
                    OFXDouble3D v = new OFXDouble3D();
                    v.X = a; v.Y = b; v.Z = c;
                    if (at == null) q.Value = v; else q.SetValueAtTime(at, v);
                    break;
                }
            case OFXParameterType.Integer2D:
                {
                    OFXInteger2DParameter q = (OFXInteger2DParameter)p;
                    OFXInteger2D v = new OFXInteger2D();
                    v.X = (int)Math.Round(a); v.Y = (int)Math.Round(b);
                    if (at == null) q.Value = v; else q.SetValueAtTime(at, v);
                    break;
                }
            case OFXParameterType.Integer3D:
                {
                    OFXInteger3DParameter q = (OFXInteger3DParameter)p;
                    OFXInteger3D v = new OFXInteger3D();
                    v.X = (int)Math.Round(a); v.Y = (int)Math.Round(b); v.Z = (int)Math.Round(c);
                    if (at == null) q.Value = v; else q.SetValueAtTime(at, v);
                    break;
                }
            default:
                break;
        }
    }

    // -------------------------------------------------------------------- media
    Media MediaOf(long id)
    {
        Media m;
        if (media.TryGetValue(id, out m)) return m;
        object j;
        if (!mediaJson.TryGetValue(id, out j)) return null;
        media[id] = null;                       // (a failure is not tried again)
        string name = Json.Text(j, "name");
        if (Json.Bool(j, "generated", false))
        {
            object gen = Json.Get(j, "generator");
            string pid = Json.Text(gen, "plugin");
            PlugInNode node = OfxNode(vegas.Generators, pid);
            if (node == null) { Problem("Generated media '" + name + "': the generator '" + pid + "' is not installed in VEGAS; its events were left out."); return null; }
            m = Media.CreateInstance(proj, node);
            ApplyOfx(m.Generator, Json.Get(gen, "state"), "generated media '" + name + "'");
        }
        else
        {
            string path = Json.Text(j, "path");
            if (!File.Exists(path))
            {
                // a project moved with its media: look next to the .reno
                string near = Path.Combine(renoDir, Path.GetFileName(path));
                if (File.Exists(near)) path = near;
            }
            if (!File.Exists(path)) { Problem("Media '" + path + "' could not be found; its events were left out."); return null; }
            try { m = proj.MediaPool.AddMedia(path); }
            catch (Exception e) { Problem("Media '" + path + "' could not be opened by VEGAS (" + e.Message + "); its events were left out."); return null; }
        }
        media[id] = m;
        return m;
    }

    // -------------------------------------------------------------------- tracks
    static CompositeMode CompositeOf(string s)
    {
        switch (s)
        {
            case "add": return CompositeMode.Add;
            case "subtract": return CompositeMode.Subtract;
            case "multiply": return CompositeMode.Multiply;
            case "cut": return CompositeMode.Cut;
            case "screen": return CompositeMode.Screen;
            case "overlay": return CompositeMode.Overlay;
            case "hardlight": return CompositeMode.HardLight;
            case "dodge": return CompositeMode.Dodge;
            case "burn": return CompositeMode.Burn;
            case "darken": return CompositeMode.Darken;
            case "lighten": return CompositeMode.Lighten;
            case "difference": return CompositeMode.Difference;
            case "diffsquared": return CompositeMode.DifferenceSquared;
            default: return CompositeMode.SrcAlpha;
        }
    }

    static VideoKeyframeType KeyTypeOf(double curve)
    {
        int c = (int)Math.Round(curve);
        return c == 1 ? VideoKeyframeType.Hold : c == 2 ? VideoKeyframeType.Smooth : VideoKeyframeType.Linear;
    }

    void TrackOf(object t, int index)
    {
        bool video = Json.Text(t, "type") != "audio";
        string name = Json.Text(t, "name");
        string label = "Track " + (index + 1) + (name.Length > 0 ? " ('" + name + "')" : "");
        Track track;
        if (video)
        {
            VideoTrack vt = new VideoTrack(proj, index, name);
            proj.Tracks.Add(vt);
            vt.CompositeLevel = (float)Json.Num(t, "level", 1.0);
            vt.SetCompositeMode(CompositeOf(Json.Text(t, "composite")), false);
            List<object> motion = Json.Items(t, "motion");
            if (motion.Count > 0) Motion(vt.TrackMotion, motion);
            // compositing parent/child: a child sits one level in from the track above it
            int nest = (int)Json.Num(t, "compositeLevel", 0);
            if (nest > 0) vt.CompositeNestingLevel = nest;
            string pmode = Json.Text(t, "parentComposite");
            if (pmode.Length > 0 && pmode != "srcalpha") parentModes.Add(new KeyValuePair<VideoTrack, string>(vt, pmode));
            List<object> pmotion = Json.Items(t, "parentMotion");
            if (pmotion.Count > 0) Motion(vt.ParentTrackMotion, pmotion);
            track = vt;
        }
        else
        {
            AudioTrack at = new AudioTrack(proj, index, name);
            proj.Tracks.Add(at);
            at.Volume = (float)Json.Num(t, "level", 1.0);
            at.PanX = (float)Json.Num(t, "pan", 0);
            AudioBusTrack bus;
            long output = Json.Id(t, "output");
            if (output != 0 && buses.TryGetValue(output, out bus)) at.BusTrack = bus;
            if (Json.Items(t, "sends").Count > 0) Problem(label + ": its sends to buses could not be set (VEGAS's scripting has no sends).");
            at.InvertPhase = Json.Bool(t, "invertPhase", false);
            string panType = Json.Text(t, "panType");
            if (panType.Length > 0 && panType != "balance0")
                Problem(label + ": its pan type (" + panType + ") could not be set (VEGAS's scripting sets pan types of buses only).");
            track = at;
        }
        tracksMade++;
        track.Mute = Json.Bool(t, "mute", false);
        track.Solo = Json.Bool(t, "solo", false);
        Chain(track.Effects, Json.Items(t, "fx"), label, video);
        EnvelopesOf(track.Envelopes, Json.Items(t, "envelopes"), label, track);
        foreach (object e in Json.Items(t, "events")) EventOf(track, e, video, label);
    }

    // Reno's track motion keys ([time, x, y, w, h, rotation, curve]: centre and size as fractions of the frame,
    // y down, degrees clockwise) as VEGAS's: pixels from the frame's centre, y up, radians.
    void Motion(TrackMotion tm, List<object> keys)
    {
        double pw = proj.Video.Width, ph = proj.Video.Height;
        foreach (object ko in keys)
        {
            double time = Json.At(ko, 0, 0);
            TrackMotionKeyframe k = null;
            foreach (TrackMotionKeyframe have in tm.MotionKeyframes)
                if (Math.Abs(Conv(have.Position) - time) < 1e-6) k = have;
            if (k == null) k = tm.InsertMotionKeyframe(Timecode.FromSeconds(time));
            k.PositionX = (Json.At(ko, 1, 0.5) - 0.5) * pw;
            k.PositionY = (0.5 - Json.At(ko, 2, 0.5)) * ph;
            k.Width = Json.At(ko, 3, 1) * pw;
            k.Height = Json.At(ko, 4, 1) * ph;
            k.RotationZ = -Json.At(ko, 5, 0) * Math.PI / 180.0;
            k.Type = KeyTypeOf(Json.At(ko, 6, 0));
        }
    }

    static double Conv(Timecode t) { return t == null ? 0 : t.ToMilliseconds() / 1000.0; }

    // -------------------------------------------------------------------- events
    void EventOf(Track track, object e, bool video, string label)
    {
        double start = Json.Num(e, "start", 0), length = Json.Num(e, "length", 0);
        string where = label + ", event at " + start.ToString("0.000", CultureInfo.InvariantCulture) + " s";
        if (!(length > 0)) return;
        Media m = MediaOf(Json.Id(e, "media"));
        if (m == null) { Problem(where + ": its media is missing; it was left out."); return; }
        double rate = Json.Num(e, "rate", 1.0);
        if (!(rate > 0)) rate = 1.0;
        double offset = Json.Num(e, "offset", 0);
        bool reverse = Json.Bool(e, "reverse", false);
        // A reversed event plays its stretch of the media backwards; VEGAS does that with a reversed subclip
        // of exactly that stretch.
        if (reverse && !m.IsGenerated())
        {
            try
            {
                Subclip sub = new Subclip(proj, m.FilePath, Timecode.FromSeconds(offset), Timecode.FromSeconds(length * rate), true, Path.GetFileName(m.FilePath) + " (reversed)");
                m = sub;
                offset = 0;
            }
            catch (Exception ex) { Problem(where + ": it plays backwards in Reno, but the reversed subclip could not be made (" + ex.Message + "); it plays forwards."); }
        }
        MediaStream stream = null;
        try { stream = video ? (MediaStream)m.GetVideoStreamByIndex(0) : (MediaStream)m.GetAudioStreamByIndex(0); } catch { }
        if (stream == null) { Problem(where + ": its media has no " + (video ? "picture" : "sound") + "; it was left out."); return; }
        if (m.IsGenerated())
        {
            // generated media is as long as VEGAS makes it unless told; make it long enough for this event
            double need = offset + length * rate;
            if (Conv(m.Length) < need) m.Length = Timecode.FromSeconds(need);
        }

        TrackEvent ev;
        if (video) ev = ((VideoTrack)track).AddVideoEvent(Timecode.FromSeconds(start), Timecode.FromSeconds(length));
        else ev = ((AudioTrack)track).AddAudioEvent(Timecode.FromSeconds(start), Timecode.FromSeconds(length));
        eventsMade++;
        ev.PlaybackRate = rate;
        Take take = ev.AddTake(stream, true);
        // VEGAS keeps a take's offset in timeline time: the position in the media divided by the rate (see
        // Export to Reno.cs; compare.cpp's "Take offset" table confirms or corrects this)
        take.Offset = Timecode.FromSeconds(offset / rate);
        string name = Json.Text(e, "name");
        if (name.Length > 0) ev.Name = name;
        ev.Mute = Json.Bool(e, "mute", false);
        ev.Loop = Json.Bool(e, "loop", true);
        ev.FadeIn.Length = Timecode.FromSeconds(Math.Max(0, Json.Num(e, "fadeIn", 0)));
        ev.FadeOut.Length = Timecode.FromSeconds(Math.Max(0, Json.Num(e, "fadeOut", 0)));
        ev.FadeIn.Curve = CurveOf(Json.Num(e, "fadeInCurve", 0));
        ev.FadeOut.Curve = CurveOf(Json.Num(e, "fadeOutCurve", 0));
        if (Json.Num(e, "fadeInRecipCurve", -1) >= 0) ev.FadeIn.ReciprocalCurve = CurveOf(Json.Num(e, "fadeInRecipCurve", 0));
        double gain = Json.Num(e, "gain", 1.0);
        if (video)
        {
            ev.FadeIn.Gain = (float)Math.Max(0, Math.Min(1, gain));
            VideoEvent ve = (VideoEvent)ev;
            ve.MaintainAspectRatio = Json.Bool(e, "keepAspect", true);
            ve.ReduceInterlace = Json.Bool(e, "reduceFlicker", false);
            string rs = Json.Text(e, "resample");
            if (rs == "force") ve.ResampleMode = VideoResampleMode.Force;
            else if (rs == "off") ve.ResampleMode = VideoResampleMode.Disable;
            double us = Json.Num(e, "undersample", 1.0);
            if (us > 0 && us < 1.0) ve.UnderSampleRate = us;
            EnvelopesOf(ve.Envelopes, Json.Items(e, "envelopes"), where, null);
            List<object> pc = Json.Items(e, "panCrop");
            if (pc.Count > 0) PanCrop(ve, stream as VideoStream, pc, where);
            Chain(ve.Effects, Json.Items(e, "fx"), where, true);
            object tr = Json.Get(e, "transition");
            if (tr != null && Json.Text(tr, "kind") == "ofx")
            {
                PlugInNode node = OfxNode(vegas.Transitions, Json.Text(tr, "plugin"));
                if (node == null) Problem(where + ": the transition '" + Json.Text(tr, "plugin") + "' is not installed in VEGAS; a cross-fade is used.");
                else
                {
                    Effect fx = new Effect(node);
                    ev.FadeIn.Transition = fx;
                    ApplyOfx(fx, Json.Get(tr, "state"), where + " (transition)");
                }
            }
        }
        else
        {
            AudioEvent ae = (AudioEvent)ev;
            // gain above 0 dB is what VEGAS's normalising gain is for
            if (gain > 1.0) { ev.FadeIn.Gain = 1.0f; ae.SetNormalize(true, gain); }
            else ev.FadeIn.Gain = (float)Math.Max(0, gain);
            // Normalize: VEGAS works out its own gain from the sound
            if (Json.Bool(e, "normalize", false)) { ae.Normalize = true; try { ae.RecalculateNorm(); } catch { } }
            ae.InvertPhase = Json.Bool(e, "invertPhase", false);
            if (Json.Items(e, "envelopes").Count > 0) Problem(where + ": its envelopes could not be set (VEGAS's scripting has none on audio events).");
            bool keepPitch = Json.Bool(e, "keepPitch", false);
            ae.Method = keepPitch ? TimeStretchPitchShift.Elastique : TimeStretchPitchShift.None;
            if (keepPitch)
            {
                ae.PitchLock = false;
                ae.PitchSemis = Json.Num(e, "pitch", 0);
            }
            else if (Math.Abs(Json.Num(e, "pitch", 0)) > 1e-9)
                Problem(where + ": its pitch change only applies with a stretch method; it plays at the pitch its rate gives.");
            Chain(ae.Effects, Json.Items(e, "fx"), where, false);
        }
        long group = Json.Id(e, "group");
        if (group != 0)
        {
            TrackEventGroup g;
            if (!groups.TryGetValue(group, out g))
            {
                g = new TrackEventGroup(proj);
                proj.Groups.Add(g);
                groups[group] = g;
            }
            g.Add(ev);
        }
    }

    // Reno's curve numbers (project.h: Linear, Fast, Slow, Smooth, Sharp, Hold) as VEGAS's.
    static CurveType CurveOf(double c)
    {
        switch ((int)Math.Round(c))
        {
            case 1: return CurveType.Fast;
            case 2: return CurveType.Slow;
            case 3: return CurveType.Smooth;
            case 4: return CurveType.Sharp;
            case 5: return CurveType.None;
            default: return CurveType.Linear;
        }
    }

    // Reno's envelopes ({"kind", "points": [[time, value, curve]...]}) onto a track's or a video event's. Values: Reno's
    // volume is a gain factor, pan -1 (left) .. 1, composite (opacity) 0..1, velocity the speed factor (1 = 100 %), as
    // VEGAS keeps them (Export to Reno.cs reads them back the same way). Times: timeline seconds for a track's,
    // seconds from the event's start for an event's.
    void EnvelopesOf(Envelopes into, List<object> envs, string where, Track track)
    {
        foreach (object eo in envs)
        {
            string kind = Json.Text(eo, "kind");
            EnvelopeType type;
            switch (kind)
            {
                case "volume": type = EnvelopeType.Volume; break;
                case "pan": type = EnvelopeType.Pan; break;
                case "composite": type = EnvelopeType.Composite; break;
                case "velocity": type = EnvelopeType.Velocity; break;
                case "mute": type = EnvelopeType.Mute; break;
                case "fadecolor": type = EnvelopeType.FadeToColor; break;
                default: Problem(where + ": its '" + kind + "' envelope could not be brought over."); continue;
            }
            List<object> pts = Json.Items(eo, "points");
            if (pts.Count == 0) continue;
            try
            {
                Envelope env = into.FindByType(type);
                if (env == null)
                {
                    env = new Envelope(type);
                    into.Add(env);
                }
                foreach (object po in pts)
                {
                    Timecode x = Timecode.FromSeconds(Json.At(po, 0, 0));
                    double y = Json.At(po, 1, 0);
                    CurveType c = CurveOf(Json.At(po, 2, 0));
                    EnvelopePoint have = env.Points.GetPointAtX(x);
                    if (have != null) { have.Y = y; have.Curve = c; }
                    else env.Points.Add(new EnvelopePoint(x, y, c));
                }
            }
            catch (Exception ex) { Problem(where + ": its " + kind + " envelope could not be set (" + ex.Message + ")."); }
        }
    }

    // Reno's pan/crop keys ([time, cx, cy, w, h, rotation, curve]: the box in the media's picture as fractions,
    // rotation in degrees clockwise) as VEGAS's: the box's corners in the media's pixels.
    void PanCrop(VideoEvent ve, VideoStream vs, List<object> keys, string where)
    {
        if (vs == null || vs.Width <= 0 || vs.Height <= 0) { Problem(where + ": its pan/crop could not be set (the media's picture size is not known)."); return; }
        double mw = vs.Width, mh = vs.Height;
        VideoMotionKeyframes list = ve.VideoMotion.Keyframes;
        for (int i = 0; i < keys.Count; i++)
        {
            object ko = keys[i];
            double time = Json.At(ko, 0, 0);
            VideoMotionKeyframe k;
            if (i == 0 && list.Count > 0 && Math.Abs(time) < 1e-9) k = list[0];
            else
            {
                k = new VideoMotionKeyframe(proj, Timecode.FromSeconds(time));
                list.Add(k);
            }
            double cx = Json.At(ko, 1, 0.5) * mw, cy = Json.At(ko, 2, 0.5) * mh, w = Json.At(ko, 3, 1) * mw, h = Json.At(ko, 4, 1) * mh;
            double a = Json.At(ko, 5, 0) * Math.PI / 180.0;          // clockwise with y down
            double ca = Math.Cos(a), sa = Math.Sin(a);
            // the corners about the centre, turned
            double[] dx = { -w / 2, w / 2, w / 2, -w / 2 }, dy = { -h / 2, -h / 2, h / 2, h / 2 };
            VideoMotionVertex[] v = new VideoMotionVertex[4];
            for (int c = 0; c < 4; c++)
                v[c] = new VideoMotionVertex((float)(cx + dx[c] * ca - dy[c] * sa), (float)(cy + dx[c] * sa + dy[c] * ca));
            k.Bounds = new VideoMotionBounds(v[0], v[1], v[2], v[3]);
            k.Center = new VideoMotionVertex((float)cx, (float)cy);
            // VEGAS's own rotation number turns anticlockwise for a positive angle (as Reno's importer reads it)
            k.Rotation = -a;
            k.Type = KeyTypeOf(Json.At(ko, 6, 0));
        }
    }
}
