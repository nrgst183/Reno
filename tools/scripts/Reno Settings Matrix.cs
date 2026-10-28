/**
 * Reno Settings Matrix: makes the .veg files Reno needs to learn what the settings it cannot read are called.
 * Run in VEGAS Pro (Tools > Scripting > Run Script). Asks for a folder (default Documents\Reno Settings Matrix\<date>),
 * builds a small base project (generated media with an OpenFX effect, a picture with Rays, an audio event, with fades),
 * saves it as 00_base.veg, then repeatedly re-opens the base, changes one thing and saves it under its own name
 * (e.g. "0123_Video.FieldOrder=UpperFieldFirst.veg"). Changes cover every settable project, bus, track, event, fade,
 * motion and effect property the scripting API shows (found by reflection), plus added markers, regions, effects, buses,
 * tracks, takes and envelopes. Also writes manifest.txt and api_properties.txt.
 * Then run vegmatrix_diff <folder> (tests/vegas/vegmatrix_diff.cpp) to list the bytes each file changed.
 * The open project is replaced by the test projects (it will not start if it has unsaved changes) and reopened at the end.
 * AddGapVariants makes the files for the .veg parts Reno does not write yet (transitions, track motion, bus settings, ...).
 * Takes a few minutes. C# 5.
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
    // ------------------------------------------------------------------------------------------ state
    Vegas vegas;
    string outDir;
    StreamWriter manifest;
    StreamWriter apiLog;
    int counter;
    int okCount, failCount;
    bool cancelled;
    Form progressForm;
    Label progressLabel;
    string wavPath, bmpPath;

    const string BaseFile = "00_base.veg";
    const string BaseBusFile = "02_base_with_bus.veg";

    delegate string Change(Vegas v);

    // one file to make: open baseFile, change (path, prop) to value (or run custom), save under name
    class Variant
    {
        public string Name;         // file name without number and extension
        public string BaseName;     // which base to start from
        public string Path;         // the object that is changed (from the Vegas object): "Project.Video"
        public string Prop;         // the property
        public object Value;        // new value (converted to the property's type)
        public string Desc;         // for the manifest
        public Change Custom;       // instead of Path/Prop/Value: does the change, returns a note
    }

    // ------------------------------------------------------------------------------------------ entry
    public void FromVegas(Vegas v)
    {
        vegas = v;
        string originalPath = null;
        try
        {
            string appName = "";
            try { appName = Convert.ToString(Get(vegas, "ShortAppName")); } catch (Exception) { }
            if (appName.IndexOf("Reno", StringComparison.OrdinalIgnoreCase) >= 0)
            {
                MessageBox.Show(vegas.MainWindow, "This script makes test files with VEGAS Pro's own project writer: run it in VEGAS Pro (Tools > Scripting > Run Script), not in Reno.",
                                "Reno Settings Matrix", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }
            // never touch the user's project: it must be saved, because the test projects replace it in the editor
            bool modified = true;
            try { modified = Convert.ToBoolean(Get(vegas.Project, "IsModified")); } catch (Exception) { }
            try { originalPath = vegas.Project.FilePath; } catch (Exception) { }
            if (modified)
            {
                MessageBox.Show(vegas.MainWindow,
                    "The project that is open has unsaved changes.\n\nThis script replaces the open project with its test projects, " +
                    "so save your project first (Ctrl+S, or close it) and run the script again.",
                    "Reno Settings Matrix", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            string defaultDir = Path.Combine(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "Reno Settings Matrix"),
                                             DateTime.Now.ToString("yyyyMMdd-HHmmss"));
            outDir = AskFolder(defaultDir, originalPath);
            if (outDir == null) return;
            Directory.CreateDirectory(outDir);

            manifest = new StreamWriter(Path.Combine(outDir, "manifest.txt"), false, new UTF8Encoding(false));
            manifest.AutoFlush = true;
            apiLog = new StreamWriter(Path.Combine(outDir, "api_properties.txt"), false, new UTF8Encoding(false));
            apiLog.AutoFlush = true;
            manifest.WriteLine("# Reno Settings Matrix");
            string appText = "";
            try { appText = Convert.ToString(Get(vegas, "LongAppName")) + " (version " + Convert.ToString(Get(vegas, "Version")) + ")"; } catch (Exception) { }
            manifest.WriteLine("# " + appText + ", " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss"));
            manifest.WriteLine("# output folder: " + outDir);
            manifest.WriteLine("# columns (tab separated): file, compared-with, status, object, change, value-read-back, note");
            manifest.WriteLine("# the base project: video track 1 'Matrix A' (generated media, 5 s, fade in/out 0.5 s, one OpenFX effect),");
            manifest.WriteLine("#   video track 2 'Matrix B' (a still picture, 5 s, a Rays effect), audio track 'Matrix audio' (a test tone, 5 s, fades)");
            manifest.WriteLine("# " + (originalPath == null ? "no project was open before" : "the project open before the run was " + originalPath + " (opened again at the end)"));

            ShowProgress("Starting...");
            Run();
        }
        catch (Exception ex)
        {
            Note("SCRIPT ERROR: " + Msg(ex));
            try { MessageBox.Show(vegas.MainWindow, "Reno Settings Matrix stopped:\n\n" + Msg(ex), "Reno Settings Matrix", MessageBoxButtons.OK, MessageBoxIcon.Error); } catch (Exception) { }
        }
        finally
        {
            try { if (progressForm != null) progressForm.Close(); } catch (Exception) { }
            try { if (manifest != null) { manifest.Flush(); manifest.Close(); } } catch (Exception) { }
            try { if (apiLog != null) { apiLog.Flush(); apiLog.Close(); } } catch (Exception) { }
            if (outDir != null && originalPath != null && File.Exists(originalPath))
            {
                try { vegas.OpenProject(originalPath); } catch (Exception) { }
            }
        }
    }

    // ------------------------------------------------------------------------------------------ the dialog
    string AskFolder(string defaultDir, string originalPath)
    {
        Form f = new Form();
        f.Text = "Reno Settings Matrix";
        f.FormBorderStyle = FormBorderStyle.FixedDialog;
        f.MaximizeBox = false;
        f.MinimizeBox = false;
        f.ShowInTaskbar = false;
        f.StartPosition = FormStartPosition.CenterParent;
        f.ClientSize = new System.Drawing.Size(560, 290);

        Label info = new Label();
        info.Left = 12; info.Top = 10; info.Width = 536; info.Height = 190;
        info.Text =
            "This script makes the test files Reno needs to learn the .veg settings that are the same in every sample project.\n\n" +
            "It builds a small base project in the folder below, then saves a copy for every single setting it can change " +
            "(project properties, tracks, events, effects, buses...), one setting per file, and writes manifest.txt.\n\n" +
            "The project that is open now is replaced by these test projects while the script runs" +
            (originalPath != null ? " (it is never saved over, and is opened again at the end)" : "") +
            ". It takes a few minutes. Do not use VEGAS meanwhile.\n\n" +
            "Output folder (a new folder is made if it does not exist):";
        f.Controls.Add(info);

        TextBox box = new TextBox();
        box.Left = 12; box.Top = 205; box.Width = 440;
        box.Text = defaultDir;
        f.Controls.Add(box);

        Button browse = new Button();
        browse.Left = 460; browse.Top = 203; browse.Width = 88; browse.Text = "Browse...";
        browse.Click += delegate(object s, EventArgs e)
        {
            FolderBrowserDialog d = new FolderBrowserDialog();
            d.Description = "Folder for the test projects";
            d.SelectedPath = Directory.Exists(box.Text) ? box.Text : Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
            if (d.ShowDialog(f) == DialogResult.OK) box.Text = d.SelectedPath;
        };
        f.Controls.Add(browse);

        Button ok = new Button();
        ok.Left = 372; ok.Top = 250; ok.Width = 84; ok.Text = "Start";
        ok.DialogResult = DialogResult.OK;
        f.Controls.Add(ok);
        Button cancel = new Button();
        cancel.Left = 464; cancel.Top = 250; cancel.Width = 84; cancel.Text = "Cancel";
        cancel.DialogResult = DialogResult.Cancel;
        f.Controls.Add(cancel);
        f.AcceptButton = ok;
        f.CancelButton = cancel;

        string result = null;
        if (f.ShowDialog(vegas.MainWindow) == DialogResult.OK && box.Text.Trim().Length > 0)
        {
            result = box.Text.Trim();
            try { result = Path.GetFullPath(result); } catch (Exception) { result = null; }
            if (result != null && originalPath != null)
            {
                // never write next to nothing that could be the user's own file: refuse the folder of the open project's file name
                try { if (string.Equals(Path.GetFullPath(originalPath), Path.Combine(result, BaseFile), StringComparison.OrdinalIgnoreCase)) result = null; } catch (Exception) { }
            }
            if (result != null && Directory.Exists(result) && Directory.GetFiles(result, "*.veg").Length > 0)
            {
                if (MessageBox.Show(vegas.MainWindow, "The folder already holds .veg files. Files with the same names are replaced.\n\nContinue?",
                                    "Reno Settings Matrix", MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) result = null;
            }
        }
        f.Dispose();
        return result;
    }

    void ShowProgress(string text)
    {
        try
        {
            if (progressForm == null)
            {
                progressForm = new Form();
                progressForm.Text = "Reno Settings Matrix";
                progressForm.FormBorderStyle = FormBorderStyle.FixedToolWindow;
                progressForm.StartPosition = FormStartPosition.CenterScreen;
                progressForm.ClientSize = new System.Drawing.Size(520, 70);
                progressLabel = new Label();
                progressLabel.Left = 10; progressLabel.Top = 8; progressLabel.Width = 500; progressLabel.Height = 30;
                progressForm.Controls.Add(progressLabel);
                Button stop = new Button();
                stop.Left = 420; stop.Top = 38; stop.Width = 90; stop.Text = "Stop";
                stop.Click += delegate(object s, EventArgs e) { cancelled = true; };
                progressForm.Controls.Add(stop);
                progressForm.Show();
            }
            progressLabel.Text = text;
            System.Windows.Forms.Application.DoEvents();
        }
        catch (Exception) { }
    }

    // ------------------------------------------------------------------------------------------ the run
    void Run()
    {
        wavPath = Path.Combine(outDir, "test_tone.wav");
        bmpPath = Path.Combine(outDir, "test_picture.bmp");
        WriteWav(wavPath);
        WriteBmp(bmpPath);

        // 1. the base project
        ShowProgress("Building the base project...");
        BuildBase(false);
        string basePath = Path.Combine(outDir, BaseFile);
        vegas.SaveProject(basePath);
        Note("base saved: " + basePath);
        WriteLine(BaseFile, "", "ok", "(base)", "the base project", "", "");

        // 2. control: the base opened and saved again, with nothing changed (shows what VEGAS writes differently every time)
        Variant control = new Variant();
        control.Name = "base_again_control";
        control.BaseName = BaseFile;
        control.Desc = "opened and saved again, nothing changed";
        control.Custom = delegate(Vegas x) { return "control"; };
        RunVariant(control, "01_base_again_control.veg");

        // 3. a second base: the same with one extra audio bus, so that the bus's own settings can be changed
        ShowProgress("Building the second base (with a bus)...");
        BuildBase(true);
        vegas.SaveProject(Path.Combine(outDir, BaseBusFile));
        WriteLine(BaseBusFile, BaseFile, "ok", "Project.AddAudioBusTrack()", "base plus one audio bus 'Matrix bus'", "", "");

        // 4. the list of changes
        vegas.OpenProject(basePath);
        List<Variant> list = new List<Variant>();
        AddApiDump();
        AddProjectSweeps(list);
        AddTrackSweeps(list);
        AddStructural(list);
        AddGapVariants(list);
        AddCoverage();
        // the bus sweeps start from the second base
        vegas.OpenProject(Path.Combine(outDir, BaseBusFile));
        AddBusSweeps(list);
        Note("variants to make: " + list.Count);

        // 5. make them
        counter = 2;
        for (int i = 0; i < list.Count; ++i)
        {
            if (cancelled) { Note("STOPPED by the user after " + i + " of " + list.Count); break; }
            Variant v = list[i];
            counter++;
            string file = counter.ToString("D4", CultureInfo.InvariantCulture) + "_" + FileSafe(v.Name) + ".veg";
            ShowProgress((i + 1) + " / " + list.Count + ":  " + v.Name);
            RunVariant(v, file);
        }
        // the base is opened once more so that the last variant does not stay open
        try { vegas.OpenProject(basePath); } catch (Exception) { }
        Note("done: " + okCount + " files made, " + failCount + " failed");
        string msg = "Done.\n\n" + okCount + " files were saved, " + failCount + " changes failed (see manifest.txt).\n\nFolder:\n" + outDir +
                     "\n\nNext: run vegmatrix_diff on that folder (tests/vegas/vegmatrix_diff.cpp).";
        MessageBox.Show(vegas.MainWindow, msg, "Reno Settings Matrix", MessageBoxButtons.OK, MessageBoxIcon.Information);
    }

    void RunVariant(Variant v, string file)
    {
        string full = Path.Combine(outDir, file);
        string status = "ok", readback = "", note = "";
        bool applied = false;
        try
        {
            vegas.OpenProject(Path.Combine(outDir, v.BaseName));
            if (v.Custom != null) note = v.Custom(vegas);
            else note = Apply(v, out readback);
            applied = true;
            vegas.SaveProject(full);
            if (!File.Exists(full)) { status = "FAILED"; note = "VEGAS did not write the file"; }
        }
        catch (Exception ex)
        {
            status = "FAILED";
            note = Msg(ex);
        }
        if (status != "ok")
        {
            failCount++;
            // keep the project unmodified for the next open: save the half-changed one to a scratch file and delete it
            string scratch = Path.Combine(outDir, "_scratch.veg");
            try { vegas.SaveProject(scratch); } catch (Exception) { }
            try { if (File.Exists(scratch)) File.Delete(scratch); } catch (Exception) { }
            try { if (!applied && File.Exists(full)) File.Delete(full); } catch (Exception) { }
        }
        else okCount++;
        string change = v.Custom != null ? v.Desc : v.Path + "." + v.Prop + " = " + Show(v.Value);
        WriteLine(status == "ok" ? file : "(" + file + ")", v.BaseName, status, v.Path == null ? "" : v.Path, change, readback, note);
    }

    string Apply(Variant v, out string readback)
    {
        readback = "";
        object target = Resolve(vegas, v.Path);
        PropertyInfo pi = FindProp(target, v.Prop);
        if (pi == null) throw new Exception("no property " + v.Prop + " on " + target.GetType().Name);
        object value = ConvertTo(pi.PropertyType, v.Value);
        try { pi.SetValue(target, value, null); }
        catch (TargetInvocationException tie) { throw new Exception(tie.InnerException != null ? tie.InnerException.Message : tie.Message); }
        try { readback = Show(pi.GetValue(Resolve(vegas, v.Path), null)); }
        catch (Exception ex) { readback = "(cannot read back: " + Msg(ex) + ")"; }
        string want = Show(value);
        return readback == want ? "" : "read back differs from what was set";
    }

    // ------------------------------------------------------------------------------------------ the base project
    void BuildBase(bool withBus)
    {
        vegas.NewProject(true, false);
        Project p = vegas.Project;
        TrySet(p.Video, "Width", 1920);
        TrySet(p.Video, "Height", 1080);
        TrySet(p.Video, "FrameRate", 30.0);

        // the plug-ins: a generator, an OpenFX video effect, a managed one (Rays), an audio effect
        PlugInNode gen = FindPlug(vegas.Generators, new string[] { "Solid Color", "Test Pattern", "Color Gradient" }, false, false);
        PlugInNode ofx = FindPlug(vegas.VideoFX, new string[] { "Invert", "Brightness and Contrast", "Gaussian Blur", "Mirror", "Black and White" }, true, false);
        PlugInNode rays = FindPlug(vegas.VideoFX, new string[] { "Rays" }, false, true);
        Note("generator: " + (gen == null ? "none found" : gen.Name));
        Note("OpenFX effect: " + (ofx == null ? "none found" : ofx.Name));
        Note("managed effect (Rays): " + (rays == null ? "none found (the Rays variants are skipped)" : rays.Name));

        Media genMedia = null;
        if (gen != null) { try { genMedia = new Media(gen); } catch (Exception ex) { Note("generated media failed: " + Msg(ex)); } }
        Media picture = null;
        try { picture = new Media(bmpPath); } catch (Exception ex) { Note("picture media failed: " + Msg(ex)); }
        Media tone = null;
        try { tone = new Media(wavPath); } catch (Exception ex) { Note("audio media failed: " + Msg(ex)); }

        Timecode zero = Timecode.FromMilliseconds(0);
        Timecode five = Timecode.FromMilliseconds(5000);
        Timecode half = Timecode.FromMilliseconds(500);

        VideoTrack v1 = p.AddVideoTrack();
        v1.Name = "Matrix video 1";
        VideoEvent a = v1.AddVideoEvent(zero, five);
        a.Name = "Matrix A";
        Media srcA = genMedia != null ? genMedia : picture;
        if (srcA != null) a.AddTake(srcA.GetVideoStreamByIndex(0));
        TrySet(a.FadeIn, "Length", half);
        TrySet(a.FadeOut, "Length", half);
        if (ofx != null) { try { a.Effects.Add(new Effect(ofx)); } catch (Exception ex) { Note("OpenFX effect not added: " + Msg(ex)); } }

        VideoTrack v2 = p.AddVideoTrack();
        v2.Name = "Matrix video 2";
        VideoEvent b = v2.AddVideoEvent(zero, five);
        b.Name = "Matrix B";
        Media srcB = picture != null ? picture : genMedia;
        if (srcB != null) b.AddTake(srcB.GetVideoStreamByIndex(0));
        if (rays != null) { try { b.Effects.Add(new Effect(rays)); } catch (Exception ex) { Note("Rays not added: " + Msg(ex)); } }

        AudioTrack t1 = p.AddAudioTrack();
        t1.Name = "Matrix audio";
        AudioEvent c = t1.AddAudioEvent(zero, five);
        c.Name = "Matrix tone";
        if (tone != null) c.AddTake(tone.GetAudioStreamByIndex(0));
        TrySet(c.FadeIn, "Length", half);
        TrySet(c.FadeOut, "Length", half);

        if (withBus)
        {
            try
            {
                object bus = Call(p, "AddAudioBusTrack");
                if (bus != null) { TrySet(bus, "Description", "Matrix bus"); TrySet(bus, "Name", "Matrix bus"); }
            }
            catch (Exception ex) { Note("audio bus not added: " + Msg(ex)); }
        }
    }

    // ------------------------------------------------------------------------------------------ what to sweep
    // objects whose settable properties are each changed once (or once per enum value)
    void AddProjectSweeps(List<Variant> list)
    {
        string[] roots = new string[] { "Project.Video", "Project.Audio", "Project.Ruler", "Project.Summary", "Project.Preview", "Project.AudioCD" };
        foreach (string r in roots) Sweep(list, BaseFile, r, 1);
        // the editor state that is saved with the project
        AddValue(list, BaseFile, "", "Cursor", 3000.0, "cursor at 3 s");
        AddValue(list, BaseFile, "", "SelectionStart", 1000.0, "selection start 1 s");
        AddValue(list, BaseFile, "", "SelectionLength", 2000.0, "selection length 2 s");
        AddValue(list, BaseFile, "", "LoopMode", true, "loop playback on");

        // methods with arguments: the 3D modes' swap and crosstalk settings
        object vid = null;
        try { vid = Resolve(vegas, "Project.Video"); } catch (Exception) { }
        Type modeType = vid == null ? null : TypeOf(vid, "Stereo3DMode");
        if (modeType != null && modeType.IsEnum)
        {
            foreach (string mode in Enum.GetNames(modeType))
            {
                if (Skipped(mode)) continue;
                string m = mode;
                AddCustom(list, BaseFile, "Video.Stereo3DSwap[" + m + "]=true", "Project.Video.SetStereo3DSwap(" + m + ", true)",
                    delegate(Vegas x) { Call(Resolve(x, "Project.Video"), "SetStereo3DSwap", Enum.Parse(modeType, m), true); return ""; });
                AddCustom(list, BaseFile, "Video.Stereo3DCrosstalk[" + m + "]=0.5", "Project.Video.SetStereo3DCrosstalkCancelation(" + m + ", 0.5)",
                    delegate(Vegas x) { Call(Resolve(x, "Project.Video"), "SetStereo3DCrosstalkCancelation", Enum.Parse(modeType, m), 0.5f); return ""; });
                AddCustom(list, BaseFile, "Video.Stereo3DCrosstalkInRender[" + m + "]=true", "Project.Video.SetIncludeCrosstalkCancelationInRenders(" + m + ", true)",
                    delegate(Vegas x) { Call(Resolve(x, "Project.Video"), "SetIncludeCrosstalkCancelationInRenders", Enum.Parse(modeType, m), true); return ""; });
            }
        }
        // summary items that the five named properties do not cover
        foreach (string fcc in new string[] { "ISBJ", "IGNR", "ISFT", "IKEY" })
        {
            string f = fcc;
            AddCustom(list, BaseFile, "Summary.Item[" + f + "]", "Project.Summary.SetItem(\"" + f + "\", \"Matrix\")",
                delegate(Vegas x) { Call(Resolve(x, "Project.Summary"), "SetItem", f, "Matrix"); return ""; });
        }
    }

    void AddTrackSweeps(List<Variant> list)
    {
        string[] roots = new string[] {
            "Project.Tracks[0]", "Project.Tracks[0].TrackMotion",
            "Project.Tracks[1]", "Project.Tracks[1].TrackMotion",
            "Project.Tracks[2]",
            "Project.Tracks[0].Events[0]", "Project.Tracks[0].Events[0].FadeIn", "Project.Tracks[0].Events[0].FadeOut", "Project.Tracks[0].Events[0].VideoMotion",
            "Project.Tracks[1].Events[0]", "Project.Tracks[1].Events[0].FadeIn", "Project.Tracks[1].Events[0].FadeOut",
            "Project.Tracks[2].Events[0]", "Project.Tracks[2].Events[0].FadeIn", "Project.Tracks[2].Events[0].FadeOut",
            "Project.Tracks[0].Events[0].Effects[0]", "Project.Tracks[1].Events[0].Effects[0]",
            "Project.Tracks[0].Events[0].Takes[0]", "Project.Tracks[2].Events[0].Takes[0]",
        };
        foreach (string r in roots) Sweep(list, BaseFile, r, 0);
    }

    void AddBusSweeps(List<Variant> list)
    {
        Sweep(list, BaseFile, "Project.MasterBus", 0);
        Sweep(list, BaseFile, "Project.VideoBus", 0);
        // the added bus: the one called Matrix bus, else the last of BusTracks
        int n = -1;
        try { n = Count(Resolve(vegas, "Project.BusTracks")); } catch (Exception ex) { Note("BusTracks: " + Msg(ex)); }
        int busIndex = -1;
        for (int i = 0; i < n; ++i)
        {
            string name = "";
            try { name = Convert.ToString(Get(Resolve(vegas, "Project.BusTracks[" + i + "]"), "Name")); } catch (Exception) { }
            try { if (name.Length == 0) name = Convert.ToString(Get(Resolve(vegas, "Project.BusTracks[" + i + "]"), "Description")); } catch (Exception) { }
            Note("bus " + i + ": " + name);
            if (name.IndexOf("Matrix bus", StringComparison.OrdinalIgnoreCase) >= 0) busIndex = i;
        }
        if (busIndex < 0 && n > 0) busIndex = n - 1;
        if (busIndex < 0) { Note("no added bus found: the bus sweeps are skipped"); return; }
        int bi = busIndex;
        Sweep(list, BaseBusFile, "Project.BusTracks[" + bi + "]", 0);
        // the audio track routed to the added bus
        AddCustom(list, BaseBusFile, "T2.BusTrack=added_bus", "Project.Tracks[2].BusTrack = BusTracks[" + bi + "]",
            delegate(Vegas x) { Set(Resolve(x, "Project.Tracks[2]"), "BusTrack", Resolve(x, "Project.BusTracks[" + bi + "]")); return ""; });
    }

    // things added to the base (one at a time)
    void AddStructural(List<Variant> list)
    {
        PlugInNode ofx = FindPlug(vegas.VideoFX, new string[] { "Invert", "Brightness and Contrast", "Gaussian Blur", "Mirror", "Black and White" }, true, false);
        PlugInNode ofx2 = FindPlug(vegas.VideoFX, new string[] { "Mirror", "Gaussian Blur", "Black and White" }, true, false);
        PlugInNode rays = FindPlug(vegas.VideoFX, new string[] { "Rays" }, false, true);
        PlugInNode afx = FindPlug(vegas.AudioFX, new string[] { "Reverb", "Chorus", "Delay", "Distortion", "Track Compressor", "Track EQ" }, false, false);
        PlugInNode afx2 = FindPlug(vegas.AudioFX, new string[] { "Chorus", "Delay", "Distortion", "Vibrato" }, false, false);

        // markers and regions
        AddCustom(list, BaseFile, "add_marker", "Project.Markers.Add(new Marker(1 s, \"m1\"))",
            delegate(Vegas x) { Call(Resolve(x, "Project.Markers"), "Add", New("Marker", Timecode.FromMilliseconds(1000), "m1")); return ""; });
        AddCustom(list, BaseFile, "add_region", "Project.Regions.Add(new Region(1 s, 2 s, \"r1\"))",
            delegate(Vegas x) { Call(Resolve(x, "Project.Regions"), "Add", New("Region", Timecode.FromMilliseconds(1000), Timecode.FromMilliseconds(2000), "r1")); return ""; });
        AddCustom(list, BaseFile, "add_command_marker", "Project.CommandMarkers.Add(new CommandMarker(1 s))",
            delegate(Vegas x) { Call(Resolve(x, "Project.CommandMarkers"), "Add", New("CommandMarker", Timecode.FromMilliseconds(1000))); return ""; });
        AddCustom(list, BaseFile, "add_cd_index", "Project.CDIndices.Add(new CDMarker(1 s))",
            delegate(Vegas x) { Call(Resolve(x, "Project.CDIndices"), "Add", New("CDMarker", Timecode.FromMilliseconds(1000))); return ""; });

        // video effects: OpenFX and managed, on events, tracks and the video bus; "apply before pan/crop" is swept with the events' effects
        if (ofx != null)
        {
            PlugInNode f = ofx;
            AddCustom(list, BaseFile, "video_fx_on_track_V1_ofx", "Tracks[0].Effects.Add(" + f.Name + ")",
                delegate(Vegas x) { Call(Resolve(x, "Project.Tracks[0].Effects"), "Add", new Effect(f)); return ""; });
            AddCustom(list, BaseFile, "video_fx_on_event_B_ofx", "Tracks[1].Events[0].Effects.Add(" + f.Name + ") (after Rays)",
                delegate(Vegas x) { Call(Resolve(x, "Project.Tracks[1].Events[0].Effects"), "Add", new Effect(f)); return ""; });
            AddCustom(list, BaseFile, "video_fx_on_video_bus_ofx", "VideoBus.Effects.Add(" + f.Name + ")",
                delegate(Vegas x) { Call(Resolve(x, "Project.VideoBus.Effects"), "Add", new Effect(f)); return ""; });
        }
        if (ofx2 != null)
        {
            PlugInNode f = ofx2;
            AddCustom(list, BaseFile, "video_fx_on_event_A_second_ofx", "Tracks[0].Events[0].Effects.Add(" + f.Name + ") (a second OpenFX)",
                delegate(Vegas x) { Call(Resolve(x, "Project.Tracks[0].Events[0].Effects"), "Add", new Effect(f)); return ""; });
        }
        if (rays != null)
        {
            PlugInNode f = rays;
            AddCustom(list, BaseFile, "video_fx_on_event_A_rays_after_ofx", "Tracks[0].Events[0].Effects.Add(" + f.Name + ") (Rays after the OpenFX)",
                delegate(Vegas x) { Call(Resolve(x, "Project.Tracks[0].Events[0].Effects"), "Add", new Effect(f)); return ""; });
            AddCustom(list, BaseFile, "video_fx_on_event_A_rays_after_ofx_rays_before_pancrop", "as above, and the Rays effect has ApplyBeforePanCrop = true",
                delegate(Vegas x)
                {
                    Call(Resolve(x, "Project.Tracks[0].Events[0].Effects"), "Add", new Effect(f));
                    Set(Resolve(x, "Project.Tracks[0].Events[0].Effects[1]"), "ApplyBeforePanCrop", true);
                    return "";
                });
            AddCustom(list, BaseFile, "video_fx_on_event_A_rays_after_ofx_ofx_before_pancrop", "as video_fx_on_event_A_rays_after_ofx, and the OpenFX effect has ApplyBeforePanCrop = true",
                delegate(Vegas x)
                {
                    Call(Resolve(x, "Project.Tracks[0].Events[0].Effects"), "Add", new Effect(f));
                    Set(Resolve(x, "Project.Tracks[0].Events[0].Effects[0]"), "ApplyBeforePanCrop", true);
                    return "";
                });
            AddCustom(list, BaseFile, "video_fx_on_track_V1_rays", "Tracks[0].Effects.Add(" + f.Name + ")",
                delegate(Vegas x) { Call(Resolve(x, "Project.Tracks[0].Effects"), "Add", new Effect(f)); return ""; });
        }

        // audio effects on an event, a track, the master bus
        if (afx != null)
        {
            PlugInNode f = afx;
            AddCustom(list, BaseFile, "audio_fx_on_event", "Tracks[2].Events[0].Effects.Add(" + f.Name + ")",
                delegate(Vegas x) { Call(Resolve(x, "Project.Tracks[2].Events[0].Effects"), "Add", new Effect(f)); return ""; });
            AddCustom(list, BaseFile, "audio_fx_on_track", "Tracks[2].Effects.Add(" + f.Name + ")",
                delegate(Vegas x) { Call(Resolve(x, "Project.Tracks[2].Effects"), "Add", new Effect(f)); return ""; });
            AddCustom(list, BaseFile, "audio_fx_on_master_bus", "MasterBus.Effects.Add(" + f.Name + ")",
                delegate(Vegas x) { Call(Resolve(x, "Project.MasterBus.Effects"), "Add", new Effect(f)); return ""; });
        }
        if (afx != null && afx2 != null)
        {
            PlugInNode f = afx, g = afx2;
            AddCustom(list, BaseFile, "audio_fx_on_track_two_effects", "Tracks[2].Effects.Add(" + f.Name + "), then (" + g.Name + ")",
                delegate(Vegas x)
                {
                    Call(Resolve(x, "Project.Tracks[2].Effects"), "Add", new Effect(f));
                    Call(Resolve(x, "Project.Tracks[2].Effects"), "Add", new Effect(g));
                    return "";
                });
        }

        // buses, tracks
        AddCustom(list, BaseFile, "add_audio_bus", "Project.AddAudioBusTrack()",
            delegate(Vegas x) { Call(Resolve(x, "Project"), "AddAudioBusTrack"); return ""; });
        if (afx != null)
        {
            PlugInNode f = afx;
            AddCustom(list, BaseFile, "add_audio_fx_bus", "Project.AddAudioFXBusTrack(" + f.Name + ")",
                delegate(Vegas x) { Call(Resolve(x, "Project"), "AddAudioFXBusTrack", f); return ""; });
        }
        AddCustom(list, BaseFile, "add_video_track", "Project.AddVideoTrack()",
            delegate(Vegas x) { Call(Resolve(x, "Project"), "AddVideoTrack"); return ""; });
        AddCustom(list, BaseFile, "add_audio_track", "Project.AddAudioTrack()",
            delegate(Vegas x) { Call(Resolve(x, "Project"), "AddAudioTrack"); return ""; });

        // takes, groups, envelopes
        AddCustom(list, BaseFile, "event_B_second_take", "Tracks[1].Events[0].AddTake of the generated media (two takes)",
            delegate(Vegas x)
            {
                PlugInNode g = FindPlug(x.Generators, new string[] { "Solid Color", "Test Pattern", "Color Gradient" }, false, false);
                if (g == null) throw new Exception("no generator");
                Media m = new Media(g);
                Call(Resolve(x, "Project.Tracks[1].Events[0]"), "AddTake", m.GetVideoStreamByIndex(0));
                return "";
            });
        AddCustom(list, BaseFile, "group_A_and_audio", "events A and the tone grouped (TrackEventGroup)",
            delegate(Vegas x)
            {
                object grp = New("TrackEventGroup");
                Call(Resolve(x, "Project.TrackEventGroups"), "Add", grp);
                Call(grp, "Add", Resolve(x, "Project.Tracks[0].Events[0]"));
                Call(grp, "Add", Resolve(x, "Project.Tracks[2].Events[0]"));
                return "";
            });
        AddCustom(list, BaseFile, "event_A_selected_and_audio_selected", "events A and the tone selected",
            delegate(Vegas x)
            {
                Set(Resolve(x, "Project.Tracks[0].Events[0]"), "Selected", true);
                Set(Resolve(x, "Project.Tracks[2].Events[0]"), "Selected", true);
                return "";
            });
        AddCustom(list, BaseFile, "envelope_audio_track_volume", "a Volume envelope with two points on the audio track",
            delegate(Vegas x)
            {
                object tr = Resolve(x, "Project.Tracks[2]");
                object env = New("Envelope", ParseEnum("EnvelopeType", "Volume"));
                Call(Get(tr, "Envelopes"), "Add", env);
                Call(Get(env, "Points"), "Add", New("EnvelopePoint", Timecode.FromMilliseconds(1000), 0.5));
                return "";
            });
        AddCustom(list, BaseFile, "envelope_video_track_composite", "a Composite level envelope with two points on video track 1",
            delegate(Vegas x)
            {
                object tr = Resolve(x, "Project.Tracks[0]");
                object env = New("Envelope", ParseEnum("EnvelopeType", "Composite"));
                Call(Get(tr, "Envelopes"), "Add", env);
                Call(Get(env, "Points"), "Add", New("EnvelopePoint", Timecode.FromMilliseconds(1000), 0.5));
                return "";
            });
        AddCustom(list, BaseFile, "event_A_motion_keyframe", "a pan/crop keyframe at 1 s on event A (VideoMotion.Keyframes.Add)",
            delegate(Vegas x)
            {
                object kf = Get(Resolve(x, "Project.Tracks[0].Events[0].VideoMotion"), "Keyframes");
                object k = Call(kf, "Add", Timecode.FromMilliseconds(1000));
                return "";
            });
        AddCustom(list, BaseFile, "track_V1_motion_keyframe", "a track motion keyframe at 1 s on video track 1",
            delegate(Vegas x) { Call(Resolve(x, "Project.Tracks[0].TrackMotion"), "InsertMotionKeyframe", Timecode.FromMilliseconds(1000)); return ""; });
        AddCustom(list, BaseFile, "track_V1_shadow_keyframe", "a track shadow keyframe at 1 s on video track 1",
            delegate(Vegas x) { Call(Resolve(x, "Project.Tracks[0].TrackMotion"), "InsertShadowKeyframe", Timecode.FromMilliseconds(1000)); return ""; });
        AddCustom(list, BaseFile, "track_V1_glow_keyframe", "a track glow keyframe at 1 s on video track 1",
            delegate(Vegas x) { Call(Resolve(x, "Project.Tracks[0].TrackMotion"), "InsertGlowKeyframe", Timecode.FromMilliseconds(1000)); return ""; });
        AddCustom(list, BaseFile, "track_group_both_video", "the two video tracks grouped (GroupSelectedTracks)",
            delegate(Vegas x)
            {
                Set(Resolve(x, "Project.Tracks[0]"), "Selected", true);
                Set(Resolve(x, "Project.Tracks[1]"), "Selected", true);
                Call(Resolve(x, "Project"), "GroupSelectedTracks");
                return "";
            });
    }


    // ------------------------------------------------------------------------------------------ what Reno cannot write yet
    // Files for the .veg parts Reno does not write because no sample shows where VEGAS keeps them (docs/NOTES.md, "VEGAS
    // projects"): transitions, track motion, bus sends and bus settings, the parent/child setting, track groups, effects and
    // envelopes on the video output and the master, VST3 plug-ins, the event undersample rate, surround pans, multichannel
    // sources, Trimmer markers and OpenFX keyframe interpolation. Each file changes one thing, so vegmatrix_diff shows
    // which bytes it is. (Red Eye Reduction areas and the sends' own level and pre/post switch have no scripting API:
    // see the list in the manifest.)
    void AddGapVariants(List<Variant> list)
    {
        Note("gap variants: files for the parts of a .veg Reno does not write yet");
        Note("MANUAL (no scripting API; make by hand in VEGAS, one change per file, from 00_base.veg): red_eye (Tools > Video > Red Eye Reduction on the picture event, one area); send_level (Mixing Console: turn the Assignable FX send knob of the audio track to Bus A to -6 dB; then again with Pre-Fader set); bus_pan (Mixing Console: pan the added bus to the left); multichannel_choice (an event of a 6 channel file: Properties > Media > channel choice); tracks_group_collapse and track-group colour");
        // --- transitions
        PlugInNode tx = null;
        try { tx = FindPlug(vegas.Transitions, new string[] { "Cross", "Dissolve", "Wipe", "Fade" }, true, false); } catch (Exception) { }
        PlugInNode tx2 = null;
        try { tx2 = FindPlug(vegas.Transitions, new string[] { "Wipe", "Iris", "Slide", "Page" }, true, false); } catch (Exception) { }
        Change overlap = delegate(Vegas x)
        {
            // video track 1: its event split at 2 s, the second part moved 0.5 s back so the two overlap
            object ev = Resolve(x, "Project.Tracks[0].Events[0]");
            object second = Call(ev, "Split", Timecode.FromMilliseconds(2000));
            if (second == null) second = Resolve(x, "Project.Tracks[0].Events[1]");
            Set(second, "Start", Timecode.FromMilliseconds(1500));
            Set(Get(second, "FadeIn"), "Length", Timecode.FromMilliseconds(500));
            return "";
        };
        AddCustom(list, BaseFile, "tx_overlap_only", "event A split at 2 s, the second part starts at 1.5 s (a cross-fade, no transition plug-in)",
            delegate(Vegas x) { return overlap(x); });
        if (tx != null)
        {
            PlugInNode p = tx;
            AddCustom(list, BaseFile, "tx_" + FileSafe(p.Name), "as tx_overlap_only with the transition '" + p.Name + "' on the second event's FadeIn",
                delegate(Vegas x)
                {
                    overlap(x);
                    Set(Get(Resolve(x, "Project.Tracks[0].Events[1]"), "FadeIn"), "Transition", new Effect(p));
                    return "";
                });
            AddCustom(list, BaseFile, "tx_progress_envelope", "as above, plus a TransitionProgress envelope with a point at 0.25 s (value 0.5) on the second event",
                delegate(Vegas x)
                {
                    overlap(x);
                    object ev = Resolve(x, "Project.Tracks[0].Events[1]");
                    Set(Get(ev, "FadeIn"), "Transition", new Effect(p));
                    object env = New("Envelope", ParseEnum("EnvelopeType", "TransitionProgress"));
                    Call(Get(ev, "Envelopes"), "Add", env);
                    Call(Get(env, "Points"), "Add", New("EnvelopePoint", Timecode.FromMilliseconds(250), 0.5));
                    return "";
                });
            AddCustom(list, BaseFile, "tx_remove", "as tx_" + FileSafe(p.Name) + " and RemoveTransition() afterwards (what is left behind)",
                delegate(Vegas x)
                {
                    overlap(x);
                    object fade = Get(Resolve(x, "Project.Tracks[0].Events[1]"), "FadeIn");
                    Set(fade, "Transition", new Effect(p));
                    Call(fade, "RemoveTransition");
                    return "";
                });
        }
        if (tx != null && tx2 != null && tx2.Name != tx.Name)
        {
            PlugInNode p = tx, q = tx2;
            AddCustom(list, BaseFile, "tx_second_kind_" + FileSafe(q.Name), "as the first transition file but the transition is '" + q.Name + "'",
                delegate(Vegas x)
                {
                    overlap(x);
                    Set(Get(Resolve(x, "Project.Tracks[0].Events[1]"), "FadeIn"), "Transition", new Effect(q));
                    return "";
                });
        }

        // --- track motion: one property of the first keyframe at a time (motion, shadow, glow, and the parent's)
        string[][] motion = new string[][] {
            new string[] { "PositionX", "0.25" }, new string[] { "PositionY", "0.125" }, new string[] { "PositionZ", "0.5" },
            new string[] { "Width", "2.5" }, new string[] { "Height", "1.5" }, new string[] { "Depth", "0.5" },
            new string[] { "RotationX", "20" }, new string[] { "RotationY", "30" }, new string[] { "RotationZ", "40" },
            new string[] { "RotationOffsetX", "0.1" }, new string[] { "RotationOffsetY", "0.2" }, new string[] { "RotationOffsetZ", "0.3" },
            new string[] { "OrientationX", "10" }, new string[] { "OrientationY", "15" }, new string[] { "OrientationZ", "25" },
            new string[] { "Smoothness", "0.5" }, new string[] { "Type", "Hold" }, new string[] { "Type", "Slow" }, new string[] { "Type", "Fast" },
            new string[] { "Type", "Smooth" }, new string[] { "Type", "Sharp" },
        };
        foreach (string[] m in motion)
            AddMotionKey(list, "motion", "Project.Tracks[0].TrackMotion", "InsertMotionKeyframe", m[0], m[1]);
        string[][] shadowGlow = new string[][] {
            new string[] { "PositionX", "0.25" }, new string[] { "PositionY", "0.125" }, new string[] { "Width", "2.5" }, new string[] { "Height", "1.5" },
            new string[] { "RotationZ", "40" }, new string[] { "Blur", "0.4" }, new string[] { "Intensity", "0.6" }, new string[] { "Smoothness", "0.5" },
            new string[] { "Type", "Hold" }, new string[] { "Type", "Smooth" },
        };
        foreach (string[] m in shadowGlow)
        {
            AddMotionKey(list, "shadow", "Project.Tracks[0].TrackMotion", "InsertShadowKeyframe", m[0], m[1]);
            AddMotionKey(list, "glow", "Project.Tracks[0].TrackMotion", "InsertGlowKeyframe", m[0], m[1]);
        }
        AddCustom(list, BaseFile, "motion_shadow_enabled", "track 1's ShadowEnabled = true after a shadow keyframe",
            delegate(Vegas x) { object tm = Resolve(x, "Project.Tracks[0].TrackMotion"); Call(tm, "InsertShadowKeyframe", Timecode.FromMilliseconds(1000)); Set(tm, "ShadowEnabled", true); return ""; });
        AddCustom(list, BaseFile, "motion_glow_enabled", "track 1's GlowEnabled = true after a glow keyframe",
            delegate(Vegas x) { object tm = Resolve(x, "Project.Tracks[0].TrackMotion"); Call(tm, "InsertGlowKeyframe", Timecode.FromMilliseconds(1000)); Set(tm, "GlowEnabled", true); return ""; });
        AddCustom(list, BaseFile, "motion_two_keys", "two motion keyframes (0.5 s and 3 s) with different positions",
            delegate(Vegas x)
            {
                object tm = Resolve(x, "Project.Tracks[0].TrackMotion");
                object a = Call(tm, "InsertMotionKeyframe", Timecode.FromMilliseconds(500));
                object b = Call(tm, "InsertMotionKeyframe", Timecode.FromMilliseconds(3000));
                Set(a, "PositionX", 0.1); Set(b, "PositionX", 0.4);
                return "";
            });
        AddCustom(list, BaseFile, "motion_key_at_zero", "one motion keyframe at 0 s with PositionX 0.3",
            delegate(Vegas x) { object a = Call(Resolve(x, "Project.Tracks[0].TrackMotion"), "InsertMotionKeyframe", Timecode.FromMilliseconds(0)); Set(a, "PositionX", 0.3); return ""; });
        AddCustom(list, BaseFile, "motion_other_track", "a motion keyframe on video track 2 (position 0.2)",
            delegate(Vegas x) { object a = Call(Resolve(x, "Project.Tracks[1].TrackMotion"), "InsertMotionKeyframe", Timecode.FromMilliseconds(1000)); Set(a, "PositionX", 0.2); return ""; });
        string[][] parentMotion = new string[][] { new string[] { "PositionX", "0.25" }, new string[] { "Width", "2.5" }, new string[] { "RotationZ", "40" } };
        foreach (string[] m in parentMotion)
            AddMotionKey(list, "parent_motion", "Project.Tracks[0].ParentTrackMotion", "InsertMotionKeyframe", m[0], m[1]);

        // --- the parent/child setting
        AddCustom(list, BaseFile, "child_track_V2_level1", "video track 2 made a child of track 1 (CompositeNestingLevel = 1)",
            delegate(Vegas x) { Set(Resolve(x, "Project.Tracks[1]"), "CompositeNestingLevel", 1); return ""; });
        AddCustom(list, BaseFile, "child_track_V2_parent_mode", "as child_track_V2_level1 and track 1's ParentCompositeMode = Add",
            delegate(Vegas x)
            {
                Set(Resolve(x, "Project.Tracks[1]"), "CompositeNestingLevel", 1);
                Set(Resolve(x, "Project.Tracks[0]"), "ParentCompositeMode", ParseEnum("CompositeMode", "Add"));
                return "";
            });

        // --- track groups
        Change makeGroup = delegate(Vegas x)
        {
            Set(Resolve(x, "Project.Tracks[0]"), "Selected", true);
            Set(Resolve(x, "Project.Tracks[1]"), "Selected", true);
            Call(Resolve(x, "Project"), "GroupSelectedTracks");
            return "";
        };
        AddCustom(list, BaseFile, "trackgroup_name", "the two video tracks grouped, group named 'Matrix group'",
            delegate(Vegas x) { makeGroup(x); Set(Resolve(x, "Project.TrackGroups[0]"), "Name", "Matrix group"); return ""; });
        AddCustom(list, BaseFile, "trackgroup_mute", "grouped, group Mute = true",
            delegate(Vegas x) { makeGroup(x); Set(Resolve(x, "Project.TrackGroups[0]"), "Mute", true); return ""; });
        AddCustom(list, BaseFile, "trackgroup_solo", "grouped, group Solo = true",
            delegate(Vegas x) { makeGroup(x); Set(Resolve(x, "Project.TrackGroups[0]"), "Solo", true); return ""; });
        AddCustom(list, BaseFile, "trackgroup_collapsed", "grouped, group collapsed",
            delegate(Vegas x) { makeGroup(x); Call(Resolve(x, "Project.TrackGroups[0]"), "CollapseTrackGroup"); return ""; });
        AddCustom(list, BaseFile, "trackgroup_video_and_audio", "video track 1 and the audio track grouped",
            delegate(Vegas x)
            {
                Set(Resolve(x, "Project.Tracks[0]"), "Selected", true);
                Set(Resolve(x, "Project.Tracks[2]"), "Selected", true);
                Call(Resolve(x, "Project"), "GroupSelectedTracks");
                return "";
            });

        // --- buses: sends (envelopes), settings, envelopes on buses, the master and the video output
        AddCustom(list, BaseBusFile, "send_envelope_to_bus_A", "the audio track gets a BusA (send) envelope with a point at 1 s (0.5)",
            delegate(Vegas x)
            {
                object env = New("Envelope", ParseEnum("EnvelopeType", "BusA"));
                Call(Get(Resolve(x, "Project.Tracks[2]"), "Envelopes"), "Add", env);
                Call(Get(env, "Points"), "Add", New("EnvelopePoint", Timecode.FromMilliseconds(1000), 0.5));
                return "";
            });
        AddCustom(list, BaseBusFile, "send_envelope_and_routed", "as above and the track's BusTrack is the added bus",
            delegate(Vegas x)
            {
                object env = New("Envelope", ParseEnum("EnvelopeType", "BusA"));
                Call(Get(Resolve(x, "Project.Tracks[2]"), "Envelopes"), "Add", env);
                Call(Get(env, "Points"), "Add", New("EnvelopePoint", Timecode.FromMilliseconds(1000), 0.5));
                Set(Resolve(x, "Project.Tracks[2]"), "BusTrack", Resolve(x, "Project.BusTracks[0]"));
                return "";
            });
        foreach (string kind in new string[] { "Volume", "Pan", "Mute" })
        {
            string k = kind;
            AddCustom(list, BaseBusFile, "bus_envelope_" + k, "a " + k + " envelope with a point at 1 s on the added bus",
                delegate(Vegas x) { return AddEnvelopeTo(Resolve(x, "Project.BusTracks[0]"), k); });
            AddCustom(list, BaseFile, "master_envelope_" + k, "a " + k + " envelope with a point at 1 s on the master bus",
                delegate(Vegas x) { return AddEnvelopeTo(Resolve(x, "Project.MasterBus"), k); });
        }
        foreach (string kind in new string[] { "FadeToColor", "Mute", "MotionBlurLength", "VideoSupersampling" })
        {
            string k = kind;
            AddCustom(list, BaseFile, "videobus_envelope_" + k, "a " + k + " envelope with a point at 1 s on the video output (VideoBus)",
                delegate(Vegas x) { return AddEnvelopeTo(Resolve(x, "Project.VideoBus"), k); });
        }
        AddCustom(list, BaseBusFile, "bus_mute_and_solo", "the added bus muted and soloed together",
            delegate(Vegas x) { object b = Resolve(x, "Project.BusTracks[0]"); Set(b, "Mute", true); Set(b, "Solo", true); return ""; });
        AddCustom(list, BaseFile, "master_mute", "the master bus Mute = true",
            delegate(Vegas x) { Set(Resolve(x, "Project.MasterBus"), "Mute", true); return ""; });
        AddCustom(list, BaseFile, "master_solo", "the master bus Solo = true",
            delegate(Vegas x) { Set(Resolve(x, "Project.MasterBus"), "Solo", true); return ""; });
        AddCustom(list, BaseFile, "video_bus_two_fx", "two OpenFX effects on the video output (order and count)",
            delegate(Vegas x)
            {
                PlugInNode a = FindPlug(x.VideoFX, new string[] { "Invert", "Gaussian Blur", "Mirror" }, true, false);
                PlugInNode b = FindPlug(x.VideoFX, new string[] { "Mirror", "Gaussian Blur", "Black and White" }, true, false);
                Call(Resolve(x, "Project.VideoBus.Effects"), "Add", new Effect(a));
                Call(Resolve(x, "Project.VideoBus.Effects"), "Add", new Effect(b));
                return "";
            });
        AddCustom(list, BaseFile, "video_bus_fx_keyframed", "an OpenFX effect on the video output with a keyed double parameter (keys at 0 and 2 s)",
            delegate(Vegas x)
            {
                PlugInNode a = FindPlug(x.VideoFX, new string[] { "Brightness and Contrast", "Gaussian Blur", "Invert" }, true, false);
                Effect fx = new Effect(a);
                Call(Resolve(x, "Project.VideoBus.Effects"), "Add", fx);
                return KeyFirstDouble(fx, "Linear", "Linear");
            });
        AddCustom(list, BaseFile, "master_bus_two_fx", "two audio effects on the master bus",
            delegate(Vegas x)
            {
                PlugInNode a = FindPlug(x.AudioFX, new string[] { "Reverb", "Chorus", "Delay" }, false, false);
                PlugInNode b = FindPlug(x.AudioFX, new string[] { "Chorus", "Delay", "Distortion" }, false, false);
                Call(Resolve(x, "Project.MasterBus.Effects"), "Add", new Effect(a));
                Call(Resolve(x, "Project.MasterBus.Effects"), "Add", new Effect(b));
                return "";
            });

        // --- VST3 plug-ins (named VST3 in the plug-in list, or with a VST3 unique id)
        List<PlugInNode> audio = new List<PlugInNode>();
        try { Collect(vegas.AudioFX, audio, 0); } catch (Exception) { }
        List<PlugInNode> vst3 = new List<PlugInNode>();
        foreach (PlugInNode n in audio)
        {
            string id = "", info = "";
            try { id = n.UniqueID; } catch (Exception) { }
            try { info = n.Info; } catch (Exception) { }
            string all = (n.Name + " " + n.Group + " " + id + " " + info);
            if (all.IndexOf("VST3", StringComparison.OrdinalIgnoreCase) >= 0 || all.IndexOf("vst 3", StringComparison.OrdinalIgnoreCase) >= 0) vst3.Add(n);
        }
        Note("VST3 plug-ins found in VEGAS's audio plug-in list: " + vst3.Count + (vst3.Count == 0 ? " (install one and run again; the VST3 files are skipped)" : ""));
        for (int i = 0; i < vst3.Count && i < 2; ++i)
        {
            PlugInNode p = vst3[i];
            string tag = FileSafe(p.Name);
            AddCustom(list, BaseFile, "vst3_on_event_" + tag, "Tracks[2].Events[0].Effects.Add(" + p.Name + ")",
                delegate(Vegas x) { Call(Resolve(x, "Project.Tracks[2].Events[0].Effects"), "Add", new Effect(p)); return ""; });
            AddCustom(list, BaseFile, "vst3_on_track_" + tag, "Tracks[2].Effects.Add(" + p.Name + ")",
                delegate(Vegas x) { Call(Resolve(x, "Project.Tracks[2].Effects"), "Add", new Effect(p)); return ""; });
            AddCustom(list, BaseFile, "vst3_on_master_" + tag, "MasterBus.Effects.Add(" + p.Name + ")",
                delegate(Vegas x) { Call(Resolve(x, "Project.MasterBus.Effects"), "Add", new Effect(p)); return ""; });
            AddCustom(list, BaseBusFile, "vst3_on_bus_" + tag, "BusTracks[0].Effects.Add(" + p.Name + ")",
                delegate(Vegas x) { Call(Resolve(x, "Project.BusTracks[0].Effects"), "Add", new Effect(p)); return ""; });
        }

        // --- the video event's undersample rate, and the playback rate beside it
        foreach (double u in new double[] { 0.5, 0.25, 2.0 })
        {
            double uu = u;
            AddCustom(list, BaseFile, "undersample_A_" + uu.ToString(CultureInfo.InvariantCulture), "Tracks[0].Events[0].UnderSampleRate = " + uu.ToString(CultureInfo.InvariantCulture),
                delegate(Vegas x) { Set(Resolve(x, "Project.Tracks[0].Events[0]"), "UnderSampleRate", uu); return ""; });
        }
        AddCustom(list, BaseFile, "undersample_B_0.5_and_rate_2", "Tracks[1].Events[0].UnderSampleRate = 0.5 and PlaybackRate = 2",
            delegate(Vegas x) { object e = Resolve(x, "Project.Tracks[1].Events[0]"); Set(e, "UnderSampleRate", 0.5); Set(e, "PlaybackRate", 2.0); return ""; });

        // --- surround: the master in 5.1 and the audio track's surround pan
        Change surround = delegate(Vegas x) { Set(Resolve(x, "Project.Audio"), "MasterBusMode", ParseEnum("AudioBusMode", "Surround")); return ""; };
        AddCustom(list, BaseFile, "surround_mode_only", "Project.Audio.MasterBusMode = Surround, nothing else",
            delegate(Vegas x) { return surround(x); });
        foreach (string[] sp in new string[][] { new string[] { "PanX", "0.5" }, new string[] { "PanY", "-0.5" }, new string[] { "PanCenter", "0.25" } })
        {
            string[] spp = sp;
            AddCustom(list, BaseFile, "surround_track_" + spp[0] + "=" + spp[1], "master in 5.1 and Tracks[2]." + spp[0] + " = " + spp[1],
                delegate(Vegas x) { surround(x); Set(Resolve(x, "Project.Tracks[2]"), spp[0], Convert.ToSingle(spp[1], CultureInfo.InvariantCulture)); return ""; });
        }
        AddCustom(list, BaseBusFile, "surround_bus_added", "master in 5.1 with the added bus",
            delegate(Vegas x) { return surround(x); });
        foreach (string pt in new string[] { "Constant Power", "Balance (0 dB center)", "Film" })
        {
            string ptt = pt;
            AddCustom(list, BaseBusFile, "bus_pantype_" + FileSafe(ptt), "the added bus's PanType = " + ptt,
                delegate(Vegas x) { Set(Resolve(x, "Project.BusTracks[0]"), "PanType", ParsePanType(ptt)); return ""; });
        }

        // --- a source with more than two channels
        AddCustom(list, BaseFile, "multichannel_6ch_event", "an audio event of a 6 channel wave file on the audio track (made here)",
            delegate(Vegas x)
            {
                string w = Path.Combine(outDir, "six_channels.wav");
                if (!File.Exists(w)) WriteWave(w, 6);
                object media = Call(Get(x.Project, "MediaPool"), "AddMedia", w);
                object tr = Resolve(x, "Project.Tracks[2]");
                object ev = Call(tr, "AddAudioEvent", Timecode.FromMilliseconds(6000), Timecode.FromMilliseconds(2000));
                Call(ev, "AddTake", Call(media, "GetAudioStreamByIndex", 0));
                return "";
            });
        AddCustom(list, BaseFile, "multichannel_6ch_event_remap", "as above with the event's Channels = Swapped",
            delegate(Vegas x)
            {
                string w = Path.Combine(outDir, "six_channels.wav");
                if (!File.Exists(w)) WriteWave(w, 6);
                object media = Call(Get(x.Project, "MediaPool"), "AddMedia", w);
                object tr = Resolve(x, "Project.Tracks[2]");
                object ev = Call(tr, "AddAudioEvent", Timecode.FromMilliseconds(6000), Timecode.FromMilliseconds(2000));
                Call(ev, "AddTake", Call(media, "GetAudioStreamByIndex", 0));
                Set(ev, "Channels", ParseEnum("ChannelRemapping", "Swap"));
                return "";
            });

        // --- Trimmer markers: markers, regions and command markers of the media itself
        AddCustom(list, BaseFile, "media_marker", "a media marker at 1 s on the audio track's media",
            delegate(Vegas x) { return AddMediaMark(MediaOf(x, "Project.Tracks[2].Events[0]"), "Markers", "MediaMarker", 0); });
        AddCustom(list, BaseFile, "media_region", "a media region 1 s to 2 s on the audio track's media",
            delegate(Vegas x) { return AddMediaMark(MediaOf(x, "Project.Tracks[2].Events[0]"), "Regions", "MediaRegion", 1000); });
        AddCustom(list, BaseFile, "media_command_marker", "a media command marker at 1 s on the audio track's media",
            delegate(Vegas x) { return AddMediaMark(MediaOf(x, "Project.Tracks[2].Events[0]"), "CommandMarkers", "MediaCommandMarker", 0); });

        // --- OpenFX keyframe interpolation of a keyed double parameter, one kind per file, on event A's first OpenFX effect
        PlugInNode kfx = FindPlug(vegas.VideoFX, new string[] { "Brightness and Contrast", "Gaussian Blur", "Invert", "Mirror" }, true, false);
        if (kfx != null)
        {
            PlugInNode kp = kfx;
            foreach (string it in new string[] { "Linear", "Fast", "Slow", "Smooth", "Sharp", "Hold", "Manual", "Split" })
            {
                string kind = it;
                AddCustom(list, BaseFile, "ofx_key_interp_" + kind, "an OpenFX effect on event A with a double parameter keyed at 0 s and 2 s; both keys " + kind,
                    delegate(Vegas x)
                    {
                        Effect fx = new Effect(kp);
                        Call(Resolve(x, "Project.Tracks[0].Events[0].Effects"), "Add", fx);
                        return KeyFirstDouble(fx, kind, kind);
                    });
            }
            AddCustom(list, BaseFile, "ofx_key_interp_mixed", "as above, the first key Smooth and the second Hold",
                delegate(Vegas x)
                {
                    Effect fx = new Effect(kp);
                    Call(Resolve(x, "Project.Tracks[0].Events[0].Effects"), "Add", fx);
                    return KeyFirstDouble(fx, "Smooth", "Hold");
                });
        }
    }

    // a point at 1 s on a new envelope of the given EnvelopeType on 'owner' (a bus track)
    static string AddEnvelopeTo(object owner, string kind)
    {
        object env = New("Envelope", ParseEnum("EnvelopeType", kind));
        Call(Get(owner, "Envelopes"), "Add", env);
        Call(Get(env, "Points"), "Add", New("EnvelopePoint", Timecode.FromMilliseconds(1000), kind == "Mute" ? 1.0 : 0.5));
        return "";
    }

    static string AddMediaMark(object media, string list, string type, int lengthMs)
    {
        object m = New(type);
        Set(m, "Position", Timecode.FromMilliseconds(1000));
        Set(m, "Label", "mark1");
        if (lengthMs > 0) Set(m, "Length", Timecode.FromMilliseconds(lengthMs));
        Call(Get(media, list), "Add", m);
        return "";
    }

    static object MediaOf(Vegas x, string eventPath)
    {
        object ev = Resolve(x, eventPath);
        object take = Get(ev, "ActiveTake");
        return Get(take, "Media");
    }

    static object ParsePanType(string name)
    {
        Type t = typeof(Vegas).Assembly.GetType("ScriptPortal.Vegas.PanType");
        foreach (string n in Enum.GetNames(t))
            if (n.Replace(" ", "").Replace("(", "").Replace(")", "").Replace("-", "").Replace(",", "").IndexOf(name.Replace(" ", "").Replace("(", "").Replace(")", "").Replace("-", ""), StringComparison.OrdinalIgnoreCase) >= 0 ||
                name.Replace(" ", "").Replace("(", "").Replace(")", "").Replace("-", "").IndexOf(n, StringComparison.OrdinalIgnoreCase) >= 0)
                return Enum.Parse(t, n);
        return Enum.Parse(t, Enum.GetNames(t)[0]);
    }

    // inserts a track motion keyframe of 'inserter' at 1 s on 'tmPath' and sets one property of it
    void AddMotionKey(List<Variant> list, string what, string tmPath, string inserter, string prop, string value)
    {
        string p = tmPath, ins = inserter, pr = prop, val = value;
        AddCustom(list, BaseFile, what + "_key_" + prop + "=" + value, what + ": a keyframe at 1 s with " + prop + " = " + value + " (track motion of " + tmPath + ")",
            delegate(Vegas x)
            {
                object kf = Call(Resolve(x, p), ins, Timecode.FromMilliseconds(1000));
                if (kf == null) throw new Exception("no keyframe made");
                PropertyInfo pi = FindProp(kf, pr);
                if (pi == null) throw new Exception("no property " + pr + " on " + kf.GetType().Name);
                object v = pi.PropertyType.IsEnum ? Enum.Parse(pi.PropertyType, val) : ConvertTo(pi.PropertyType, val);
                pi.SetValue(kf, v, null);
                return "";
            });
    }

    // makes the first double parameter of an OpenFX effect animated with keys at 0 s and 2 s, and sets their interpolation
    static string KeyFirstDouble(Effect fx, string first, string second)
    {
        object ofx = Get(fx, "OFXEffect");
        foreach (object prm in (IEnumerable)Get(ofx, "Parameters"))
        {
            if (prm.GetType().Name != "OFXDoubleParameter") continue;
            Set(prm, "IsAnimated", true);
            double v = Convert.ToDouble(Get(prm, "Value"), CultureInfo.InvariantCulture);
            double lo = Convert.ToDouble(Get(prm, "Min"), CultureInfo.InvariantCulture), hi = Convert.ToDouble(Get(prm, "Max"), CultureInfo.InvariantCulture);
            double alt = v + (hi - lo) * 0.25 <= hi ? v + (hi - lo) * 0.25 : v - (hi - lo) * 0.25;
            Call(prm, "SetValueAtTime", Timecode.FromMilliseconds(0), v);
            Call(prm, "SetValueAtTime", Timecode.FromMilliseconds(2000), alt);
            object keys = Get(prm, "Keyframes");
            object k0 = ((IList)keys)[0], k1 = ((IList)keys)[((IList)keys).Count - 1];
            Set(k0, "Interpolation", ParseEnum("OFXInterpolationType", first));
            Set(k1, "Interpolation", ParseEnum("OFXInterpolationType", second));
            return "parameter " + Get(prm, "Name");
        }
        throw new Exception("the effect has no double parameter");
    }

    // a PCM wave file of silence-like tone with 'channels' channels (16 bit, 48 kHz, 3 s), a different tone per channel
    void WriteWave(string path, int channels)
    {
        int rate = 48000, frames = rate * 3;
        using (FileStream fs = new FileStream(path, FileMode.Create))
        using (BinaryWriter w = new BinaryWriter(fs))
        {
            int bytes = frames * channels * 2;
            w.Write(Encoding.ASCII.GetBytes("RIFF")); w.Write(36 + bytes); w.Write(Encoding.ASCII.GetBytes("WAVEfmt "));
            w.Write(16); w.Write((short)1); w.Write((short)channels); w.Write(rate); w.Write(rate * channels * 2); w.Write((short)(channels * 2)); w.Write((short)16);
            w.Write(Encoding.ASCII.GetBytes("data")); w.Write(bytes);
            for (int i = 0; i < frames; ++i)
                for (int c = 0; c < channels; ++c)
                    w.Write((short)(Math.Sin(2 * Math.PI * (220 * (c + 1)) * i / rate) * 8000));
        }
    }

    // ------------------------------------------------------------------------------------------ the sweep
    static readonly string[] SkipProps = new string[] {
        "Index", "DisplayIndex", "Start", "End", "Track", "ActiveTake", "Group", "Project", "Parent", "Handle", "MediaID", "NodeID",
        "FilePath", "RecordedFilesFolder", "PrerenderedFilesFolder", "RenderPath", "CapturePath", "DefaultCapturePath", "Offset",
    };
    static readonly string[] NestedProps = new string[] { "FadeIn", "FadeOut", "VideoMotion", "TrackMotion" };

    static double[] Curated(string name)
    {
        switch (name)
        {
        case "Width": return new double[] { 1280, 3840 };
        case "Height": return new double[] { 720, 2160 };
        case "FrameRate": return new double[] { 24, 25, 29.97, 60 };
        case "PixelAspectRatio": return new double[] { 0.9091, 1.2121 };
        case "SampleRate": return new double[] { 44100, 96000 };
        case "BitDepth": return new double[] { 16, 24 };
        case "BeatsPerMinute": return new double[] { 90 };
        case "BeatsPerMeasure": return new double[] { 3 };
        case "Volume": return new double[] { 0.5 };
        case "CompositeLevel": return new double[] { 0.5 };
        case "PanX": return new double[] { -1, 0.5 };
        case "PlaybackRate": return new double[] { 0.5, 2 };
        case "UnderSampleRate": return new double[] { 0.5 };
        case "NormalizeGain": return new double[] { 0.5 };
        case "PitchSemis": return new double[] { 2 };
        case "FormantShift": return new double[] { 1 };
        case "LFELowpassFilterCutoffFrequency": return new double[] { 80 };
        case "FirstTrack": return new double[] { 2 };
        case "LensSeparation": return new double[] { 0.5 };
        case "DepthAdjust": return new double[] { 0.5 };
        case "CompositeNestingLevel": return new double[] { 1 };
        case "MonoRenderOrder": case "StereoRenderOrder": case "SurroundRenderOrder": return new double[] { 1 };
        }
        return null;
    }

    static bool Skipped(string enumName)
    {
        return enumName.StartsWith("Internal", StringComparison.OrdinalIgnoreCase) || enumName.StartsWith("Unknown", StringComparison.OrdinalIgnoreCase) ||
               enumName.StartsWith("Invalid", StringComparison.OrdinalIgnoreCase) || enumName.StartsWith("Undefined", StringComparison.OrdinalIgnoreCase) ||
               enumName.StartsWith("Count", StringComparison.OrdinalIgnoreCase);
    }

    // lists a change for every settable property of the object at path; depth > 0 also goes into the nested objects
    void Sweep(List<Variant> list, string baseName, string path, int depth)
    {
        object target;
        try { target = Resolve(vegas, path); }
        catch (Exception ex)
        {
            Note("sweep " + path + ": not reachable (" + Msg(ex) + ")");
            apiLog.WriteLine("## " + path + "  NOT REACHABLE: " + Msg(ex));
            return;
        }
        apiLog.WriteLine("## " + path + "  (" + target.GetType().FullName + ")");
        PropertyInfo[] props = target.GetType().GetProperties(BindingFlags.Public | BindingFlags.Instance);
        foreach (PropertyInfo pi in props)
        {
            if (pi.GetIndexParameters().Length > 0) continue;
            string name = pi.Name;
            Type t = pi.PropertyType;
            MethodInfo setter = pi.GetSetMethod();
            bool writable = pi.CanWrite && setter != null;
            object cur = null;
            string curText = "(not read)";
            bool simple = t.IsPrimitive || t.IsEnum || t == typeof(string) || t == typeof(decimal) || t.Name == "Timecode" || t.Name == "VideoColor";
            if (simple)
            {
                try { cur = pi.GetValue(target, null); curText = Show(cur); }
                catch (Exception ex) { curText = "(error: " + Msg(ex) + ")"; }
            }
            apiLog.WriteLine(path + "\t" + name + "\t" + t.Name + "\t" + (writable ? "settable" : "read-only") + "\t" + curText);
            if (!writable)
            {
                if (depth > 0 && Array.IndexOf(NestedProps, name) >= 0) Sweep(list, baseName, path + "." + name, depth - 1);
                continue;
            }
            if (Array.IndexOf(SkipProps, name) >= 0) continue;
            if (curText.StartsWith("(error")) { apiLog.WriteLine("    (not changed: reading it fails)"); continue; }
            bool isFade = path.EndsWith(".FadeIn") || path.EndsWith(".FadeOut");
            foreach (object value in Values(pi, cur, isFade))
            {
                Variant v = new Variant();
                v.BaseName = baseName;
                v.Path = path;
                v.Prop = name;
                v.Value = value;
                v.Name = ShortPath(path) + "." + name + "=" + Show(value);
                list.Add(v);
            }
        }
    }

    static List<object> Values(PropertyInfo pi, object cur, bool isFade)
    {
        List<object> r = new List<object>();
        Type t = pi.PropertyType;
        string name = pi.Name;
        if (t.IsEnum)
        {
            if (t.IsDefined(typeof(FlagsAttribute), false)) return r;
            foreach (string n in Enum.GetNames(t))
            {
                if (Skipped(n)) continue;
                object val = Enum.Parse(t, n);
                if (cur != null && val.Equals(cur)) continue;
                // two names for one number are one setting
                bool dupe = false;
                foreach (object o in r) if (Convert.ToInt64(o) == Convert.ToInt64(val)) dupe = true;
                if (!dupe && Convert.ToInt64(val) != (cur == null ? long.MinValue : Convert.ToInt64(cur))) r.Add(val);
            }
        }
        else if (t == typeof(bool)) { if (cur != null) r.Add(!(bool)cur); }
        else if (t == typeof(string))
        {
            if (name.IndexOf("Folder", StringComparison.OrdinalIgnoreCase) < 0 && name.IndexOf("Path", StringComparison.OrdinalIgnoreCase) < 0)
                r.Add("Matrix " + name);
        }
        else if (t.Name == "Timecode")
        {
            if (isFade && name == "Length") r.Add(1000.0);
            else if (name == "StartTime") r.Add(5000.0);
            else if (name == "SnapOffset") r.Add(250.0);
        }
        else if (t.IsPrimitive && t != typeof(char) && t != typeof(IntPtr))
        {
            double[] c = Curated(name);
            double now = 0;
            try { now = Convert.ToDouble(cur, CultureInfo.InvariantCulture); } catch (Exception) { }
            if (c != null)
            {
                foreach (double d in c) if (Math.Abs(d - now) > 1e-9) r.Add(d);
            }
            else if (t == typeof(float) || t == typeof(double)) r.Add(now == 0 ? 0.5 : now * 0.5);
            else if (t == typeof(uint) || t == typeof(ulong) || t == typeof(ushort) || t == typeof(byte)) r.Add(now + 1);
            else r.Add(now + 1);
        }
        return r;
    }

    void AddValue(List<Variant> list, string baseName, string path, string prop, object value, string desc)
    {
        Variant v = new Variant();
        v.BaseName = baseName;
        v.Path = path;
        v.Prop = prop;
        v.Value = value;
        v.Desc = desc;
        v.Name = (path.Length == 0 ? "" : ShortPath(path) + ".") + prop + "=" + Show(value);
        list.Add(v);
    }

    void AddCustom(List<Variant> list, string baseName, string name, string desc, Change action)
    {
        Variant v = new Variant();
        v.BaseName = baseName;
        v.Name = name;
        v.Desc = desc;
        v.Custom = action;
        list.Add(v);
    }

    static string ShortPath(string path)
    {
        string s = path.StartsWith("Project.") ? path.Substring(8) : path;
        return s.Replace("BusTracks[", "Bus").Replace("Tracks[", "T").Replace("Events[", "E").Replace("Effects[", "Fx").Replace("Takes[", "Take").Replace("]", "");
    }

    // what the script sees of the API: written once, from the base project
    void AddApiDump()
    {
        apiLog.WriteLine("# Properties of the objects the script changes: path, property, type, settable or read-only, current value in the base project.");
        apiLog.WriteLine("# Only settable ones are changed (each enum value / bool flip / a few numbers per property). Nested objects are listed under their own path.");
        apiLog.WriteLine("# Methods of the project's video / audio / ruler objects follow the sections.");
    }

    // which of the settings Reno wants does this VEGAS's API show at all? (a property whose name has one of the words)
    void AddCoverage()
    {
        apiLog.WriteLine();
        apiLog.WriteLine("# COVERAGE: settings Reno wants to decode, and whether the scripting API has a property for them (searched by name).");
        Cov("project field order", "Project.Video", "FieldOrder");
        Cov("project pixel format / bit depth", "Project.Video", "PixelFormat", "BitDepth");
        Cov("project render quality", "Project.Video", "RenderQuality");
        Cov("project motion blur type", "Project.Video", "MotionBlur");
        Cov("project deinterlace method", "Project.Video", "Deinterlace");
        Cov("project resample / adjust source media", "Project.Video", "esample", "AdjustSource");
        Cov("project view transform / colour management (ACES, OCIO)", "Project.Video", "ViewTransform", "Transform", "Color", "ACES", "OCIO", "Gamma");
        Cov("project 360 output", "Project.Video", "360");
        Cov("project stereo 3D", "Project.Video", "Stereo3D");
        Cov("project master display / HDR", "Project.Video", "HDR", "MasterDisplay", "Nits", "MaxCLL");
        Cov("project output rotation", "Project.Video", "Rotation");
        Cov("project audio sample rate / bit depth / channels", "Project.Audio", "SampleRate", "BitDepth", "Channel", "MasterBusMode");
        Cov("project audio resample quality / LFE", "Project.Audio", "esample", "LFE");
        Cov("ruler format / time format / tempo", "Project.Ruler", "Format", "Beat", "Tempo", "StartTime");
        Cov("summary fields", "Project.Summary", "Title", "Artist", "Engineer", "Copyright", "Comments");
        Cov("preview quality / size", "Project.Preview", "Quality", "Size");
        Cov("video track compositing / motion", "Project.Tracks[0]", "Composite", "Motion", "Nesting", "Parent");
        Cov("video event resample / aspect", "Project.Tracks[0].Events[0]", "esample", "Aspect", "Interlace", "UnderSample");
        Cov("fades and curves", "Project.Tracks[0].Events[0].FadeIn", "Curve", "Length");
        Cov("effect apply before pan/crop", "Project.Tracks[0].Events[0].Effects[0]", "PanCrop");
        Cov("effect apply before pan/crop (managed)", "Project.Tracks[1].Events[0].Effects[0]", "PanCrop");
        Cov("master bus options", "Project.MasterBus", "Volume", "Pan", "Mute", "Solo", "Mode", "Render");
        Cov("audio track mixer options", "Project.Tracks[2]", "Volume", "Pan", "Bus", "Phase", "Mute", "Solo", "Automation");

        apiLog.WriteLine();
        apiLog.WriteLine("# METHODS (public, by name and parameter types) of the main objects");
        string[] objs = new string[] { "Project", "Project.Video", "Project.Audio", "Project.Ruler", "Project.Summary", "Project.Preview",
                                       "Project.Tracks[0]", "Project.Tracks[2]", "Project.Tracks[0].Events[0]", "Project.MasterBus" };
        foreach (string path in objs)
        {
            object o;
            try { o = Resolve(vegas, path); } catch (Exception) { continue; }
            foreach (MethodInfo mi in o.GetType().GetMethods(BindingFlags.Public | BindingFlags.Instance | BindingFlags.DeclaredOnly))
            {
                if (mi.IsSpecialName) continue;
                StringBuilder sb = new StringBuilder();
                foreach (ParameterInfo pa in mi.GetParameters()) { if (sb.Length > 0) sb.Append(", "); sb.Append(pa.ParameterType.Name + " " + pa.Name); }
                apiLog.WriteLine(path + "\t" + mi.Name + "(" + sb + ")\t" + mi.ReturnType.Name);
            }
        }
    }

    void Cov(string label, string path, params string[] words)
    {
        List<string> found = new List<string>();
        try
        {
            object o = Resolve(vegas, path);
            foreach (PropertyInfo pi in o.GetType().GetProperties(BindingFlags.Public | BindingFlags.Instance))
                foreach (string w in words)
                    if (pi.Name.IndexOf(w, StringComparison.OrdinalIgnoreCase) >= 0) { found.Add(pi.Name + (pi.CanWrite ? "" : " (read-only)")); break; }
            foreach (MethodInfo mi in o.GetType().GetMethods(BindingFlags.Public | BindingFlags.Instance))
                foreach (string w in words)
                    if (!mi.IsSpecialName && mi.Name.IndexOf(w, StringComparison.OrdinalIgnoreCase) >= 0 && mi.DeclaringType != typeof(object)) { found.Add(mi.Name + "()"); break; }
        }
        catch (Exception ex) { apiLog.WriteLine("COVERAGE\t" + label + "\t" + path + "\tnot reachable: " + Msg(ex)); return; }
        apiLog.WriteLine("COVERAGE\t" + label + "\t" + path + "\t" + (found.Count == 0 ? "MISSING (no property)" : string.Join(", ", found.ToArray())));
    }

    // ------------------------------------------------------------------------------------------ reflection helpers
    static string Msg(Exception ex)
    {
        while (ex is TargetInvocationException && ex.InnerException != null) ex = ex.InnerException;
        return ex.GetType().Name + ": " + ex.Message;
    }

    static PropertyInfo FindProp(object o, string name)
    {
        if (o == null) return null;
        foreach (PropertyInfo pi in o.GetType().GetProperties(BindingFlags.Public | BindingFlags.Instance))
            if (pi.Name == name && pi.GetIndexParameters().Length == 0) return pi;
        return null;
    }

    static Type TypeOf(object o, string prop)
    {
        PropertyInfo pi = FindProp(o, prop);
        return pi == null ? null : pi.PropertyType;
    }

    static object Get(object o, string name)
    {
        PropertyInfo pi = FindProp(o, name);
        if (pi == null) throw new Exception("no property " + name + " on " + (o == null ? "null" : o.GetType().Name));
        try { return pi.GetValue(o, null); }
        catch (TargetInvocationException tie) { throw new Exception(tie.InnerException != null ? tie.InnerException.Message : tie.Message); }
    }

    static void Set(object o, string name, object value)
    {
        PropertyInfo pi = FindProp(o, name);
        if (pi == null) throw new Exception("no property " + name + " on " + (o == null ? "null" : o.GetType().Name));
        try { pi.SetValue(o, ConvertTo(pi.PropertyType, value), null); }
        catch (TargetInvocationException tie) { throw new Exception(tie.InnerException != null ? tie.InnerException.Message : tie.Message); }
    }

    void TrySet(object o, string name, object value)
    {
        try { Set(o, name, value); }
        catch (Exception ex) { Note("could not set " + name + " in the base project: " + Msg(ex)); }
    }

    // calls a method by name with these arguments (the first overload whose parameters take them)
    static object Call(object o, string name, params object[] args)
    {
        if (o == null) throw new Exception("cannot call " + name + " on null");
        foreach (MethodInfo mi in o.GetType().GetMethods(BindingFlags.Public | BindingFlags.Instance))
        {
            if (mi.Name != name) continue;
            ParameterInfo[] ps = mi.GetParameters();
            if (ps.Length != args.Length) continue;
            object[] conv = new object[args.Length];
            bool fits = true;
            for (int i = 0; i < ps.Length && fits; ++i)
            {
                try { conv[i] = ConvertTo(ps[i].ParameterType, args[i]); }
                catch (Exception) { fits = false; }
            }
            if (!fits) continue;
            try { return mi.Invoke(o, conv); }
            catch (TargetInvocationException tie) { throw new Exception(tie.InnerException != null ? tie.InnerException.Message : tie.Message); }
        }
        throw new Exception("no method " + name + "(" + args.Length + " arguments) on " + o.GetType().Name);
    }

    // new ScriptPortal.Vegas.<typeName>(args)
    static object New(string typeName, params object[] args)
    {
        Type t = typeof(Vegas).Assembly.GetType("ScriptPortal.Vegas." + typeName);
        if (t == null) throw new Exception("this VEGAS has no type " + typeName);
        foreach (ConstructorInfo ci in t.GetConstructors(BindingFlags.Public | BindingFlags.Instance))
        {
            ParameterInfo[] ps = ci.GetParameters();
            if (ps.Length != args.Length) continue;
            object[] conv = new object[args.Length];
            bool fits = true;
            for (int i = 0; i < ps.Length && fits; ++i)
            {
                try { conv[i] = ConvertTo(ps[i].ParameterType, args[i]); }
                catch (Exception) { fits = false; }
            }
            if (!fits) continue;
            try { return ci.Invoke(conv); }
            catch (TargetInvocationException tie) { throw new Exception(tie.InnerException != null ? tie.InnerException.Message : tie.Message); }
        }
        throw new Exception("no constructor of " + typeName + " takes " + args.Length + " arguments");
    }

    static object ParseEnum(string typeName, string value)
    {
        Type t = typeof(Vegas).Assembly.GetType("ScriptPortal.Vegas." + typeName);
        if (t == null || !t.IsEnum) throw new Exception("this VEGAS has no enum " + typeName);
        return Enum.Parse(t, value);
    }

    static object ConvertTo(Type t, object v)
    {
        if (v == null) return null;
        if (t.IsInstanceOfType(v)) return v;
        if (t.IsEnum)
        {
            if (v is string) return Enum.Parse(t, (string)v);
            return Enum.ToObject(t, v);
        }
        if (t.Name == "Timecode") return Timecode.FromMilliseconds(Convert.ToDouble(v, CultureInfo.InvariantCulture));
        return Convert.ChangeType(v, t, CultureInfo.InvariantCulture);
    }

    // "Project.Tracks[0].Events[0].FadeIn" from the Vegas object
    static object Resolve(object root, string path)
    {
        object cur = root;
        if (path == null || path.Length == 0) return cur;
        foreach (string seg in path.Split('.'))
        {
            string name = seg;
            int idx = -1;
            int b = seg.IndexOf('[');
            if (b >= 0)
            {
                name = seg.Substring(0, b);
                idx = int.Parse(seg.Substring(b + 1, seg.Length - b - 2), CultureInfo.InvariantCulture);
            }
            if (name.Length > 0) cur = Get(cur, name);
            if (idx >= 0) cur = Item(cur, idx);
            if (cur == null) throw new Exception(seg + " is null");
        }
        return cur;
    }

    static object Item(object list, int idx)
    {
        IList il = list as IList;
        if (il != null)
        {
            if (idx < 0 || idx >= il.Count) throw new Exception("index " + idx + " of " + il.Count);
            return il[idx];
        }
        PropertyInfo ip = list.GetType().GetProperty("Item", new Type[] { typeof(int) });
        if (ip != null)
        {
            try { return ip.GetValue(list, new object[] { idx }); }
            catch (TargetInvocationException tie) { throw new Exception(tie.InnerException != null ? tie.InnerException.Message : tie.Message); }
        }
        IEnumerable en = list as IEnumerable;
        if (en != null)
        {
            int i = 0;
            foreach (object o in en) { if (i == idx) return o; ++i; }
        }
        throw new Exception("no item " + idx + " in " + list.GetType().Name);
    }

    static int Count(object list)
    {
        ICollection c = list as ICollection;
        if (c != null) return c.Count;
        int n = 0;
        IEnumerable en = list as IEnumerable;
        if (en != null) foreach (object o in en) ++n;
        return n;
    }

    static string Show(object v)
    {
        if (v == null) return "null";
        if (v is double) return ((double)v).ToString("0.######", CultureInfo.InvariantCulture);
        if (v is float) return ((float)v).ToString("0.######", CultureInfo.InvariantCulture);
        if (v is bool) return (bool)v ? "true" : "false";
        return Convert.ToString(v, CultureInfo.InvariantCulture);
    }

    // the first plug-in (looking in folders too) whose name has one of the words, in the order of the words;
    // ofx: true = only OpenFX ones, managed: true = only ones that are not OpenFX (for Rays)
    static PlugInNode FindPlug(PlugInNode root, string[] words, bool ofx, bool managed)
    {
        List<PlugInNode> all = new List<PlugInNode>();
        Collect(root, all, 0);
        foreach (string w in words)
            foreach (PlugInNode n in all)
            {
                if (n.Name == null || n.Name.IndexOf(w, StringComparison.OrdinalIgnoreCase) < 0) continue;
                if (!Fits(n, ofx, managed)) continue;
                return n;
            }
        if (ofx || managed) return null;
        foreach (PlugInNode n in all) return n;
        return null;
    }

    static bool Fits(PlugInNode n, bool ofx, bool managed)
    {
        bool isOfx = false;
        try { isOfx = Convert.ToBoolean(Get(n, "IsOFX")); } catch (Exception) { }
        if (ofx && !isOfx) return false;
        if (managed && isOfx) return false;
        return true;
    }

    static void Collect(PlugInNode node, List<PlugInNode> into, int depth)
    {
        if (node == null || depth > 6) return;
        bool container = depth == 0;
        try { container = Convert.ToBoolean(Get(node, "IsContainer")); } catch (Exception) { }
        if (!container) { if (depth > 0) into.Add(node); return; }
        try { foreach (PlugInNode child in node) Collect(child, into, depth + 1); } catch (Exception) { }
    }

    // ------------------------------------------------------------------------------------------ output
    void Note(string text)
    {
        try { if (manifest != null) manifest.WriteLine("# " + text.Replace("\r", " ").Replace("\n", " ")); } catch (Exception) { }
    }

    void WriteLine(string file, string baseName, string status, string obj, string change, string readback, string note)
    {
        try
        {
            manifest.WriteLine(Clean(file) + "\t" + Clean(baseName) + "\t" + status + "\t" + Clean(obj) + "\t" + Clean(change) + "\t" + Clean(readback) + "\t" + Clean(note));
        }
        catch (Exception) { }
    }

    static string Clean(string s)
    {
        return (s ?? "").Replace("\t", " ").Replace("\r", " ").Replace("\n", " ");
    }

    static string FileSafe(string name)
    {
        StringBuilder sb = new StringBuilder();
        foreach (char c in name)
        {
            if (char.IsLetterOrDigit(c) || c == '.' || c == '=' || c == '_' || c == '-' || c == '+') sb.Append(c);
            else sb.Append('_');
        }
        string s = sb.ToString();
        return s.Length > 110 ? s.Substring(0, 110) : s;
    }

    // ------------------------------------------------------------------------------------------ test media
    // 5 s of a 440 Hz tone, 48 kHz 16-bit stereo
    static void WriteWav(string path)
    {
        int rate = 48000, seconds = 5, channels = 2;
        int frames = rate * seconds;
        int bytes = frames * channels * 2;
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
            for (int i = 0; i < frames; ++i)
            {
                short s = (short)(Math.Sin(2.0 * Math.PI * 440.0 * i / rate) * 8000.0);
                w.Write(s);
                w.Write(s);
            }
        }
    }

    // a 64 x 36 24-bit picture with a colour gradient
    static void WriteBmp(string path)
    {
        int width = 64, height = 36;
        int rowBytes = ((width * 3 + 3) / 4) * 4;
        int size = rowBytes * height;
        using (FileStream fs = new FileStream(path, FileMode.Create))
        using (BinaryWriter w = new BinaryWriter(fs))
        {
            w.Write((byte)'B'); w.Write((byte)'M');
            w.Write(54 + size);
            w.Write(0);
            w.Write(54);
            w.Write(40);
            w.Write(width);
            w.Write(height);
            w.Write((short)1);
            w.Write((short)24);
            w.Write(0);
            w.Write(size);
            w.Write(2835); w.Write(2835);
            w.Write(0); w.Write(0);
            byte[] row = new byte[rowBytes];
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    row[x * 3 + 0] = (byte)(255 * y / (height - 1));
                    row[x * 3 + 1] = (byte)(255 * x / (width - 1));
                    row[x * 3 + 2] = (byte)(128 + 127 * (x ^ y) / 63);
                }
                w.Write(row);
            }
        }
    }
}
