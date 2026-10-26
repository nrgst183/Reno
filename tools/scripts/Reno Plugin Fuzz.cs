/**
 * Reno Plugin Fuzz: saves VEGAS's own result for every video effect, transition and audio effect, to compare Reno against.
 * Run in VEGAS Pro 18 (Tools > Scripting > Run Script). Asks for an output folder (default Documents\Reno Plugin Fuzz\<date>),
 * makes a new 320x180 30 fps project (the open project must have no unsaved changes) and writes to the folder:
 *   fuzz_picture.png, fuzz_picture2.png, fuzz_test.wav   generated test media
 *   fuzz.veg                  the test project
 *   manifest.json             every test: kind, plug-in, variant, parameters, time range, snapshot files, status
 *   effects.txt               every plug-in found, with presets and (OpenFX) parameter ranges
 *   snap_NNNN.png             VEGAS's picture of each video test (transitions: _25 _50 _75)
 *   fuzz_audio.wav            the project's audio rendered by VEGAS
 *   fuzz_log.txt, api.txt, render_templates.txt   debugging output
 * Video track 1 has one 1 s event per test (defaults, up to 4 presets, 3 random settings for OpenFX effects); track 2 has the
 * transitions; the audio track has a 2 s event per audio effect followed by a 1.5 s gap for the tail.
 * It takes a while. The progress window has a Stop button and manifest.json is rewritten as it goes. If VEGAS crashes,
 * progress.txt names the plug-in; list such plug-ins (one per line) in skip.txt in the output folder or its parent and run again.
 * C# 5. Anything uncertain goes through reflection, so a missing property costs a log line, not the run.
 **/
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
    const string Title = "Reno Plugin Fuzz";
    const int Fps = 30;
    const int EffectFrames = 30;            // a video test: 1 s
    const int TransFrames = 60;             // a transition test: A 45 frames, B 45 frames, overlapping by 30
    const int TransAFrames = 45;
    const int TransOverlap = 30;
    const double AudioLength = 2.0;         // the test sound
    const double AudioSlot = 3.5;           // sound + 1.5 s for the tail
    const int PresetCap = 4;
    const int TransPresetCap = 2;
    const int RandomVariants = 3;
    static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

    Vegas vegas;
    Project p;
    string folder;
    StreamWriter log, effectsTxt, apiTxt, progressTxt;
    Form progressForm;
    Label progressLabel;
    ProgressBar progressBar;
    bool cancelled;
    List<FuzzTest> tests = new List<FuzzTest>();
    List<string> skipList = new List<string>();
    Media picture1, picture2, sound;
    VideoTrack vtrack1, vtrack2;
    AudioTrack atrack;
    long nextFrame = 0;                     // the video timeline's end so far
    int nextAudioSlot = 0;
    string audioMode = "event";             // "event": the effect is on the audio event; "track": one track per test
    string renderInfo = "";
    string renderStatus = "not done";
    string renderTemplate = "";
    string vegasVersion = "";
    Dictionary<string, bool> apiDumped = new Dictionary<string, bool>();
    int snapshotsDone = 0, snapshotsFailed = 0;

    // ------------------------------------------------------------------------------------------ records
    class ParamRec
    {
        public string Name = "", Type = "", Value = "", ReadBack = "", Min = "", Max = "", Default = "", Source = "", Error = "", Skipped = "";
    }

    class FuzzTest
    {
        public int Id;
        public string Kind = "", Plugin = "", Uid = "", ClassId = "", Variant = "", Mode = "";
        public bool Ofx;
        public double Start, Length;                        // the test's range on the timeline (s)
        public long StartFrame, LengthFrames;
        public double TransStart, TransLength;              // transitions: where the transition itself is (s)
        public long TransStartFrame, TransLengthFrames;
        public List<ParamRec> Params = new List<ParamRec>();
        public List<string> SnapFiles = new List<string>();
        public List<long> SnapFrames = new List<long>();
        public List<int> SnapPercent = new List<int>();
        public List<string> SnapResult = new List<string>();
        public string Status = "ok", Error = "", Note = "";
    }

    class Plug
    {
        public PlugInNode Node;
        public string Name = "", Uid = "", ClassId = "", Kind = "", Flags = "";
        public bool Ofx;
        public List<string> Presets = new List<string>();
    }

    delegate void Apply(Effect fx, FuzzTest t);

    // ------------------------------------------------------------------------------------------ entry
    public void FromVegas(Vegas v)
    {
        vegas = v;
        try { Run(); }
        catch (Exception e)
        {
            Log("SCRIPT ERROR: " + e.ToString());
            try { MessageBox.Show(vegas.MainWindow, Title + " stopped:\n\n" + Msg(e) + "\n\nWhat was made so far is in the output folder (manifest.json, fuzz_log.txt).", Title); } catch (Exception) { }
        }
        finally
        {
            try { if (progressForm != null) progressForm.Close(); } catch (Exception) { }
            try { WriteManifest(); } catch (Exception) { }
            try { if (log != null) { log.Flush(); log.Close(); } } catch (Exception) { }
            try { if (effectsTxt != null) { effectsTxt.Flush(); effectsTxt.Close(); } } catch (Exception) { }
            try { if (apiTxt != null) { apiTxt.Flush(); apiTxt.Close(); } } catch (Exception) { }
            try { if (progressTxt != null) { progressTxt.Flush(); progressTxt.Close(); } } catch (Exception) { }
        }
    }

    void Run()
    {
        string appName = "";
        try { appName = Convert.ToString(Get(vegas, "ShortAppName")); } catch (Exception) { }
        if (appName.IndexOf("Reno", StringComparison.OrdinalIgnoreCase) >= 0)
        {
            MessageBox.Show(vegas.MainWindow, "This script needs VEGAS Pro itself (Tools > Scripting > Run Script), not Reno.", Title);
            return;
        }
        bool modified = true;
        try { modified = Convert.ToBoolean(Get(vegas.Project, "IsModified")); } catch (Exception) { }
        if (modified)
        {
            MessageBox.Show(vegas.MainWindow, "The project that is open has unsaved changes.\n\nThis script replaces the open project by its test project, so save it first (Ctrl+S, or close it) and run the script again.",
                            Title, MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }
        string root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "Reno Plugin Fuzz");
        folder = AskFolder(Path.Combine(root, DateTime.Now.ToString("yyyyMMdd_HHmm", Inv)));
        if (folder == null) return;
        Directory.CreateDirectory(folder);
        log = new StreamWriter(Path.Combine(folder, "fuzz_log.txt"), false, new UTF8Encoding(false));
        log.AutoFlush = true;
        effectsTxt = new StreamWriter(Path.Combine(folder, "effects.txt"), false, new UTF8Encoding(false));
        apiTxt = new StreamWriter(Path.Combine(folder, "api.txt"), false, new UTF8Encoding(false));
        progressTxt = new StreamWriter(Path.Combine(folder, "progress.txt"), false, new UTF8Encoding(false));
        progressTxt.AutoFlush = true;
        try { vegasVersion = Convert.ToString(Get(vegas, "LongAppName")) + " " + Convert.ToString(Get(vegas, "Version")); } catch (Exception) { }
        Log("started " + vegasVersion + " into " + folder);
        ReadSkipList();
        Progress("Writing the test media...", 0, 1);

        // the media
        string pic1Path = Path.Combine(folder, "fuzz_picture.png");
        string pic2Path = Path.Combine(folder, "fuzz_picture2.png");
        string wavPath = Path.Combine(folder, "fuzz_test.wav");
        WritePictures(pic1Path, pic2Path);
        WriteWav(wavPath);

        // the new project
        Progress("Making the new project...", 0, 1);
        if (!vegas.NewProject(true, false)) return;
        p = vegas.Project;
        p.Video.Width = 320;
        p.Video.Height = 180;
        p.Video.FrameRate = Fps;
        p.Video.PixelAspectRatio = 1.0;
        p.Video.FieldOrder = VideoFieldOrder.ProgressiveScan;
        TrySet(p.Video, "RenderQuality", "Best");
        TrySet(p.Video, "PixelFormat", "Normal");
        TrySet(Get(p, "Audio"), "SampleRate", 48000);
        TrySet(p.Preview, "RenderQuality", "Best");
        TrySet(p.Preview, "FullSize", true);
        TrySet(p.Preview, "PreviewMode", "Full");
        picture1 = new Media(pic1Path);
        picture2 = new Media(pic2Path);
        sound = new Media(wavPath);

        // the plug-ins
        Progress("Looking for plug-ins...", 0, 1);
        List<Plug> videoFx = Plugs(vegas.VideoFX, "video");
        List<Plug> transitions = Plugs(Get(vegas, "Transitions") as PlugInNode, "transition");
        List<Plug> audioFx = Plugs(vegas.AudioFX, "audio");
        Log("plug-ins: " + videoFx.Count + " video effects, " + transitions.Count + " transitions, " + audioFx.Count + " audio effects");
        WriteEffectsTxt(videoFx, transitions, audioFx);
        DumpApi();

        // build
        int total = videoFx.Count + transitions.Count + audioFx.Count;
        int done = 0;
        vtrack1 = p.AddVideoTrack();
        vtrack1.Name = "Fuzz video 1 (effects)";
        foreach (Plug pl in videoFx)
        {
            if (cancelled) break;
            Progress("Building video effect " + pl.Name, done++, total);
            BuildVideoPlug(pl);
        }
        vtrack2 = p.AddVideoTrack();
        vtrack2.Name = "Fuzz video 2 (transitions)";
        foreach (Plug pl in transitions)
        {
            if (cancelled) break;
            Progress("Building transition " + pl.Name, done++, total);
            BuildTransition(pl);
        }
        if (!cancelled && audioFx.Count > 0)
        {
            DecideAudioMode();
            foreach (Plug pl in audioFx)
            {
                if (cancelled) break;
                Progress("Building audio effect " + pl.Name, done++, total);
                BuildAudioPlug(pl);
            }
        }
        Log("built " + tests.Count + " tests");

        // save the project, then take the pictures
        string vegPath = Path.Combine(folder, "fuzz.veg");
        Progress("Saving fuzz.veg...", 0, 1);
        try { vegas.SaveProject(vegPath); Log("saved " + vegPath); }
        catch (Exception e) { Log("SAVE FAILED: " + Msg(e)); }
        WriteManifest();
        TakeSnapshots();
        WriteManifest();

        // the audio
        if (!cancelled || nextAudioSlot > 0)
        {
            Progress("Rendering the audio with VEGAS...", 0, 1);
            RenderAudio(Path.Combine(folder, "fuzz_audio.wav"));
        }
        WriteManifest();
        Progress("Done", 1, 1);

        int failed = 0;
        foreach (FuzzTest t in tests) if (t.Status != "ok") failed++;
        string msg = (cancelled ? "STOPPED early.\n\n" : "Done.\n\n") + tests.Count + " tests (" + failed + " failed to build), " + snapshotsDone + " snapshots (" + snapshotsFailed + " failed).\n" +
                     "Audio render: " + renderStatus + (renderTemplate.Length > 0 ? " (" + renderTemplate + ")" : "") + ".\n\n" +
                     "Folder:\n" + folder;
        try { MessageBox.Show(vegas.MainWindow, msg, Title, MessageBoxButtons.OK, MessageBoxIcon.Information); } catch (Exception) { }
    }

    // ------------------------------------------------------------------------------------------ the dialogs
    string AskFolder(string defaultDir)
    {
        Form f = new Form();
        f.Text = Title;
        f.FormBorderStyle = FormBorderStyle.FixedDialog;
        f.MaximizeBox = false;
        f.MinimizeBox = false;
        f.ShowInTaskbar = false;
        f.StartPosition = FormStartPosition.CenterParent;
        f.ClientSize = new System.Drawing.Size(560, 270);
        Label info = new Label();
        info.Left = 12; info.Top = 10; info.Width = 536; info.Height = 180;
        info.Text =
            "This makes a new test project (320x180, 30 fps, 48 kHz stereo) with a test for every video effect, transition and audio " +
            "effect VEGAS has - at its defaults, with its presets and (OpenFX effects) with random settings - saves a picture of each " +
            "test, renders the audio, and writes fuzz.veg, manifest.json and effects.txt, for Reno to be compared with.\n\n" +
            "The project that is open now is replaced (it is never saved over). It takes a while (many hundreds of pictures); do not use VEGAS meanwhile.\n\n" +
            "Output folder (made if it does not exist):";
        f.Controls.Add(info);
        TextBox box = new TextBox();
        box.Left = 12; box.Top = 195; box.Width = 440;
        box.Text = defaultDir;
        f.Controls.Add(box);
        Button browse = new Button();
        browse.Left = 460; browse.Top = 193; browse.Width = 88; browse.Text = "Browse...";
        browse.Click += delegate(object s, EventArgs e)
        {
            FolderBrowserDialog d = new FolderBrowserDialog();
            d.Description = "Folder for the fuzz test";
            d.SelectedPath = Directory.Exists(box.Text) ? box.Text : Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
            if (d.ShowDialog(f) == DialogResult.OK) box.Text = d.SelectedPath;
        };
        f.Controls.Add(browse);
        Button ok = new Button();
        ok.Left = 372; ok.Top = 230; ok.Width = 84; ok.Text = "Start";
        ok.DialogResult = DialogResult.OK;
        f.Controls.Add(ok);
        Button cancel = new Button();
        cancel.Left = 464; cancel.Top = 230; cancel.Width = 84; cancel.Text = "Cancel";
        cancel.DialogResult = DialogResult.Cancel;
        f.Controls.Add(cancel);
        f.AcceptButton = ok;
        f.CancelButton = cancel;
        string result = null;
        if (f.ShowDialog(vegas.MainWindow) == DialogResult.OK && box.Text.Trim().Length > 0)
        {
            try { result = Path.GetFullPath(box.Text.Trim()); } catch (Exception) { result = null; }
            if (result != null && Directory.Exists(result) && File.Exists(Path.Combine(result, "manifest.json")))
            {
                if (MessageBox.Show(vegas.MainWindow, "The folder already holds a fuzz run. Its files are replaced.\n\nContinue?", Title, MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) result = null;
            }
        }
        f.Dispose();
        return result;
    }

    void Progress(string text, int done, int total)
    {
        try
        {
            if (progressTxt != null) progressTxt.WriteLine(DateTime.Now.ToString("HH:mm:ss", Inv) + "  " + text);
            if (progressForm == null)
            {
                progressForm = new Form();
                progressForm.Text = Title;
                progressForm.FormBorderStyle = FormBorderStyle.FixedToolWindow;
                progressForm.StartPosition = FormStartPosition.CenterScreen;
                progressForm.ClientSize = new System.Drawing.Size(560, 100);
                progressLabel = new Label();
                progressLabel.Left = 10; progressLabel.Top = 8; progressLabel.Width = 540; progressLabel.Height = 30;
                progressForm.Controls.Add(progressLabel);
                progressBar = new ProgressBar();
                progressBar.Left = 10; progressBar.Top = 42; progressBar.Width = 540; progressBar.Height = 16;
                progressForm.Controls.Add(progressBar);
                Button stop = new Button();
                stop.Left = 460; stop.Top = 66; stop.Width = 90; stop.Text = "Stop";
                stop.Click += delegate(object s, EventArgs e) { cancelled = true; };
                progressForm.Controls.Add(stop);
                progressForm.Show();
            }
            progressLabel.Text = text;
            progressBar.Maximum = Math.Max(1, total);
            progressBar.Value = Math.Max(0, Math.Min(progressBar.Maximum, done));
            System.Windows.Forms.Application.DoEvents();
        }
        catch (Exception) { }
    }

    void Log(string text)
    {
        try { if (log != null) log.WriteLine(DateTime.Now.ToString("HH:mm:ss", Inv) + "  " + text); } catch (Exception) { }
    }

    void ReadSkipList()
    {
        string[] places = { Path.Combine(folder, "skip.txt"), Path.Combine(Path.GetDirectoryName(folder) ?? folder, "skip.txt") };
        foreach (string f in places)
        {
            try
            {
                if (!File.Exists(f)) continue;
                foreach (string line in File.ReadAllLines(f))
                {
                    string s = line.Trim();
                    if (s.Length > 0 && !s.StartsWith("#")) skipList.Add(s);
                }
                Log("skip list " + f + ": " + skipList.Count + " names");
            }
            catch (Exception) { }
        }
    }

    bool Skipped(string name)
    {
        foreach (string s in skipList) if (string.Equals(s, name, StringComparison.OrdinalIgnoreCase)) return true;
        return false;
    }

    // ------------------------------------------------------------------------------------------ plug-ins
    List<Plug> Plugs(PlugInNode root, string kind)
    {
        List<PlugInNode> nodes = new List<PlugInNode>();
        Collect(root, nodes, 0);
        List<Plug> list = new List<Plug>();
        Dictionary<string, bool> seen = new Dictionary<string, bool>();
        foreach (PlugInNode n in nodes)
        {
            Plug pl = new Plug();
            pl.Node = n;
            pl.Kind = kind;
            pl.Name = n.Name ?? "";
            pl.Uid = FirstText(n, "UniqueID", "ID");
            pl.ClassId = FirstText(n, "ClassID", "ClassId", "Guid");
            try { pl.Ofx = Convert.ToBoolean(Get(n, "IsOFX")); } catch (Exception) { }
            string flags = "";
            foreach (string fl in new string[] { "IsDirectX", "IsVST", "IsVST3", "IsOFX", "IsLegacy", "IsAudio", "IsVideo" })
            {
                try { object o = Get(n, fl); if (o != null && Convert.ToBoolean(o)) flags += (flags.Length > 0 ? "," : "") + fl.Substring(2); } catch (Exception) { }
            }
            pl.Flags = flags;
            pl.Presets = PresetNames(n);
            if (seen.ContainsKey(pl.Name + "|" + pl.Uid)) continue;
            seen[pl.Name + "|" + pl.Uid] = true;
            list.Add(pl);
        }
        return list;
    }

    static void Collect(PlugInNode node, List<PlugInNode> into, int depth)
    {
        if (node == null || depth > 8) return;
        bool container = depth == 0;
        try { container = Convert.ToBoolean(Get(node, "IsContainer")); } catch (Exception) { }
        if (!container) { if (depth > 0) into.Add(node); return; }
        try { foreach (PlugInNode child in node) Collect(child, into, depth + 1); } catch (Exception) { }
    }

    static List<string> PresetNames(PlugInNode plug)
    {
        List<string> names = new List<string>();
        try
        {
            IEnumerable presets = Get(plug, "Presets") as IEnumerable;
            if (presets != null) foreach (object pr in presets) { string n = S(Get(pr, "Name")); if (n.Length > 0) names.Add(n); }
        }
        catch (Exception) { }
        return names;
    }

    static List<string> Pick(List<string> all, int cap)
    {
        if (all.Count <= cap) return all;
        List<string> r = new List<string>();
        for (int i = 0; i < cap; ++i)
        {
            string s = all[(int)Math.Round(i * (all.Count - 1.0) / (cap - 1))];
            if (!r.Contains(s)) r.Add(s);
        }
        return r;
    }

    void WriteEffectsTxt(List<Plug> v, List<Plug> t, List<Plug> a)
    {
        effectsTxt.WriteLine("# Reno Plugin Fuzz: the plug-ins " + vegasVersion + " has");
        WriteGroup("VIDEO EFFECTS", v, true);
        WriteGroup("TRANSITIONS", t, false);
        WriteGroup("AUDIO EFFECTS", a, false);
        effectsTxt.Flush();
    }

    void WriteGroup(string title, List<Plug> list, bool withParams)
    {
        effectsTxt.WriteLine();
        effectsTxt.WriteLine("==== " + title + " (" + list.Count + ")");
        foreach (Plug pl in list)
        {
            effectsTxt.WriteLine(pl.Name + "\t" + pl.Uid + "\t" + pl.ClassId + "\t" + (pl.Ofx ? "OpenFX" : "not OpenFX") + (pl.Flags.Length > 0 ? "\t" + pl.Flags : ""));
            effectsTxt.WriteLine("  presets (" + pl.Presets.Count + "): " + string.Join(" | ", pl.Presets.ToArray()));
            if (withParams && pl.Ofx && !Skipped(pl.Name))
            {
                try
                {
                    Effect probe = new Effect(pl.Node);
                    object ofxe = Get(probe, "OFXEffect");
                    foreach (object par in ParamsOf(ofxe))
                    {
                        DumpOnce(par);
                        effectsTxt.WriteLine("  param " + S(Get(par, "Name")) + " (" + par.GetType().Name + ") min " + Fmt(FirstObj(par, "Min", "Minimum")) + " max " + Fmt(FirstObj(par, "Max", "Maximum")) +
                                             " displaymin " + Fmt(Get(par, "DisplayMin")) + " displaymax " + Fmt(Get(par, "DisplayMax")) + " default " + Fmt(Get(par, "Default")) + ValueNote(par));
                    }
                    DumpOnce(probe);
                    DumpOnce(ofxe);
                }
                catch (Exception e) { effectsTxt.WriteLine("  (parameters: " + Msg(e) + ")"); }
            }
        }
        effectsTxt.Flush();
    }

    static string ValueNote(object par)
    {
        string n = "";
        try
        {
            IEnumerable ch = Get(par, "Choices") as IEnumerable;
            if (ch != null)
            {
                List<string> names = new List<string>();
                foreach (object c in ch) { names.Add(ChoiceName(c)); if (names.Count > 40) break; }
                n += " choices [" + string.Join(" | ", names.ToArray()) + "]";
            }
        }
        catch (Exception) { }
        return n;
    }

    static IEnumerable<object> ParamsOf(object ofxEffect)
    {
        List<object> list = new List<object>();
        if (ofxEffect == null) return list;
        IEnumerable pars = Get(ofxEffect, "Parameters") as IEnumerable;
        if (pars != null) foreach (object o in pars) if (o != null) list.Add(o);
        return list;
    }

    // ------------------------------------------------------------------------------------------ the test list helpers
    FuzzTest NewTest(string kind, Plug pl, string variant)
    {
        FuzzTest t = new FuzzTest();
        t.Id = tests.Count;
        t.Kind = kind;
        t.Plugin = pl.Name;
        t.Uid = pl.Uid;
        t.ClassId = pl.ClassId;
        t.Ofx = pl.Ofx;
        t.Variant = variant;
        return t;
    }

    void Finish(FuzzTest t)
    {
        tests.Add(t);
        if (t.Status != "ok") Log("FAILED " + t.Kind + " '" + t.Plugin + "' " + t.Variant + ": " + t.Error);
        if (tests.Count % 20 == 0) WriteManifest();
        System.Windows.Forms.Application.DoEvents();
    }

    // ------------------------------------------------------------------------------------------ video effects
    void BuildVideoPlug(Plug pl)
    {
        if (Skipped(pl.Name)) { Log("skipped (skip.txt): " + pl.Name); return; }
        Mark("video effect " + pl.Name);
        VideoTest(pl, "default", null);
        foreach (string preset in Pick(pl.Presets, PresetCap))
        {
            string pr = preset;
            VideoTest(pl, "preset:" + pr, delegate(Effect fx, FuzzTest t) { fx.Preset = pr; });
        }
        if (pl.Ofx)
        {
            for (int k = 1; k <= RandomVariants; ++k)
            {
                Rng rng = new Rng(Fnv(pl.Name) + (uint)k * 7919u);
                VideoTest(pl, "random" + k, delegate(Effect fx, FuzzTest t) { ApplyRandom(fx, t, rng); });
            }
        }
    }

    void VideoTest(Plug pl, string variant, Apply apply)
    {
        FuzzTest t = NewTest("video", pl, variant);
        long f0 = nextFrame;
        nextFrame += EffectFrames;
        t.StartFrame = f0; t.LengthFrames = EffectFrames; t.Start = f0 / (double)Fps; t.Length = EffectFrames / (double)Fps;
        t.SnapFrames.Add(f0 + EffectFrames / 2);
        t.SnapPercent.Add(50);
        VideoEvent ev = null;
        try
        {
            ev = vtrack1.AddVideoEvent(Timecode.FromFrames(f0), Timecode.FromFrames(EffectFrames));
            ev.Name = pl.Name + " " + variant;
            DumpOnce(ev); DumpOnce(vtrack1);
            ev.AddTake(picture1.GetVideoStreamByIndex(0));
            Effect fx = new Effect(pl.Node);
            ev.Effects.Add(fx);
            if (apply != null) apply(fx, t);
            if (pl.Ofx && !variant.StartsWith("random")) ReadBack(fx, t);
            int bad = 0;
            foreach (ParamRec r in t.Params) if (r.Error.Length > 0) bad++;
            if (bad > 0) t.Note = bad + " parameter(s) could not be set";
        }
        catch (Exception e)
        {
            t.Status = "FAILED";
            t.Error = Msg(e);
            TryRemove(vtrack1.Events, ev);
        }
        Finish(t);
    }

    // ------------------------------------------------------------------------------------------ OpenFX parameters
    void ReadBack(Effect fx, FuzzTest t)
    {
        object ofxe = Get(fx, "OFXEffect");
        foreach (object par in ParamsOf(ofxe))
        {
            ParamRec r = new ParamRec();
            r.Name = S(Get(par, "Name"));
            r.Type = par.GetType().Name;
            r.Source = "readback";
            try
            {
                PropertyInfo vp = par.GetType().GetProperty("Value");
                if (vp == null || !vp.CanRead) { r.Skipped = "no value"; }
                else
                {
                    object v = vp.GetValue(par, null);
                    r.Value = FmtValue(v);
                    r.ReadBack = r.Value;
                }
                FillRange(par, r);
            }
            catch (Exception e) { r.Error = Msg(e); }
            t.Params.Add(r);
        }
    }

    void ApplyRandom(Effect fx, FuzzTest t, Rng rng)
    {
        object ofxe = Get(fx, "OFXEffect");
        if (ofxe == null) throw new Exception("no OFXEffect");
        foreach (object par in ParamsOf(ofxe)) t.Params.Add(RandomizeParam(par, rng));
    }

    static void FillRange(object par, ParamRec r)
    {
        r.Min = Fmt(FirstObj(par, "Min", "Minimum"));
        r.Max = Fmt(FirstObj(par, "Max", "Maximum"));
        r.Default = Fmt(Get(par, "Default"));
    }

    ParamRec RandomizeParam(object par, Rng rng)
    {
        ParamRec r = new ParamRec();
        r.Source = "set";
        try
        {
            r.Name = S(Get(par, "Name"));
            r.Type = par.GetType().Name;
            FillRange(par, r);
            PropertyInfo vp = par.GetType().GetProperty("Value");
            if (vp == null || !vp.CanRead || !vp.CanWrite) { r.Skipped = "not settable"; r.Source = "none"; return r; }
            Type vt = vp.PropertyType;
            object nv = null;
            IEnumerable choices = null;
            try { choices = Get(par, "Choices") as IEnumerable; } catch (Exception) { }
            List<object> list = new List<object>();
            if (choices != null && vt != typeof(bool)) foreach (object c in choices) list.Add(c);
            if (list.Count > 0)
            {
                int idx = rng.Next(list.Count);
                nv = list[idx];
                if (!vt.IsAssignableFrom(nv.GetType())) nv = Convert.ChangeType(nv, vt, Inv);
            }
            else if (vt == typeof(bool)) nv = rng.Next(2) == 1;
            else if (vt == typeof(string)) { r.Skipped = "text"; r.Source = "none"; return r; }
            else if (vt.IsEnum)
            {
                Array vals = Enum.GetValues(vt);
                nv = vals.GetValue(rng.Next(vals.Length));
            }
            else if (IsNumber(vt))
            {
                double lo, hi;
                RangeOf(par, 0, false, out lo, out hi);
                double x = lo + rng.NextDouble() * (hi - lo);
                if (vt != typeof(double) && vt != typeof(float) && vt != typeof(decimal)) x = Math.Max(Math.Ceiling(lo), Math.Min(Math.Floor(hi), Math.Round(x)));
                nv = Convert.ChangeType(x, vt, Inv);
            }
            else
            {
                string[] names = CompNames(vt);
                if (names == null) { r.Skipped = "unknown value type " + vt.Name; r.Source = "none"; return r; }
                object cur = vp.GetValue(par, null);
                object bag = cur;
                if (bag == null) bag = Activator.CreateInstance(vt);
                bool colour = names[0] == "R" || names[0] == "Red";
                for (int i = 0; i < names.Length; ++i)
                {
                    MemberInfo mi = FindMember(vt, names[i]);
                    if (mi == null) continue;
                    Type mt = MemberType(mi);
                    if (!IsNumber(mt)) continue;
                    double lo, hi;
                    RangeOf(par, i, colour, out lo, out hi);
                    double x = lo + rng.NextDouble() * (hi - lo);
                    if (mt != typeof(double) && mt != typeof(float) && mt != typeof(decimal)) x = Math.Max(Math.Ceiling(lo), Math.Min(Math.Floor(hi), Math.Round(x)));
                    SetMember(bag, mi, Convert.ChangeType(x, mt, Inv));
                }
                nv = bag;
            }
            r.Value = FmtValue(nv);
            vp.SetValue(par, nv, null);
            r.ReadBack = FmtValue(vp.GetValue(par, null));
        }
        catch (Exception e) { r.Error = Msg(e); }
        return r;
    }

    static bool IsNumber(Type t)
    {
        return t == typeof(double) || t == typeof(float) || t == typeof(int) || t == typeof(long) || t == typeof(short) || t == typeof(byte) || t == typeof(uint) || t == typeof(decimal);
    }

    static string[] CompNames(Type t)
    {
        string[][] sets = { new string[] { "X", "Y", "Z" }, new string[] { "R", "G", "B", "A" }, new string[] { "Red", "Green", "Blue", "Alpha" }, new string[] { "Width", "Height" } };
        foreach (string[] set in sets)
        {
            if (FindMember(t, set[0]) == null) continue;
            List<string> names = new List<string>();
            foreach (string n in set) if (FindMember(t, n) != null) names.Add(n);
            return names.ToArray();
        }
        return null;
    }

    static MemberInfo FindMember(Type t, string name)
    {
        PropertyInfo pi = t.GetProperty(name);
        if (pi != null) return pi;
        return t.GetField(name);
    }

    static Type MemberType(MemberInfo mi)
    {
        PropertyInfo pi = mi as PropertyInfo;
        if (pi != null) return pi.PropertyType;
        return ((FieldInfo)mi).FieldType;
    }

    static object GetMember(object o, MemberInfo mi)
    {
        PropertyInfo pi = mi as PropertyInfo;
        if (pi != null) return pi.GetValue(o, null);
        return ((FieldInfo)mi).GetValue(o);
    }

    static void SetMember(object o, MemberInfo mi, object v)
    {
        PropertyInfo pi = mi as PropertyInfo;
        if (pi != null) { if (pi.CanWrite) pi.SetValue(o, v, null); return; }
        ((FieldInfo)mi).SetValue(o, v);
    }

    // the numbers a value or a range holds: a number alone, or X Y Z / R G B A
    static double[] Comps(object o)
    {
        if (o == null) return new double[0];
        if (o is string || o is bool) return new double[0];
        try
        {
            if (o is IConvertible && !(o is char) && !(o is DateTime)) return new double[] { Convert.ToDouble(o, Inv) };
        }
        catch (Exception) { return new double[0]; }
        string[] names = CompNames(o.GetType());
        if (names == null) return new double[0];
        List<double> d = new List<double>();
        foreach (string n in names)
        {
            try { d.Add(Convert.ToDouble(GetMember(o, FindMember(o.GetType(), n)), Inv)); } catch (Exception) { d.Add(double.NaN); }
        }
        return d.ToArray();
    }

    static bool Good(double v) { return !double.IsNaN(v) && !double.IsInfinity(v) && Math.Abs(v) < 1e6; }

    // the range the i-th component of the parameter may take: the display range if it has one, else min..max, else around the default
    static void RangeOf(object par, int i, bool colour, out double lo, out double hi)
    {
        lo = double.NaN; hi = double.NaN;
        double[] a = Comps(Get(par, "DisplayMin")), b = Comps(Get(par, "DisplayMax"));
        if (a.Length == 0 || b.Length == 0) { a = Comps(FirstObj(par, "Min", "Minimum")); b = Comps(FirstObj(par, "Max", "Maximum")); }
        if (a.Length > 0 && b.Length > 0)
        {
            double x = a[Math.Min(i, a.Length - 1)], y = b[Math.Min(i, b.Length - 1)];
            if (Good(x) && Good(y) && y > x) { lo = x; hi = y; }
            else
            {
                // a one-sided limit: use the finite side
                double mn = Comps(FirstObj(par, "Min", "Minimum")).Length > 0 ? Comps(FirstObj(par, "Min", "Minimum"))[Math.Min(i, Comps(FirstObj(par, "Min", "Minimum")).Length - 1)] : double.NaN;
                double mx = Comps(FirstObj(par, "Max", "Maximum")).Length > 0 ? Comps(FirstObj(par, "Max", "Maximum"))[Math.Min(i, Comps(FirstObj(par, "Max", "Maximum")).Length - 1)] : double.NaN;
                if (Good(mn) && Good(mx) && mx > mn) { lo = mn; hi = mx; }
            }
        }
        if (double.IsNaN(lo))
        {
            double[] d = Comps(Get(par, "Default"));
            double def = d.Length > 0 ? d[Math.Min(i, d.Length - 1)] : 0.0;
            if (!Good(def)) def = 0.0;
            if (colour) { lo = 0.0; hi = 1.0; }
            else if (def >= 0 && def <= 1) { lo = 0.0; hi = 1.0; }
            else { lo = def - 50.0; hi = def + 50.0; }
        }
    }

    static string FmtValue(object v)
    {
        if (v == null) return "";
        double[] c = Comps(v);
        if (c.Length > 1 || (c.Length == 1 && !(v is IConvertible)))
        {
            string[] parts = new string[c.Length];
            for (int i = 0; i < c.Length; ++i) parts[i] = c[i].ToString("R", Inv);
            return string.Join(",", parts);
        }
        if (v is bool) return ((bool)v) ? "true" : "false";
        if (v is double) return ((double)v).ToString("R", Inv);
        if (v is IConvertible && !(v is string) && !v.GetType().IsEnum) return Convert.ToString(v, Inv);
        if (v.GetType().IsEnum) return v.ToString();
        string cn = ChoiceName(v);
        return cn;
    }

    static string ChoiceName(object c)
    {
        if (c == null) return "";
        string n = "";
        try { n = S(Get(c, "Name")); } catch (Exception) { }
        if (n.Length == 0) n = Convert.ToString(c, Inv);
        string idx = "";
        try { object v = FirstObj(c, "Index", "Value"); if (v != null && !(v is string)) idx = Convert.ToString(v, Inv); } catch (Exception) { }
        return idx.Length > 0 ? n + " (" + idx + ")" : n;
    }

    // ------------------------------------------------------------------------------------------ transitions
    void BuildTransition(Plug pl)
    {
        if (Skipped(pl.Name)) { Log("skipped (skip.txt): " + pl.Name); return; }
        Mark("transition " + pl.Name);
        TransitionTest(pl, "default", null);
        foreach (string preset in Pick(pl.Presets, TransPresetCap))
            TransitionTest(pl, "preset:" + preset, preset);
    }

    void TransitionTest(Plug pl, string variant, string preset)
    {
        FuzzTest t = NewTest("transition", pl, variant);
        long f0 = nextFrame;
        nextFrame += TransFrames;
        t.StartFrame = f0; t.LengthFrames = TransFrames; t.Start = f0 / (double)Fps; t.Length = TransFrames / (double)Fps;
        t.TransStartFrame = f0 + (TransAFrames - TransOverlap); t.TransLengthFrames = TransOverlap;
        t.TransStart = t.TransStartFrame / (double)Fps; t.TransLength = TransOverlap / (double)Fps;
        int[] pct = { 25, 50, 75 };
        foreach (int pc in pct)
        {
            t.SnapPercent.Add(pc);
            t.SnapFrames.Add(t.TransStartFrame + (long)Math.Round(TransOverlap * pc / 100.0));
        }
        VideoEvent a = null, b = null;
        try
        {
            a = vtrack2.AddVideoEvent(Timecode.FromFrames(f0), Timecode.FromFrames(TransAFrames));
            a.Name = pl.Name + " A";
            a.AddTake(picture1.GetVideoStreamByIndex(0));
            b = vtrack2.AddVideoEvent(Timecode.FromFrames(f0 + TransAFrames - TransOverlap), Timecode.FromFrames(TransFrames - (TransAFrames - TransOverlap)));
            b.Name = pl.Name + " B";
            b.AddTake(picture2.GetVideoStreamByIndex(0));
            // the cross-fade is B's fade-in, as long as the overlap
            TrySet(b.FadeIn, "Length", Timecode.FromFrames(TransOverlap));
            Effect fx = new Effect(pl.Node);
            DumpOnce(b.FadeIn); DumpOnce(fx);
            Set(b.FadeIn, "Transition", fx);
            object now = Get(b.FadeIn, "Transition");
            if (now == null) throw new Exception("the transition did not stay on the event");
            Effect applied = now as Effect;
            if (applied != null) fx = applied;
            if (preset != null) fx.Preset = preset;
        }
        catch (Exception e)
        {
            t.Status = "FAILED";
            t.Error = Msg(e);
            TryRemove(vtrack2.Events, a);
            TryRemove(vtrack2.Events, b);
        }
        Finish(t);
    }

    // ------------------------------------------------------------------------------------------ audio effects
    void DecideAudioMode()
    {
        audioMode = "event";
        try
        {
            AudioTrack probe = p.AddAudioTrack();
            AudioEvent ev = probe.AddAudioEvent(Timecode.FromSeconds(0), Timecode.FromSeconds(1));
            object effs = Get(ev, "Effects");
            if (effs == null) audioMode = "track";
            TryRemove(probe.Events, ev);
            TryRemove(p.Tracks, probe);
        }
        catch (Exception e) { audioMode = "track"; Log("audio event effects not available: " + Msg(e)); }
        Log("audio effects are put on the " + audioMode);
        if (audioMode == "event")
        {
            atrack = p.AddAudioTrack();
            atrack.Name = "Fuzz audio";
        }
    }

    void BuildAudioPlug(Plug pl)
    {
        if (Skipped(pl.Name)) { Log("skipped (skip.txt): " + pl.Name); return; }
        Mark("audio effect " + pl.Name);
        AudioTest(pl, "default", null);
        foreach (string preset in Pick(pl.Presets, PresetCap)) AudioTest(pl, "preset:" + preset, preset);
    }

    void AudioTest(Plug pl, string variant, string preset)
    {
        FuzzTest t = NewTest("audio", pl, variant);
        t.Mode = audioMode;
        double s0 = nextAudioSlot * AudioSlot;
        nextAudioSlot++;
        t.Start = s0; t.Length = AudioSlot;
        t.StartFrame = (long)Math.Round(s0 * Fps); t.LengthFrames = (long)Math.Round(AudioSlot * Fps);
        AudioTrack tr = null;
        AudioEvent ev = null;
        try
        {
            if (audioMode == "event") tr = atrack;
            else { tr = p.AddAudioTrack(); tr.Name = "Fuzz " + t.Id + " " + pl.Name; }
            ev = tr.AddAudioEvent(Timecode.FromSeconds(s0), Timecode.FromSeconds(AudioLength));
            ev.Name = pl.Name + " " + variant;
            DumpOnce(ev); DumpOnce(tr);
            ev.AddTake(sound.GetAudioStreamByIndex(0));
            TrySet(ev.FadeIn, "Length", Timecode.FromMilliseconds(0));
            TrySet(ev.FadeOut, "Length", Timecode.FromMilliseconds(0));
            Effect fx = new Effect(pl.Node);
            object chain = Get(audioMode == "event" ? (object)ev : (object)tr, "Effects");
            if (chain == null) throw new Exception("no Effects collection on the " + audioMode);
            Call(chain, "Add", fx);
            if (preset != null) fx.Preset = preset;
        }
        catch (Exception e)
        {
            t.Status = "FAILED";
            t.Error = Msg(e);
            if (tr != null) TryRemove(tr.Events, ev);
            if (audioMode == "track") TryRemove(p.Tracks, tr);
        }
        Finish(t);
    }

    void Mark(string what)
    {
        try { progressTxt.WriteLine(DateTime.Now.ToString("HH:mm:ss", Inv) + "  BUILDING " + what); } catch (Exception) { }
    }

    // ------------------------------------------------------------------------------------------ snapshots
    void TakeSnapshots()
    {
        int total = 0;
        foreach (FuzzTest t in tests) if (t.Kind != "audio" && t.Status == "ok") total += t.SnapFrames.Count;
        int n = 0;
        foreach (FuzzTest t in tests)
        {
            if (t.Kind == "audio" || t.Status != "ok") continue;
            Mark("snapshots of " + t.Kind + " " + t.Plugin + " " + t.Variant);
            for (int k = 0; k < t.SnapFrames.Count; ++k)
            {
                if (cancelled) { t.SnapFiles.Add(""); t.SnapResult.Add("not taken (stopped)"); continue; }
                string file = "snap_" + t.Id.ToString("0000", Inv) + (t.Kind == "transition" ? "_" + t.SnapPercent[k].ToString(Inv) : "") + ".png";
                string result = "ok";
                try
                {
                    Timecode at = Timecode.FromFrames(t.SnapFrames[k]);
                    vegas.Transport.CursorPosition = at;
                    vegas.UpdateUI();
                    System.Windows.Forms.Application.DoEvents();
                    object r = CallSnapshot(Path.Combine(folder, file), at);
                    if (r != null && r.ToString() != "Complete") result = "snapshot: " + r;
                    if (!File.Exists(Path.Combine(folder, file))) result = "no file written";
                }
                catch (Exception e) { result = "FAILED: " + Msg(e); }
                if (result == "ok") snapshotsDone++; else { snapshotsFailed++; Log("snapshot " + file + ": " + result); }
                t.SnapFiles.Add(file);
                t.SnapResult.Add(result);
                n++;
                if (n % 5 == 0) Progress("Picture " + n + " of " + total + ": " + t.Plugin + " " + t.Variant, n, total);
            }
            if (n % 50 == 0) WriteManifest();
        }
    }

    object CallSnapshot(string path, Timecode at)
    {
        // Vegas.SaveSnapshot(path, ImageFileFormat.PNG [, at])
        foreach (MethodInfo m in vegas.GetType().GetMethods())
        {
            if (m.Name != "SaveSnapshot") continue;
            ParameterInfo[] ps = m.GetParameters();
            object fmt = Enum.Parse(ps[1].ParameterType, "PNG");
            if (ps.Length == 2) return m.Invoke(vegas, new object[] { path, fmt });
            if (ps.Length == 3 && ps[2].ParameterType == typeof(Timecode)) return m.Invoke(vegas, new object[] { path, fmt, at });
        }
        throw new Exception("this VEGAS has no SaveSnapshot");
    }

    // ------------------------------------------------------------------------------------------ rendering the audio
    void RenderAudio(string path)
    {
        StringBuilder list = new StringBuilder();
        object chosenTemplate = null;
        string chosenName = "";
        int bestScore = -1;
        try
        {
            IEnumerable renderers = Get(vegas, "Renderers") as IEnumerable;
            if (renderers == null) throw new Exception("this VEGAS has no Renderers");
            foreach (object r in renderers)
            {
                string rname = FirstText(r, "FileTypeName", "Name");
                IEnumerable templates = null;
                try { templates = Get(r, "Templates") as IEnumerable; } catch (Exception) { }
                if (templates == null) continue;
                foreach (object tmpl in templates)
                {
                    string tname = S(Get(tmpl, "Name"));
                    string ext = FirstText(tmpl, "FileExtensions", "Extension", "FileExtension");
                    list.AppendLine(rname + "\t" + tname + "\t" + ext);
                    int score = ScoreTemplate(rname, tname);
                    if (score > bestScore) { bestScore = score; chosenTemplate = tmpl; chosenName = rname + " / " + tname; }
                }
            }
        }
        catch (Exception e) { list.AppendLine("ERROR " + Msg(e)); }
        try { File.WriteAllText(Path.Combine(folder, "render_templates.txt"), list.ToString(), new UTF8Encoding(false)); } catch (Exception) { }
        if (chosenTemplate == null || bestScore < 10)
        {
            renderStatus = "FAILED: no suitable Wave template (see render_templates.txt); render the project's audio by hand: File > Render As > Wave, 48,000 Hz 32-bit float or 16 Bit stereo, to fuzz_audio.wav";
            Log(renderStatus);
            return;
        }
        renderTemplate = chosenName;
        Log("rendering the audio with " + chosenName);
        try
        {
            if (File.Exists(path)) File.Delete(path);
            double end = Math.Max(0.1, nextAudioSlot * AudioSlot);
            Timecode start = Timecode.FromSeconds(0), length = Timecode.FromSeconds(end);
            object result = null;
            bool called = false;
            foreach (MethodInfo m in vegas.GetType().GetMethods())
            {
                if (m.Name != "Render") continue;
                ParameterInfo[] ps = m.GetParameters();
                if (ps.Length == 4 && ps[0].ParameterType == typeof(string) && ps[1].ParameterType.IsInstanceOfType(chosenTemplate) && ps[2].ParameterType == typeof(Timecode))
                {
                    result = m.Invoke(vegas, new object[] { path, chosenTemplate, start, length });
                    called = true;
                    break;
                }
            }
            if (!called)
            {
                foreach (MethodInfo m in vegas.GetType().GetMethods())
                {
                    if (m.Name != "Render") continue;
                    ParameterInfo[] ps = m.GetParameters();
                    if (ps.Length == 2 && ps[0].ParameterType == typeof(string) && ps[1].ParameterType.IsInstanceOfType(chosenTemplate))
                    {
                        result = m.Invoke(vegas, new object[] { path, chosenTemplate });
                        called = true;
                        break;
                    }
                }
            }
            if (!called) throw new Exception("no usable Vegas.Render overload");
            renderStatus = (result == null ? "returned nothing" : result.ToString()) + (File.Exists(path) ? "" : " (but no file)");
            if (renderStatus.StartsWith("Complete")) renderStatus = "ok" + renderStatus.Substring("Complete".Length);
        }
        catch (Exception e) { renderStatus = "FAILED: " + Msg(e); }
        Log("audio render: " + renderStatus);
    }

    // how well a render template suits: Wave, 48 kHz, stereo, 32-bit float best, 16-bit next
    static int ScoreTemplate(string renderer, string name)
    {
        string r = (renderer ?? "").ToLowerInvariant(), n = (name ?? "").ToLowerInvariant();
        if (r.IndexOf("wave") < 0 || r.IndexOf("64") >= 0 || r.IndexOf("w64") >= 0) return -1;
        if (n.IndexOf("48,000") < 0 && n.IndexOf("48000") < 0 && n.IndexOf("48 khz") < 0) return -1;
        if (n.IndexOf("stereo") < 0) return -1;
        if (n.IndexOf("float") >= 0 && n.IndexOf("32") >= 0) return 30;
        if (n.IndexOf("16 bit") >= 0 || n.IndexOf("16-bit") >= 0) return 20;
        return 10;
    }

    // ------------------------------------------------------------------------------------------ manifest.json
    static string Q(string s)
    {
        if (s == null) s = "";
        StringBuilder sb = new StringBuilder("\"");
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
                    if (c < ' ') sb.Append("\\u" + ((int)c).ToString("x4", Inv)); else sb.Append(c);
                    break;
            }
        }
        return sb.Append('"').ToString();
    }

    static string Num(double d) { return d.ToString("R", Inv); }

    void WriteManifest()
    {
        if (folder == null) return;
        StringBuilder sb = new StringBuilder();
        sb.AppendLine("{");
        sb.AppendLine("  \"tool\": \"Reno Plugin Fuzz\", \"format\": 1,");
        sb.AppendLine("  \"vegas\": " + Q(vegasVersion) + ", \"created\": " + Q(DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss", Inv)) + ",");
        sb.AppendLine("  \"project\": {\"file\": \"fuzz.veg\", \"width\": 320, \"height\": 180, \"fps\": " + Fps + ", \"sample_rate\": 48000, \"channels\": 2},");
        sb.AppendLine("  \"media\": {\"picture\": \"fuzz_picture.png\", \"picture2\": \"fuzz_picture2.png\", \"sound\": \"fuzz_test.wav\", \"sound_length_s\": " + Num(AudioLength) + "},");
        sb.AppendLine("  \"audio_effect_mode\": " + Q(audioMode) + ",");
        sb.AppendLine("  \"audio_render\": {\"file\": \"fuzz_audio.wav\", \"template\": " + Q(renderTemplate) + ", \"status\": " + Q(renderStatus) + ", \"end_s\": " + Num(nextAudioSlot * AudioSlot) + "},");
        sb.AppendLine("  \"stopped_early\": " + (cancelled ? "true" : "false") + ",");
        sb.AppendLine("  \"tests\": [");
        for (int i = 0; i < tests.Count; ++i)
        {
            FuzzTest t = tests[i];
            sb.Append("    {\"id\": " + t.Id + ", \"kind\": " + Q(t.Kind) + ", \"plugin\": " + Q(t.Plugin) + ", \"uid\": " + Q(t.Uid) + ", \"class_id\": " + Q(t.ClassId) + ", \"ofx\": " + (t.Ofx ? "true" : "false") +
                      ", \"variant\": " + Q(t.Variant) + ", \"status\": " + Q(t.Status) + ", \"error\": " + Q(t.Error) + ", \"note\": " + Q(t.Note));
            sb.Append(", \"start_s\": " + Num(t.Start) + ", \"length_s\": " + Num(t.Length) + ", \"start_frame\": " + t.StartFrame + ", \"length_frames\": " + t.LengthFrames);
            if (t.Kind == "transition")
                sb.Append(", \"transition_start_s\": " + Num(t.TransStart) + ", \"transition_length_s\": " + Num(t.TransLength) + ", \"transition_start_frame\": " + t.TransStartFrame + ", \"transition_length_frames\": " + t.TransLengthFrames);
            if (t.Kind == "audio")
                sb.Append(", \"event_length_s\": " + Num(AudioLength) + ", \"mode\": " + Q(t.Mode));
            sb.Append(", \"snapshots\": [");
            for (int k = 0; k < t.SnapFrames.Count; ++k)
            {
                if (k > 0) sb.Append(", ");
                string file = k < t.SnapFiles.Count ? t.SnapFiles[k] : "";
                string res = k < t.SnapResult.Count ? t.SnapResult[k] : "not taken";
                sb.Append("{\"file\": " + Q(file) + ", \"frame\": " + t.SnapFrames[k] + ", \"time_s\": " + Num(t.SnapFrames[k] / (double)Fps) + ", \"percent\": " + t.SnapPercent[k] + ", \"result\": " + Q(res) + "}");
            }
            sb.Append("], \"params\": [");
            for (int k = 0; k < t.Params.Count; ++k)
            {
                ParamRec r = t.Params[k];
                if (k > 0) sb.Append(", ");
                sb.Append("{\"name\": " + Q(r.Name) + ", \"type\": " + Q(r.Type) + ", \"source\": " + Q(r.Source) + ", \"value\": " + Q(r.Value) + ", \"read_back\": " + Q(r.ReadBack) +
                          ", \"min\": " + Q(r.Min) + ", \"max\": " + Q(r.Max) + ", \"default\": " + Q(r.Default) + ", \"skipped\": " + Q(r.Skipped) + ", \"error\": " + Q(r.Error) + "}");
            }
            sb.Append("]}");
            sb.AppendLine(i + 1 < tests.Count ? "," : "");
        }
        sb.AppendLine("  ]");
        sb.AppendLine("}");
        string path = Path.Combine(folder, "manifest.json");
        string tmp = path + ".tmp";
        File.WriteAllText(tmp, sb.ToString(), new UTF8Encoding(false));
        if (File.Exists(path)) File.Delete(path);
        File.Move(tmp, path);
    }

    // ------------------------------------------------------------------------------------------ the API dump (for debugging this script)
    void DumpApi()
    {
        try
        {
            DumpOnce(vegas);
            DumpOnce(p);
            DumpOnce(p.Video);
            DumpOnce(Get(p, "Audio"));
            DumpOnce(p.Preview);
            DumpOnce(picture1);
            List<PlugInNode> nodes = new List<PlugInNode>();
            Collect(vegas.VideoFX, nodes, 0);
            if (nodes.Count > 0) DumpOnce(nodes[0]);
            DumpMethods(vegas, "Render");
            DumpMethods(vegas, "SaveSnapshot");
        }
        catch (Exception e) { apiTxt.WriteLine("(dump failed: " + Msg(e) + ")"); }
        apiTxt.Flush();
    }

    void DumpMethods(object o, string name)
    {
        foreach (MethodInfo m in o.GetType().GetMethods())
        {
            if (m.Name != name) continue;
            List<string> ps = new List<string>();
            foreach (ParameterInfo pi in m.GetParameters()) ps.Add(pi.ParameterType.Name + " " + pi.Name);
            apiTxt.WriteLine("method " + o.GetType().Name + "." + name + "(" + string.Join(", ", ps.ToArray()) + ") : " + m.ReturnType.Name);
        }
    }

    void DumpOnce(object o)
    {
        if (o == null) return;
        string tn = o.GetType().FullName;
        if (apiDumped.ContainsKey(tn)) return;
        apiDumped[tn] = true;
        apiTxt.WriteLine();
        apiTxt.WriteLine("== " + tn);
        foreach (PropertyInfo pi in o.GetType().GetProperties())
        {
            string v = "";
            try { if (pi.GetIndexParameters().Length == 0 && pi.CanRead) { object x = pi.GetValue(o, null); v = x == null ? "null" : (x is IEnumerable && !(x is string) ? "(collection)" : Convert.ToString(x, Inv)); } }
            catch (Exception e) { v = "(throws " + Msg(e) + ")"; }
            apiTxt.WriteLine("  " + pi.PropertyType.Name + " " + pi.Name + (pi.CanWrite ? " {get;set;}" : " {get;}") + " = " + v);
        }
        apiTxt.Flush();
    }

    // ------------------------------------------------------------------------------------------ test media
    // deterministic pseudo-random numbers (the same on every machine), for the noise and the random settings
    class Rng
    {
        uint s;
        public Rng(uint seed) { s = seed == 0 ? 0x9E3779B9u : seed; Next(); Next(); }
        uint Step() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
        public double NextDouble() { return (Step() >> 8) / 16777216.0; }
        public int Next(int n) { return (int)(NextDouble() * n); }
        public int Next() { return (int)(Step() & 0x7fffffff); }
    }

    static uint Fnv(string s)
    {
        uint h = 2166136261u;
        foreach (char c in s) { h ^= (uint)c; h *= 16777619u; }
        return h;
    }

    // 320x180: colour bars on top, grey and colour ramps in the middle, detail (checkers, lines, rings, a circle) below; no alpha
    static void WritePictures(string path1, string path2)
    {
        const int W = 320, H = 180;
        int[,] r = new int[W, H], g = new int[W, H], b = new int[W, H];
        int[][] bars = {
            new int[] { 235, 235, 235 }, new int[] { 235, 235, 16 }, new int[] { 16, 235, 235 }, new int[] { 16, 235, 16 },
            new int[] { 235, 16, 235 }, new int[] { 235, 16, 16 }, new int[] { 16, 16, 235 }, new int[] { 16, 16, 16 } };
        for (int y = 0; y < H; ++y)
        {
            for (int x = 0; x < W; ++x)
            {
                int cr, cg, cb;
                if (y < 60)
                {
                    int[] bar = bars[x * 8 / W];
                    cr = bar[0]; cg = bar[1]; cb = bar[2];
                }
                else if (y < 80) { cr = cg = cb = x * 255 / (W - 1); }
                else if (y < 100)
                {
                    int t = x * 255 / (W - 1);
                    int band = x * 3 / W;
                    cr = band == 0 ? t : 0; cg = band == 1 ? t : 0; cb = band == 2 ? t : 0;
                    // a vertical ramp in the last band's lower half
                    if (band == 2) { cr = 255 * (y - 80) / 19; }
                }
                else
                {
                    // detail area
                    int dx = x - 240, dy = y - 140;
                    double rad = Math.Sqrt(dx * dx + dy * dy);
                    if (x < 100) { int c = (((x / 4) + (y / 4)) & 1) == 0 ? 230 : 25; cr = cg = cb = c; }
                    else if (x < 160)
                    {
                        // fine lines, 1 and 2 pixels: horizontal on the left half, vertical on the right
                        bool on = x < 130 ? (y % 2 == 0) : (x % 3 == 0);
                        int c = on ? 255 : 0; cr = cg = cb = c;
                    }
                    else if (rad < 36)
                    {
                        int ring = (int)(rad * rad / 12.0) & 1;
                        cr = ring == 0 ? 255 : 30; cg = ring == 0 ? 200 : 60; cb = ring == 0 ? 40 : 160;
                    }
                    else
                    {
                        // a smooth diagonal gradient with a hard edge
                        int v = ((x + y) * 255 / (W + H)) & 255;
                        cr = v; cg = 255 - v; cb = (x - 160) * 255 / 160;
                        if (x > 270 && y > 160) { cr = cg = cb = 128; }
                    }
                }
                r[x, y] = cr; g[x, y] = cg; b[x, y] = cb;
            }
        }
        SavePicture(path1, r, g, b, W, H, false);
        SavePicture(path2, r, g, b, W, H, true);
    }

    static void SavePicture(string path, int[,] r, int[,] g, int[,] b, int W, int H, bool second)
    {
        using (System.Drawing.Bitmap bmp = new System.Drawing.Bitmap(W, H, System.Drawing.Imaging.PixelFormat.Format24bppRgb))
        {
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                {
                    int cr = r[x, y], cg = g[x, y], cb = b[x, y];
                    if (second)
                    {
                        // tinted differently: the channels turned round, warmed, with diagonal stripes
                        int nr = cb, ng = cr, nb = cg;
                        nr = nr * 255 / 255; ng = ng * 215 / 255; nb = nb * 150 / 255;
                        if (((x + y) / 12) % 6 == 0) { nr = 255 - nr; ng = 255 - ng; nb = 255 - nb; }
                        cr = nr; cg = ng; cb = nb;
                    }
                    bmp.SetPixel(x, y, System.Drawing.Color.FromArgb(255, Clamp(cr), Clamp(cg), Clamp(cb)));
                }
            bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png);
        }
    }

    static int Clamp(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

    // 2 s, 48 kHz, 16-bit stereo: a log sweep, a tone burst, clicks, a noise burst, then silence (for tails)
    static void WriteWav(string path)
    {
        const int rate = 48000, channels = 2;
        int frames = (int)(rate * AudioLength);
        short[] data = new short[frames * channels];
        Rng noise = new Rng(12345u);
        double phaseL = 0, phaseR = 0;
        for (int i = 0; i < frames; ++i)
        {
            double t = i / (double)rate;
            double l = 0, rr = 0;
            if (t < 0.60)
            {
                // 100 Hz -> 8 kHz exponential sweep, a short fade at both ends
                double f = 100.0 * Math.Pow(80.0, t / 0.60);
                phaseL += 2.0 * Math.PI * f / rate;
                phaseR += 2.0 * Math.PI * f * 1.01 / rate;
                double env = Math.Min(1.0, Math.Min(t / 0.01, (0.60 - t) / 0.01));
                l = 0.5 * env * Math.Sin(phaseL);
                rr = 0.4 * env * Math.Sin(phaseR + 0.7);
            }
            else if (t >= 0.62 && t < 0.72)
            {
                // tone burst: 1 kHz left, 1.5 kHz right, 5 ms ramps
                double env = Math.Min(1.0, Math.Min((t - 0.62) / 0.005, (0.72 - t) / 0.005));
                l = 0.7 * env * Math.Sin(2.0 * Math.PI * 1000.0 * (t - 0.62));
                rr = 0.7 * env * Math.Sin(2.0 * Math.PI * 1500.0 * (t - 0.62));
            }
            else if (t >= 0.75 && t < 1.05)
            {
                // clicks: single-sample impulses, then short decaying bursts
                int k = i - (int)(0.80 * rate);
                if (k == 0) { l = 0.9; rr = 0.9; }
                int k2 = i - (int)(0.85 * rate);
                if (k2 == 0) { l = -0.9; rr = -0.9; }
                int k3 = i - (int)(0.90 * rate);
                if (k3 >= 0 && k3 < 96) { double d = Math.Exp(-k3 / 20.0); l = 0.8 * d * Math.Sin(2.0 * Math.PI * 3000.0 * k3 / rate); rr = 0.8 * d * Math.Sin(2.0 * Math.PI * 4500.0 * k3 / rate); }
                int k4 = i - (int)(0.95 * rate);
                if (k4 == 0) { l = 0.6; rr = -0.6; }
                int k5 = i - (int)(1.00 * rate);
                if (k5 >= 0 && k5 < 4) { l = 0.9; rr = 0.0; }
            }
            else if (t >= 1.10 && t < 1.40)
            {
                // a noise burst with 5 ms ramps (left and right differ)
                double env = Math.Min(1.0, Math.Min((t - 1.10) / 0.005, (1.40 - t) / 0.005));
                l = 0.3 * env * (noise.NextDouble() * 2.0 - 1.0);
                rr = 0.3 * env * (noise.NextDouble() * 2.0 - 1.0);
            }
            data[i * 2] = (short)Math.Round(Math.Max(-1.0, Math.Min(1.0, l)) * 32767.0);
            data[i * 2 + 1] = (short)Math.Round(Math.Max(-1.0, Math.Min(1.0, rr)) * 32767.0);
        }
        int bytes = data.Length * 2;
        using (FileStream fs = new FileStream(path, FileMode.Create))
        using (BinaryWriter w = new BinaryWriter(fs))
        {
            w.Write(Encoding.ASCII.GetBytes("RIFF"));
            w.Write(36 + bytes);
            w.Write(Encoding.ASCII.GetBytes("WAVEfmt "));
            w.Write(16);
            w.Write((short)1);
            w.Write((short)channels);
            w.Write(rate);
            w.Write(rate * channels * 2);
            w.Write((short)(channels * 2));
            w.Write((short)16);
            w.Write(Encoding.ASCII.GetBytes("data"));
            w.Write(bytes);
            foreach (short s in data) w.Write(s);
        }
    }

    // ------------------------------------------------------------------------------------------ reflection helpers
    static object Get(object o, string prop)
    {
        if (o == null) return null;
        PropertyInfo pi = o.GetType().GetProperty(prop);
        if (pi == null)
        {
            FieldInfo fi = o.GetType().GetField(prop);       // (OFXChoice's Name and Index are fields)
            return fi == null ? null : fi.GetValue(o);
        }
        if (pi.GetIndexParameters().Length != 0) return null;
        return pi.GetValue(o, null);
    }

    static object FirstObj(object o, params string[] props)
    {
        foreach (string pr in props)
        {
            try { object v = Get(o, pr); if (v != null) return v; } catch (Exception) { }
        }
        return null;
    }

    static string FirstText(object o, params string[] props)
    {
        foreach (string pr in props)
        {
            try { object v = Get(o, pr); if (v != null) { string s = Convert.ToString(v, Inv); if (s.Length > 0) return s; } } catch (Exception) { }
        }
        return "";
    }

    static void Set(object o, string prop, object value)
    {
        PropertyInfo pi = o.GetType().GetProperty(prop);
        if (pi == null) throw new Exception("no property " + prop);
        Type t = pi.PropertyType;
        if (t.IsEnum && value is string) value = Enum.Parse(t, (string)value);
        else if (value != null && !t.IsAssignableFrom(value.GetType())) value = Convert.ChangeType(value, t, Inv);
        pi.SetValue(o, value, null);
    }

    static void TrySet(object o, string prop, object value) { try { Set(o, prop, value); } catch (Exception) { } }

    static object Call(object o, string method, params object[] args)
    {
        foreach (MethodInfo m in o.GetType().GetMethods())
            if (m.Name == method && m.GetParameters().Length == args.Length) return m.Invoke(o, args);
        throw new Exception("no method " + method);
    }

    static void TryRemove(object collection, object item)
    {
        if (collection == null || item == null) return;
        try { Call(collection, "Remove", item); } catch (Exception) { }
    }

    static string S(object o) { return o == null ? "" : Convert.ToString(o, Inv); }

    static string Fmt(object o)
    {
        if (o == null) return "";
        try { return FmtValue(o); } catch (Exception) { return Convert.ToString(o, Inv); }
    }

    static string Msg(Exception e)
    {
        Exception b = e;
        while (b.InnerException != null) b = b.InnerException;
        return b.GetType().Name + ": " + b.Message;
    }
}
