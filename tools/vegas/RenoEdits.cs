// RenoEdits: run inside VEGAS Pro 18 (Tools > Scripting > Run Script...), or in Reno (Tools > Scripting > Run Script...).
// It builds small projects, applies each editing operation to them - through the scripting API (TrackEvent.Split,
// AdjustStartLength, Copy ...) and through VEGAS's own Edit commands (Vegas.InvokeCommand: Split, Trim Start, Delete,
// Cut / Paste, Paste Insert, Post-Edit Ripple ...) - and writes, for every scenario, a folder with
//
//     before.veg  after.veg   the project before and after the operations
//     result.json             what the program reports for both (event positions, lengths, take offsets, fades,
//                             envelopes, Pan/Crop keys, effects, groups ... in 100 ns ticks) and the operations done
//
// Reno's tools\vegas\editparity_compare.py compares two such folders (VEGAS's and Reno's), and Reno's own replay
// program (tests\ui\editparity_replay.cpp) redoes the operations of a VEGAS result on its before.veg.
//
// What it needs: nothing but VEGAS. It writes its own test media (a 20 s stereo WAV and a 20 s uncompressed AVI) into
// the output folder. Nothing else on the computer is touched; every scenario starts a new project (the open project is
// closed WITHOUT saving: save your work first). Each step has its own try/catch, so a failure only shows in the log.
// The whole run takes a few minutes. Zip the output folder and upload it.
//
// C# 5 (VEGAS 18's compiler), compiled against ScriptPortal.Vegas.dll.

using System;
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
        string dir = null;
        try
        {
            if (vegas.Project != null && vegas.Project.IsModified)
            {
                DialogResult r = MessageBox.Show("RenoEdits closes the open project WITHOUT saving it for every scenario it runs.\n\n" +
                    "Save your work first (No), or go on and lose the changes (Yes)?", "RenoEdits", MessageBoxButtons.YesNo, MessageBoxIcon.Warning);
                if (r != DialogResult.Yes) return;
            }
            FolderBrowserDialog d = new FolderBrowserDialog();
            d.Description = "RenoEdits: pick (or make) an empty folder for the results";
            d.SelectedPath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "RenoEdits");
            if (d.ShowDialog() != DialogResult.OK) return;
            dir = d.SelectedPath;
            Directory.CreateDirectory(dir);
        }
        catch (Exception e)
        {
            MessageBox.Show("RenoEdits could not start: " + e.Message);
            return;
        }
        EditRunner run = new EditRunner(vegas, dir);
        string summary;
        try { summary = run.RunAll(); }
        catch (Exception e) { summary = "stopped by an error: " + e; run.Log("FATAL " + e); }
        run.Close();
        MessageBox.Show(summary + "\n\nZip the folder and upload it:\n" + dir, "RenoEdits");
    }
}

// ------------------------------------------------------------------------------------------------ JSON
public class JO
{
    public List<string> Keys = new List<string>();
    public List<object> Vals = new List<object>();
    public JO Add(string key, object value) { Keys.Add(key); Vals.Add(value); return this; }
}

public static class Js
{
    public static string Make(object v)
    {
        StringBuilder sb = new StringBuilder();
        Write(sb, v, 0);
        sb.Append('\n');
        return sb.ToString();
    }

    static void Indent(StringBuilder sb, int n) { sb.Append('\n'); sb.Append(' ', n * 2); }

    public static void Write(StringBuilder sb, object v, int depth)
    {
        if (v == null) { sb.Append("null"); return; }
        if (v is bool) { sb.Append((bool)v ? "true" : "false"); return; }
        if (v is string) { Quote(sb, (string)v); return; }
        if (v is double || v is float)
        {
            double d = Convert.ToDouble(v);
            if (double.IsNaN(d) || double.IsInfinity(d)) sb.Append("null");
            else sb.Append(d.ToString("R", CultureInfo.InvariantCulture));
            return;
        }
        if (v is int || v is long || v is short || v is byte || v is uint || v is ulong)
        {
            sb.Append(Convert.ToInt64(v).ToString(CultureInfo.InvariantCulture));
            return;
        }
        JO o = v as JO;
        if (o != null)
        {
            if (o.Keys.Count == 0) { sb.Append("{}"); return; }
            sb.Append('{');
            for (int i = 0; i < o.Keys.Count; i++)
            {
                if (i > 0) sb.Append(',');
                Indent(sb, depth + 1);
                Quote(sb, o.Keys[i]);
                sb.Append(": ");
                Write(sb, o.Vals[i], depth + 1);
            }
            Indent(sb, depth);
            sb.Append('}');
            return;
        }
        List<object> list = v as List<object>;
        if (list != null)
        {
            if (list.Count == 0) { sb.Append("[]"); return; }
            // short lists of plain numbers stay on one line
            bool flat = true;
            foreach (object x in list) if (x is JO || x is List<object>) { flat = false; break; }
            sb.Append('[');
            for (int i = 0; i < list.Count; i++)
            {
                if (i > 0) sb.Append(flat ? ", " : ",");
                if (!flat) Indent(sb, depth + 1);
                Write(sb, list[i], depth + 1);
            }
            if (!flat) Indent(sb, depth);
            sb.Append(']');
            return;
        }
        Quote(sb, v.ToString());
    }

    static void Quote(StringBuilder sb, string s)
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
                    if (c < ' ') sb.Append("\\u").Append(((int)c).ToString("x4"));
                    else sb.Append(c);
                    break;
            }
        }
        sb.Append('"');
    }
}

// ------------------------------------------------------------------------------------------------ test media
public static class TestMedia
{
    // 20 s of stereo 48 kHz 16-bit: a tone that climbs with the time (so an offset can be heard and measured), a click on
    // every second, and the left channel a little louder
    public static void WriteWav(string path)
    {
        int rate = 48000, seconds = 20, frames = rate * seconds;
        using (BinaryWriter w = new BinaryWriter(File.Create(path)))
        {
            int dataBytes = frames * 4;
            w.Write(new char[] { 'R', 'I', 'F', 'F' });
            w.Write(36 + dataBytes);
            w.Write(new char[] { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
            w.Write(16);
            w.Write((short)1);
            w.Write((short)2);
            w.Write(rate);
            w.Write(rate * 4);
            w.Write((short)4);
            w.Write((short)16);
            w.Write(new char[] { 'd', 'a', 't', 'a' });
            w.Write(dataBytes);
            double phase = 0;
            for (int i = 0; i < frames; i++)
            {
                double t = (double)i / rate;
                double freq = 220.0 + 40.0 * t;
                phase += 2.0 * Math.PI * freq / rate;
                double v = 0.3 * Math.Sin(phase);
                double inSecond = t - Math.Floor(t);
                if (inSecond < 0.004) v += 0.5 * Math.Sin(inSecond * 2.0 * Math.PI * 1000.0);
                short l = (short)(Math.Max(-1.0, Math.Min(1.0, v)) * 30000);
                short r = (short)(Math.Max(-1.0, Math.Min(1.0, v * 0.8)) * 30000);
                w.Write(l);
                w.Write(r);
            }
        }
    }

    // 20 s of uncompressed 24-bit video, 64 x 48, 30 fps: the colour changes every frame and a bar runs across
    public static void WriteAvi(string path)
    {
        int width = 64, height = 48, fps = 30, frames = 20 * fps;
        int stride = width * 3;                 // (64 * 3 is a multiple of 4: no row padding)
        int frameBytes = stride * height;
        using (BinaryWriter w = new BinaryWriter(File.Create(path)))
        {
            int movi = 4 + frames * (8 + frameBytes);
            int hdrl = 4 + (8 + 56) + (8 + 4 + (8 + 56) + (8 + 40));        // 'hdrl', avih, and the strl LIST (header, 'strl', strh, strf)
            int riff = 4 + (12 + hdrl - 4) + (12 + movi - 4) + (8 + frames * 16);
            w.Write(new char[] { 'R', 'I', 'F', 'F' });
            w.Write(riff);
            w.Write(new char[] { 'A', 'V', 'I', ' ' });
            w.Write(new char[] { 'L', 'I', 'S', 'T' });
            w.Write(hdrl);
            w.Write(new char[] { 'h', 'd', 'r', 'l' });
            w.Write(new char[] { 'a', 'v', 'i', 'h' });
            w.Write(56);
            w.Write(1000000 / fps);
            w.Write(frameBytes * fps);
            w.Write(0);
            w.Write(0x10);                      // has an index
            w.Write(frames);
            w.Write(0);
            w.Write(1);
            w.Write(frameBytes);
            w.Write(width);
            w.Write(height);
            w.Write(0); w.Write(0); w.Write(0); w.Write(0);
            w.Write(new char[] { 'L', 'I', 'S', 'T' });
            w.Write(4 + (8 + 56) + (8 + 40));
            w.Write(new char[] { 's', 't', 'r', 'l' });
            w.Write(new char[] { 's', 't', 'r', 'h' });
            w.Write(56);
            w.Write(new char[] { 'v', 'i', 'd', 's' });
            w.Write(new char[] { 'D', 'I', 'B', ' ' });
            w.Write(0);
            w.Write((short)0);
            w.Write((short)0);
            w.Write(0);
            w.Write(1);                         // scale
            w.Write(fps);                       // rate
            w.Write(0);
            w.Write(frames);
            w.Write(frameBytes);
            w.Write(-1);
            w.Write(0);
            w.Write((short)0); w.Write((short)0); w.Write((short)width); w.Write((short)height);
            w.Write(new char[] { 's', 't', 'r', 'f' });
            w.Write(40);
            w.Write(40);
            w.Write(width);
            w.Write(height);
            w.Write((short)1);
            w.Write((short)24);
            w.Write(0);
            w.Write(frameBytes);
            w.Write(0); w.Write(0); w.Write(0); w.Write(0);
            w.Write(new char[] { 'L', 'I', 'S', 'T' });
            w.Write(movi);
            w.Write(new char[] { 'm', 'o', 'v', 'i' });
            byte[] row = new byte[frameBytes];
            for (int f = 0; f < frames; f++)
            {
                int hue = (f * 7) % 256;
                int bar = (f * 64 / (fps * 2)) % width;
                for (int y = 0; y < height; y++)
                    for (int x = 0; x < width; x++)
                    {
                        int o = (height - 1 - y) * stride + x * 3;      // bottom-up
                        bool inBar = x >= bar && x < bar + 4;
                        row[o] = (byte)(inBar ? 255 : hue);
                        row[o + 1] = (byte)(inBar ? 255 : 255 - hue);
                        row[o + 2] = (byte)(inBar ? 255 : (f % 30) * 8);
                    }
                w.Write(new char[] { '0', '0', 'd', 'b' });
                w.Write(frameBytes);
                w.Write(row);
            }
            w.Write(new char[] { 'i', 'd', 'x', '1' });
            w.Write(frames * 16);
            for (int f = 0; f < frames; f++)
            {
                w.Write(new char[] { '0', '0', 'd', 'b' });
                w.Write(0x10);
                w.Write(4 + f * (8 + frameBytes));
                w.Write(frameBytes);
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------ the runner
public delegate void ScenarioBody(EditRunner c);

public class EditRunner
{
    public Vegas vegas;
    public Project proj;
    string outDir;
    StreamWriter log;
    string wavPath, aviPath;
    public Media wav, avi;
    string scenarioDir;
    JO current;                         // the result being built
    List<object> ops;
    int ok, failed, uiStyle = -1;
    List<string> failures = new List<string>();
    List<TrackEventGroup> groupList = new List<TrackEventGroup>();

    public EditRunner(Vegas v, string dir)
    {
        vegas = v;
        outDir = dir;
        log = new StreamWriter(Path.Combine(dir, "log.txt"), false, Encoding.UTF8);
        log.AutoFlush = true;
        Log("RenoEdits " + DateTime.Now.ToString("s"));
        try { Log("VEGAS " + vegas.Version); } catch (Exception) { }
    }

    public void Close() { try { log.Close(); } catch (Exception) { } }

    public void Log(string s)
    {
        try { log.WriteLine(s); } catch (Exception) { }
    }

    // ---- helpers for times
    public static Timecode Sec(double s) { return Timecode.FromSeconds(s); }
    public static long Ticks(Timecode t) { return t.Nanos; }

    // ---- the scenarios
    List<KeyValuePair<string, ScenarioBody>> scenarios = new List<KeyValuePair<string, ScenarioBody>>();

    void Add(string name, ScenarioBody body) { scenarios.Add(new KeyValuePair<string, ScenarioBody>(name, body)); }

    public string RunAll()
    {
        wavPath = Path.Combine(outDir, "media_tone.wav");
        aviPath = Path.Combine(outDir, "media_video.avi");
        TestMedia.WriteWav(wavPath);
        try { TestMedia.WriteAvi(aviPath); } catch (Exception e) { Log("could not write the AVI: " + e.Message); aviPath = null; }
        RegisterApiScenarios();
        RegisterUiScenarios();
        int done = 0;
        foreach (KeyValuePair<string, ScenarioBody> sc in scenarios)
        {
            Log("==== " + sc.Key);
            current = new JO();
            ops = new List<object>();
            current.Add("scenario", sc.Key);
            scenarioDir = Path.Combine(outDir, sc.Key);
            string err = null;
            try
            {
                Directory.CreateDirectory(scenarioDir);
                sc.Value(this);
            }
            catch (Exception e)
            {
                err = e.ToString();
                Log("SCENARIO FAILED " + sc.Key + ": " + e);
                failures.Add(sc.Key + ": " + e.Message);
                failed++;
            }
            current.Add("error", err);
            current.Add("ops", ops);
            try { File.WriteAllText(Path.Combine(scenarioDir, "result.json"), Js.Make(current), new UTF8Encoding(false)); }
            catch (Exception e) { Log("could not write result.json: " + e.Message); }
            done++;
        }
        try { vegas.NewProject(false, false); } catch (Exception) { }
        string s = done + " scenarios run; " + ok + " operations worked, " + failed + " failed (see log.txt).";
        Log(s);
        return s;
    }

    // ---- video media that VEGAS takes without any codec: first the plain AVI; if VEGAS refuses it, a PNG image sequence
    // (System.Drawing writes the pictures), and as the last resort a generated Solid Color of 20 s. Only the AVI can be
    // turned into a reversed subclip (a Subclip needs a file), so the reversed video scenarios need it.
    public string videoKind = "none";
    string seqFirst = null;
    int seqCount = 0;

    Media MakeVideoMedia()
    {
        videoKind = "none";
        if (aviPath != null)
        {
            try { Media m = proj.MediaPool.AddMedia(aviPath); videoKind = "avi"; return m; }
            catch (Exception e) { Log("VEGAS did not take the AVI: " + e.Message); }
        }
        try
        {
            if (seqFirst == null) WriteSequence();
            if (seqFirst != null)
            {
                Media m = proj.MediaPool.AddImageSequence(seqFirst, seqCount, 30.0);
                videoKind = "sequence";
                Log("  video media: PNG sequence of " + seqCount + " pictures");
                return m;
            }
        }
        catch (Exception e) { Log("VEGAS did not take the PNG sequence: " + e.Message); }
        try
        {
            PlugInNode node = vegas.Generators.GetChildByName("Solid Color");
            if (node == null) node = vegas.Generators.GetChildByName("VEGAS Solid Color");
            if (node != null)
            {
                Media m = Media.CreateInstance(proj, node);      // goes into the project's media pool by itself
                m.Length = Sec(20);
                videoKind = "generated";
                Log("  video media: generated Solid Color, 20 s");
                return m;
            }
            Log("no Solid Color generator found");
        }
        catch (Exception e) { Log("VEGAS did not make generated media: " + e.Message); }
        return null;
    }

    void WriteSequence()
    {
        string folder = Path.Combine(outDir, "media_sequence");
        Directory.CreateDirectory(folder);
        int fps = 30, frames = 20 * fps;
        for (int f = 0; f < frames; f++)
        {
            string file = Path.Combine(folder, "seq_" + f.ToString("D5", CultureInfo.InvariantCulture) + ".png");
            using (System.Drawing.Bitmap bmp = new System.Drawing.Bitmap(64, 48))
            {
                using (System.Drawing.Graphics g = System.Drawing.Graphics.FromImage(bmp))
                {
                    g.Clear(System.Drawing.Color.FromArgb((f * 7) % 256, 255 - (f * 7) % 256, (f % 30) * 8));
                    g.FillRectangle(System.Drawing.Brushes.White, (f * 64 / (fps * 2)) % 64, 0, 4, 48);
                }
                bmp.Save(file, System.Drawing.Imaging.ImageFormat.Png);
            }
            if (f == 0) seqFirst = file;
        }
        seqCount = frames;
    }

    // ---- a fresh project
    public void Begin(string description, int videoTracks, int audioTracks)
    {
        current.Add("description", description);
        if (!vegas.NewProject(false, false)) throw new Exception("VEGAS did not make a new project");
        proj = vegas.Project;
        try { proj.Video.FrameRate = 30.0; } catch (Exception e) { Log("frame rate 30: " + e.Message); }
        for (int i = 0; i < videoTracks; i++) proj.AddVideoTrack();
        for (int i = 0; i < audioTracks; i++) proj.AddAudioTrack();
        wav = proj.MediaPool.AddMedia(wavPath);
        avi = MakeVideoMedia();
        try { vegas.Cursor = Sec(0); vegas.SelectionStart = Sec(0); vegas.SelectionLength = Sec(0); } catch (Exception) { }
        JO p = new JO();
        try { p.Add("frameRate", proj.Video.FrameRate).Add("width", proj.Video.Width).Add("height", proj.Video.Height).Add("sampleRate", proj.Audio.SampleRate); } catch (Exception) { }
        p.Add("videoMedia", videoKind);
        current.Add("project", p);
    }

    // ---- making events (take offsets are given in seconds of the media / timeline, as the take reports them)
    public AudioEvent Audio(int track, double start, double length, double takeOffset, string name)
    {
        return Audio(track, start, length, takeOffset, 1.0, name);
    }

    public AudioEvent Audio(int track, double start, double length, double takeOffset, double rate, string name)
    {
        AudioTrack tr = (AudioTrack)proj.Tracks[track];
        AudioEvent ev = tr.AddAudioEvent(Sec(start), Sec(length));
        if (rate != 1.0) ev.PlaybackRate = rate;
        Take tk = ev.AddTake(wav.GetAudioStreamByIndex(0), true);
        tk.Offset = Sec(takeOffset);
        ev.Name = name;
        return ev;
    }

    public AudioEvent AudioReversed(int track, double start, double length, double mediaStart, double mediaLength, string name)
    {
        AudioTrack tr = (AudioTrack)proj.Tracks[track];
        AudioEvent ev = tr.AddAudioEvent(Sec(start), Sec(length));
        Subclip sub = new Subclip(proj, wavPath, Sec(mediaStart), Sec(mediaLength), true, "reversed " + name);
        Take tk = ev.AddTake(sub.GetAudioStreamByIndex(0), true);
        tk.Offset = Sec(0);
        ev.Name = name;
        return ev;
    }

    public VideoEvent Video(int track, double start, double length, double takeOffset, string name)
    {
        return Video(track, start, length, takeOffset, 1.0, name);
    }

    public VideoEvent Video(int track, double start, double length, double takeOffset, double rate, string name)
    {
        if (avi == null) throw new Exception("there is no video media");
        VideoTrack tr = (VideoTrack)proj.Tracks[track];
        VideoEvent ev = tr.AddVideoEvent(Sec(start), Sec(length));
        if (rate != 1.0) ev.PlaybackRate = rate;
        Take tk = ev.AddTake(avi.GetVideoStreamByIndex(0), true);
        tk.Offset = Sec(takeOffset);
        ev.Name = name;
        return ev;
    }

    public VideoEvent VideoReversed(int track, double start, double length, double mediaStart, double mediaLength, string name)
    {
        if (avi == null || videoKind != "avi") throw new Exception("a reversed video subclip needs the AVI (video media: " + videoKind + ")");
        VideoTrack tr = (VideoTrack)proj.Tracks[track];
        VideoEvent ev = tr.AddVideoEvent(Sec(start), Sec(length));
        Subclip sub = new Subclip(proj, aviPath, Sec(mediaStart), Sec(mediaLength), true, "reversed " + name);
        Take tk = ev.AddTake(sub.GetVideoStreamByIndex(0), true);
        tk.Offset = Sec(0);
        ev.Name = name;
        return ev;
    }

    public TrackEvent Ev(int track, int index) { return proj.Tracks[track].Events[index]; }

    public void SetFades(TrackEvent ev, double inLen, CurveType inCurve, double outLen, CurveType outCurve)
    {
        ev.FadeIn.Length = Sec(inLen);
        ev.FadeIn.Curve = inCurve;
        ev.FadeOut.Length = Sec(outLen);
        ev.FadeOut.Curve = outCurve;
    }

    public void AddEnvelopePoints(Envelopes list, EnvelopeType type, double[] times, double[] values, CurveType curve)
    {
        Envelope env = list.FindByType(type);
        if (env == null)
        {
            env = new Envelope(type);
            list.Add(env);
        }
        for (int i = 0; i < times.Length; i++)
        {
            EnvelopePoint have = env.Points.GetPointAtX(Sec(times[i]));
            if (have != null) { have.Y = values[i]; have.Curve = curve; }
            else env.Points.Add(new EnvelopePoint(Sec(times[i]), values[i], curve));
        }
    }

    public void AddMotionKeys(VideoEvent ve, double[] times, double[] scales)
    {
        VideoMotionKeyframes list = ve.VideoMotion.Keyframes;
        for (int i = 0; i < times.Length; i++)
        {
            VideoMotionKeyframe k;
            if (times[i] == 0 && list.Count > 0) k = list[0];
            else
            {
                k = new VideoMotionKeyframe(proj, Sec(times[i]));
                list.Add(k);
            }
            float w = 64f * (float)scales[i], h = 48f * (float)scales[i];
            float cx = 32f, cy = 24f;
            k.Bounds = new VideoMotionBounds(new VideoMotionVertex(cx - w / 2, cy - h / 2), new VideoMotionVertex(cx + w / 2, cy - h / 2),
                new VideoMotionVertex(cx + w / 2, cy + h / 2), new VideoMotionVertex(cx - w / 2, cy + h / 2));
            k.Center = new VideoMotionVertex(cx, cy);
        }
    }

    public void ClearSelection()
    {
        foreach (Track t in proj.Tracks)
            foreach (TrackEvent e in t.Events)
                e.Selected = false;
    }

    // (an event that is not there, because an earlier command failed, is only logged: the scenario goes on and shows what happened)
    public void Select(int track, int index)
    {
        try { Ev(track, index).Selected = true; }
        catch (Exception e) { Log("  Select(" + track + ", " + index + "): " + e.Message); }
    }

    // the track Paste goes to: the selected one
    public void SelectTrack(int index)
    {
        for (int i = 0; i < proj.Tracks.Count; i++) proj.Tracks[i].Selected = (i == index);
    }

    // ---- recording what happens
    JO Where(int track, int index)
    {
        return new JO().Add("track", track).Add("index", index);
    }

    // Does one operation, with its own try/catch; 'args' is what a replay needs to do it again.
    public bool Op(string kind, string name, JO args, Action act)
    {
        JO o = new JO();
        o.Add("kind", kind).Add("op", name).Add("args", args);
        string err = null;
        try { act(); }
        catch (Exception e) { err = e.GetType().Name + ": " + e.Message; }
        o.Add("ok", err == null).Add("error", err);
        ops.Add(o);
        if (err == null) ok++;
        else { failed++; failures.Add(current.Vals[0] + "/" + name + ": " + err); Log("  FAILED " + name + ": " + err); }
        Log("  " + kind + " " + name + (err == null ? " ok" : " -> " + err));
        return err == null;
    }

    // the state before and after (saved as before.veg / after.veg, and dumped into the result)
    public void Snapshot(string which)
    {
        ops.Add(new JO().Add("kind", "snapshot").Add("op", which));
        try { current.Add(which, DumpProject()); }
        catch (Exception e) { current.Add(which, "dump failed: " + e.Message); Log("dump failed: " + e); }
        try
        {
            if (!vegas.SaveProject(Path.Combine(scenarioDir, which + ".veg"))) Log("VEGAS did not save " + which + ".veg");
        }
        catch (Exception e) { Log("save " + which + ": " + e.Message); }
    }

    public void SetCursor(double s) { vegas.Cursor = Sec(s); }
    public void SetTimeSelection(double a, double b) { vegas.SelectionStart = Sec(a); vegas.SelectionLength = Sec(b - a); }

    public void OpSetup(string name, JO args, Action act) { Op("setup", name, args, act); }

    // ---- VEGAS's own commands. Vegas.InvokeCommand(section, name) takes two strings (read from ScriptPortal.Vegas.dll); the names
    // are the keyboard map's ("Edit.Split", "Edit.Trim.Start", "Edit.Delete" ... they are in vegas180.exe as such) and the section
    // is the keyboard map's context: Global, TrackView, TrackList, Explorer, Trimmer, Mixer, EditDetails, TrackMotion. Which
    // section the editing commands want is not documented, so it is tried out the first time. VEGAS may only post the command
    // to its window, which acts on it when the message loop runs: so after each try the script pumps messages for a while.
    // If nothing works the command is done through the scripting API where there is an equivalent (the op then says viaApi).
    // The first run found Edit.Split working in TrackView and then only tried TrackView for every other command (the others did
    // nothing there). Now each command finds its own section (remembered per command), and a command no section takes is tried as
    // the keyboard shortcut VEGAS has for it (SendKeys to the VEGAS window) before the scripting-API equivalent is used.
    static string[] Sections = new string[] { "TrackView", "Global", "TrackList", "EditDetails", "Trimmer", "Explorer", "Mixer", "TrackMotion", "" };
    Dictionary<string, string> cmdSection = new Dictionary<string, string>();
    Dictionary<string, bool> cmdShort = new Dictionary<string, bool>();
    Dictionary<string, string> cmdKeys = null;

    // VEGAS Pro's default keyboard map for the commands the program uses (SendKeys notation)
    string KeysFor(string cmd)
    {
        if (cmdKeys == null)
        {
            cmdKeys = new Dictionary<string, string>();
            cmdKeys["Edit.Delete"] = "{DELETE}";
            cmdKeys["Edit.Cut"] = "^x";
            cmdKeys["Edit.Copy"] = "^c";
            cmdKeys["Edit.Paste"] = "^v";
            cmdKeys["Edit.PasteInsert"] = "^+v";
            cmdKeys["Edit.Split"] = "s";
        }
        string k;
        return cmdKeys.TryGetValue(cmd, out k) ? k : null;
    }

    void Pump(int ms)
    {
        for (int waited = 0; waited < ms; waited += 25)
        {
            try { Application.DoEvents(); } catch (Exception) { }
            System.Threading.Thread.Sleep(25);
        }
        try { vegas.WaitForIdle(); } catch (Exception) { }
        try { vegas.UpdateUI(); } catch (Exception) { }
    }

    void FocusTimeline()
    {
        string[] names = new string[] { "TrackView", "Track View", "Timeline", "TimelineView" };
        for (int i = 0; i < names.Length; i++)
        {
            try { vegas.ActivateDockView(names[i]); }
            catch (Exception) { }
        }
    }

    public string Signature()
    {
        StringBuilder sb = new StringBuilder();
        foreach (Track t in proj.Tracks)
        {
            sb.Append('|').Append(t.Index).Append(':');
            foreach (TrackEvent e in t.Events)
            {
                sb.Append(e.Start.Nanos).Append(',').Append(e.Length.Nanos).Append(',');
                try { sb.Append(e.ActiveTake == null ? 0 : e.ActiveTake.Offset.Nanos); } catch (Exception) { }
                sb.Append(',').Append(e.FadeIn.Length.Nanos).Append(',').Append(e.FadeOut.Length.Nanos).Append(';');
            }
        }
        return sb.ToString();
    }

    // Runs a VEGAS command. 'changes': the command is expected to change the events (so a spelling that changes nothing is
    // not the right one).
    public bool Command(string cmd, bool changes)
    {
        string before = Signature();
        string shortName = cmd.Contains(".") ? cmd.Substring(cmd.IndexOf('.') + 1) : cmd;
        string tried = "";
        FocusTimeline();
        string known;
        bool haveKnown = cmdSection.TryGetValue(cmd, out known);
        for (int si = 0; si < Sections.Length; si++)
        {
            if (haveKnown && Sections[si] != known) continue;
            if (!changes && !haveKnown && si > 0) break;
            for (int ni = 0; ni < 2; ni++)
            {
                bool wantShort;
                if (haveKnown && cmdShort.TryGetValue(cmd, out wantShort) && (ni == 1) != wantShort) continue;
                string nm = ni == 0 ? cmd : shortName;
                try
                {
                    vegas.InvokeCommand(Sections[si], nm);
                }
                catch (Exception e)
                {
                    tried += " [" + Sections[si] + "|" + nm + ": " + e.Message + "]";
                    continue;
                }
                if (!changes) { Pump(150); return true; }
                for (int w = 0; w < 10; w++)
                {
                    Pump(120);
                    if (Signature() != before)
                    {
                        if (!haveKnown) { cmdSection[cmd] = Sections[si]; cmdShort[cmd] = ni == 1; Log("  InvokeCommand works for " + cmd + " as section='" + Sections[si] + "' name='" + nm + "'"); }
                        return true;
                    }
                }
                tried += " [" + Sections[si] + "|" + nm + ": nothing changed]";
            }
        }
        string keys = KeysFor(cmd);
        if (changes && keys != null)
        {
            try
            {
                FocusTimeline();
                SendKeys.SendWait(keys);
                for (int w = 0; w < 10; w++)
                {
                    Pump(120);
                    if (Signature() != before) { Log("  " + cmd + " worked as the key " + keys); return true; }
                }
                tried += " [key " + keys + ": nothing changed]";
            }
            catch (Exception e) { tried += " [key " + keys + ": " + e.Message + "]"; }
        }
        throw new Exception("VEGAS command " + cmd + " did nothing or failed:" + tried);
    }

    // ---- the scripting-API equivalents of the editing commands (used when InvokeCommand does nothing)
    List<TrackEvent> UiTargets(double at)
    {
        List<TrackEvent> sel = new List<TrackEvent>();
        List<TrackEvent> under = new List<TrackEvent>();
        foreach (Track t in proj.Tracks)
            foreach (TrackEvent e in t.Events)
            {
                if (e.Selected) sel.Add(e);
                else if (!e.Locked && e.Start.ToMilliseconds() < at * 1000.0 && e.End.ToMilliseconds() > at * 1000.0) under.Add(e);
            }
        return sel.Count > 0 ? sel : under;
    }

    void ApiEquivalent(string cmd)
    {
        double cur = vegas.Cursor.ToMilliseconds() / 1000.0;
        double ss = vegas.SelectionStart.ToMilliseconds() / 1000.0, sl = vegas.SelectionLength.ToMilliseconds() / 1000.0;
        if (cmd == "Edit.Split")
        {
            List<double> points = new List<double>();
            if (sl > 0) { points.Add(ss); points.Add(ss + sl); } else points.Add(cur);
            for (int p = 0; p < points.Count; p++)
            {
                List<TrackEvent> targets = UiTargets(points[p]);
                for (int k = 0; k < targets.Count; k++)
                {
                    TrackEvent e = targets[k];
                    double st = e.Start.ToMilliseconds() / 1000.0, en = e.End.ToMilliseconds() / 1000.0;
                    if (points[p] > st && points[p] < en) e.Split(Sec(points[p] - st));
                }
            }
        }
        else if (cmd == "Edit.Trim.Start" || cmd == "Edit.Trim.End")
        {
            List<TrackEvent> targets = UiTargets(cur);
            for (int k = 0; k < targets.Count; k++)
            {
                TrackEvent e = targets[k];
                double st = e.Start.ToMilliseconds() / 1000.0, en = e.End.ToMilliseconds() / 1000.0;
                if (!(cur > st && cur < en)) continue;
                if (cmd == "Edit.Trim.Start") e.AdjustStartLength(Sec(cur), Sec(en - cur), true);
                else e.AdjustStartLength(Sec(st), Sec(cur - st), true);
            }
        }
        else if (cmd == "Edit.Trim" && sl > 0)
        {
            List<TrackEvent> targets = UiTargets(ss);
            for (int k = 0; k < targets.Count; k++)
            {
                TrackEvent e = targets[k];
                double st = e.Start.ToMilliseconds() / 1000.0, en = e.End.ToMilliseconds() / 1000.0;
                double a = Math.Max(st, ss), b = Math.Min(en, ss + sl);
                if (b > a) e.AdjustStartLength(Sec(a), Sec(b - a), true);
            }
        }
        else if (cmd == "Edit.Delete" && sl <= 0)
        {
            List<TrackEvent> targets = UiTargets(cur);
            for (int k = 0; k < targets.Count; k++)
                if (targets[k].Selected) targets[k].Track.Events.Remove(targets[k]);
        }
        else throw new Exception("no scripting-API equivalent of " + cmd + " here");
    }

    // a command the replay program knows by name: ui.<command>
    public bool UiOp(string cmd, JO args, bool changes)
    {
        bool viaApi = false;
        bool ok = Op("ui", cmd, args, delegate()
        {
            try { Command(cmd, changes); }
            catch (Exception first)
            {
                Log("  " + first.Message);
                if (!changes) throw;
                viaApi = true;
                ApiEquivalent(cmd);
            }
        });
        if (viaApi && ops.Count > 0)
        {
            JO last = ops[ops.Count - 1] as JO;
            if (last != null) last.Add("viaApi", true);
        }
        return ok;
    }

    // ---------------------------------------------------------------------------------------------- the dump
    public JO DumpProject()
    {
        JO o = new JO();
        groupList.Clear();
        try { o.Add("cursor", vegas.Cursor.Nanos); } catch (Exception) { }
        try { o.Add("selectionStart", vegas.SelectionStart.Nanos).Add("selectionLength", vegas.SelectionLength.Nanos); } catch (Exception) { }
        List<object> tracks = new List<object>();
        foreach (Track t in proj.Tracks) tracks.Add(DumpTrack(t));
        o.Add("tracks", tracks);
        List<object> marks = new List<object>();
        try
        {
            foreach (Marker m in proj.Markers) marks.Add(new JO().Add("position", m.Position.Nanos).Add("label", m.Label));
        }
        catch (Exception) { }
        o.Add("markers", marks);
        List<object> regions = new List<object>();
        try
        {
            foreach (Region r in proj.Regions) regions.Add(new JO().Add("position", r.Position.Nanos).Add("length", r.Length.Nanos).Add("label", r.Label));
        }
        catch (Exception) { }
        o.Add("regions", regions);
        return o;
    }

    JO DumpTrack(Track t)
    {
        JO o = new JO();
        o.Add("index", t.Index).Add("type", t.IsVideo() ? "video" : "audio").Add("name", t.Name).Add("mute", t.Mute).Add("solo", t.Solo);
        List<object> envs = new List<object>();
        try { foreach (Envelope env in t.Envelopes) envs.Add(DumpEnvelope(env)); } catch (Exception) { }
        o.Add("envelopes", envs);
        List<object> evs = new List<object>();
        int i = 0;
        foreach (TrackEvent e in t.Events)
        {
            try { evs.Add(DumpEvent(e, t.Index, i)); }
            catch (Exception ex) { evs.Add(new JO().Add("index", i).Add("dumpError", ex.Message)); Log("event dump failed: " + ex); }
            i++;
        }
        o.Add("events", evs);
        return o;
    }

    JO DumpEnvelope(Envelope env)
    {
        JO o = new JO();
        o.Add("type", env.Type.ToString());
        List<object> pts = new List<object>();
        foreach (EnvelopePoint p in env.Points)
            pts.Add(new JO().Add("x", p.X.Nanos).Add("y", p.Y).Add("curve", p.Curve.ToString()));
        o.Add("points", pts);
        return o;
    }

    JO DumpFade(Fade f)
    {
        JO o = new JO();
        o.Add("length", f.Length.Nanos).Add("curve", f.Curve.ToString());
        try { o.Add("reciprocalCurve", f.ReciprocalCurve.ToString()); } catch (Exception) { }
        try { o.Add("gain", f.Gain); } catch (Exception) { }
        return o;
    }

    JO DumpEvent(TrackEvent e, int trackIndex, int index)
    {
        JO o = new JO();
        o.Add("track", trackIndex).Add("index", index);
        o.Add("kind", e.IsVideo() ? "video" : "audio").Add("name", e.Name);
        o.Add("start", e.Start.Nanos).Add("length", e.Length.Nanos).Add("end", e.End.Nanos);
        o.Add("playbackRate", e.PlaybackRate).Add("mute", e.Mute).Add("locked", e.Locked).Add("loop", e.Loop).Add("selected", e.Selected);
        int gid = -1;
        if (e.IsGrouped)
        {
            TrackEventGroup g = e.Group;
            gid = groupList.IndexOf(g);
            if (gid < 0) { groupList.Add(g); gid = groupList.Count - 1; }
        }
        o.Add("group", gid);
        o.Add("fadeIn", DumpFade(e.FadeIn)).Add("fadeOut", DumpFade(e.FadeOut));
        try
        {
            Effect tr = e.FadeIn.Transition as Effect;
            o.Add("transition", tr == null ? null : tr.PlugIn.Name);
        }
        catch (Exception) { }
        List<object> takes = new List<object>();
        foreach (Take tk in e.Takes)
        {
            JO t = new JO();
            t.Add("name", tk.Name).Add("active", tk.IsActive).Add("offset", tk.Offset.Nanos);
            try { t.Add("length", tk.Length.Nanos); } catch (Exception) { }
            try
            {
                Media m = tk.Media;
                t.Add("media", m == null ? null : Path.GetFileName(m.FilePath));
                Subclip sc = m as Subclip;
                if (sc != null)
                {
                    t.Add("subclipReversed", sc.IsReversed).Add("subclipStart", sc.Start.Nanos).Add("subclipLength", sc.Length.Nanos);
                }
            }
            catch (Exception) { }
            takes.Add(t);
        }
        o.Add("takes", takes);
        VideoEvent ve = e as VideoEvent;
        AudioEvent ae = e as AudioEvent;
        if (ve != null)
        {
            try { o.Add("maintainAspectRatio", ve.MaintainAspectRatio).Add("reduceInterlace", ve.ReduceInterlace).Add("resample", ve.ResampleMode.ToString()).Add("undersample", ve.UnderSampleRate); } catch (Exception) { }
            List<object> envs = new List<object>();
            try { foreach (Envelope env in ve.Envelopes) envs.Add(DumpEnvelope(env)); } catch (Exception) { }
            o.Add("envelopes", envs);
            List<object> keys = new List<object>();
            try
            {
                foreach (VideoMotionKeyframe k in ve.VideoMotion.Keyframes)
                {
                    JO ko = new JO();
                    ko.Add("position", k.Position.Nanos).Add("type", k.Type.ToString());
                    ko.Add("topLeft", new List<object>(new object[] { (double)k.TopLeft.X, (double)k.TopLeft.Y }));
                    ko.Add("topRight", new List<object>(new object[] { (double)k.TopRight.X, (double)k.TopRight.Y }));
                    ko.Add("bottomRight", new List<object>(new object[] { (double)k.BottomRight.X, (double)k.BottomRight.Y }));
                    ko.Add("bottomLeft", new List<object>(new object[] { (double)k.BottomLeft.X, (double)k.BottomLeft.Y }));
                    ko.Add("rotation", Convert.ToDouble(k.Rotation));
                    keys.Add(ko);
                }
            }
            catch (Exception) { }
            o.Add("panCrop", keys);
            o.Add("effects", DumpEffects(ve.Effects));
        }
        if (ae != null)
        {
            try { o.Add("normalize", ae.Normalize).Add("invertPhase", ae.InvertPhase).Add("pitchSemis", ae.PitchSemis).Add("method", ae.Method.ToString()); } catch (Exception) { }
            o.Add("effects", DumpEffects(ae.Effects));
        }
        return o;
    }

    List<object> DumpEffects(Effects fx)
    {
        List<object> list = new List<object>();
        try
        {
            foreach (Effect f in fx)
            {
                JO o = new JO();
                o.Add("name", f.PlugIn.Name).Add("bypass", f.Bypass);
                list.Add(o);
            }
        }
        catch (Exception) { }
        return list;
    }

    // ---------------------------------------------------------------------------------------------- scenarios
    // Names of ops (kind api / ui) and their args are what editparity_replay understands: see tests\ui\editparity_replay.cpp.

    void SplitApi(int track, int index, double offset)
    {
        TrackEvent ev = Ev(track, index);
        Op("api", "split", Where(track, index).Add("offset", Ticks(Sec(offset))), delegate() { ev.Split(Sec(offset)); });
    }

    void AdjustApi(int track, int index, double start, double length, bool takes)
    {
        TrackEvent ev = Ev(track, index);
        Op("api", "adjustStartLength", Where(track, index).Add("start", Ticks(Sec(start))).Add("length", Ticks(Sec(length))).Add("adjustTakes", takes),
            delegate() { ev.AdjustStartLength(Sec(start), Sec(length), takes); });
    }

    void RegisterApiScenarios()
    {
        // -------- TrackEvent.Split
        Add("split_audio_plain", delegate(EditRunner c)
        {
            c.Begin("Split of audio events by TrackEvent.Split: in the middle, twice, and again at the new half", 0, 3);
            c.Audio(0, 2, 8, 3, "A");
            c.Audio(1, 2, 8, 0, "B");
            c.Audio(2, 0, 20, 0, "C");
            c.Snapshot("before");
            c.SplitApi(0, 0, 3);
            c.SplitApi(0, 1, 2);
            c.SplitApi(1, 0, 4.0166667);
            c.SplitApi(2, 0, 10);
            c.Snapshot("after");
        });

        Add("split_audio_fades", delegate(EditRunner c)
        {
            c.Begin("Split of audio events with fades: in the middle, inside the fade-in, inside the fade-out, and fades longer than the event", 0, 5);
            AudioEvent a = c.Audio(0, 1, 10, 1, "mid");
            c.SetFades(a, 1.5, CurveType.Smooth, 2.5, CurveType.Slow);
            AudioEvent b = c.Audio(1, 1, 10, 1, "inFadeIn");
            c.SetFades(b, 2.0, CurveType.Fast, 2.0, CurveType.Sharp);
            AudioEvent d = c.Audio(2, 1, 10, 1, "inFadeOut");
            c.SetFades(d, 2.0, CurveType.Fast, 2.0, CurveType.Sharp);
            AudioEvent e = c.Audio(3, 1, 4, 1, "longFades");
            c.SetFades(e, 3.0, CurveType.Linear, 3.0, CurveType.Linear);
            AudioEvent f = c.Audio(4, 1, 10, 1, "reciprocal");
            c.SetFades(f, 2.0, CurveType.Smooth, 2.0, CurveType.Smooth);
            try { f.FadeIn.ReciprocalCurve = CurveType.Sharp; } catch (Exception ex) { c.Log("reciprocal: " + ex.Message); }
            c.Snapshot("before");
            c.SplitApi(0, 0, 5);
            c.SplitApi(1, 0, 1.0);
            c.SplitApi(2, 0, 9.0);
            c.SplitApi(3, 0, 2.0);
            c.SplitApi(4, 0, 5);
            c.Snapshot("after");
        });

        Add("split_audio_rate", delegate(EditRunner c)
        {
            c.Begin("Split of audio events with a playback rate other than 1 (take offset and length after the split)", 0, 3);
            c.Audio(0, 2, 6, 4, 2.0, "fast");
            c.Audio(1, 2, 6, 4, 0.5, "slow");
            c.Audio(2, 2, 6, 4, 1.25, "odd");
            c.Snapshot("before");
            c.SplitApi(0, 0, 2.5);
            c.SplitApi(1, 0, 2.5);
            c.SplitApi(2, 0, 2.5);
            c.Snapshot("after");
        });

        Add("split_audio_reversed", delegate(EditRunner c)
        {
            c.Begin("Split of a reversed audio event (a reversed subclip): which half plays which part of the media", 0, 2);
            c.AudioReversed(0, 2, 6, 3, 6, "rev");
            c.AudioReversed(1, 2, 6, 3, 6, "rev2");
            c.Snapshot("before");
            c.SplitApi(0, 0, 2);
            c.SplitApi(1, 0, 5);
            c.Snapshot("after");
        });

        Add("split_audio_loop", delegate(EditRunner c)
        {
            c.Begin("Split of a looping event longer than its media (30 s from a 20 s file), before and after the loop point", 0, 2);
            AudioEvent a = c.Audio(0, 0, 30, 0, "loop");
            a.Loop = true;
            AudioEvent b = c.Audio(1, 0, 30, 5, "loop5");
            b.Loop = true;
            c.Snapshot("before");
            c.SplitApi(0, 0, 25);
            c.SplitApi(1, 0, 10);
            c.Snapshot("after");
        });

        // -------- looping events (the test file is 20 s long). Events are made longer than the file so that the media repeats.
        // 'loopN' is the same with the take starting N seconds into the file (the take is then 20 - N s long).
        Add("loop_split_points", delegate(EditRunner c)
        {
            c.Begin("Split of looping events (50 s from a 20 s file): before, exactly at and after the first and second loop point; and the same with the take starting 5 s in", 0, 8);
            double[] offs = new double[] { 0, 0, 0, 0, 5, 5, 5, 5 };
            string[] names = new string[] { "before12", "at20", "after25", "at40", "off5at10", "off5at15", "off5at20", "off5at35" };
            double[] at = new double[] { 12, 20, 25, 40, 10, 15, 20, 35 };
            for (int i = 0; i < 8; i++) { AudioEvent a = c.Audio(i, 2, 50, offs[i], names[i]); a.Loop = true; }
            c.Snapshot("before");
            for (int i = 0; i < 8; i++) c.SplitApi(i, 0, at[i]);
            c.Snapshot("after");
        });

        Add("loop_split_chain", delegate(EditRunner c)
        {
            c.Begin("Split of a looping event again and again (each right half split again) to see how the take offset and length are carried on", 0, 2);
            AudioEvent a = c.Audio(0, 0, 50, 0, "chain");
            a.Loop = true;
            AudioEvent b = c.Audio(1, 0, 50, 5, "chain5");
            b.Loop = true;
            c.Snapshot("before");
            c.SplitApi(0, 0, 25);         // right half starts at 25 s: media 5 s
            c.SplitApi(0, 1, 5);          // at 30 s: media 10 s
            c.SplitApi(0, 2, 10);         // at 40 s: exactly media 0 again
            c.SplitApi(1, 0, 25);
            c.SplitApi(1, 1, 5);
            c.SplitApi(1, 2, 10);
            c.Snapshot("after");
        });

        Add("loop_split_extend", delegate(EditRunner c)
        {
            c.Begin("Looping events split and then lengthened: does the take's length (the media after the offset, cut to the left half) limit the loop of a half that is made longer again", 0, 2);
            AudioEvent a = c.Audio(0, 0, 30, 0, "ext");
            a.Loop = true;
            AudioEvent b = c.Audio(1, 0, 30, 5, "ext5");
            b.Loop = true;
            c.Snapshot("before");
            c.SplitApi(0, 0, 8);           // left half 8 s: take length cut to 8 s
            c.SplitApi(1, 0, 8);
            c.Snapshot("mid");
            c.AdjustApi(0, 0, 0, 45, true);        // the left half longer than the file again
            c.AdjustApi(1, 0, 0, 45, true);
            c.AdjustApi(0, 1, 8, 5, true);         // the right half shorter, then
            c.AdjustApi(1, 1, 8, 60, true);        // much longer
            c.Snapshot("after");
        });

        Add("loop_split_rate", delegate(EditRunner c)
        {
            c.Begin("Split of looping events with a playback rate: rate 2 (the file lasts 10 s on the timeline) and rate 0.5 (40 s), at and after the loop point", 0, 4);
            AudioEvent a = c.Audio(0, 0, 30, 0, 2.0, "fast");
            a.Loop = true;
            AudioEvent b = c.Audio(1, 0, 30, 0, 2.0, "fast");
            b.Loop = true;
            AudioEvent d = c.Audio(2, 0, 100, 0, 0.5, "slow");
            d.Loop = true;
            AudioEvent e = c.Audio(3, 0, 100, 0, 0.5, "slow");
            e.Loop = true;
            c.Snapshot("before");
            c.SplitApi(0, 0, 10);
            c.SplitApi(1, 0, 17);
            c.SplitApi(2, 0, 40);
            c.SplitApi(3, 0, 70);
            c.Snapshot("after");
        });

        Add("loop_split_reversed", delegate(EditRunner c)
        {
            c.Begin("Split of looping reversed events (a reversed 6 s subclip repeated over 20 s): at, before and after the loop point", 0, 3);
            AudioEvent a = c.AudioReversed(0, 0, 20, 3, 6, "rev");
            a.Loop = true;
            AudioEvent b = c.AudioReversed(1, 0, 20, 3, 6, "rev");
            b.Loop = true;
            AudioEvent d = c.AudioReversed(2, 0, 20, 3, 6, "rev");
            d.Loop = true;
            c.Snapshot("before");
            c.SplitApi(0, 0, 6);
            c.SplitApi(1, 0, 4);
            c.SplitApi(2, 0, 9);
            c.Snapshot("after");
        });

        Add("loop_split_fades", delegate(EditRunner c)
        {
            c.Begin("Split of a looping event inside its fade-in, inside its fade-out, and at a loop point inside a long fade", 0, 4);
            for (int i = 0; i < 4; i++)
            {
                AudioEvent a = c.Audio(i, 2, 30, i == 3 ? 5 : 0, "fl" + i);
                a.Loop = true;
                c.SetFades(a, i == 2 ? 25 : 5, CurveType.Smooth, 5, CurveType.Smooth);
            }
            c.Snapshot("before");
            c.SplitApi(0, 0, 3);
            c.SplitApi(1, 0, 27);
            c.SplitApi(2, 0, 20);
            c.SplitApi(3, 0, 15);
            c.Snapshot("after");
        });

        Add("loop_off", delegate(EditRunner c)
        {
            c.Begin("Loop switched off on events longer than the file (30 s from 20 s): nothing changes by itself; split inside, at the end of and past the media; and switched on again", 0, 5);
            AudioEvent a = c.Audio(0, 0, 30, 0, "off");
            a.Loop = false;
            AudioEvent b = c.Audio(1, 0, 30, 0, "off");
            b.Loop = false;
            AudioEvent d = c.Audio(2, 0, 30, 0, "off");
            d.Loop = false;
            AudioEvent e = c.Audio(3, 0, 30, 5, "off5");
            e.Loop = false;
            AudioEvent f = c.Audio(4, 0, 30, 0, "toggle");
            f.Loop = false;
            c.Snapshot("before");
            c.SplitApi(0, 0, 10);
            c.SplitApi(1, 0, 20);
            c.SplitApi(2, 0, 25);
            c.SplitApi(3, 0, 25);
            TrackEvent tg = c.Ev(4, 0);
            c.Op("api", "setLoop", c.Where(4, 0).Add("loop", true), delegate() { tg.Loop = true; });
            c.Snapshot("mid");
            c.SplitApi(4, 0, 25);
            c.Snapshot("after");
        });

        Add("loop_trim_start", delegate(EditRunner c)
        {
            c.Begin("Trimming the START of looping events (AdjustStartLength, takes follow): inside the file, past the first loop point, earlier than the take start", 0, 6);
            double[] offs = new double[] { 0, 0, 0, 5, 5, 5 };
            for (int i = 0; i < 6; i++) { AudioEvent a = c.Audio(i, 10, 50, offs[i], "ts" + i); a.Loop = true; }
            c.Snapshot("before");
            c.AdjustApi(0, 0, 13, 47, true);         // 3 s later
            c.AdjustApi(1, 0, 35, 25, true);         // 25 s later: past the loop point
            c.AdjustApi(2, 0, 6, 54, true);          // 4 s earlier: before the start of the file, loop on
            c.AdjustApi(3, 0, 13, 47, true);         // take at 5 s: 3 s later
            c.AdjustApi(4, 0, 30, 30, true);         // 20 s later: past the loop point (take 5 s in)
            c.AdjustApi(5, 0, 8, 52, true);          // 2 s earlier: media before the take start exists (5 s of it)
            c.Snapshot("after");
        });

        Add("loop_trim_end", delegate(EditRunner c)
        {
            c.Begin("Trimming the END of looping events: shorter than the file, exactly the file, past the file, much past it", 0, 5);
            double[] offs = new double[] { 0, 0, 0, 5, 5 };
            double[] lens = new double[] { 30, 30, 8, 30, 8 };
            for (int i = 0; i < 5; i++) { AudioEvent a = c.Audio(i, 10, lens[i], offs[i], "te" + i); a.Loop = true; }
            c.Snapshot("before");
            c.AdjustApi(0, 0, 10, 12, true);         // shorter than the file
            c.AdjustApi(1, 0, 10, 20, true);         // exactly the file
            c.AdjustApi(2, 0, 10, 45, true);         // 8 -> 45: past the file twice
            c.AdjustApi(3, 0, 10, 12, true);
            c.AdjustApi(4, 0, 10, 40, true);         // take at 5 s: 15 s of it, then repeats
            c.Snapshot("after");
        });

        Add("loop_set_length", delegate(EditRunner c)
        {
            c.Begin("The property setters on looping events: Length past the file and back, Start and End moves, Loop on / off while the event is longer than the file", 0, 4);
            AudioEvent a = c.Audio(0, 2, 8, 1, "len");
            a.Loop = true;
            AudioEvent b = c.Audio(1, 2, 8, 1, "len");
            b.Loop = false;
            AudioEvent d = c.Audio(2, 2, 30, 1, "start");
            d.Loop = true;
            AudioEvent e = c.Audio(3, 2, 30, 1, "end");
            e.Loop = true;
            c.Snapshot("before");
            TrackEvent ta = c.Ev(0, 0), tb = c.Ev(1, 0), td = c.Ev(2, 0), te = c.Ev(3, 0);
            c.Op("api", "setLength", c.Where(0, 0).Add("length", Ticks(Sec(45))), delegate() { ta.Length = Sec(45); });
            c.Op("api", "setLength", c.Where(1, 0).Add("length", Ticks(Sec(45))), delegate() { tb.Length = Sec(45); });
            c.Op("api", "setStart", c.Where(2, 0).Add("start", Ticks(Sec(12))), delegate() { td.Start = Sec(12); });
            c.Op("api", "setEnd", c.Where(3, 0).Add("end", Ticks(Sec(12))), delegate() { te.End = Sec(12); });
            c.Snapshot("mid");
            c.Op("api", "setLength", c.Where(0, 0).Add("length", Ticks(Sec(5))), delegate() { ta.Length = Sec(5); });
            c.Op("api", "setLength", c.Where(1, 0).Add("length", Ticks(Sec(5))), delegate() { tb.Length = Sec(5); });
            c.Snapshot("after");
        });

        Add("split_audio_takes", delegate(EditRunner c)
        {
            c.Begin("Split of an event with two takes (the second from another part of the file): both takes' offsets", 0, 1);
            AudioEvent a = c.Audio(0, 2, 8, 3, "takes");
            Take t2 = a.AddTake(c.wav.GetAudioStreamByIndex(0), false);
            t2.Offset = Sec(10);
            t2.Name = "second";
            c.Snapshot("before");
            c.SplitApi(0, 0, 3);
            c.Snapshot("after");
        });

        Add("split_group", delegate(EditRunner c)
        {
            c.Begin("Split of one event of a group, or of both: does the group divide, and which new events share a group", 1, 2);
            VideoEvent v = c.Video(0, 2, 8, 0, "V");
            AudioEvent a = c.Audio(1, 2, 8, 0, "A");
            AudioEvent a2 = c.Audio(2, 2, 8, 0, "A2");
            try
            {
                TrackEventGroup g = new TrackEventGroup(c.proj);
                c.proj.Groups.Add(g);
                g.Add(v);
                g.Add(a);
            }
            catch (Exception ex) { c.Log("group: " + ex.Message); }
            c.Snapshot("before");
            c.SplitApi(0, 0, 3);           // only the picture
            c.SplitApi(1, 0, 3);           // and now the sound too
            c.SplitApi(2, 0, 3);           // an event outside the group
            c.Snapshot("after");
        });

        Add("split_video_keys", delegate(EditRunner c)
        {
            c.Begin("Split of a video event with Pan/Crop keys, a velocity envelope, a composite envelope and an effect: how the keys divide", 1, 0);
            VideoEvent v = c.Video(0, 2, 8, 1, "keys");
            c.AddMotionKeys(v, new double[] { 0, 3, 6 }, new double[] { 1.0, 0.5, 0.8 });
            c.AddEnvelopePoints(v.Envelopes, EnvelopeType.Velocity, new double[] { 0, 4, 8 }, new double[] { 1.0, 2.0, 0.5 }, CurveType.Linear);
            c.AddEnvelopePoints(v.Track.Envelopes, EnvelopeType.Composite, new double[] { 1, 5 }, new double[] { 1.0, 0.3 }, CurveType.Smooth);
            try
            {
                PlugInNode node = c.vegas.VideoFX.GetChildByName("VEGAS Invert");
                if (node == null) node = c.vegas.VideoFX.GetChildByName("Invert");
                if (node != null) v.Effects.AddEffect(node);
            }
            catch (Exception ex) { c.Log("effect: " + ex.Message); }
            c.Snapshot("before");
            c.SplitApi(0, 0, 4);
            c.Snapshot("after");
        });

        Add("split_video_velocity", delegate(EditRunner c)
        {
            c.Begin("Split of video events with a velocity envelope (speed changes): the media offset of the second half", 1, 0);
            VideoEvent v = c.Video(0, 0, 8, 2, "vel");
            c.AddEnvelopePoints(v.Envelopes, EnvelopeType.Velocity, new double[] { 0, 4, 8 }, new double[] { 1.0, 3.0, 0.5 }, CurveType.Linear);
            c.Snapshot("before");
            c.SplitApi(0, 0, 3);
            c.Snapshot("after");
        });

        Add("split_video_reversed", delegate(EditRunner c)
        {
            c.Begin("Split of a reversed video event", 1, 0);
            c.VideoReversed(0, 1, 6, 4, 6, "rev");
            c.Snapshot("before");
            c.SplitApi(0, 0, 2);
            c.Snapshot("after");
        });

        Add("split_video_crossfade", delegate(EditRunner c)
        {
            c.Begin("Split of video events that cross-fade (overlap 1 s): inside the cross-fade and after it", 1, 0);
            VideoEvent a = c.Video(0, 0, 5, 0, "first");
            VideoEvent b = c.Video(0, 4, 6, 2, "second");
            b.FadeIn.Length = Sec(1);
            a.FadeOut.Length = Sec(1);
            c.Snapshot("before");
            c.SplitApi(0, 1, 0.5);        // inside the cross-fade
            c.SplitApi(0, 0, 4.5);        // the first one inside the cross-fade
            c.Snapshot("after");
        });

        Add("split_edge_cases", delegate(EditRunner c)
        {
            c.Begin("Split at the start, at the end, outside, negative, and of a locked event", 0, 4);
            c.Audio(0, 2, 6, 1, "start");
            c.Audio(1, 2, 6, 1, "end");
            c.Audio(2, 2, 6, 1, "outside");
            AudioEvent l = c.Audio(3, 2, 6, 1, "locked");
            l.Locked = true;
            c.Snapshot("before");
            c.SplitApi(0, 0, 0);
            c.SplitApi(1, 0, 6);
            c.SplitApi(2, 0, 7);
            c.SplitApi(2, 0, -1);
            c.SplitApi(3, 0, 3);
            c.Snapshot("after");
        });

        // -------- AdjustStartLength and the property setters
        Add("adjust_audio", delegate(EditRunner c)
        {
            c.Begin("AdjustStartLength on audio events: trim start, trim end, extend start (media available / not), adjustTakes false, long fades", 0, 8);
            c.Audio(0, 4, 8, 5, "trimStart");
            c.Audio(1, 4, 8, 5, "trimEnd");
            c.Audio(2, 4, 8, 5, "extendStart");
            c.Audio(3, 4, 8, 5, "noTakes");
            c.Audio(4, 4, 8, 0, "pastMediaStart");
            AudioEvent f = c.Audio(5, 4, 8, 5, "fades");
            c.SetFades(f, 3.0, CurveType.Smooth, 3.0, CurveType.Smooth);
            c.Audio(6, 4, 8, 5, 2.0, "rate2");
            c.AudioReversed(7, 4, 8, 3, 10, "reversed");
            c.Snapshot("before");
            c.AdjustApi(0, 0, 6, 6, true);
            c.AdjustApi(1, 0, 4, 5, true);
            c.AdjustApi(2, 0, 2, 10, true);
            c.AdjustApi(3, 0, 6, 6, false);
            c.AdjustApi(4, 0, 2, 10, true);
            c.AdjustApi(5, 0, 7, 3, true);
            c.AdjustApi(6, 0, 6, 6, true);
            c.AdjustApi(7, 0, 6, 6, true);
            c.Snapshot("after");
        });

        Add("adjust_video_keys", delegate(EditRunner c)
        {
            c.Begin("AdjustStartLength on a video event with Pan/Crop keys and envelopes: do keys and points move with the start edge", 1, 0);
            VideoEvent v = c.Video(0, 2, 8, 2, "keys");
            c.AddMotionKeys(v, new double[] { 0, 3, 6 }, new double[] { 1.0, 0.5, 0.8 });
            c.AddEnvelopePoints(v.Track.Envelopes, EnvelopeType.Composite, new double[] { 1, 5 }, new double[] { 1.0, 0.3 }, CurveType.Smooth);
            c.AddEnvelopePoints(v.Envelopes, EnvelopeType.Velocity, new double[] { 0, 4, 8 }, new double[] { 1.0, 2.0, 0.5 }, CurveType.Linear);
            c.Snapshot("before");
            c.AdjustApi(0, 0, 4, 6, true);          // trim the start by 2 s
            c.Snapshot("mid");
            c.AdjustApi(0, 0, 4, 3, true);          // and the end
            c.Snapshot("after");
        });

        Add("set_start_length", delegate(EditRunner c)
        {
            c.Begin("The property setters: Length (trim end), Start (move), End, and moving an event on top of another on the same track", 0, 5);
            c.Audio(0, 2, 6, 1, "length");
            c.Audio(1, 2, 6, 1, "start");
            c.Audio(2, 2, 6, 1, "end");
            c.Audio(3, 2, 6, 1, "overlapA");
            c.Audio(3, 10, 6, 1, "overlapB");
            c.Audio(4, 2, 6, 1, "pastMedia");
            c.Snapshot("before");
            TrackEvent a = c.Ev(0, 0), b = c.Ev(1, 0), d = c.Ev(2, 0), o = c.Ev(3, 1), p = c.Ev(4, 0);
            c.Op("api", "setLength", c.Where(0, 0).Add("length", Ticks(Sec(3))), delegate() { a.Length = Sec(3); });
            c.Op("api", "setStart", c.Where(1, 0).Add("start", Ticks(Sec(5))), delegate() { b.Start = Sec(5); });
            c.Op("api", "setEnd", c.Where(2, 0).Add("end", Ticks(Sec(5))), delegate() { d.End = Sec(5); });
            c.Op("api", "setStart", c.Where(3, 1).Add("start", Ticks(Sec(6))), delegate() { o.Start = Sec(6); });
            c.Op("api", "setLength", c.Where(4, 0).Add("length", Ticks(Sec(30))), delegate() { p.Length = Sec(30); });
            c.Snapshot("after");
        });

        Add("playback_rate", delegate(EditRunner c)
        {
            c.Begin("AdjustPlaybackRate (with and without moving the start) and the PlaybackRate setter: length, offset", 0, 3);
            c.Audio(0, 2, 8, 1, "adjustKeepStart");
            c.Audio(1, 2, 8, 1, "adjustMoveStart");
            c.Audio(2, 2, 8, 1, "setter");
            c.Snapshot("before");
            TrackEvent a = c.Ev(0, 0), b = c.Ev(1, 0), d = c.Ev(2, 0);
            c.Op("api", "adjustPlaybackRate", c.Where(0, 0).Add("rate", 2.0).Add("adjustStart", false), delegate() { a.AdjustPlaybackRate(2.0, false); });
            c.Op("api", "adjustPlaybackRate", c.Where(1, 0).Add("rate", 2.0).Add("adjustStart", true), delegate() { b.AdjustPlaybackRate(2.0, true); });
            c.Op("api", "setPlaybackRate", c.Where(2, 0).Add("rate", 0.5), delegate() { d.PlaybackRate = 0.5; });
            c.Snapshot("after");
        });

        Add("copy_event", delegate(EditRunner c)
        {
            c.Begin("TrackEvent.Copy to another place on the same track and to another track: fades, takes, group, keys come along?", 1, 2);
            VideoEvent v = c.Video(0, 2, 6, 1, "V");
            c.SetFades(v, 1, CurveType.Smooth, 1, CurveType.Slow);
            c.AddMotionKeys(v, new double[] { 0, 3 }, new double[] { 1.0, 0.5 });
            AudioEvent a = c.Audio(1, 2, 6, 1, "A");
            c.SetFades(a, 1, CurveType.Fast, 1, CurveType.Slow);
            try
            {
                TrackEventGroup g = new TrackEventGroup(c.proj);
                c.proj.Groups.Add(g);
                g.Add(v);
                g.Add(a);
            }
            catch (Exception ex) { c.Log("group: " + ex.Message); }
            c.Snapshot("before");
            TrackEvent va = c.Ev(0, 0), aa = c.Ev(1, 0);
            c.Op("api", "copy", c.Where(0, 0).Add("toTrack", 0).Add("start", Ticks(Sec(10))), delegate() { va.Copy(c.proj.Tracks[0], Sec(10)); });
            c.Op("api", "copy", c.Where(1, 0).Add("toTrack", 2).Add("start", Ticks(Sec(4))), delegate() { aa.Copy(c.proj.Tracks[2], Sec(4)); });
            c.Snapshot("after");
        });

        Add("remove_event", delegate(EditRunner c)
        {
            c.Begin("Removing an event with Events.Remove: a grouped one and a plain one", 1, 2);
            VideoEvent v = c.Video(0, 2, 6, 1, "V");
            AudioEvent a = c.Audio(1, 2, 6, 1, "A");
            c.Audio(2, 2, 6, 1, "plain");
            try
            {
                TrackEventGroup g = new TrackEventGroup(c.proj);
                c.proj.Groups.Add(g);
                g.Add(v);
                g.Add(a);
            }
            catch (Exception ex) { c.Log("group: " + ex.Message); }
            c.Snapshot("before");
            TrackEvent va = c.Ev(0, 0), pl = c.Ev(2, 0);
            c.Op("api", "remove", c.Where(0, 0), delegate() { c.proj.Tracks[0].Events.Remove(va); });
            c.Op("api", "remove", c.Where(2, 0), delegate() { c.proj.Tracks[2].Events.Remove(pl); });
            c.Snapshot("after");
        });

        Add("overlap_crossfade", delegate(EditRunner c)
        {
            c.Begin("Events that overlap, made by the API and by moving: what happens to the fades (automatic cross-fades are an option in the program)", 1, 1);
            c.Video(0, 0, 5, 0, "v1");
            c.Video(0, 3, 5, 4, "v2");
            c.Audio(1, 0, 5, 0, "a1");
            c.Audio(1, 8, 5, 4, "a2");
            c.Snapshot("before");
            TrackEvent a2 = c.Ev(1, 1);
            c.Op("api", "setStart", c.Where(1, 1).Add("start", Ticks(Sec(4))), delegate() { a2.Start = Sec(4); });
            c.Snapshot("after");
        });
    }

    // ---------------------------------------------------------------------------------------------- the program's own commands
    // Each puts the cursor / time selection / selection in place with setup ops, then calls the command.
    void RegisterUiScenarios()
    {
        Add("ui_split_selected", delegate(EditRunner c)
        {
            c.Begin("Edit > Split (S) at the cursor with ONE of three events under it selected", 0, 3);
            c.Audio(0, 2, 8, 0, "one");
            c.Audio(1, 2, 8, 0, "two");
            c.Audio(2, 2, 8, 0, "three");
            c.ClearSelection();
            c.Select(1, 0);
            c.SetCursor(5);
            c.Snapshot("before");
            c.UiOp("Edit.Split", new JO().Add("cursor", Ticks(Sec(5))).Add("selected", Sel(1, 0)), true);
            c.Snapshot("after");
        });

        Add("ui_split_none_selected", delegate(EditRunner c)
        {
            c.Begin("Edit > Split with nothing selected: events under the cursor on all tracks (a locked one too?)", 1, 3);
            c.Video(0, 2, 8, 0, "V");
            c.Audio(1, 2, 8, 0, "A");
            c.Audio(2, 2, 8, 0, "locked").Locked = true;
            c.Audio(3, 6, 4, 0, "notUnderCursor");
            c.ClearSelection();
            c.SetCursor(5);
            c.Snapshot("before");
            c.UiOp("Edit.Split", new JO().Add("cursor", Ticks(Sec(5))).Add("selected", new List<object>()), true);
            c.Snapshot("after");
        });

        Add("ui_split_time_selection", delegate(EditRunner c)
        {
            c.Begin("Edit > Split with a time selection 4 s to 7 s: with no event selected, and with one selected", 0, 4);
            c.Audio(0, 2, 8, 0, "all1");
            c.Audio(1, 2, 8, 0, "all2");
            c.Audio(2, 5, 10, 0, "partlyIn");
            c.Audio(3, 2, 2.5, 0, "outside");
            c.ClearSelection();
            c.SetCursor(0);
            c.SetTimeSelection(4, 7);
            c.Snapshot("before");
            c.UiOp("Edit.Split", new JO().Add("selectionStart", Ticks(Sec(4))).Add("selectionLength", Ticks(Sec(3))).Add("selected", new List<object>()), true);
            c.Snapshot("mid");
            c.ClearSelection();
            c.Select(1, 0);
            c.SetTimeSelection(8.5, 9.5);
            c.UiOp("Edit.Split", new JO().Add("selectionStart", Ticks(Sec(8.5))).Add("selectionLength", Ticks(Sec(1))).Add("selected", Sel(1, 0)), true);
            c.Snapshot("after");
        });

        Add("ui_split_group", delegate(EditRunner c)
        {
            c.Begin("Edit > Split with one event of a group selected (and with both)", 1, 1);
            VideoEvent v = c.Video(0, 2, 8, 0, "V");
            AudioEvent a = c.Audio(1, 2, 8, 0, "A");
            try
            {
                TrackEventGroup g = new TrackEventGroup(c.proj);
                c.proj.Groups.Add(g);
                g.Add(v);
                g.Add(a);
            }
            catch (Exception ex) { c.Log("group: " + ex.Message); }
            c.ClearSelection();
            c.Select(0, 0);
            c.SetCursor(4);
            c.Snapshot("before");
            c.UiOp("Edit.Split", new JO().Add("cursor", Ticks(Sec(4))).Add("selected", Sel(0, 0)), true);
            c.Snapshot("after");
        });

        Add("ui_split_fades_keys", delegate(EditRunner c)
        {
            c.Begin("Edit > Split of events with fades, keys and envelopes, cursor inside the fade-in / fade-out / middle", 1, 3);
            VideoEvent v = c.Video(0, 2, 8, 1, "V");
            c.SetFades(v, 2, CurveType.Smooth, 2, CurveType.Smooth);
            c.AddMotionKeys(v, new double[] { 0, 3, 6 }, new double[] { 1.0, 0.5, 0.8 });
            c.AddEnvelopePoints(v.Track.Envelopes, EnvelopeType.Composite, new double[] { 1, 5 }, new double[] { 1.0, 0.3 }, CurveType.Smooth);
            AudioEvent a = c.Audio(1, 2, 8, 1, "inFadeIn");
            c.SetFades(a, 2, CurveType.Fast, 2, CurveType.Slow);
            AudioEvent b = c.Audio(2, 2, 8, 1, "inFadeOut");
            c.SetFades(b, 2, CurveType.Fast, 2, CurveType.Slow);
            c.ClearSelection();
            c.Select(0, 0);
            c.SetCursor(6);
            c.Snapshot("before");
            c.UiOp("Edit.Split", new JO().Add("cursor", Ticks(Sec(6))).Add("selected", Sel(0, 0)), true);
            c.ClearSelection();
            c.Select(1, 0);
            c.SetCursor(3);
            c.UiOp("Edit.Split", new JO().Add("cursor", Ticks(Sec(3))).Add("selected", Sel(1, 0)), true);
            c.ClearSelection();
            c.Select(2, 0);
            c.SetCursor(9);
            c.UiOp("Edit.Split", new JO().Add("cursor", Ticks(Sec(9))).Add("selected", Sel(2, 0)), true);
            c.Snapshot("after");
        });

        Add("ui_split_quantize", delegate(EditRunner c)
        {
            c.Begin("Edit > Split with the cursor between frames (3.016 s at 30 fps): is the split put on a frame", 0, 2);
            c.Audio(0, 2, 8, 0, "q1");
            c.Audio(1, 2, 8, 0, "q2");
            c.ClearSelection();
            c.Select(0, 0);
            c.SetCursor(3.016);
            c.Snapshot("before");
            c.UiOp("Edit.Split", new JO().Add("cursor", Ticks(Sec(3.016))).Add("selected", Sel(0, 0)), true);
            c.Snapshot("after");
        });

        Add("ui_trim_start_end", delegate(EditRunner c)
        {
            c.Begin("Edit > Trim Start / Trim End at the cursor, with one event selected, several selected, cursor outside an event", 0, 4);
            c.Audio(0, 2, 8, 3, "single");
            c.Audio(1, 2, 8, 3, "multiA");
            c.Audio(2, 3, 6, 3, "multiB");
            c.Audio(3, 2, 8, 3, "unselected");
            c.ClearSelection();
            c.Select(0, 0);
            c.SetCursor(4);
            c.Snapshot("before");
            c.UiOp("Edit.Trim.Start", new JO().Add("cursor", Ticks(Sec(4))).Add("selected", Sel(0, 0)), true);
            c.ClearSelection();
            c.Select(0, 0);
            c.Select(1, 0);
            c.Select(2, 0);
            c.SetCursor(7);
            c.UiOp("Edit.Trim.End", new JO().Add("cursor", Ticks(Sec(7))).Add("selected", Sel3()), true);
            c.Snapshot("mid");
            c.ClearSelection();
            c.Select(3, 0);
            c.SetCursor(12);                 // after the event: what does Trim Start do
            c.UiOp("Edit.Trim.Start", new JO().Add("cursor", Ticks(Sec(12))).Add("selected", Sel(3, 0)), true);
            c.Snapshot("after");
        });

        Add("ui_trim_start_reversed", delegate(EditRunner c)
        {
            c.Begin("Edit > Trim Start and Trim End of a reversed event and of one with a velocity envelope", 1, 1);
            c.AudioReversed(1, 2, 8, 3, 8, "rev");
            VideoEvent v = c.Video(0, 2, 8, 2, "vel");
            c.AddEnvelopePoints(v.Envelopes, EnvelopeType.Velocity, new double[] { 0, 4, 8 }, new double[] { 1.0, 3.0, 0.5 }, CurveType.Linear);
            c.ClearSelection();
            c.Select(1, 0);
            c.SetCursor(4);
            c.Snapshot("before");
            c.UiOp("Edit.Trim.Start", new JO().Add("cursor", Ticks(Sec(4))).Add("selected", Sel(1, 0)), true);
            c.ClearSelection();
            c.Select(0, 0);
            c.SetCursor(5);
            c.UiOp("Edit.Trim.Start", new JO().Add("cursor", Ticks(Sec(5))).Add("selected", Sel(0, 0)), true);
            c.Snapshot("after");
        });

        Add("ui_trim_selection", delegate(EditRunner c)
        {
            c.Begin("Edit > Trim (Ctrl+T) with a time selection 4 s to 7 s: selected events only, and with none selected", 0, 3);
            c.Audio(0, 2, 8, 0, "a");
            c.Audio(1, 3, 10, 0, "b");
            c.Audio(2, 5, 1, 0, "inside");
            c.ClearSelection();
            c.Select(0, 0);
            c.SetTimeSelection(4, 7);
            c.Snapshot("before");
            c.UiOp("Edit.Trim", new JO().Add("selectionStart", Ticks(Sec(4))).Add("selectionLength", Ticks(Sec(3))).Add("selected", Sel(0, 0)), true);
            c.Snapshot("mid");
            c.ClearSelection();
            c.UiOp("Edit.Trim", new JO().Add("selectionStart", Ticks(Sec(4))).Add("selectionLength", Ticks(Sec(3))).Add("selected", new List<object>()), true);
            c.Snapshot("after");
        });

        Add("ui_delete_selected", delegate(EditRunner c)
        {
            c.Begin("Edit > Delete of a selected event, of one in a group (the partner too?), and of one inside a time selection", 1, 2);
            VideoEvent v = c.Video(0, 2, 6, 0, "V");
            AudioEvent a = c.Audio(1, 2, 6, 0, "A");
            c.Audio(2, 2, 6, 0, "plain");
            try
            {
                TrackEventGroup g = new TrackEventGroup(c.proj);
                c.proj.Groups.Add(g);
                g.Add(v);
                g.Add(a);
            }
            catch (Exception ex) { c.Log("group: " + ex.Message); }
            c.ClearSelection();
            c.Select(0, 0);
            c.Snapshot("before");
            c.UiOp("Edit.Delete", new JO().Add("selected", Sel(0, 0)), true);
            c.Snapshot("mid");
            c.ClearSelection();
            c.Select(2, 0);
            c.SetTimeSelection(3, 5);
            c.UiOp("Edit.Delete", new JO().Add("selectionStart", Ticks(Sec(3))).Add("selectionLength", Ticks(Sec(2))).Add("selected", Sel(2, 0)), true);
            c.Snapshot("after");
        });

        Add("ui_delete_time_selection", delegate(EditRunner c)
        {
            c.Begin("Edit > Delete with a time selection 4 s to 7 s and no event selected: the time is taken out of every track (events split), no ripple", 1, 2);
            c.Video(0, 2, 8, 0, "V");
            c.Audio(1, 3, 10, 0, "A");
            c.Audio(2, 5, 1, 0, "inside");
            c.Audio(2, 8, 3, 0, "after");
            c.ClearSelection();
            c.SetTimeSelection(4, 7);
            c.Snapshot("before");
            c.UiOp("Edit.Delete", new JO().Add("selectionStart", Ticks(Sec(4))).Add("selectionLength", Ticks(Sec(3))).Add("selected", new List<object>()), true);
            c.Snapshot("after");
        });

        Add("ui_cut_paste", delegate(EditRunner c)
        {
            c.Begin("Edit > Cut, then Paste at a new cursor position; Copy then Paste on a track that has an event there", 0, 3);
            AudioEvent a = c.Audio(0, 2, 4, 1, "cutMe");
            c.SetFades(a, 1, CurveType.Smooth, 1, CurveType.Slow);
            c.Audio(1, 2, 4, 1, "copyMe");
            c.Audio(1, 12, 4, 0, "inTheWay");
            c.ClearSelection();
            c.Select(0, 0);
            c.Snapshot("before");
            c.UiOp("Edit.Cut", new JO().Add("selected", Sel(0, 0)), true);
            c.SetCursor(9);
            c.SelectTrack(0);
            c.UiOp("Edit.Paste", new JO().Add("cursor", Ticks(Sec(9))).Add("track", 0), true);
            c.Snapshot("mid");
            c.ClearSelection();
            c.Select(1, 0);
            c.UiOp("Edit.Copy", new JO().Add("selected", Sel(1, 0)), false);
            c.SetCursor(13);
            c.SelectTrack(1);
            c.UiOp("Edit.Paste", new JO().Add("cursor", Ticks(Sec(13))).Add("track", 1), true);
            c.Snapshot("after");
        });

        Add("ui_paste_insert", delegate(EditRunner c)
        {
            c.Begin("Edit > Paste Insert with the cursor in the middle of an event (it is split), later events move", 0, 2);
            c.Audio(0, 2, 6, 0, "src");
            c.Audio(1, 0, 10, 0, "target");
            c.Audio(1, 12, 4, 0, "later");
            c.ClearSelection();
            c.Select(0, 0);
            c.Snapshot("before");
            c.UiOp("Edit.Copy", new JO().Add("selected", Sel(0, 0)), false);
            c.SetCursor(4);
            c.ClearSelection();
            c.SelectTrack(1);
            c.UiOp("Edit.PasteInsert", new JO().Add("cursor", Ticks(Sec(4))).Add("track", 1), true);
            c.Snapshot("after");
        });

        Add("ui_ripple_after_delete", delegate(EditRunner c)
        {
            c.Begin("Delete an event then Post-Edit Ripple (Affected Tracks / All Tracks)", 1, 2);
            c.Video(0, 0, 4, 0, "v1");
            c.Video(0, 4, 4, 0, "v2");
            c.Audio(1, 0, 4, 0, "a1");
            c.Audio(1, 4, 4, 0, "a2");
            c.Audio(2, 0, 4, 0, "b1");
            c.Audio(2, 4, 4, 0, "b2");
            c.ClearSelection();
            c.Select(1, 0);
            c.Snapshot("before");
            c.UiOp("Edit.Delete", new JO().Add("selected", Sel(1, 0)), true);
            c.UiOp("Edit.Ripple.AffectedTracks", new JO(), true);
            c.Snapshot("mid");
            c.ClearSelection();
            c.Select(0, 0);
            c.UiOp("Edit.Delete", new JO().Add("selected", Sel(0, 0)), true);
            c.UiOp("Edit.Ripple.All", new JO(), true);
            c.Snapshot("after");
        });

        Add("ui_heal_closegaps", delegate(EditRunner c)
        {
            c.Begin("Split, then Event Heal of the two halves; Close Gaps on events with gaps between", 0, 2);
            c.Audio(0, 2, 8, 1, "healMe");
            c.Audio(1, 1, 2, 0, "g1");
            c.Audio(1, 5, 2, 0, "g2");
            c.Audio(1, 9, 2, 0, "g3");
            c.ClearSelection();
            c.Select(0, 0);
            c.SetCursor(5);
            c.Snapshot("before");
            c.UiOp("Edit.Split", new JO().Add("cursor", Ticks(Sec(5))).Add("selected", Sel(0, 0)), true);
            c.ClearSelection();
            c.Select(0, 0);
            c.Select(0, 1);
            c.UiOp("Edit.EventHeal", new JO().Add("selected", Sel2(0, 0, 0, 1)), true);
            c.Snapshot("mid");
            c.ClearSelection();
            c.Select(1, 0);
            c.Select(1, 1);
            c.Select(1, 2);
            c.UiOp("Edit.CloseGaps", new JO().Add("selected", Sel3b()), true);
            c.Snapshot("after");
        });
    }

    // the selection as the replay wants it: [[track, index], ...]
    static List<object> Sel(int t, int i)
    {
        List<object> l = new List<object>();
        l.Add(new List<object>(new object[] { t, i }));
        return l;
    }

    static List<object> Sel2(int t1, int i1, int t2, int i2)
    {
        List<object> l = Sel(t1, i1);
        l.Add(new List<object>(new object[] { t2, i2 }));
        return l;
    }

    static List<object> Sel3()
    {
        List<object> l = Sel(0, 0);
        l.Add(new List<object>(new object[] { 1, 0 }));
        l.Add(new List<object>(new object[] { 2, 0 }));
        return l;
    }

    static List<object> Sel3b()
    {
        List<object> l = Sel(1, 0);
        l.Add(new List<object>(new object[] { 1, 1 }));
        l.Add(new List<object>(new object[] { 1, 2 }));
        return l;
    }
}
