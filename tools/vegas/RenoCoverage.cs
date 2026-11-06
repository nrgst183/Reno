// RenoCoverage: run inside VEGAS Pro 18 (Tools > Scripting > Run Script...). Makes projects that use everything VEGAS's
// scripting reaches, saves each as a .veg next to a .json "manifest" of what VEGAS's own object model reports about it
// AFTER it was made (VEGAS may adjust what the script asked for), and optionally takes a few snapshot pictures and a
// short WAV render. Reno compares the manifests with what it reads from the same .veg (tools/vegcov_compare.py).
//
// Three modes (one window asks which):
//   Coverage   projects for: project properties, tracks (compositing, track motion, buses, groups), media made by the
//              script (WAV, PNG pictures, a PNG sequence, a subclip, a nested project, every generator with its
//              presets), events of every kind (fades with every curve, cross-fades, takes, loops, reversed, velocity,
//              rates, pitch and stretch methods, groups, pan/crop), every transition with presets, every video effect on
//              an event, a track, a media and the video output, every audio effect the machine lists (DirectX, VST,
//              VST3) on an event, a track, a bus and the master, every envelope type, markers, regions, command markers,
//              CD tracks.
//   Fuzz       <count> random projects from <seed>; the same seed and count make the same projects (as long as the same
//              plug-ins are installed).
//   Describe   writes a manifest for every .veg in a folder (VEGAS opens them): for files Reno saved, to check that
//              VEGAS reads what Reno wrote (tools/vegcov_compare.py roundtrip makes such files).
//
// Every item is in its own try/catch and logged: one failure never stops the run. coverage_log.txt has everything, and
// summary.txt the counts at the end. A run can be repeated: projects that have a manifest already are skipped (the
// option can be turned off). If VEGAS itself crashes on a plug-in, start the script again: progress.txt names the
// plug-in it died on and the script skips it from then on.
//
// What to send back: zip the whole output folder (projects, media, manifests, snapshots, logs) and upload it.
// The folder holds only files the script made (the media it needs are written into it).
//
// C# 5, written against ScriptPortal.Vegas 18.0.0.527; no other files are needed.

using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text;
using System.Windows.Forms;
using ScriptPortal.Vegas;

// A set of strings. HashSet lives in System.Core, which VEGAS's script compiler does not reference, so this is a
// Dictionary (mscorlib) underneath.
public class StrSet : IEnumerable<string>
{
    readonly Dictionary<string, bool> d = new Dictionary<string, bool>();
    public bool Add(string s) { if (d.ContainsKey(s)) return false; d[s] = true; return true; }
    public bool Contains(string s) { return d.ContainsKey(s); }
    public int Count { get { return d.Count; } }
    public IEnumerator<string> GetEnumerator() { return d.Keys.GetEnumerator(); }
    IEnumerator IEnumerable.GetEnumerator() { return d.Keys.GetEnumerator(); }
}

public class EntryPoint
{
    public void FromVegas(Vegas vegas)
    {
        RenoCoverage c = new RenoCoverage(vegas);
        c.Run();
    }
}

// ------------------------------------------------------------------------------------------------ JSON
// A small JSON writer: objects keep the order their members were added in.
public class JObj
{
    public List<KeyValuePair<string, object>> Items = new List<KeyValuePair<string, object>>();
    public JObj Add(string key, object value)
    {
        Items.Add(new KeyValuePair<string, object>(key, value));
        return this;
    }
}

public class JArr
{
    public List<object> Items = new List<object>();
    public JArr Add(object value)
    {
        Items.Add(value);
        return this;
    }
    public int Count { get { return Items.Count; } }
}

public static class JW
{
    public static string Write(object v)
    {
        StringBuilder sb = new StringBuilder();
        Value(sb, v, 0);
        sb.Append('\n');
        return sb.ToString();
    }

    static void Indent(StringBuilder sb, int depth)
    {
        sb.Append('\n');
        for (int i = 0; i < depth; i++) sb.Append(' ');
    }

    static void Value(StringBuilder sb, object v, int depth)
    {
        if (v == null) { sb.Append("null"); return; }
        if (v is JObj)
        {
            JObj o = (JObj)v;
            if (o.Items.Count == 0) { sb.Append("{}"); return; }
            sb.Append('{');
            for (int i = 0; i < o.Items.Count; i++)
            {
                if (i > 0) sb.Append(',');
                Indent(sb, depth + 1);
                Str(sb, o.Items[i].Key);
                sb.Append(": ");
                Value(sb, o.Items[i].Value, depth + 1);
            }
            Indent(sb, depth);
            sb.Append('}');
            return;
        }
        if (v is JArr)
        {
            JArr a = (JArr)v;
            if (a.Items.Count == 0) { sb.Append("[]"); return; }
            // arrays of plain numbers or strings stay on one line
            bool flat = true;
            foreach (object x in a.Items) if (x is JObj || x is JArr) { flat = false; break; }
            sb.Append('[');
            for (int i = 0; i < a.Items.Count; i++)
            {
                if (i > 0) sb.Append(flat ? ", " : ",");
                if (!flat) Indent(sb, depth + 1);
                Value(sb, a.Items[i], depth + 1);
            }
            if (!flat) Indent(sb, depth);
            sb.Append(']');
            return;
        }
        if (v is string) { Str(sb, (string)v); return; }
        if (v is bool) { sb.Append(((bool)v) ? "true" : "false"); return; }
        if (v is Enum) { Str(sb, v.ToString()); return; }
        if (v is Timecode) { sb.Append(((Timecode)v).Nanos.ToString(CultureInfo.InvariantCulture)); return; }
        if (v is Guid) { Str(sb, v.ToString().ToUpperInvariant()); return; }
        if (v is VideoColor)
        {
            VideoColor c = (VideoColor)v;
            sb.Append('[').Append(c.R).Append(", ").Append(c.G).Append(", ").Append(c.B).Append(", ").Append(c.A).Append(']');
            return;
        }
        if (v is double) { Num(sb, (double)v); return; }
        if (v is float) { Num(sb, (double)(float)v); return; }
        if (v is int || v is long || v is uint || v is ulong || v is short || v is ushort || v is byte || v is sbyte)
        {
            sb.Append(Convert.ToString(v, CultureInfo.InvariantCulture));
            return;
        }
        Str(sb, v.ToString());
    }

    static void Num(StringBuilder sb, double d)
    {
        if (double.IsNaN(d) || double.IsInfinity(d)) { Str(sb, d.ToString(CultureInfo.InvariantCulture)); return; }
        sb.Append(d.ToString("R", CultureInfo.InvariantCulture));
    }

    static void Str(StringBuilder sb, string s)
    {
        sb.Append('"');
        foreach (char ch in s)
        {
            switch (ch)
            {
                case '"': sb.Append("\\\""); break;
                case '\\': sb.Append("\\\\"); break;
                case '\n': sb.Append("\\n"); break;
                case '\r': sb.Append("\\r"); break;
                case '\t': sb.Append("\\t"); break;
                default:
                    if (ch < ' ') sb.Append("\\u").Append(((int)ch).ToString("x4", CultureInfo.InvariantCulture));
                    else sb.Append(ch);
                    break;
            }
        }
        sb.Append('"');
    }
}

// ------------------------------------------------------------------------------------------------ the script
public class CovOptions
{
    public string Folder = "";
    public string Mode = "coverage";          // coverage, fuzz, describe
    public int Seed = 1, Count = 20;
    public bool VideoFx = true, Transitions = true, Generators = true, AudioFx = true, OtherVideo = false;
    public bool General = true;               // project properties, tracks, events, envelopes, markers
    public bool Snapshots = true, Render = false, SkipDone = true;
    public int Presets = 4;                   // presets tried per plug-in
    public string DescribeFolder = "";
}

public class RenoCoverage
{
    const string Title = "Reno Coverage";
    const int ChunkVideoFx = 6;               // plug-ins per project file
    const int ChunkTransitions = 10;
    const int ChunkGenerators = 6;
    const int ChunkAudioFx = 5;
    static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

    Vegas vegas;
    Project proj;
    CovOptions opt;
    string folder, mediaDir, projDir;
    StreamWriter log, progressFile;
    Form progressForm;
    Label progressLabel;
    bool cancelled;
    int itemsOk, itemsFailed, scenariosDone, scenariosSkipped, scenariosFailed, snapshotsMade, rendersMade;
    List<string> failures = new List<string>();
    // things VEGAS itself refuses (checked, not script mistakes): logged, counted, never a failure
    List<string> refused = new List<string>();
    List<string> manifestErrors;              // the current manifest's "errors" (properties VEGAS refused to give)
    DateTime started;
    StrSet crashed = new StrSet();
    Dictionary<string, string> media = new Dictionary<string, string>();   // name -> path of the files the script made
    // the plug-ins found
    List<PlugInNode> videoFx = new List<PlugInNode>(), transitions = new List<PlugInNode>(), generators = new List<PlugInNode>(), audioFx = new List<PlugInNode>();
    Dictionary<PlugInNode, string> nodeGroup = new Dictionary<PlugInNode, string>();

    public RenoCoverage(Vegas v) { vegas = v; }

    // ---------------------------------------------------------------------------------------- entry
    public void Run()
    {
        try
        {
            string appName = "";
            try { appName = vegas.ShortAppName ?? ""; } catch (Exception) { }
            if (appName.IndexOf("Reno", StringComparison.OrdinalIgnoreCase) >= 0)
            {
                MessageBox.Show("This script needs VEGAS Pro itself (Tools > Scripting > Run Script), not Reno.", Title);
                return;
            }
            bool modified = true;
            try { modified = vegas.Project.IsModified; } catch (Exception) { }
            if (modified)
            {
                MessageBox.Show("The project that is open has unsaved changes.\n\nThis script replaces the open project by test projects, so save it first (Ctrl+S, or close it) and run the script again.",
                                Title, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            opt = AskOptions();
            if (opt == null) return;
            folder = opt.Folder;
            mediaDir = Path.Combine(folder, "media");
            projDir = Path.Combine(folder, "projects");
            Directory.CreateDirectory(folder);
            Directory.CreateDirectory(mediaDir);
            Directory.CreateDirectory(projDir);
            log = new StreamWriter(Path.Combine(folder, "coverage_log.txt"), true, new UTF8Encoding(false));
            log.AutoFlush = true;
            started = DateTime.Now;
            string ver = "";
            try { ver = vegas.LongAppName + " " + vegas.Version; } catch (Exception) { }
            Log("---- started " + started.ToString("s", Inv) + " " + ver + " mode=" + opt.Mode + " seed=" + opt.Seed + " count=" + opt.Count);
            ReadCrashList();
            progressFile = new StreamWriter(Path.Combine(folder, "progress.txt"), true, new UTF8Encoding(false));
            progressFile.AutoFlush = true;
            Progress("Starting...");

            if (opt.Mode == "describe") DescribeFolderMode();
            else
            {
                FindPlugIns();
                if (opt.Mode == "fuzz") FuzzMode();
                else CoverageMode();
            }
        }
        catch (Exception e)
        {
            Log("SCRIPT ERROR: " + e.ToString());
            try { MessageBox.Show("Reno Coverage stopped: " + Short(e) + "\n\nWhat was made so far is in the folder; run the script again to go on.", Title); } catch (Exception) { }
        }
        finally
        {
            try { WriteSummary(); } catch (Exception) { }
            try { if (progressForm != null) progressForm.Close(); } catch (Exception) { }
            try { if (progressFile != null) progressFile.Close(); } catch (Exception) { }
            try { if (log != null) { log.Flush(); log.Close(); } } catch (Exception) { }
        }
    }

    void WriteSummary()
    {
        if (folder == null) return;
        StringBuilder sb = new StringBuilder();
        sb.AppendLine("Reno Coverage " + (cancelled ? "(stopped early)" : "(finished)"));
        sb.AppendLine("started " + started.ToString("s", Inv) + ", took " + ((int)(DateTime.Now - started).TotalMinutes) + " minutes");
        sb.AppendLine("projects made: " + scenariosDone + ", skipped (already there): " + scenariosSkipped + ", failed as a whole: " + scenariosFailed);
        sb.AppendLine("items that worked: " + itemsOk + ", items that failed: " + itemsFailed + ", refused by VEGAS (not failures): " + refused.Count);
        sb.AppendLine("snapshots: " + snapshotsMade + ", WAV renders: " + rendersMade);
        sb.AppendLine("plug-ins found: " + videoFx.Count + " video effects, " + transitions.Count + " transitions, " + generators.Count + " generators, " + audioFx.Count + " audio effects");
        if (crashed.Count > 0) sb.AppendLine("plug-ins VEGAS died on earlier (skipped): " + string.Join(", ", new List<string>(crashed).ToArray()));
        sb.AppendLine();
        sb.AppendLine("failed items (first 200):");
        for (int i = 0; i < failures.Count && i < 200; i++) sb.AppendLine("  " + failures[i]);
        sb.AppendLine();
        sb.AppendLine("not supported by VEGAS (first 200):");
        for (int i = 0; i < refused.Count && i < 200; i++) sb.AppendLine("  " + refused[i]);
        File.WriteAllText(Path.Combine(folder, "summary.txt"), sb.ToString(), new UTF8Encoding(false));
        string text = sb.ToString();
        if (text.Length > 1500) text = text.Substring(0, 1500) + "\n...";
        try { MessageBox.Show(text + "\n\nFolder:\n" + folder + "\n\nZip that folder and upload it.", Title, MessageBoxButtons.OK, MessageBoxIcon.Information); } catch (Exception) { }
    }

    // ---------------------------------------------------------------------------------------- log, progress, items
    void Log(string s)
    {
        try { if (log != null) log.WriteLine(DateTime.Now.ToString("HH:mm:ss", Inv) + " " + s); } catch (Exception) { }
    }

    static string Short(Exception e)
    {
        string m = e.Message ?? e.GetType().Name;
        m = m.Replace("\r", " ").Replace("\n", " ");
        if (m.Length > 300) m = m.Substring(0, 300);
        return e.GetType().Name + ": " + m;
    }

    void Progress(string what)
    {
        Log("> " + what);
        try
        {
            if (progressForm == null)
            {
                progressForm = new Form();
                progressForm.Text = Title;
                progressForm.Width = 560;
                progressForm.Height = 150;
                progressForm.StartPosition = FormStartPosition.CenterScreen;
                progressForm.FormBorderStyle = FormBorderStyle.FixedDialog;
                progressForm.ControlBox = false;
                progressLabel = new Label();
                progressLabel.Left = 12; progressLabel.Top = 12; progressLabel.Width = 520; progressLabel.Height = 50;
                progressForm.Controls.Add(progressLabel);
                Button stop = new Button();
                stop.Text = "Stop";
                stop.Left = 12; stop.Top = 72; stop.Width = 90;
                stop.Click += delegate(object s, EventArgs ea) { cancelled = true; stop.Enabled = false; stop.Text = "Stopping..."; };
                progressForm.Controls.Add(stop);
                progressForm.Show();
            }
            progressLabel.Text = what + "\n" + scenariosDone + " projects made, " + itemsFailed + " items failed";
            System.Windows.Forms.Application.DoEvents();
        }
        catch (Exception) { }
    }

    // Runs one thing; a failure is logged and counted and the run goes on.
    bool Item(string what, Action a)
    {
        if (cancelled) return false;
        try
        {
            a();
            itemsOk++;
            return true;
        }
        catch (Exception e)
        {
            itemsFailed++;
            string line = what + ": " + Short(e);
            Log("FAIL " + line);
            failures.Add(line);
            return false;
        }
    }

    // Like Item, for something VEGAS may legitimately refuse: a refusal (any exception) is written down as
    // "not supported by VEGAS" and is not a failure; the same project then simply does not hold the thing.
    bool Try(string what, Action a)
    {
        if (cancelled) return false;
        try { a(); itemsOk++; return true; }
        catch (Exception e)
        {
            string line = what + ": " + Short(e);
            Log("NOT SUPPORTED BY VEGAS " + line);
            refused.Add(line);
            return false;
        }
    }

    void ReadCrashList()
    {
        // progress.txt has "BEGIN id" before a plug-in is touched and "END id" after; a BEGIN without END is where VEGAS died
        string path = Path.Combine(folder, "progress.txt");
        if (!File.Exists(path)) return;
        Dictionary<string, bool> open = new Dictionary<string, bool>();
        foreach (string line in File.ReadAllLines(path))
        {
            if (line.StartsWith("BEGIN ")) open[line.Substring(6)] = true;
            else if (line.StartsWith("END ")) open.Remove(line.Substring(4));
        }
        foreach (KeyValuePair<string, bool> kv in open) crashed.Add(kv.Key);
        if (crashed.Count > 0) Log("plug-ins VEGAS died on in an earlier run: " + string.Join("; ", new List<string>(crashed).ToArray()));
    }

    void Begin(string id) { try { progressFile.WriteLine("BEGIN " + id); } catch (Exception) { } }
    void End(string id) { try { progressFile.WriteLine("END " + id); } catch (Exception) { } }

    // ---------------------------------------------------------------------------------------- the window
    CovOptions AskOptions()
    {
        CovOptions o = new CovOptions();
        Form f = new Form();
        f.Text = Title;
        f.FormBorderStyle = FormBorderStyle.FixedDialog;
        f.MaximizeBox = false;
        f.MinimizeBox = false;
        f.ShowInTaskbar = false;
        f.StartPosition = FormStartPosition.CenterScreen;
        f.ClientSize = new System.Drawing.Size(600, 470);

        Label l1 = new Label(); l1.Text = "Output folder (the whole folder is what you zip and upload):"; l1.Left = 12; l1.Top = 10; l1.Width = 570; f.Controls.Add(l1);
        TextBox tf = new TextBox(); tf.Left = 12; tf.Top = 32; tf.Width = 480;
        tf.Text = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "Reno Coverage");
        f.Controls.Add(tf);
        Button br = new Button(); br.Text = "Browse..."; br.Left = 500; br.Top = 30; br.Width = 86; f.Controls.Add(br);
        br.Click += delegate(object s, EventArgs ea)
        {
            FolderBrowserDialog d = new FolderBrowserDialog();
            d.Description = "Output folder";
            if (d.ShowDialog() == DialogResult.OK) tf.Text = d.SelectedPath;
        };

        GroupBox gm = new GroupBox(); gm.Text = "What to make"; gm.Left = 12; gm.Top = 66; gm.Width = 574; gm.Height = 120; f.Controls.Add(gm);
        RadioButton rc = new RadioButton(); rc.Text = "Coverage: projects for every kind of object (recommended, run this first)"; rc.Left = 12; rc.Top = 20; rc.Width = 550; rc.Checked = true; gm.Controls.Add(rc);
        RadioButton rf = new RadioButton(); rf.Text = "Fuzz: random projects. Seed:"; rf.Left = 12; rf.Top = 46; rf.Width = 190; gm.Controls.Add(rf);
        NumericUpDown nSeed = new NumericUpDown(); nSeed.Left = 206; nSeed.Top = 44; nSeed.Width = 90; nSeed.Minimum = 0; nSeed.Maximum = 2000000000; nSeed.Value = 1; gm.Controls.Add(nSeed);
        Label lc = new Label(); lc.Text = "Projects:"; lc.Left = 306; lc.Top = 48; lc.Width = 60; gm.Controls.Add(lc);
        NumericUpDown nCount = new NumericUpDown(); nCount.Left = 368; nCount.Top = 44; nCount.Width = 70; nCount.Minimum = 1; nCount.Maximum = 2000; nCount.Value = 20; gm.Controls.Add(nCount);
        RadioButton rd = new RadioButton(); rd.Text = "Describe: a manifest for every .veg in a folder (files Reno saved):"; rd.Left = 12; rd.Top = 74; rd.Width = 400; gm.Controls.Add(rd);
        TextBox td = new TextBox(); td.Left = 30; td.Top = 94; td.Width = 456; gm.Controls.Add(td);
        Button bd = new Button(); bd.Text = "Browse..."; bd.Left = 492; bd.Top = 92; bd.Width = 70; gm.Controls.Add(bd);
        bd.Click += delegate(object s, EventArgs ea)
        {
            FolderBrowserDialog d = new FolderBrowserDialog();
            d.Description = "Folder with the .veg files to describe";
            if (d.ShowDialog() == DialogResult.OK) { td.Text = d.SelectedPath; rd.Checked = true; }
        };

        GroupBox gp = new GroupBox(); gp.Text = "Coverage and fuzz: which parts"; gp.Left = 12; gp.Top = 194; gp.Width = 574; gp.Height = 148; f.Controls.Add(gp);
        CheckBox cGen = Check(gp, "Project properties, tracks, media, events, envelopes, markers", 12, 20, 400, true);
        CheckBox cVfx = Check(gp, "Every video effect (on an event, track, media and video output)", 12, 44, 400, true);
        CheckBox cTr = Check(gp, "Every transition with presets", 12, 68, 400, true);
        CheckBox cGn = Check(gp, "Every generator with presets", 12, 92, 400, true);
        CheckBox cAfx = Check(gp, "Every audio effect VEGAS lists (DirectX, VST, VST3)", 12, 116, 400, true);
        CheckBox cOth = Check(gp, "Include other makers' video plug-ins", 340, 44, 230, false);
        Label lp = new Label(); lp.Text = "Presets per plug-in:"; lp.Left = 340; lp.Top = 74; lp.Width = 120; gp.Controls.Add(lp);
        NumericUpDown nPre = new NumericUpDown(); nPre.Left = 462; nPre.Top = 72; nPre.Width = 60; nPre.Minimum = 0; nPre.Maximum = 40; nPre.Value = 4; gp.Controls.Add(nPre);

        GroupBox gx = new GroupBox(); gx.Text = "Extras"; gx.Left = 12; gx.Top = 350; gx.Width = 574; gx.Height = 72; f.Controls.Add(gx);
        CheckBox cSnap = Check(gx, "A few snapshot pictures per project (slower)", 12, 20, 300, true);
        CheckBox cRen = Check(gx, "A short WAV render of projects with sound (slower)", 12, 44, 330, false);
        CheckBox cSkip = Check(gx, "Skip projects that have a manifest already", 340, 20, 230, true);

        Button ok = new Button(); ok.Text = "Start"; ok.Left = 420; ok.Top = 434; ok.Width = 80; ok.DialogResult = DialogResult.OK; f.Controls.Add(ok);
        Button cancel = new Button(); cancel.Text = "Cancel"; cancel.Left = 506; cancel.Top = 434; cancel.Width = 80; cancel.DialogResult = DialogResult.Cancel; f.Controls.Add(cancel);
        f.AcceptButton = ok;
        f.CancelButton = cancel;
        if (f.ShowDialog() != DialogResult.OK) return null;

        o.Folder = tf.Text.Trim();
        o.Mode = rf.Checked ? "fuzz" : rd.Checked ? "describe" : "coverage";
        o.Seed = (int)nSeed.Value;
        o.Count = (int)nCount.Value;
        o.General = cGen.Checked;
        o.VideoFx = cVfx.Checked;
        o.Transitions = cTr.Checked;
        o.Generators = cGn.Checked;
        o.AudioFx = cAfx.Checked;
        o.OtherVideo = cOth.Checked;
        o.Presets = (int)nPre.Value;
        o.Snapshots = cSnap.Checked;
        o.Render = cRen.Checked;
        o.SkipDone = cSkip.Checked;
        o.DescribeFolder = td.Text.Trim();
        if (o.Folder.Length == 0) return null;
        if (o.Mode == "describe" && (o.DescribeFolder.Length == 0 || !Directory.Exists(o.DescribeFolder)))
        {
            MessageBox.Show("Describe needs a folder with .veg files.", Title);
            return null;
        }
        return o;
    }

    static CheckBox Check(Control parent, string text, int x, int y, int w, bool on)
    {
        CheckBox c = new CheckBox();
        c.Text = text;
        c.Left = x; c.Top = y; c.Width = w;
        c.Checked = on;
        parent.Controls.Add(c);
        return c;
    }

    // ======================================================================================== the manifest
    // What VEGAS's object model says about the open project, in the schema Reno's "reno_cli model" writes (field names are
    // the API's; times are Timecode.Nanos, named ...Nanos; enumerations as their names). Every property is read inside its
    // own try/catch: one VEGAS refuses to give is null here and named in "errors".

    void Err(string where, Exception e)
    {
        if (manifestErrors != null && manifestErrors.Count < 400) manifestErrors.Add(where + ": " + Short(e));
    }

    void Put(JObj o, string w, string name, Func<object> get)
    {
        try { o.Add(name, get()); }
        catch (Exception e) { o.Add(name, null); Err(w + "." + name, e); }
    }

    // The property or field called name. Types in this API hide an inherited property with a different one of the same
    // name (OFX parameters' Value and Keyframes), and Type.GetProperty(name) then throws AmbiguousMatchException (58 + 17
    // "keyframes" errors in the first run), so the most derived declaration is used.
    static object Member(object o, string name)
    {
        if (o == null) return null;
        BindingFlags own = BindingFlags.Public | BindingFlags.Instance | BindingFlags.DeclaredOnly;
        for (Type t = o.GetType(); t != null; t = t.BaseType)
        {
            PropertyInfo p = t.GetProperty(name, own);
            if (p != null && p.GetIndexParameters().Length == 0) return p.GetValue(o, null);
            FieldInfo f = t.GetField(name, own);
            if (f != null) return f.GetValue(o);
        }
        return null;
    }

    static string Norm(string s) { return s ?? ""; }

    static JArr Xy(float x, float y) { return new JArr().Add((double)x).Add((double)y); }

    public JObj DescribeProject(string source)
    {
        manifestErrors = new List<string>();
        Project p = vegas.Project;
        JObj root = new JObj();
        root.Add("schema", "reno-model/1");
        root.Add("source", source);
        try { root.Add("app", vegas.LongAppName + " " + vegas.Version); } catch (Exception) { root.Add("app", "VEGAS"); }
        long nps = 10000000;
        try { nps = Timecode.FromSeconds(1.0).Nanos; } catch (Exception) { }
        root.Add("nanosPerSecond", nps);
        try { root.Add("projectFile", Path.GetFileName(p.FilePath ?? "")); } catch (Exception) { }

        JObj proj = new JObj();
        JObj v = new JObj();
        ProjectVideoProperties pv = p.Video;
        Put(v, "project.video", "width", () => pv.Width);
        Put(v, "project.video", "height", () => pv.Height);
        Put(v, "project.video", "frameRate", () => pv.FrameRate);
        Put(v, "project.video", "fieldOrder", () => pv.FieldOrder);
        Put(v, "project.video", "pixelAspectRatio", () => pv.PixelAspectRatio);
        Put(v, "project.video", "renderQuality", () => pv.RenderQuality);
        Put(v, "project.video", "motionBlurType", () => pv.MotionBlurType);
        Put(v, "project.video", "deinterlaceMethod", () => pv.DeinterlaceMethod);
        Put(v, "project.video", "pixelFormat", () => pv.PixelFormat);
        Put(v, "project.video", "outputRotation", () => pv.OutputRotation);
        Put(v, "project.video", "hdrMode", () => pv.HDRMode);
        Put(v, "project.video", "t360Output", () => pv.T360Output);
        Put(v, "project.video", "stereo3DMode", () => pv.Stereo3DMode);
        proj.Add("video", v);
        JObj a = new JObj();
        ProjectAudioProperties pa = p.Audio;
        Put(a, "project.audio", "sampleRate", () => pa.SampleRate);
        Put(a, "project.audio", "bitDepth", () => pa.BitDepth);
        Put(a, "project.audio", "masterBusMode", () => pa.MasterBusMode);
        Put(a, "project.audio", "resampleQuality", () => pa.ResampleQuality);
        Put(a, "project.audio", "lfeLowpassFilterEnabled", () => pa.LFELowpassFilterEnabled);
        Put(a, "project.audio", "lfeLowpassFilterCutoffFrequency", () => pa.LFELowpassFilterCutoffFrequency);
        Put(a, "project.audio", "lfeLowpassFilterQuality", () => pa.LFELowpassFilterQuality);
        proj.Add("audio", a);
        JObj r = new JObj();
        RulerProperties pr = p.Ruler;
        Put(r, "project.ruler", "format", () => pr.Format);
        Put(r, "project.ruler", "startTimeNanos", () => pr.StartTime);
        Put(r, "project.ruler", "beatsPerMinute", () => pr.BeatsPerMinute);
        Put(r, "project.ruler", "beatsPerMeasure", () => pr.BeatsPerMeasure);
        Put(r, "project.ruler", "beatValue", () => (int)pr.BeatValue);
        proj.Add("ruler", r);
        JObj sm = new JObj();
        SummaryProperties ps = p.Summary;
        Put(sm, "project.summary", "title", () => Norm(ps.Title));
        Put(sm, "project.summary", "artist", () => Norm(ps.Artist));
        Put(sm, "project.summary", "engineer", () => Norm(ps.Engineer));
        Put(sm, "project.summary", "copyright", () => Norm(ps.Copyright));
        Put(sm, "project.summary", "comments", () => Norm(ps.Comments));
        proj.Add("summary", sm);
        JObj cd = new JObj();
        AudioCDProperties pcd = p.AudioCD;
        Put(cd, "project.audioCD", "upc", () => Norm(pcd.UPC));
        Put(cd, "project.audioCD", "firstTrack", () => pcd.FirstTrack);
        proj.Add("audioCD", cd);
        Put(proj, "project", "lengthNanos", () => p.Length);
        root.Add("project", proj);

        // media
        JArr ma = new JArr();
        try
        {
            // MediaPool.Values throws NotSupportedException (the first run listed no media at all); the pool itself enumerates
            foreach (object mo in p.MediaPool)
            {
                Media m = mo as Media;
                if (m == null && mo is DictionaryEntry) m = ((DictionaryEntry)mo).Value as Media;
                if (m != null) ma.Add(DescribeMedia(m, "media[" + ma.Count + "]"));
            }
        }
        catch (Exception e) { Err("media", e); }
        root.Add("media", ma);

        // buses first (tracks refer to them by position)
        List<BusTrack> buses = new List<BusTrack>();
        try { foreach (BusTrack b in p.BusTracks) if (!IsMaster(b)) buses.Add(b); } catch (Exception e) { Err("audioBuses", e); }

        // event groups: events are numbered track by track; a group's number is where it first appears
        Dictionary<TrackEventGroup, int> groupNumber = new Dictionary<TrackEventGroup, int>();
        JArr tracks = new JArr();
        int ti = 0;
        try
        {
            foreach (Track t in p.Tracks)
            {
                tracks.Add(DescribeTrack(t, ti, buses, groupNumber, "tracks[" + ti + "]"));
                ti++;
            }
        }
        catch (Exception e) { Err("tracks", e); }
        root.Add("tracks", tracks);

        JArr ba = new JArr();
        for (int i = 0; i < buses.Count; i++)
        {
            BusTrack b = buses[i];
            string w = "audioBuses[" + i + "]";
            JObj bo = new JObj();
            bo.Add("index", i);
            Put(bo, w, "name", () => Norm(b.Name));
            Put(bo, w, "description", () => Norm(b.Description));
            AudioBusTrack ab = b as AudioBusTrack;
            if (ab != null)
            {
                Put(bo, w, "mute", () => ab.Mute);
                Put(bo, w, "solo", () => ab.Solo);
                Put(bo, w, "panType", () => ab.PanType);
                Put(bo, w, "isFXBus", () => ab is AudioFXBusTrack);
            }
            bo.Add("effects", DescribeEffects(b.Effects, w + ".effects"));
            bo.Add("envelopes", DescribeEnvelopes(b.Envelopes, w + ".envelopes"));
            ba.Add(bo);
        }
        root.Add("audioBuses", ba);

        JObj master = new JObj();
        try
        {
            AudioBusTrack mb = p.MasterBus;
            master.Add("effects", DescribeEffects(mb.Effects, "masterBus.effects"));
            master.Add("envelopes", DescribeEnvelopes(mb.Envelopes, "masterBus.envelopes"));
        }
        catch (Exception e) { Err("masterBus", e); }
        root.Add("masterBus", master);

        JObj vbus = new JObj();
        try
        {
            VideoBusTrack vb = p.VideoBus;
            vbus.Add("effects", DescribeEffects(vb.Effects, "videoBus.effects"));
            vbus.Add("envelopes", DescribeEnvelopes(vb.Envelopes, "videoBus.envelopes"));
            Put(vbus, "videoBus", "topFadeColor", () => vb.TopFadeColor);
            Put(vbus, "videoBus", "bottomFadeColor", () => vb.BottomFadeColor);
            Put(vbus, "videoBus", "bypass", () => vb.Bypass);
        }
        catch (Exception e) { Err("videoBus", e); }
        root.Add("videoBus", vbus);

        JArr mk = new JArr(), rg = new JArr(), cm = new JArr(), cdt = new JArr(), cdi = new JArr();
        try { foreach (Marker m in p.Markers) mk.Add(DescribeMarker(m, "markers[" + mk.Count + "]")); } catch (Exception e) { Err("markers", e); }
        try { foreach (Region m in p.Regions) rg.Add(DescribeMarker(m, "regions[" + rg.Count + "]")); } catch (Exception e) { Err("regions", e); }
        try { foreach (CommandMarker m in p.CommandMarkers) cm.Add(DescribeMarker(m, "commandMarkers[" + cm.Count + "]")); } catch (Exception e) { Err("commandMarkers", e); }
        try { foreach (CDRegion m in p.CDTracks) cdt.Add(DescribeMarker(m, "cdTracks[" + cdt.Count + "]")); } catch (Exception e) { Err("cdTracks", e); }
        try { foreach (CDMarker m in p.CDIndices) cdi.Add(DescribeMarker(m, "cdIndices[" + cdi.Count + "]")); } catch (Exception e) { Err("cdIndices", e); }
        root.Add("markers", mk);
        root.Add("regions", rg);
        root.Add("commandMarkers", cm);
        root.Add("cdTracks", cdt);
        root.Add("cdIndices", cdi);

        // track groups (VEGAS's track grouping: tracks that mute and solo together)
        JArr tg = new JArr();
        try
        {
            foreach (TrackGroup g in p.TrackGroups)
            {
                JObj go = new JObj();
                string w = "trackGroups[" + tg.Count + "]";
                Put(go, w, "name", () => Norm(g.Name));
                JArr members = new JArr();
                foreach (Track t in g) members.Add(t.Index);
                go.Add("tracks", members);
                tg.Add(go);
            }
        }
        catch (Exception e) { Err("trackGroups", e); }
        root.Add("trackGroups", tg);

        JArr errs = new JArr();
        foreach (string s in manifestErrors) errs.Add(s);
        root.Add("errors", errs);
        return root;
    }

    static bool IsMaster(BusTrack b)
    {
        try { return b.IsMaster(); } catch (Exception) { return false; }
    }

    JObj DescribeMedia(Media m, string w)
    {
        JObj o = new JObj();
        string kind = "file";
        try
        {
            if (m is NestedProject) kind = "nested";
            else if (m.IsGenerated()) kind = "generated";
            else if (m.IsImageSequence()) kind = "sequence";
            else if (m.IsSubclip()) kind = "subclip";
        }
        catch (Exception e) { Err(w + ".kind", e); }
        o.Add("kind", kind);
        o.Add("key", MediaKey(m));
        if (kind == "generated")
        {
            try { o.Add("generator", DescribeEffect(m.Generator, w + ".generator")); } catch (Exception e) { Err(w + ".generator", e); }
        }
        else Put(o, w, "fileName", () => Path.GetFileName(m.FilePath ?? ""));
        Put(o, w, "hasVideo", () => m.HasVideo());
        Put(o, w, "hasAudio", () => m.HasAudio());
        Put(o, w, "lengthNanos", () => m.Length);
        Put(o, w, "useCount", () => m.UseCount);
        try
        {
            VideoStream vs = m.GetVideoStreamByIndex(0);
            if (vs != null)
            {
                Put(o, w, "width", () => vs.Width);
                Put(o, w, "height", () => vs.Height);
                Put(o, w, "frameRate", () => vs.FrameRate);
                Put(o, w, "pixelAspectRatio", () => vs.PixelAspectRatio);
                Put(o, w, "fieldOrder", () => vs.FieldOrder);
                Put(o, w, "alphaChannel", () => vs.AlphaChannel);
            }
        }
        catch (Exception e) { Err(w + ".video", e); }
        try
        {
            AudioStream au = m.GetAudioStreamByIndex(0);
            if (au != null)
            {
                Put(o, w, "sampleRate", () => au.SampleRate);
                Put(o, w, "channels", () => au.Channels);
                Put(o, w, "bitDepth", () => au.BitDepth);
            }
        }
        catch (Exception e) { Err(w + ".audio", e); }
        Subclip sc = m as Subclip;
        if (sc != null)
        {
            Put(o, w, "subclipStartNanos", () => sc.Start);
            Put(o, w, "subclipLengthNanos", () => sc.Length);
            Put(o, w, "subclipReversed", () => sc.IsReversed);
        }
        JArr mm = new JArr();
        try { foreach (MediaMarker k in m.Markers) mm.Add(new JObj().Add("positionNanos", k.Position).Add("label", Norm(k.Label))); } catch (Exception e) { Err(w + ".markers", e); }
        o.Add("markers", mm);
        try { o.Add("effects", DescribeEffects(m.Effects, w + ".effects")); } catch (Exception e) { Err(w + ".effects", e); }
        return o;
    }

    // The key that ties an event's take to its media: the file name, or "gen:" and the generator's id
    static string MediaKey(Media m)
    {
        try
        {
            if (m != null && m.IsGenerated())
            {
                Effect g = m.Generator;
                string uid = g != null && g.PlugIn != null ? g.PlugIn.UniqueID : "";
                return "gen:" + (uid ?? "");
            }
            return Path.GetFileName(m.FilePath ?? "");
        }
        catch (Exception) { return ""; }
    }

    JObj DescribeMarker(Marker m, string w)
    {
        JObj o = new JObj();
        Put(o, w, "positionNanos", () => m.Position);
        Put(o, w, "label", () => Norm(m.Label));
        Region rg = m as Region;
        if (rg != null) Put(o, w, "lengthNanos", () => rg.Length);
        CommandMarker cm = m as CommandMarker;
        if (cm != null)
        {
            Put(o, w, "commandType", () => cm.CommandType == null ? "" : cm.CommandType.ToString());
            Put(o, w, "commandParameter", () => Norm(cm.CommandParameter));
            Put(o, w, "comment", () => Norm(cm.Comment));
        }
        return o;
    }

    JObj DescribeTrack(Track t, int index, List<BusTrack> buses, Dictionary<TrackEventGroup, int> groupNumber, string w)
    {
        JObj o = new JObj();
        bool video = false;
        try { video = t.IsVideo(); } catch (Exception e) { Err(w + ".type", e); }
        o.Add("index", index);
        o.Add("type", video ? "video" : "audio");
        Put(o, w, "name", () => Norm(t.Name));
        Put(o, w, "mute", () => t.Mute);
        Put(o, w, "solo", () => t.Solo);
        Put(o, w, "selected", () => t.Selected);
        o.Add("envelopes", DescribeEnvelopes(t.Envelopes, w + ".envelopes"));
        o.Add("effects", DescribeEffects(t.Effects, w + ".effects"));
        if (video)
        {
            VideoTrack vt = t as VideoTrack;
            JObj v = new JObj();
            if (vt != null)
            {
                Put(v, w, "compositeLevel", () => vt.CompositeLevel);
                Put(v, w, "compositeMode", () => vt.CompositeMode);
                Put(v, w, "compositeNestingLevel", () => vt.CompositeNestingLevel);
                Put(v, w, "parentCompositeMode", () => vt.ParentCompositeMode);
                Put(v, w, "isCompositingChild", () => vt.IsCompositingChild);
                Put(v, w, "isCompositingParent", () => vt.IsCompositingParent);
                Put(v, w, "topFadeColor", () => vt.TopFadeColor);
                Put(v, w, "bottomFadeColor", () => vt.BottomFadeColor);
                Put(v, w, "bypassMotionBlur", () => vt.BypassMotionBlur);
                Effect cme = null;
                try { cme = vt.CompositeModeEffect; } catch (Exception) { }
                if (cme != null) v.Add("compositeModeEffect", DescribeEffect(cme, w + ".compositeModeEffect"));
                try
                {
                    TrackMotion tm = vt.TrackMotion;
                    v.Add("motion", DescribeMotion(tm.MotionKeyframes, w + ".motion"));
                    Put(v, w, "shadowEnabled", () => tm.ShadowEnabled);
                    Put(v, w, "glowEnabled", () => tm.GlowEnabled);
                    JArr sk = new JArr();
                    foreach (TrackShadowKeyframe k in tm.ShadowKeyframes) sk.Add(DescribeTrackKey(k, w + ".shadow"));
                    v.Add("shadow", sk);
                    JArr gk = new JArr();
                    foreach (TrackGlowKeyframe k in tm.GlowKeyframes) gk.Add(DescribeTrackKey(k, w + ".glow"));
                    v.Add("glow", gk);
                    Put(v, w, "lensSeparation", () => tm.LensSeparation);
                    Put(v, w, "depthAdjust", () => tm.DepthAdjust);
                }
                catch (Exception e) { Err(w + ".trackMotion", e); }
                try
                {
                    if (vt.IsCompositingParent)
                        v.Add("parentMotion", DescribeMotion(vt.ParentTrackMotion.MotionKeyframes, w + ".parentMotion"));
                }
                catch (Exception e) { Err(w + ".parentMotion", e); }
            }
            o.Add("video", v);
        }
        else
        {
            AudioTrack at = t as AudioTrack;
            JObj a = new JObj();
            if (at != null)
            {
                Put(a, w, "volume", () => at.Volume);
                Put(a, w, "panX", () => at.PanX);
                Put(a, w, "panY", () => at.PanY);
                Put(a, w, "invertPhase", () => at.InvertPhase);
                try
                {
                    BusTrack bt = at.BusTrack;
                    JObj bo = new JObj();
                    bool master = bt == null || IsMaster(bt);
                    bo.Add("isMaster", master);
                    int bi = -1;
                    if (!master) for (int i = 0; i < buses.Count; i++) if (buses[i].Index == bt.Index) bi = i;
                    bo.Add("index", bi);
                    a.Add("busTrack", bo);
                }
                catch (Exception e) { Err(w + ".busTrack", e); }
            }
            o.Add("audio", a);
        }
        JArr evs = new JArr();
        int ei = 0;
        try
        {
            foreach (TrackEvent ev in t.Events)
            {
                evs.Add(DescribeEvent(ev, video, groupNumber, w + ".events[" + ei + "]"));
                ei++;
            }
        }
        catch (Exception e) { Err(w + ".events", e); }
        o.Add("events", evs);
        return o;
    }

    JArr DescribeMotion(TrackMotionKeyframeList keys, string w)
    {
        JArr a = new JArr();
        foreach (TrackMotionKeyframe k in keys)
        {
            JObj o = DescribeTrackKey(k, w);
            Put(o, w, "z", () => k.PositionZ);
            Put(o, w, "depth", () => k.Depth);
            Put(o, w, "rotationX", () => k.RotationX);
            Put(o, w, "rotationY", () => k.RotationY);
            Put(o, w, "rotationOffsetZ", () => k.RotationOffsetZ);
            Put(o, w, "orientationX", () => k.OrientationX);
            Put(o, w, "orientationY", () => k.OrientationY);
            a.Add(o);
        }
        return a;
    }

    JObj DescribeTrackKey(BaseTrackMotionKeyframe k, string w)
    {
        JObj o = new JObj();
        Put(o, w, "positionNanos", () => k.Position);
        Put(o, w, "smoothness", () => k.Smoothness);
        Put(o, w, "type", () => k.Type);
        Put(o, w, "x", () => k.PositionX);
        Put(o, w, "y", () => k.PositionY);
        Put(o, w, "w", () => k.Width);
        Put(o, w, "h", () => k.Height);
        Put(o, w, "rotationZ", () => k.RotationZ);
        Put(o, w, "rotationOffsetX", () => k.RotationOffsetX);
        Put(o, w, "rotationOffsetY", () => k.RotationOffsetY);
        Put(o, w, "orientationZ", () => k.OrientationZ);
        TrackShadowKeyframe sk = k as TrackShadowKeyframe;
        if (sk != null) { Put(o, w, "blur", () => sk.Blur); Put(o, w, "intensity", () => sk.Intensity); Put(o, w, "color", () => sk.Color); }
        TrackGlowKeyframe gk = k as TrackGlowKeyframe;
        if (gk != null) { Put(o, w, "blur", () => gk.Blur); Put(o, w, "intensity", () => gk.Intensity); Put(o, w, "color", () => gk.Color); }
        return o;
    }

    JObj DescribeEvent(TrackEvent ev, bool video, Dictionary<TrackEventGroup, int> groupNumber, string w)
    {
        JObj o = new JObj();
        o.Add("type", video ? "video" : "audio");
        Put(o, w, "name", () => Norm(ev.Name));
        Put(o, w, "startNanos", () => ev.Start);
        Put(o, w, "lengthNanos", () => ev.Length);
        Put(o, w, "playbackRate", () => ev.PlaybackRate);
        Put(o, w, "mute", () => ev.Mute);
        Put(o, w, "locked", () => ev.Locked);
        Put(o, w, "loop", () => ev.Loop);
        Put(o, w, "selected", () => ev.Selected);
        Put(o, w, "snapOffsetNanos", () => ev.SnapOffset);
        try
        {
            int g = -1;
            if (ev.IsGrouped && ev.Group != null)
            {
                if (!groupNumber.TryGetValue(ev.Group, out g)) { g = groupNumber.Count; groupNumber[ev.Group] = g; }
            }
            o.Add("group", g);
        }
        catch (Exception e) { o.Add("group", null); Err(w + ".group", e); }

        JArr takes = new JArr();
        int active = -1, ti = 0;
        try
        {
            foreach (Take tk in ev.Takes)
            {
                JObj to = new JObj();
                string tw = w + ".takes[" + ti + "]";
                try { to.Add("mediaKey", MediaKey(tk.Media)); } catch (Exception e) { Err(tw + ".media", e); }
                Put(to, tw, "name", () => Norm(tk.Name));
                Put(to, tw, "streamIndex", () => tk.StreamIndex);
                Put(to, tw, "offsetNanos", () => tk.Offset);
                Put(to, tw, "lengthNanos", () => tk.Length);
                Put(to, tw, "isActive", () => tk.IsActive);
                try { if (tk.IsActive) active = ti; } catch (Exception) { }
                takes.Add(to);
                ti++;
            }
        }
        catch (Exception e) { Err(w + ".takes", e); }
        o.Add("takes", takes);
        o.Add("activeTake", active);

        o.Add("fadeIn", DescribeFade(ev.FadeIn, w + ".fadeIn", true, video));
        o.Add("fadeOut", DescribeFade(ev.FadeOut, w + ".fadeOut", false, video));

        VideoEvent ve = ev as VideoEvent;
        AudioEvent ae = ev as AudioEvent;
        if (ve != null)
        {
            o.Add("envelopes", DescribeEnvelopes(ve.Envelopes, w + ".envelopes"));
            o.Add("effects", DescribeEffects(ve.Effects, w + ".effects"));
            JObj v = new JObj();
            Put(v, w, "undersampleRate", () => ve.UnderSampleRate);
            Put(v, w, "maintainAspectRatio", () => ve.MaintainAspectRatio);
            Put(v, w, "reduceInterlace", () => ve.ReduceInterlace);
            Put(v, w, "resampleMode", () => ve.ResampleMode);
            try
            {
                VideoMotion vm = ve.VideoMotion;
                Put(v, w, "scaleToFill", () => vm.ScaleToFill);
                JArr keys = new JArr();
                foreach (VideoMotionKeyframe k in vm.Keyframes)
                {
                    JObj ko = new JObj();
                    string kw = w + ".panCrop";
                    Put(ko, kw, "positionNanos", () => k.Position);
                    Put(ko, kw, "smoothness", () => k.Smoothness);
                    Put(ko, kw, "type", () => k.Type);
                    Put(ko, kw, "rotation", () => k.Rotation);
                    Put(ko, kw, "center", () => Xy(k.Center.X, k.Center.Y));
                    Put(ko, kw, "topLeft", () => Xy(k.TopLeft.X, k.TopLeft.Y));
                    Put(ko, kw, "topRight", () => Xy(k.TopRight.X, k.TopRight.Y));
                    Put(ko, kw, "bottomRight", () => Xy(k.BottomRight.X, k.BottomRight.Y));
                    Put(ko, kw, "bottomLeft", () => Xy(k.BottomLeft.X, k.BottomLeft.Y));
                    keys.Add(ko);
                }
                v.Add("motion", keys);
                // the size of the picture the corners are in
                try
                {
                    foreach (Take tk in ev.Takes)
                    {
                        if (!tk.IsActive) continue;
                        VideoStream vs = tk.MediaStream as VideoStream;
                        if (vs != null) { v.Add("mediaWidth", vs.Width); v.Add("mediaHeight", vs.Height); }
                    }
                }
                catch (Exception e) { Err(w + ".mediaSize", e); }
            }
            catch (Exception e) { Err(w + ".videoMotion", e); }
            o.Add("video", v);
        }
        else if (ae != null)
        {
            o.Add("effects", DescribeEffects(ae.Effects, w + ".effects"));
            JObj a = new JObj();
            Put(a, w, "normalize", () => ae.Normalize);
            Put(a, w, "normalizeGain", () => ae.NormalizeGain);
            Put(a, w, "invertPhase", () => ae.InvertPhase);
            Put(a, w, "channels", () => ae.Channels);
            Put(a, w, "method", () => ae.Method);
            Put(a, w, "pitchLock", () => ae.PitchLock);
            Put(a, w, "pitchSemis", () => ae.PitchSemis);
            TimeStretchPitchShift method = TimeStretchPitchShift.None;
            try { method = ae.Method; } catch (Exception) { }
            if (method == TimeStretchPitchShift.Classic) Put(a, w, "classicAttribute", () => ae.ClassicAttribute);
            if (method == TimeStretchPitchShift.Elastique) Put(a, w, "elastiqueAttribute", () => ae.ElastiqueAttribute);
            // formant lock and shift exist for the elastique method only (E_FAIL on any other: 474 errors in the first run)
            if (method == TimeStretchPitchShift.Elastique)
            {
                Put(a, w, "formantLock", () => ae.FormantLock);
                Put(a, w, "formantShift", () => ae.FormantShift);
            }
            o.Add("audio", a);
        }
        return o;
    }

    JObj DescribeFade(Fade f, string w, bool fadeIn, bool video)
    {
        JObj o = new JObj();
        Put(o, w, "lengthNanos", () => f.Length);
        Put(o, w, "curve", () => f.Curve);
        if (fadeIn) Put(o, w, "reciprocalCurve", () => f.ReciprocalCurve);
        Put(o, w, "gain", () => f.Gain);
        if (fadeIn)
        {
            try
            {
                Effect tr = f.Transition as Effect;
                o.Add("transition", tr == null ? null : DescribeEffect(tr, w + ".transition"));
            }
            catch (Exception e) { o.Add("transition", null); Err(w + ".transition", e); }
        }
        try { o.Add("envelopes", DescribeEnvelopes(f.Envelopes, w + ".envelopes")); }
        // a fade that is not a cross-fade has no envelope list: E_FAIL (475 errors in the first run), meaning none
        catch (System.Runtime.InteropServices.COMException) { o.Add("envelopes", new JArr()); }
        catch (Exception e) { Err(w + ".envelopes", e); }
        return o;
    }

    JArr DescribeEnvelopes(Envelopes envs, string w)
    {
        JArr a = new JArr();
        try
        {
            int i = 0;
            foreach (Envelope e in envs)
            {
                JObj o = new JObj();
                string ew = w + "[" + i + "]";
                Put(o, ew, "type", () => e.Type);
                Put(o, ew, "name", () => Norm(e.Name));
                Put(o, ew, "min", () => e.Min);
                Put(o, ew, "max", () => e.Max);
                Put(o, ew, "neutral", () => e.Neutral);
                JArr pts = new JArr();
                try
                {
                    foreach (EnvelopePoint pt in e.Points)
                    {
                        JObj po = new JObj();
                        Put(po, ew, "xNanos", () => pt.X);
                        Put(po, ew, "y", () => pt.Y);
                        Put(po, ew, "curve", () => pt.Curve);
                        pts.Add(po);
                    }
                }
                catch (Exception ex) { Err(ew + ".points", ex); }
                o.Add("points", pts);
                a.Add(o);
                i++;
            }
        }
        catch (Exception ex) { Err(w, ex); }
        return a;
    }

    JArr DescribeEffects(Effects fxs, string w)
    {
        JArr a = new JArr();
        try
        {
            int i = 0;
            foreach (Effect fx in fxs)
            {
                a.Add(DescribeEffect(fx, w + "[" + i + "]"));
                i++;
            }
        }
        catch (Exception e) { Err(w, e); }
        return a;
    }

    JObj DescribeEffect(Effect fx, string w)
    {
        JObj o = new JObj();
        if (fx == null) return o;
        PlugInNode node = null;
        try { node = fx.PlugIn; } catch (Exception e) { Err(w + ".plugIn", e); }
        if (node != null)
        {
            Put(o, w, "name", () => Norm(node.Name));
            Put(o, w, "uniqueId", () => Norm(node.UniqueID));
            Put(o, w, "classId", () => node.ClassID);
            Put(o, w, "group", () => Norm(node.Group));
            Put(o, w, "isAudio", () => node.IsAudio);
            Put(o, w, "isVideo", () => node.IsVideo);
        }
        Put(o, w, "isOFX", () => fx.IsOFX);
        Put(o, w, "bypass", () => fx.Bypass);
        Put(o, w, "applyBeforePanCrop", () => fx.ApplyBeforePanCrop);
        Put(o, w, "preset", () => fx.CurrentPreset == null ? null : fx.CurrentPreset.Name);
        Put(o, w, "description", () => Norm(fx.Description));
        try
        {
            JArr ks = new JArr();
            foreach (Keyframe k in fx.Keyframes)
                ks.Add(new JObj().Add("positionNanos", k.Position).Add("type", k.Type));
            if (ks.Count > 0) o.Add("keyframes", ks);
        }
        catch (Exception e) { Err(w + ".keyframes", e); }
        bool ofx = false;
        try { ofx = fx.IsOFX; } catch (Exception) { }
        if (ofx)
        {
            try { DescribeParams(fx.OFXEffect, o, w + ".params"); }
            catch (Exception e) { Err(w + ".params", e); }
        }
        return o;
    }

    // How many numbers an OpenFX parameter type has; 0 for text, -1 for the types that hold no value.
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

    void DescribeParams(OFXEffect ofx, JObj into, string w)
    {
        if (ofx == null) return;
        JObj ps = new JObj();
        JArr other = new JArr();
        foreach (OFXParameter p in ofx.Parameters)
        {
            string name = "";
            string type = "";
            try { name = p.Name ?? ""; type = p.ParameterType.ToString(); } catch (Exception ex0) { Err(w, ex0); continue; }
            int dims = Dims(type);
            bool persisted = true;
            try { persisted = p.Persisted; } catch (Exception) { }
            if (name.Length == 0 || dims < 0 || !persisted) { other.Add(name + ":" + type + (persisted ? "" : ":not persisted")); continue; }
            JObj e = new JObj();
            string pw = w + "." + name;
            e.Add("type", type);
            object value = null;
            try { value = Member(p, "Value"); } catch (Exception ex) { Err(pw, ex); }
            if (dims == 0) e.Add("value", value as string ?? "");
            else e.Add("value", ValueArray(value, dims));
            try { object d = Member(p, "Default"); if (dims == 0) e.Add("default", d as string ?? ""); else e.Add("default", ValueArray(d, dims)); } catch (Exception) { }
            bool animated = false;
            try { animated = p.IsAnimated; } catch (Exception) { }
            if (animated)
            {
                JArr ks = new JArr();
                try
                {
                    IEnumerable keys = Member(p, "Keyframes") as IEnumerable;
                    if (keys != null)
                        foreach (object k in keys)
                        {
                            OFXKeyframe bk = k as OFXKeyframe;
                            JObj ko = new JObj();
                            if (bk != null) { ko.Add("timeNanos", bk.Time); ko.Add("interpolation", bk.Interpolation); }
                            object kv = Member(k, "Value");
                            if (dims == 0) ko.Add("value", kv as string ?? ""); else ko.Add("value", ValueArray(kv, dims));
                            ks.Add(ko);
                        }
                }
                catch (Exception ex) { Err(pw + ".keyframes", ex); }
                e.Add("keyframes", ks);
            }
            if (type == "Choice")
            {
                try
                {
                    JArr names = new JArr();
                    OFXChoiceParameter cp = p as OFXChoiceParameter;
                    if (cp != null && cp.Choices != null) foreach (OFXChoice c in cp.Choices) names.Add(c.Name ?? "");
                    e.Add("choices", names);
                }
                catch (Exception ex) { Err(pw + ".choices", ex); }
            }
            ps.Add(name, e);
        }
        into.Add("params", ps);
        if (other.Count > 0) into.Add("paramsOther", other);
    }

    // An OpenFX value as numbers: a number, a boolean, a choice (its index), or a structure with R/G/B/A or X/Y/Z members.
    static JArr ValueArray(object v, int dims)
    {
        JArr a = new JArr();
        if (v == null) return a;
        if (v is bool) { a.Add(((bool)v) ? 1.0 : 0.0); return a; }
        if (v is double || v is float || v is int || v is long || v is uint || v is short) { a.Add(Convert.ToDouble(v, Inv)); return a; }
        object index = Member(v, "Index");
        if (index != null && dims == 1) { a.Add(Convert.ToDouble(index, Inv)); return a; }
        string[][] shapes = new string[][] { new string[] { "R", "G", "B", "A" }, new string[] { "X", "Y", "Z" } };
        foreach (string[] shape in shapes)
        {
            JArr t = new JArr();
            for (int i = 0; i < shape.Length && t.Count < dims; i++)
            {
                object x = Member(v, shape[i]);
                if (x == null) break;
                t.Add(Convert.ToDouble(x, Inv));
            }
            if (t.Count == dims) return t;
        }
        return a;
    }

    // ======================================================================================== plug-ins
    static string PlugKey(PlugInNode n) { return (n.Name ?? "") + "|" + (n.UniqueID ?? ""); }

    void Collect(PlugInNode node, string path, List<PlugInNode> into, int depth)
    {
        if (node == null || depth > 8) return;
        foreach (PlugInNode child in node)
        {
            string name = child.Name ?? "";
            string p2 = path.Length == 0 ? name : path + "/" + name;
            bool container = false;
            try { container = child.IsContainer; } catch (Exception) { }
            if (container) Collect(child, p2, into, depth + 1);
            else
            {
                into.Add(child);
                nodeGroup[child] = path;
            }
        }
    }

    static int CompareNodes(PlugInNode a, PlugInNode b)
    {
        int c = string.CompareOrdinal(a.Name ?? "", b.Name ?? "");
        return c != 0 ? c : string.CompareOrdinal(a.UniqueID ?? "", b.UniqueID ?? "");
    }

    bool IsOwnMaker(PlugInNode n)
    {
        bool ofx = false;
        try { ofx = n.IsOFX; } catch (Exception) { }
        if (!ofx) return true;
        string uid = (n.UniqueID ?? "").ToLowerInvariant();
        return uid.Contains("vegascreativesoftware") || uid.Contains("sonycreativesoftware") || uid.Contains("magix") || uid.Contains("sony") || uid.Contains("vegas");
    }

    bool IsVst3(PlugInNode n)
    {
        string g = "";
        nodeGroup.TryGetValue(n, out g);
        string all = ((g ?? "") + "|" + (n.UniqueID ?? "") + "|" + (n.Name ?? "")).ToLowerInvariant();
        return all.Contains("vst3") || all.Contains("vst 3");
    }

    void GatherPlugIns(PlugInNode root, string kind, List<PlugInNode> into, StringBuilder sb, bool ownOnly)
    {
        List<PlugInNode> all = new List<PlugInNode>();
        try { Collect(root, "", all, 0); } catch (Exception e) { Log("listing " + kind + " failed: " + Short(e)); }
        StrSet seen = new StrSet();
        foreach (PlugInNode n in all)
        {
            string key = PlugKey(n);
            if (seen.Contains(key)) continue;
            seen.Add(key);
            string presets = "";
            try
            {
                List<string> names = new List<string>();
                foreach (EffectPreset pr in n.Presets) names.Add(pr.Name);
                presets = string.Join("|", names.ToArray());
            }
            catch (Exception) { }
            bool ofx = false, vst3 = false;
            try { ofx = n.IsOFX; } catch (Exception) { }
            vst3 = IsVst3(n);
            string g = "";
            nodeGroup.TryGetValue(n, out g);
            sb.AppendLine(kind + "\t" + n.Name + "\t" + n.UniqueID + "\t" + n.ClassID + "\t" + (ofx ? "OFX" : "-") + "\t" + (vst3 ? "VST3" : "-") + "\t" + g + "\t" + presets);
            if (ownOnly && !IsOwnMaker(n)) continue;
            if (crashed.Contains(kind + ":" + key)) { Log("skipping " + n.Name + " (VEGAS died on it earlier)"); continue; }
            into.Add(n);
        }
        into.Sort(CompareNodes);
    }

    void FindPlugIns()
    {
        Progress("Looking at the plug-ins VEGAS has...");
        StringBuilder sb = new StringBuilder();
        sb.AppendLine("# kind\tname\tunique id\tclass id\tOFX\tVST3\tgroup\tpresets");
        GatherPlugIns(vegas.VideoFX, "video", videoFx, sb, !opt.OtherVideo);
        GatherPlugIns(vegas.Transitions, "transition", transitions, sb, !opt.OtherVideo);
        GatherPlugIns(vegas.Generators, "generator", generators, sb, !opt.OtherVideo);
        GatherPlugIns(vegas.AudioFX, "audio", audioFx, sb, false);
        File.WriteAllText(Path.Combine(folder, "plugins.txt"), sb.ToString(), new UTF8Encoding(false));
        Log("plug-ins: " + videoFx.Count + " video, " + transitions.Count + " transitions, " + generators.Count + " generators, " + audioFx.Count + " audio");
    }

    static List<string> PresetNames(PlugInNode n, int cap)
    {
        List<string> names = new List<string>();
        try
        {
            foreach (EffectPreset pr in n.Presets)
            {
                if (names.Count >= cap) break;
                if (!string.IsNullOrEmpty(pr.Name)) names.Add(pr.Name);
            }
        }
        catch (Exception) { }
        return names;
    }

    static string SafeName(string s)
    {
        StringBuilder sb = new StringBuilder();
        foreach (char c in s ?? "")
        {
            if (char.IsLetterOrDigit(c)) sb.Append(c);
            else if (sb.Length > 0 && sb[sb.Length - 1] != '_') sb.Append('_');
        }
        string r = sb.ToString().Trim('_');
        if (r.Length > 40) r = r.Substring(0, 40);
        return r.Length == 0 ? "x" : r;
    }

    // ======================================================================================== the media the script writes
    static void WriteWav(string path, int rate, int channels, int bits, double seconds, double freq)
    {
        int frames = (int)(seconds * rate);
        int bytesPer = bits / 8;
        int dataLen = frames * channels * bytesPer;
        bool ext = channels > 2 || bits > 16;
        int fmtLen = ext ? 40 : 16;
        using (FileStream fs = new FileStream(path, FileMode.Create))
        using (BinaryWriter w = new BinaryWriter(fs))
        {
            w.Write(Encoding.ASCII.GetBytes("RIFF"));
            w.Write(4 + 8 + fmtLen + 8 + dataLen);
            w.Write(Encoding.ASCII.GetBytes("WAVE"));
            w.Write(Encoding.ASCII.GetBytes("fmt "));
            w.Write(fmtLen);
            w.Write((ushort)(ext ? 0xFFFE : 1));
            w.Write((short)channels);
            w.Write(rate);
            w.Write(rate * channels * bytesPer);
            w.Write((short)(channels * bytesPer));
            w.Write((short)bits);
            if (ext)
            {
                w.Write((short)22);
                w.Write((short)bits);
                w.Write(channels == 6 ? 0x3F : channels == 2 ? 3 : (1 << channels) - 1);
                w.Write(new byte[] { 1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71 });
            }
            w.Write(Encoding.ASCII.GetBytes("data"));
            w.Write(dataLen);
            for (int i = 0; i < frames; i++)
            {
                double t = (double)i / rate;
                // a slow beat so that the time stretch and the reversed events can be heard to differ
                double env = 0.5 * (0.6 + 0.4 * Math.Sin(2.0 * Math.PI * 1.5 * t));
                for (int c = 0; c < channels; c++)
                {
                    double v = env * Math.Sin(2.0 * Math.PI * freq * (1.0 + 0.25 * c) * t);
                    if (bits == 16) w.Write((short)Math.Round(v * 32767.0));
                    else if (bits == 24)
                    {
                        int s = (int)Math.Round(v * 8388607.0);
                        w.Write((byte)(s & 0xFF)); w.Write((byte)((s >> 8) & 0xFF)); w.Write((byte)((s >> 16) & 0xFF));
                    }
                    else w.Write((int)Math.Round(v * 2147483647.0));
                }
            }
        }
    }

    static void SavePng(string path, int w, int h, int variant, bool alpha)
    {
        using (System.Drawing.Bitmap bmp = new System.Drawing.Bitmap(w, h, System.Drawing.Imaging.PixelFormat.Format32bppArgb))
        {
            using (System.Drawing.Graphics g = System.Drawing.Graphics.FromImage(bmp))
            {
                g.Clear(alpha ? System.Drawing.Color.FromArgb(0, 0, 0, 0) : System.Drawing.Color.FromArgb(255, 20 + 40 * (variant % 5), 60, 120 - 15 * (variant % 5)));
                using (System.Drawing.Drawing2D.LinearGradientBrush br = new System.Drawing.Drawing2D.LinearGradientBrush(
                    new System.Drawing.Rectangle(0, 0, w, h),
                    System.Drawing.Color.FromArgb(alpha ? 200 : 255, 255 - 30 * (variant % 5), 180, 40 * (variant % 5)),
                    System.Drawing.Color.FromArgb(alpha ? 40 : 255, 20, 90 + 20 * (variant % 5), 240), 35.0f + 20 * (variant % 5)))
                {
                    g.FillEllipse(br, w / 10, h / 10, w * 8 / 10, h * 8 / 10);
                }
                using (System.Drawing.Pen pen = new System.Drawing.Pen(System.Drawing.Color.White, Math.Max(2, w / 80)))
                {
                    g.DrawRectangle(pen, w / 20, h / 20, w - w / 10, h - h / 10);
                    g.DrawLine(pen, 0, 0, w, h);
                }
                using (System.Drawing.Font f = new System.Drawing.Font("Arial", Math.Max(8, h / 8), System.Drawing.FontStyle.Bold))
                using (System.Drawing.SolidBrush tb = new System.Drawing.SolidBrush(System.Drawing.Color.Yellow))
                {
                    g.DrawString("RENO " + variant, f, tb, w / 6, h / 3);
                }
            }
            bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png);
        }
    }

    // Writes the media the scenarios use (once; a second run finds them).
    void MakeMediaFiles()
    {
        Item("write media files", delegate
        {
            string[] names = { "wav_mono_44k.wav", "wav_stereo_48k.wav", "wav_stereo_24bit.wav", "wav_surround_6ch.wav" };
            if (!File.Exists(Path.Combine(mediaDir, names[0]))) WriteWav(Path.Combine(mediaDir, names[0]), 44100, 1, 16, 4.0, 330.0);
            if (!File.Exists(Path.Combine(mediaDir, names[1]))) WriteWav(Path.Combine(mediaDir, names[1]), 48000, 2, 16, 8.0, 440.0);
            if (!File.Exists(Path.Combine(mediaDir, names[2]))) WriteWav(Path.Combine(mediaDir, names[2]), 48000, 2, 24, 4.0, 523.0);
            if (!File.Exists(Path.Combine(mediaDir, names[3]))) WriteWav(Path.Combine(mediaDir, names[3]), 48000, 6, 16, 4.0, 262.0);
            if (!File.Exists(Path.Combine(mediaDir, "pic_a.png"))) SavePng(Path.Combine(mediaDir, "pic_a.png"), 640, 360, 1, false);
            if (!File.Exists(Path.Combine(mediaDir, "pic_b.png"))) SavePng(Path.Combine(mediaDir, "pic_b.png"), 640, 360, 3, false);
            if (!File.Exists(Path.Combine(mediaDir, "pic_small.png"))) SavePng(Path.Combine(mediaDir, "pic_small.png"), 160, 90, 2, false);
            if (!File.Exists(Path.Combine(mediaDir, "pic_alpha.png"))) SavePng(Path.Combine(mediaDir, "pic_alpha.png"), 640, 360, 4, true);
            if (!File.Exists(Path.Combine(mediaDir, "pic_square.png"))) SavePng(Path.Combine(mediaDir, "pic_square.png"), 400, 400, 0, false);
            if (!File.Exists(Path.Combine(mediaDir, "pic_hd.png"))) SavePng(Path.Combine(mediaDir, "pic_hd.png"), 1920, 1080, 2, false);
            for (int i = 0; i < 10; i++)
            {
                string p = Path.Combine(mediaDir, "seq_" + i.ToString("0000", Inv) + ".png");
                if (!File.Exists(p)) SavePng(p, 320, 180, i, false);
            }
        });
    }

    // ======================================================================================== building blocks
    Dictionary<string, Media> mediaCache = new Dictionary<string, Media>();

    Media Pic(string file)
    {
        Media m;
        if (mediaCache.TryGetValue(file, out m)) return m;
        m = proj.MediaPool.AddMedia(Path.Combine(mediaDir, file));
        mediaCache[file] = m;
        return m;
    }

    Media Seq()
    {
        Media m;
        if (mediaCache.TryGetValue("seq", out m)) return m;
        m = proj.MediaPool.AddImageSequence(Path.Combine(mediaDir, "seq_0000.png"), 10, 10.0);
        mediaCache["seq"] = m;
        return m;
    }

    static Timecode T(double seconds) { return Timecode.FromSeconds(seconds); }

    VideoTrack AddVTrack(string name)
    {
        VideoTrack t = proj.AddVideoTrack();
        if (name != null) t.Name = name;
        return t;
    }

    AudioTrack AddATrack(string name)
    {
        AudioTrack t = proj.AddAudioTrack();
        if (name != null) t.Name = name;
        return t;
    }

    VideoEvent AddVideo(VideoTrack t, Media m, double start, double len)
    {
        VideoEvent ev = t.AddVideoEvent(T(start), T(len));
        ev.AddTake(m.GetVideoStreamByIndex(0), true);
        return ev;
    }

    AudioEvent AddAudio(AudioTrack t, Media m, double start, double len)
    {
        AudioEvent ev = t.AddAudioEvent(T(start), T(len));
        ev.AddTake(m.GetAudioStreamByIndex(0), true);
        return ev;
    }

    // fades and cross-fades refuse CurveType.None ("Invalid curve type"); only envelope points take it
    static readonly CurveType[] FadeCurves = { CurveType.Linear, CurveType.Fast, CurveType.Slow, CurveType.Smooth, CurveType.Sharp };
    static readonly CurveType[] AllCurves = { CurveType.Linear, CurveType.Fast, CurveType.Slow, CurveType.Smooth, CurveType.Sharp, CurveType.None };
    static readonly VideoKeyframeType[] AllKeyTypes = { VideoKeyframeType.Linear, VideoKeyframeType.Hold, VideoKeyframeType.Slow, VideoKeyframeType.Fast, VideoKeyframeType.Smooth, VideoKeyframeType.Sharp };

    // adds a point to an envelope (changes the one at that time if there is one)
    static void EnvPoint(Envelope env, double t, double y, CurveType c)
    {
        EnvelopePoint have = env.Points.GetPointAtX(T(t));
        if (have != null) { have.Y = y; have.Curve = c; }
        else env.Points.Add(new EnvelopePoint(T(t), y, c));
    }

    // the envelope of this type on an owner, made if it is not there
    static Envelope EnvOf(Envelopes list, EnvelopeType type)
    {
        Envelope env = list.FindByType(type);
        if (env == null)
        {
            env = new Envelope(type);
            list.Add(env);
        }
        return env;
    }

    static TrackMotionKeyframe MotionKey(TrackMotion tm, double t)
    {
        foreach (TrackMotionKeyframe have in tm.MotionKeyframes)
            if (Math.Abs(have.Position.ToMilliseconds() / 1000.0 - t) < 1e-6) return have;
        return tm.InsertMotionKeyframe(T(t));
    }

    static VideoMotionKeyframe PanKey(VideoEvent ve, double t)
    {
        VideoMotionKeyframes list = ve.VideoMotion.Keyframes;
        foreach (VideoMotionKeyframe have in list)
            if (Math.Abs(have.Position.ToMilliseconds() / 1000.0 - t) < 1e-6) return have;
        VideoMotionKeyframe k = new VideoMotionKeyframe(T(t));
        list.Add(k);
        return k;
    }

    // Sets the pan/crop box of a key: centre and size in the media's pixels, rotation in radians.
    static void SetBox(VideoMotionKeyframe k, double cx, double cy, double w, double h, double rot)
    {
        double ca = Math.Cos(rot), sa = Math.Sin(rot);
        double[] dx = { -w / 2, w / 2, w / 2, -w / 2 }, dy = { -h / 2, -h / 2, h / 2, h / 2 };
        VideoMotionVertex[] v = new VideoMotionVertex[4];
        for (int c = 0; c < 4; c++)
            v[c] = new VideoMotionVertex((float)(cx + dx[c] * ca - dy[c] * sa), (float)(cy + dx[c] * sa + dy[c] * ca));
        k.Bounds = new VideoMotionBounds(v[0], v[1], v[2], v[3]);
        k.Center = new VideoMotionVertex((float)cx, (float)cy);
    }

    // ---- OpenFX settings
    static void SetOfx(OFXParameter p, double[] v, Timecode at)
    {
        double a = v.Length > 0 ? v[0] : 0, b = v.Length > 1 ? v[1] : 0, c = v.Length > 2 ? v[2] : 0, d = v.Length > 3 ? v[3] : 1;
        switch (p.ParameterType)
        {
            case OFXParameterType.Double:
                { OFXDoubleParameter q = (OFXDoubleParameter)p; if (at == null) q.Value = a; else q.SetValueAtTime(at, a); break; }
            case OFXParameterType.Integer:
                { OFXIntegerParameter q = (OFXIntegerParameter)p; int i = (int)Math.Round(a); if (at == null) q.Value = i; else q.SetValueAtTime(at, i); break; }
            case OFXParameterType.Boolean:
                { OFXBooleanParameter q = (OFXBooleanParameter)p; bool bv = a != 0; if (at == null) q.Value = bv; else q.SetValueAtTime(at, bv); break; }
            case OFXParameterType.Choice:
                {
                    OFXChoiceParameter q = (OFXChoiceParameter)p;
                    int i = (int)Math.Round(a);
                    if (q.Choices == null || i < 0 || i >= q.Choices.Length) break;
                    if (at == null) q.Value = q.Choices[i]; else q.SetValueAtTime(at, q.Choices[i]);
                    break;
                }
            case OFXParameterType.RGB:
                { OFXRGBParameter q = (OFXRGBParameter)p; OFXColor col = new OFXColor(a, b, c); if (at == null) q.Value = col; else q.SetValueAtTime(at, col); break; }
            case OFXParameterType.RGBA:
                { OFXRGBAParameter q = (OFXRGBAParameter)p; OFXColor col = new OFXColor(a, b, c, d); if (at == null) q.Value = col; else q.SetValueAtTime(at, col); break; }
            case OFXParameterType.Double2D:
                { OFXDouble2DParameter q = (OFXDouble2DParameter)p; OFXDouble2D x = new OFXDouble2D(); x.X = a; x.Y = b; if (at == null) q.Value = x; else q.SetValueAtTime(at, x); break; }
            case OFXParameterType.Double3D:
                { OFXDouble3DParameter q = (OFXDouble3DParameter)p; OFXDouble3D x = new OFXDouble3D(); x.X = a; x.Y = b; x.Z = c; if (at == null) q.Value = x; else q.SetValueAtTime(at, x); break; }
            case OFXParameterType.Integer2D:
                { OFXInteger2DParameter q = (OFXInteger2DParameter)p; OFXInteger2D x = new OFXInteger2D(); x.X = (int)Math.Round(a); x.Y = (int)Math.Round(b); if (at == null) q.Value = x; else q.SetValueAtTime(at, x); break; }
            case OFXParameterType.Integer3D:
                { OFXInteger3DParameter q = (OFXInteger3DParameter)p; OFXInteger3D x = new OFXInteger3D(); x.X = (int)Math.Round(a); x.Y = (int)Math.Round(b); x.Z = (int)Math.Round(c); if (at == null) q.Value = x; else q.SetValueAtTime(at, x); break; }
            case OFXParameterType.String:
                break;
            default:
                break;
        }
    }

    // The range a setting is tried over: the shown range, else the allowed one, else null.
    static bool RangeOf(OFXParameter p, out double lo, out double hi)
    {
        lo = 0; hi = 1;
        try
        {
            OFXDoubleParameter dp = p as OFXDoubleParameter;
            if (dp != null)
            {
                lo = dp.DisplayMin; hi = dp.DisplayMax;
                if (!(hi > lo) || Math.Abs(lo) > 1e6 || Math.Abs(hi) > 1e6) { lo = dp.Min; hi = dp.Max; }
            }
            OFXIntegerParameter ip = p as OFXIntegerParameter;
            if (ip != null)
            {
                lo = ip.DisplayMin; hi = ip.DisplayMax;
                if (!(hi > lo) || Math.Abs(lo) > 1e6 || Math.Abs(hi) > 1e6) { lo = ip.Min; hi = ip.Max; }
            }
            if (dp == null && ip == null) return false;
        }
        catch (Exception) { return false; }
        return hi > lo && Math.Abs(lo) < 1e6 && Math.Abs(hi) < 1e6;
    }

    // Changes up to 'limit' settings of an effect to 'frac' of their range (booleans flip, choices go to the next one,
    // colours and text change); with 'keys' the first number setting also gets keyframes at 0.5 s and 2 s.
    // Returns how many were changed.
    int TweakParams(Effect fx, double frac, bool keys, int limit)
    {
        if (fx == null) return 0;
        OFXEffect ofx = null;
        try
        {
            // the older built-in effects (Deform, Swirl, Wave, Film Grain, Credit Roll, the 3D transitions ...) answer
            // E_FAIL to IsOFX itself: VEGAS has no parameter access for them, so there is nothing to change
            if (!fx.IsOFX) return 0;
            ofx = fx.OFXEffect;
        }
        catch (Exception e)
        {
            string line = "parameters of " + SafeFxName(fx) + ": " + Short(e);
            Log("NOT SUPPORTED BY VEGAS " + line);
            refused.Add(line);
            return 0;
        }
        if (ofx == null) return 0;
        int n = 0;
        bool keyed = false;
        foreach (OFXParameter p in ofx.Parameters)
        {
            if (n >= limit) break;
            try
            {
                if (p.Secret || !p.Enabled || !p.Persisted) continue;
                double lo, hi;
                switch (p.ParameterType)
                {
                    case OFXParameterType.Double:
                    case OFXParameterType.Integer:
                        if (!RangeOf(p, out lo, out hi)) continue;
                        SetOfx(p, new double[] { lo + (hi - lo) * frac }, null);
                        if (keys && !keyed && p.CanAnimate)
                        {
                            p.IsAnimated = true;
                            SetOfx(p, new double[] { lo + (hi - lo) * 0.2 }, T(0.5));
                            SetOfx(p, new double[] { lo + (hi - lo) * 0.8 }, T(2.0));
                            keyed = true;
                        }
                        n++;
                        break;
                    case OFXParameterType.Boolean:
                        { OFXBooleanParameter b = (OFXBooleanParameter)p; SetOfx(p, new double[] { b.Value ? 0 : 1 }, null); n++; break; }
                    case OFXParameterType.Choice:
                        {
                            OFXChoiceParameter c = (OFXChoiceParameter)p;
                            if (c.Choices == null || c.Choices.Length < 2) break;
                            int cur = c.Value == null ? 0 : c.Value.Index;
                            SetOfx(p, new double[] { (cur + 1) % c.Choices.Length }, null);
                            n++;
                            break;
                        }
                    case OFXParameterType.RGB:
                        SetOfx(p, new double[] { 0.9 * frac, 0.4, 1.0 - frac }, null); n++; break;
                    case OFXParameterType.RGBA:
                        SetOfx(p, new double[] { 0.9 * frac, 0.4, 1.0 - frac, 0.75 }, null); n++; break;
                    case OFXParameterType.String:
                        { OFXStringParameter s = (OFXStringParameter)p; s.Value = "Reno " + (int)(frac * 100); n++; break; }
                    default:
                        break;
                }
            }
            catch (Exception e) { Log("  setting '" + SafeParamName(p) + "': " + Short(e)); }
        }
        try { ofx.AllParametersChanged(); } catch (Exception) { }
        return n;
    }

    static string SafeFxName(Effect fx)
    {
        try { return fx.PlugIn.Name; } catch (Exception) { return "an effect"; }
    }

    static string SafeParamName(OFXParameter p)
    {
        try { return p.Name; } catch (Exception) { return "?"; }
    }

    // ======================================================================================== projects
    // Makes a project: a new one with plain settings, 'build' fills it, then it is saved, described, and (as the
    // options say) photographed and rendered.
    void Scenario(string name, Action build)
    {
        if (cancelled) return;
        string vegPath = Path.Combine(projDir, name + ".veg");
        string jsonPath = Path.Combine(projDir, name + ".json");
        if (opt.SkipDone && File.Exists(jsonPath)) { scenariosSkipped++; Log("skip " + name + " (manifest is there)"); return; }
        Progress(name);
        Log("== project " + name);
        mediaCache.Clear();
        bool ok = false;
        try
        {
            if (!vegas.NewProject(false, false)) throw new Exception("NewProject did not make a project");
            proj = vegas.Project;
            proj.Video.Width = 640;
            proj.Video.Height = 360;
            proj.Video.FrameRate = 30.0;
            proj.Video.PixelAspectRatio = 1.0;
            proj.Video.FieldOrder = VideoFieldOrder.ProgressiveScan;
            proj.Audio.SampleRate = 48000;
            try { build(); }
            catch (Exception e) { itemsFailed++; failures.Add(name + " (building): " + Short(e)); Log("FAIL building " + name + ": " + e.ToString()); }
            ok = SaveAndDescribe(name, vegPath, jsonPath);
        }
        catch (Exception e)
        {
            scenariosFailed++;
            failures.Add(name + ": " + Short(e));
            Log("PROJECT FAILED " + name + ": " + e.ToString());
        }
        if (ok) scenariosDone++;
        else scenariosFailed++;
        try { System.Windows.Forms.Application.DoEvents(); } catch (Exception) { }
    }

    bool SaveAndDescribe(string name, string vegPath, string jsonPath)
    {
        bool saved = false;
        try { if (File.Exists(vegPath)) File.Delete(vegPath); saved = vegas.SaveProject(vegPath); }
        catch (Exception e) { Log("save " + name + " failed: " + Short(e)); }
        if (!saved || !File.Exists(vegPath)) { failures.Add(name + ": the project was not saved"); return false; }
        JObj m = null;
        try { m = DescribeProject("vegas"); }
        catch (Exception e) { Log("describe " + name + " failed: " + e.ToString()); failures.Add(name + ": describe failed: " + Short(e)); return false; }
        m.Add("scenario", name);
        File.WriteAllText(jsonPath, JW.Write(m), new UTF8Encoding(false));
        if (opt.Snapshots) Item("snapshots " + name, delegate { TakeSnapshots(name); });
        if (opt.Render) Item("render " + name, delegate { RenderWav(name); });
        return true;
    }

    void TakeSnapshots(string name)
    {
        double len = proj.Length.ToMilliseconds() / 1000.0;
        bool anyVideo = false;
        foreach (Track t in proj.Tracks) if (t.IsVideo() && t.Events.Count > 0) anyVideo = true;
        if (!anyVideo || len <= 0) return;
        string dir = Path.Combine(folder, "snapshots");
        Directory.CreateDirectory(dir);
        PreviewVideoProperties pv = proj.Preview;
        try { pv.RenderQuality = VideoRenderQuality.Best; pv.FullSize = true; pv.PreviewSize = VideoPreviewSize.Full; } catch (Exception) { }
        double[] fractions = { 0.1, 0.5, 0.9 };
        for (int i = 0; i < fractions.Length && !cancelled; i++)
        {
            double at = Math.Max(0.0, Math.Min(len - 0.05, len * fractions[i]));
            string path = Path.Combine(dir, name + "_" + i + ".png");
            Timecode tc = T(at);
            vegas.Transport.CursorPosition = tc;
            RenderStatus st = vegas.SaveSnapshot(path, ImageFileFormat.PNG, tc);
            if (st == RenderStatus.Complete && File.Exists(path)) snapshotsMade++;
            else Log("  snapshot " + i + " of " + name + ": " + st);
        }
    }

    void RenderWav(string name)
    {
        bool anyAudio = false;
        foreach (Track t in proj.Tracks) if (t.IsAudio() && t.Events.Count > 0) anyAudio = true;
        if (!anyAudio) return;
        Renderer r = null;
        foreach (Renderer rr in vegas.Renderers)
        {
            string n = (rr.FileTypeName ?? "") + " " + (rr.Name ?? "");
            if (rr.ClassID == Renderer.CLSID_SfWaveRenderClass || n.IndexOf("Wave", StringComparison.OrdinalIgnoreCase) >= 0) { r = rr; break; }
        }
        if (r == null) throw new Exception("no WAV renderer");
        RenderTemplate tpl = null;
        foreach (RenderTemplate t in r.Templates)
        {
            if (t.IsValid()) { tpl = t; if ((t.Name ?? "").IndexOf("48,000", StringComparison.Ordinal) >= 0 && (t.Name ?? "").IndexOf("Stereo", StringComparison.OrdinalIgnoreCase) >= 0) break; }
        }
        if (tpl == null) throw new Exception("no valid WAV template");
        string dir = Path.Combine(folder, "renders");
        Directory.CreateDirectory(dir);
        string path = Path.Combine(dir, name + ".wav");
        if (File.Exists(path)) File.Delete(path);
        RenderArgs args = new RenderArgs(proj);
        args.OutputFile = path;
        args.RenderTemplate = tpl;
        args.Start = T(0);
        double len = Math.Min(3.0, proj.Length.ToMilliseconds() / 1000.0);
        if (len <= 0) return;
        args.Length = T(len);
        args.UseSelection = false;
        try { args.ShowOpenButtonsOnComplete = false; } catch (Exception) { }
        RenderStatus st = vegas.Render(args);
        if (st == RenderStatus.Complete && File.Exists(path)) rendersMade++;
        else Log("  render of " + name + ": " + st);
    }

    // ======================================================================================== coverage mode
    void CoverageMode()
    {
        MakeMediaFiles();
        if (opt.General)
        {
            PropsScenarios();
            TrackScenarios();
            MediaScenarios();
            VideoEventScenarios();
            AudioEventScenarios();
            EnvelopeScenarios();
            MarkerScenarios();
        }
        if (opt.Transitions) TransitionScenarios();
        if (opt.VideoFx) VideoFxScenarios();
        if (opt.Generators) GeneratorScenarios();
        if (opt.AudioFx) AudioFxScenarios();
        Progress("Done");
    }

    Media Wav(string file) { return Pic(file); }

    // ---------------------------------------------------------------------------------------- project properties
    void Props(string name, Action<Project> set)
    {
        Scenario(name, delegate
        {
            Item(name + ": settings", delegate { set(proj); });
            Item(name + ": content", delegate
            {
                VideoTrack vt = AddVTrack("Video");
                AddVideo(vt, Pic("pic_a.png"), 0, 2);
                AudioTrack at = AddATrack("Audio");
                AddAudio(at, Wav("wav_stereo_48k.wav"), 0, 2);
            });
        });
    }

    void PropsScenarios()
    {
        Props("props_ntsc_hd", delegate(Project p)
        {
            p.Video.Width = 1920; p.Video.Height = 1080; p.Video.FrameRate = 29.97; p.Video.FieldOrder = VideoFieldOrder.ProgressiveScan;
            p.Video.PixelAspectRatio = 1.0; p.Video.RenderQuality = VideoRenderQuality.Best; p.Video.MotionBlurType = MotionBlurType.Gaussian;
            p.Video.PixelFormat = PixelFormat.Int8Bit;
            p.Audio.SampleRate = 48000; p.Audio.BitDepth = 16;
            p.Ruler.Format = RulerFormat.TimeAndFrames;
            p.Summary.Title = "Reno coverage title"; p.Summary.Artist = "Reno artist"; p.Summary.Engineer = "Reno engineer";
            p.Summary.Copyright = "Reno copyright"; p.Summary.Comments = "Reno comments\nsecond line";
        });
        Props("props_pal_dv", delegate(Project p)
        {
            p.Video.Width = 720; p.Video.Height = 576; p.Video.FrameRate = 25.0; p.Video.FieldOrder = VideoFieldOrder.UpperFieldFirst;
            p.Video.PixelAspectRatio = 1.0926; p.Video.RenderQuality = VideoRenderQuality.Draft; p.Video.MotionBlurType = MotionBlurType.Pyramid;
            p.Video.DeinterlaceMethod = VideoDeinterlaceMethod.BlendFields;
            p.Audio.SampleRate = 44100; p.Audio.BitDepth = 16;
            p.Ruler.Format = RulerFormat.SmpteEBU;
        });
        Props("props_ntsc_dv", delegate(Project p)
        {
            p.Video.Width = 720; p.Video.Height = 480; p.Video.FrameRate = 29.97; p.Video.FieldOrder = VideoFieldOrder.LowerFieldFirst;
            p.Video.PixelAspectRatio = 0.9091; p.Video.RenderQuality = VideoRenderQuality.Preview; p.Video.MotionBlurType = MotionBlurType.Box;
            p.Video.DeinterlaceMethod = VideoDeinterlaceMethod.InterpolateFields;
            p.Audio.SampleRate = 48000; p.Audio.BitDepth = 16;
            p.Ruler.Format = RulerFormat.SmpteDrop;
        });
        Props("props_uhd_float", delegate(Project p)
        {
            p.Video.Width = 3840; p.Video.Height = 2160; p.Video.FrameRate = 23.976; p.Video.FieldOrder = VideoFieldOrder.ProgressiveScan;
            p.Video.PixelFormat = PixelFormat.Float32Bit; p.Video.RenderQuality = VideoRenderQuality.Good;
            p.Video.MotionBlurType = MotionBlurType.AsymmetricPyramid;
            p.Audio.SampleRate = 96000; p.Audio.BitDepth = 24; p.Audio.ResampleQuality = AudioResampleQuality.Good;
            p.Ruler.Format = RulerFormat.SmpteFilmSync;
        });
        // VEGAS answers E_UNEXPECTED when the pixel format is set to full-range float on a project that is still
        // 8-bit; it took the width, height and frame rate and refused the rest (so the first run held none of them).
        // Each step is tried by itself, HDR mode first and the plain 32-bit float format as the fallback.
        Props("props_hdr10", delegate(Project p)
        {
            p.Video.Width = 1920; p.Video.Height = 1080; p.Video.FrameRate = 25.0;
            p.Video.MotionBlurType = MotionBlurType.AsymmetricGaussian;
            p.Ruler.Format = RulerFormat.AbsoluteFrames;
            if (!Try("HDR10 full-range float format", delegate { p.Video.PixelFormat = PixelFormat.Float32BitFullRange; }))
                Try("HDR10 32-bit float format", delegate { p.Video.PixelFormat = PixelFormat.Float32Bit; });
            Try("HDR10 mode", delegate { p.Video.HDRMode = HDRMode.HDR10; });
        });
        Props("props_hlg_360", delegate(Project p)
        {
            p.Video.Width = 1280; p.Video.Height = 720; p.Video.FrameRate = 59.94;
            p.Video.MotionBlurType = MotionBlurType.AsymmetricBox;
            p.Ruler.Format = RulerFormat.Samples;
            if (!Try("HLG full-range float format", delegate { p.Video.PixelFormat = PixelFormat.Float32BitFullRange; }))
                Try("HLG 32-bit float format", delegate { p.Video.PixelFormat = PixelFormat.Float32Bit; });
            Try("HLG mode", delegate { p.Video.HDRMode = HDRMode.HLG; });
            Try("360 output", delegate { p.Video.T360Output = T360Output.On; });
        });
        Props("props_surround", delegate(Project p)
        {
            p.Audio.MasterBusMode = AudioBusMode.Surround;
            p.Audio.LFELowpassFilterEnabled = true; p.Audio.LFELowpassFilterCutoffFrequency = 100; p.Audio.LFELowpassFilterQuality = LowPassFilterQuality.Good;
            p.Audio.ResampleQuality = AudioResampleQuality.Preview;
            p.Ruler.Format = RulerFormat.MeasuresAndBeats; p.Ruler.BeatsPerMinute = 97.5; p.Ruler.BeatsPerMeasure = 3; p.Ruler.BeatValue = BeatValue.Eighth;
            p.Ruler.StartTime = T(1.5);
            p.AudioCD.UPC = "012345678905"; p.AudioCD.FirstTrack = 5;
        });
        Props("props_surround_nolfe", delegate(Project p)
        {
            p.Audio.MasterBusMode = AudioBusMode.Surround;
            p.Audio.LFELowpassFilterEnabled = false; p.Audio.ResampleQuality = AudioResampleQuality.Best;
            p.Ruler.Format = RulerFormat.Seconds;
        });
        Props("props_portrait_rotated", delegate(Project p)
        {
            p.Video.Width = 1080; p.Video.Height = 1920; p.Video.FrameRate = 30.0; p.Video.OutputRotation = VideoOutputRotation.QuarterTurnClockwise;
            p.Ruler.Format = RulerFormat.Time;
        });
        Props("props_rotated_180", delegate(Project p) { p.Video.OutputRotation = VideoOutputRotation.Inverted; p.Ruler.Format = RulerFormat.Microseconds; });
        Props("props_rotated_270", delegate(Project p) { p.Video.OutputRotation = VideoOutputRotation.QuarterTurnCounterclockwise; p.Ruler.Format = RulerFormat.Nanoseconds; });
        for (int f = 1; f <= 15; f++)
        {
            RulerFormat rf = (RulerFormat)f;
            Props("props_ruler_" + f.ToString("00", Inv) + "_" + rf, delegate(Project p) { p.Ruler.Format = rf; p.Ruler.StartTime = T(0.5); });
        }
        int[] rates = { 8000, 11025, 22050, 32000, 44100, 48000, 88200, 96000, 192000 };
        foreach (int rate in rates)
        {
            int r2 = rate;
            // 192000 Hz is outside what the mixer accepts (ArgumentException); the other rates are fine
            Props("props_audio_" + r2, delegate(Project p)
            {
                if (r2 > 96000) { Try("sample rate " + r2, delegate { p.Audio.SampleRate = (uint)r2; }); }
                else p.Audio.SampleRate = (uint)r2;
                p.Audio.BitDepth = r2 >= 88200 ? 24u : 16u;
            });
        }
        double[] fpsList = { 15.0, 23.976, 24.0, 25.0, 29.97, 30.0, 47.952, 48.0, 50.0, 59.94, 60.0 };
        foreach (double fps in fpsList)
        {
            double f2 = fps;
            Props("props_fps_" + f2.ToString("0.###", Inv).Replace('.', '_'), delegate(Project p) { p.Video.FrameRate = f2; });
        }
    }

    // ---------------------------------------------------------------------------------------- tracks
    void TrackScenarios()
    {
        Scenario("tracks_video_composite", delegate
        {
            CompositeMode[] modes = { CompositeMode.SrcAlpha, CompositeMode.SrcAlpha3D, CompositeMode.Add, CompositeMode.Subtract, CompositeMode.Multiply, CompositeMode.Cut,
                CompositeMode.Screen, CompositeMode.Overlay, CompositeMode.HardLight, CompositeMode.Dodge, CompositeMode.Burn, CompositeMode.Darken,
                CompositeMode.Lighten, CompositeMode.Difference, CompositeMode.DifferenceSquared };
            for (int i = 0; i < modes.Length; i++)
            {
                CompositeMode cm = modes[i];
                double level = 1.0 - 0.05 * i;
                Item("composite mode " + cm, delegate
                {
                    VideoTrack t = AddVTrack("Mode " + cm);
                    t.SetCompositeMode(cm, false);
                    t.CompositeLevel = (float)level;
                    AddVideo(t, Pic("pic_a.png"), 0, 2);
                });
            }
            // custom compositing modes are effects (the compositing plug-ins VEGAS lists)
            List<PlugInNode> comps = new List<PlugInNode>();
            Item("list compositing plug-ins", delegate { Collect(vegas.VideoCompositeFX, "", comps, 0); });
            comps.Sort(CompareNodes);
            for (int i = 0; i < comps.Count && i < 24; i++)
            {
                PlugInNode node = comps[i];
                Item("compositing plug-in " + node.Name, delegate
                {
                    VideoTrack t = AddVTrack("Custom " + node.Name);
                    t.CompositeModeEffect = new Effect(node);
                    AddVideo(t, Pic("pic_b.png"), 0, 2);
                });
            }
        });

        Scenario("tracks_video_parentchild", delegate
        {
            Media a = Pic("pic_a.png"), b = Pic("pic_b.png");
            VideoTrack parent = null, c1 = null, c2 = null, c3 = null, lone = null;
            Item("parent and children", delegate
            {
                // proj.AddVideoTrack() puts the new track at the TOP, so make them bottom-up: the top-to-bottom order
                // is Parent, Child 1, Child 2, Grandchild, Alone, and a child needs its parent above it
                // (the first run built them the other way round and VEGAS refused every nesting level)
                lone = AddVTrack("Alone");
                AddVideo(lone, a, 0, 3);
                c3 = AddVTrack("Grandchild");
                AddVideo(c3, Pic("pic_small.png"), 0, 3);
                c2 = AddVTrack("Child 2");
                AddVideo(c2, Pic("pic_alpha.png"), 0, 3);
                c1 = AddVTrack("Child 1");
                AddVideo(c1, b, 0, 3);
                parent = AddVTrack("Parent");
                AddVideo(parent, a, 0, 3);
            });
            Item("nesting levels", delegate { c1.CompositeNestingLevel = 1; c2.CompositeNestingLevel = 1; c3.CompositeNestingLevel = 2; });
            Item("parent composite mode", delegate { parent.SetParentCompositeMode(CompositeMode.Multiply, false); });
            Item("child composite modes", delegate { c1.SetCompositeMode(CompositeMode.Screen, false); c2.SetCompositeMode(CompositeMode.Add, false); c2.CompositeLevel = 0.5f; });
            Item("parent motion", delegate
            {
                TrackMotion pm = parent.ParentTrackMotion;
                TrackMotionKeyframe k0 = MotionKey(pm, 0.0);
                k0.PositionX = -40; k0.PositionY = 20; k0.Width = 400; k0.Height = 225; k0.RotationZ = 0.1;
                TrackMotionKeyframe k1 = MotionKey(pm, 2.0);
                k1.PositionX = 40; k1.PositionY = -20; k1.Width = 500; k1.Height = 281; k1.RotationZ = -0.2; k1.Type = VideoKeyframeType.Smooth;
            });
            Item("parent track's own composite", delegate { parent.CompositeLevel = 0.8f; });
        });

        Scenario("tracks_video_motion", delegate
        {
            Media a = Pic("pic_a.png");
            for (int i = 0; i < AllKeyTypes.Length; i++)
            {
                VideoKeyframeType kt = AllKeyTypes[i];
                int idx = i;
                Item("track motion key type " + kt, delegate
                {
                    VideoTrack t = AddVTrack("Motion " + kt);
                    AddVideo(t, a, 0, 4);
                    TrackMotion tm = t.TrackMotion;
                    TrackMotionKeyframe k0 = MotionKey(tm, 0.0);
                    k0.PositionX = -100; k0.PositionY = 50; k0.Width = 320; k0.Height = 180; k0.RotationZ = 0.0; k0.Type = kt; k0.Smoothness = 10 * idx;
                    TrackMotionKeyframe k1 = MotionKey(tm, 2.0);
                    k1.PositionX = 100; k1.PositionY = -50; k1.Width = 480; k1.Height = 270; k1.RotationZ = 0.35; k1.Type = VideoKeyframeType.Linear;
                    TrackMotionKeyframe k2 = MotionKey(tm, 3.5);
                    k2.PositionX = 0; k2.PositionY = 0; k2.Width = 640; k2.Height = 360; k2.RotationZ = -0.5; k2.RotationOffsetX = 12; k2.RotationOffsetY = -8;
                });
            }
            Item("track motion 3D", delegate
            {
                VideoTrack t = AddVTrack("Motion 3D");
                t.SetCompositeMode(CompositeMode.SrcAlpha3D, false);
                AddVideo(t, a, 0, 4);
                TrackMotion tm = t.TrackMotion;
                TrackMotionKeyframe k0 = MotionKey(tm, 0.0);
                k0.PositionZ = 120; k0.RotationX = 0.4; k0.RotationY = -0.3; k0.OrientationX = 0.1; k0.OrientationY = 0.2; k0.OrientationZ = 0.3;
                k0.RotationOffsetZ = 15; k0.Depth = 2.0;
                TrackMotionKeyframe k1 = MotionKey(tm, 2.0);
                k1.PositionZ = -60; k1.RotationX = -0.4; k1.RotationY = 0.6;
            });
            Item("track shadow", delegate
            {
                VideoTrack t = AddVTrack("Shadow");
                AddVideo(t, Pic("pic_alpha.png"), 0, 4);
                TrackMotion tm = t.TrackMotion;
                tm.ShadowEnabled = true;
                TrackShadowKeyframe s0 = tm.InsertShadowKeyframe(T(0.0));
                s0.Blur = 0.3; s0.Intensity = 0.6; s0.Color = new VideoColor(1f, 0f, 0f, 1f); s0.PositionX = 10; s0.PositionY = -10; s0.Width = 640; s0.Height = 360;
                TrackShadowKeyframe s1 = tm.InsertShadowKeyframe(T(2.0));
                s1.Blur = 0.1; s1.Intensity = 0.9; s1.Color = new VideoColor(0f, 0f, 1f, 1f); s1.Type = VideoKeyframeType.Hold;
            });
            Item("track glow", delegate
            {
                VideoTrack t = AddVTrack("Glow");
                AddVideo(t, Pic("pic_alpha.png"), 0, 4);
                TrackMotion tm = t.TrackMotion;
                tm.GlowEnabled = true;
                TrackGlowKeyframe g0 = tm.InsertGlowKeyframe(T(0.0));
                g0.Blur = 0.4; g0.Intensity = 0.7; g0.Color = new VideoColor(0f, 1f, 0f, 1f); g0.Width = 700; g0.Height = 400;
                TrackGlowKeyframe g1 = tm.InsertGlowKeyframe(T(3.0));
                g1.Blur = 0.2; g1.Intensity = 0.3; g1.Color = new VideoColor(1f, 1f, 0f, 0.5f); g1.Type = VideoKeyframeType.Sharp;
            });
        });

        Scenario("tracks_video_misc", delegate
        {
            Media a = Pic("pic_a.png");
            VideoTrack t1 = null, t2 = null, t3 = null;
            Item("tracks", delegate
            {
                t1 = AddVTrack("Fade colours"); AddVideo(t1, a, 0, 2);
                t2 = AddVTrack("Muted"); AddVideo(t2, a, 0, 2);
                t3 = AddVTrack("Solo, no motion blur"); AddVideo(t3, a, 0, 2);
            });
            Item("fade colours", delegate { t1.TopFadeColor = new VideoColor(1f, 0.5f, 0f, 1f); t1.BottomFadeColor = new VideoColor(0f, 0.25f, 1f, 0.5f); });
            Item("mute", delegate { t2.Mute = true; });
            Item("solo", delegate { t3.Solo = true; });
            Item("bypass motion blur", delegate { t3.BypassMotionBlur = true; });
            Item("video output fade colours", delegate { proj.VideoBus.TopFadeColor = new VideoColor(0.2f, 0.4f, 0.6f, 1f); proj.VideoBus.BottomFadeColor = new VideoColor(0.9f, 0.1f, 0.1f, 1f); });
            Item("track group of video tracks", delegate
            {
                t1.Selected = true; t2.Selected = true; t3.Selected = false;
                TrackGroup g = proj.GroupSelectedTracks();
                if (g != null) g.Name = "Video group";
            });
        });

        Scenario("tracks_audio", delegate
        {
            Media w = Wav("wav_stereo_48k.wav");
            AudioTrack[] ts = new AudioTrack[6];
            Item("audio tracks", delegate
            {
                for (int i = 0; i < ts.Length; i++) { ts[i] = AddATrack("Audio " + (i + 1)); AddAudio(ts[i], w, 0, 3); }
            });
            Item("volumes and pans", delegate
            {
                ts[0].Volume = 0.5f; ts[0].PanX = -1.0f;
                ts[1].Volume = 1.5f; ts[1].PanX = 1.0f;
                ts[2].Volume = 0.0f; ts[2].PanX = 0.25f;
                ts[3].Volume = 2.0f; ts[3].PanX = -0.5f;
            });
            Item("pan y and centre (surround values)", delegate { ts[4].PanY = 0.5f; ts[4].PanCenter = 0.5f; });
            Item("invert phase", delegate { ts[1].InvertPhase = true; });
            Item("mute", delegate { ts[2].Mute = true; });
            Item("solo", delegate { ts[3].Solo = true; });
            Item("arm record", delegate { ts[5].ArmRecord = true; });
            AudioBusTrack[] buses = new AudioBusTrack[PanTypes.Length];
            for (int i = 0; i < PanTypes.Length; i++)
            {
                PanType pt = PanTypes[i];
                int idx = i;
                Item("bus with pan type " + pt, delegate
                {
                    AudioBusTrack b = proj.AddAudioBusTrack();
                    b.Description = "Bus " + pt;
                    b.PanType = pt;
                    b.Mute = idx == 2;
                    b.Solo = idx == 3;
                    buses[idx] = b;
                });
            }
            Item("track routing to buses", delegate
            {
                if (buses[0] != null) ts[0].BusTrack = buses[0];
                if (buses[1] != null) ts[1].BusTrack = buses[1];
                if (buses[1] != null) ts[2].BusTrack = buses[1];
            });
            Item("bus render flags", delegate
            {
                if (buses[0] == null) return;
                // surround render flags are for the master bus only (VEGAS throws NotSupportedException on any other bus)
                buses[0].MonoRenderEnabled = true; buses[0].StereoRenderEnabled = false;
                buses[0].MonoRenderOrder = 1; buses[0].StereoRenderOrder = 2;
            });
            Item("master bus render flags", delegate
            {
                proj.MasterBus.SurroundRenderEnabled = true; proj.MasterBus.SurroundRenderOrder = 3;
            });
            Item("track group of audio tracks", delegate
            {
                foreach (Track t in proj.Tracks) t.Selected = false;
                ts[0].Selected = true; ts[1].Selected = true;
                TrackGroup g = proj.GroupSelectedTracks();
                if (g != null) { g.Name = "Audio group"; g.Mute = false; }
            });
            if (audioFx.Count > 0)
                Item("audio FX bus", delegate
                {
                    PlugInNode node = FindAudioNode("Reverb");
                    if (node == null) node = audioFx[0];
                    AudioFXBusTrack fb = proj.AddAudioFXBusTrack(node);
                    fb.Description = "FX bus";
                });
        });
    }

    static readonly PanType[] PanTypes = { PanType.Add, PanType.ConstantPower, PanType.Balance, PanType.Notch3Db, PanType.Notch6Db, PanType.Film };

    PlugInNode FindAudioNode(string contains)
    {
        foreach (PlugInNode n in audioFx) if ((n.Name ?? "").IndexOf(contains, StringComparison.OrdinalIgnoreCase) >= 0) return n;
        return null;
    }

    // ---------------------------------------------------------------------------------------- media
    void MediaScenarios()
    {
        Scenario("media_nested_inner", delegate
        {
            Item("inner project content", delegate
            {
                VideoTrack t = AddVTrack("Inner video");
                AddVideo(t, Pic("pic_b.png"), 0, 2);
                AudioTrack at = AddATrack("Inner audio");
                AddAudio(at, Wav("wav_mono_44k.wav"), 0, 2);
            });
        });

        Scenario("media_kinds", delegate
        {
            Media[] vids = new Media[8];
            Item("pictures", delegate
            {
                vids[0] = Pic("pic_a.png"); vids[1] = Pic("pic_hd.png"); vids[2] = Pic("pic_small.png"); vids[3] = Pic("pic_alpha.png"); vids[4] = Pic("pic_square.png");
            });
            Item("image sequence", delegate { vids[5] = Seq(); });
            Item("nested project", delegate
            {
                string inner = Path.Combine(projDir, "media_nested_inner.veg");
                if (File.Exists(inner)) vids[6] = proj.MediaPool.AddMedia(inner);
            });
            for (int i = 0; i < vids.Length; i++)
            {
                Media m = vids[i];
                int idx = i;
                if (m == null) continue;
                Item("video event for media " + idx, delegate
                {
                    VideoTrack t = AddVTrack("Media " + idx);
                    double len = 2.0;
                    try { if (m.Length.ToMilliseconds() / 1000.0 < len) len = Math.Max(0.5, m.Length.ToMilliseconds() / 1000.0); } catch (Exception) { }
                    if (m.HasVideo()) AddVideo(t, m, 0, len);
                });
            }
            Media[] auds = new Media[6];
            Item("sound files", delegate
            {
                auds[0] = Wav("wav_mono_44k.wav"); auds[1] = Wav("wav_stereo_48k.wav"); auds[2] = Wav("wav_stereo_24bit.wav"); auds[3] = Wav("wav_surround_6ch.wav");
            });
            Item("subclip", delegate
            {
                auds[4] = new Subclip(proj, Path.Combine(mediaDir, "wav_stereo_48k.wav"), T(1.0), T(2.0), false, "Subclip forward");
            });
            Item("reversed subclip", delegate
            {
                auds[5] = new Subclip(proj, Path.Combine(mediaDir, "wav_stereo_48k.wav"), T(1.0), T(2.0), true, "Subclip reversed");
            });
            for (int i = 0; i < auds.Length; i++)
            {
                Media m = auds[i];
                int idx = i;
                if (m == null) continue;
                Item("audio event for media " + idx, delegate
                {
                    AudioTrack t = AddATrack("Sound " + idx);
                    AddAudio(t, m, 0, 2.0);
                });
            }
            Item("media properties", delegate
            {
                Media m = Pic("pic_a.png");
                m.Comment = "Reno comment";
                m.TapeName = "Reno tape";
                m.UseCustomTimecode = true;
                m.TimecodeIn = T(10.0);
                m.RulerFormat = RulerFormat.Seconds;
            });
            Item("media bin", delegate
            {
                MediaBin bin = new MediaBin(proj, "Reno bin");
                proj.MediaPool.RootMediaBin.Add(bin);
                bin.Add(Pic("pic_b.png"));
            });
            // VEGAS refuses to add media markers from a script ("Media markers and regions can not be added");
            // asked once so the log says so, never a failure
            Try("media markers", delegate
            {
                Media m = Wav("wav_stereo_48k.wav");
                m.Markers.Add(new MediaMarker());
            });
            // Media.Length can only be set on generated media (NotSupportedException on a picture file), so the "still" is a generator
            Item("generated media of a chosen length", delegate
            {
                if (generators.Count == 0) throw new Exception("no generator plug-in to make a still from");
                Media m = Media.CreateInstance(proj, generators[0]);
                m.Length = T(7.0);
                VideoTrack t = AddVTrack("Long generated still");
                AddVideo(t, m, 0, 6.0);
            });
        });
    }

    // ---------------------------------------------------------------------------------------- video events
    void VideoEventScenarios()
    {
        Scenario("events_video_fades", delegate
        {
            Media a = Pic("pic_a.png");
            for (int i = 0; i < FadeCurves.Length; i++)
            {
                CurveType cin = FadeCurves[i], cout = FadeCurves[(i + 1) % FadeCurves.Length];
                double fin = 0.3 + 0.1 * i, fout = 0.4 + 0.1 * i;
                Item("fades " + cin + " / " + cout, delegate
                {
                    VideoTrack t = AddVTrack("Fades " + cin);
                    VideoEvent ev = AddVideo(t, a, 0.5, 4.0);
                    ev.FadeIn.Length = T(fin); ev.FadeIn.Curve = cin;
                    ev.FadeOut.Length = T(fout); ev.FadeOut.Curve = cout;
                });
            }
            Item("fade gains", delegate
            {
                VideoTrack t = AddVTrack("Fade gain");
                VideoEvent ev = AddVideo(t, a, 0.0, 4.0);
                ev.FadeIn.Length = T(1.0); ev.FadeIn.Gain = 0.25f;
                ev.FadeOut.Length = T(1.0); ev.FadeOut.Gain = 0.75f;
            });
        });

        Scenario("events_video_crossfades", delegate
        {
            Media a = Pic("pic_a.png"), b = Pic("pic_b.png");
            VideoTrack t = null;
            Item("track", delegate { t = AddVTrack("Cross-fades"); });
            for (int i = 0; i < FadeCurves.Length; i++)
            {
                CurveType cin = FadeCurves[i], crec = FadeCurves[(FadeCurves.Length - 1 - i)];
                double t0 = i * 6.0;
                Item("cross-fade " + cin + " / " + crec, delegate
                {
                    VideoEvent e1 = AddVideo(t, a, t0, 3.0);
                    VideoEvent e2 = AddVideo(t, b, t0 + 2.0, 3.0);
                    e2.FadeIn.Length = T(1.0);
                    e2.FadeIn.Curve = cin;
                    e2.FadeIn.ReciprocalCurve = crec;
                });
            }
            Item("cross-fade shorter than the overlap", delegate
            {
                double t0 = FadeCurves.Length * 6.0;
                VideoEvent e1 = AddVideo(t, a, t0, 3.0);
                VideoEvent e2 = AddVideo(t, b, t0 + 1.0, 3.0);
                e2.FadeIn.Length = T(0.5);
            });
        });

        Scenario("events_video_flags", delegate
        {
            Media a = Pic("pic_a.png");
            VideoTrack t = null;
            Item("track", delegate { t = AddVTrack("Flags"); });
            double at = 0.0;
            Item("named and muted", delegate { VideoEvent ev = AddVideo(t, a, at, 1.0); ev.Name = "Named event"; ev.Mute = true; });
            at += 1.5;
            Item("locked", delegate { VideoEvent ev = AddVideo(t, a, at, 1.0); ev.Locked = true; });
            at += 1.5;
            Item("loop off", delegate { VideoEvent ev = AddVideo(t, a, at, 1.0); ev.Loop = false; });
            at += 1.5;
            Item("selected", delegate { VideoEvent ev = AddVideo(t, a, at, 1.0); ev.Selected = true; });
            at += 1.5;
            Item("snap offset", delegate { VideoEvent ev = AddVideo(t, a, at, 1.0); ev.SnapOffset = T(0.4); });
            at += 1.5;
            Item("aspect ratio off", delegate { VideoEvent ev = AddVideo(t, Pic("pic_square.png"), at, 1.0); ev.MaintainAspectRatio = false; });
            at += 1.5;
            Item("reduce interlace", delegate { VideoEvent ev = AddVideo(t, a, at, 1.0); ev.ReduceInterlace = true; });
            at += 1.5;
            foreach (VideoResampleMode rm in Enum.GetValues(typeof(VideoResampleMode)))
            {
                VideoResampleMode mode = rm;
                double at2 = at;
                Item("resample " + mode, delegate { VideoEvent ev = AddVideo(t, a, at2, 1.0); ev.ResampleMode = mode; });
                at += 1.5;
            }
            double us = at;
            Item("undersample", delegate { VideoEvent ev = AddVideo(t, a, us, 1.0); ev.UnderSampleRate = 0.5; });
            at += 1.5;
            double[] rates = { 0.25, 0.5, 0.75, 1.5, 2.0, 4.0 };
            foreach (double rate in rates)
            {
                double r2 = rate, at3 = at;
                Item("playback rate " + r2, delegate { VideoEvent ev = AddVideo(t, Seq(), at3, 1.0); ev.PlaybackRate = r2; });
                at += 1.5;
            }
            double ar = at;
            Item("adjust playback rate", delegate { VideoEvent ev = AddVideo(t, Seq(), ar, 1.0); ev.AdjustPlaybackRate(2.0, true); });
            at += 1.5;
            double lp = at;
            Item("loop past the media", delegate { VideoEvent ev = AddVideo(t, Seq(), lp, 3.0); ev.Loop = true; });
        });

        Scenario("events_video_takes_groups", delegate
        {
            Media a = Pic("pic_a.png"), b = Pic("pic_b.png"), c = Pic("pic_alpha.png");
            VideoTrack t = null;
            AudioTrack at = null;
            Item("tracks", delegate { t = AddVTrack("Takes"); at = AddATrack("Group sound"); });
            Item("three takes, the second active", delegate
            {
                VideoEvent ev = AddVideo(t, a, 0.0, 2.0);
                ev.AddTake(b.GetVideoStreamByIndex(0), false, "Take B");
                ev.AddTake(c.GetVideoStreamByIndex(0), false, "Take C");
                ev.ActiveTake = ev.Takes[1];
            });
            Item("take offset", delegate
            {
                VideoEvent ev = AddVideo(t, Seq(), 3.0, 0.5);
                ev.ActiveTake.Offset = T(0.3);
            });
            Item("take offset with a playback rate", delegate
            {
                VideoEvent ev = AddVideo(t, Seq(), 4.0, 0.5);
                ev.PlaybackRate = 2.0;
                ev.ActiveTake.Offset = T(0.2);
            });
            Item("video and sound grouped", delegate
            {
                VideoEvent ve = AddVideo(t, a, 5.0, 2.0);
                AudioEvent ae = AddAudio(at, Wav("wav_stereo_48k.wav"), 5.0, 2.0);
                TrackEventGroup g = new TrackEventGroup(proj);
                proj.Groups.Add(g);
                g.Add(ve);
                g.Add(ae);
            });
            Item("two video events grouped", delegate
            {
                VideoEvent e1 = AddVideo(t, a, 8.0, 1.0);
                VideoEvent e2 = AddVideo(t, b, 9.5, 1.0);
                TrackEventGroup g = new TrackEventGroup(proj);
                proj.Groups.Add(g);
                g.Add(e1);
                g.Add(e2);
            });
        });

        Scenario("events_video_velocity", delegate
        {
            Media s = Seq();
            for (int i = 0; i < AllCurves.Length; i++)
            {
                CurveType c = AllCurves[i];
                Item("velocity envelope with curve " + c, delegate
                {
                    VideoTrack t = AddVTrack("Velocity " + c);
                    VideoEvent ev = AddVideo(t, s, 0.0, 3.0);
                    Envelope env = EnvOf(ev.Envelopes, EnvelopeType.Velocity);
                    EnvPoint(env, 0.0, 1.0, c);
                    EnvPoint(env, 1.0, 2.0, c);
                    EnvPoint(env, 2.0, 0.5, c);
                });
            }
            Item("negative velocity (reverse)", delegate
            {
                VideoTrack t = AddVTrack("Velocity reverse");
                VideoEvent ev = AddVideo(t, s, 0.0, 3.0);
                Envelope env = EnvOf(ev.Envelopes, EnvelopeType.Velocity);
                EnvPoint(env, 0.0, -1.0, CurveType.Linear);
                EnvPoint(env, 1.5, 0.0, CurveType.Linear);
                EnvPoint(env, 2.5, 3.0, CurveType.Linear);
            });
            Item("velocity with a playback rate", delegate
            {
                VideoTrack t = AddVTrack("Velocity and rate");
                VideoEvent ev = AddVideo(t, s, 0.0, 3.0);
                ev.PlaybackRate = 2.0;
                Envelope env = EnvOf(ev.Envelopes, EnvelopeType.Velocity);
                EnvPoint(env, 0.0, 0.5, CurveType.Linear);
                EnvPoint(env, 2.0, 1.5, CurveType.Smooth);
            });
            Item("event envelopes the API allows", delegate
            {
                VideoTrack t = AddVTrack("Event envelopes");
                VideoEvent ev = AddVideo(t, Pic("pic_a.png"), 0.0, 3.0);
                foreach (EnvelopeType et in new EnvelopeType[] { EnvelopeType.Composite, EnvelopeType.Velocity, EnvelopeType.Mute, EnvelopeType.Volume, EnvelopeType.Pan, EnvelopeType.FadeToColor })
                {
                    EnvelopeType type = et;
                    bool can = false;
                    try { can = ev.CanAddEnvelope(type); } catch (Exception) { }
                    Log("  video event can add " + type + ": " + can);
                    if (!can) continue;
                    Item("event envelope " + type, delegate
                    {
                        Envelope env = EnvOf(ev.Envelopes, type);
                        EnvPoint(env, 0.0, env.Neutral, CurveType.Linear);
                        EnvPoint(env, 1.0, env.Min + (env.Max - env.Min) * 0.25, CurveType.Smooth);
                        EnvPoint(env, 2.0, env.Min + (env.Max - env.Min) * 0.75, CurveType.Fast);
                    });
                }
            });
        });

        Scenario("events_video_pancrop", delegate
        {
            Media a = Pic("pic_a.png");
            for (int i = 0; i < AllKeyTypes.Length; i++)
            {
                VideoKeyframeType kt = AllKeyTypes[i];
                int idx = i;
                Item("pan/crop key type " + kt, delegate
                {
                    VideoTrack t = AddVTrack("Pan/crop " + kt);
                    VideoEvent ev = AddVideo(t, a, 0.0, 4.0);
                    VideoMotionKeyframe k0 = PanKey(ev, 0.0);
                    SetBox(k0, 320, 180, 640, 360, 0.0);
                    k0.Type = kt; k0.Smoothness = 10 * idx;
                    VideoMotionKeyframe k1 = PanKey(ev, 2.0);
                    SetBox(k1, 200, 120, 320, 180, 0.0);
                    VideoMotionKeyframe k2 = PanKey(ev, 3.5);
                    SetBox(k2, 400, 250, 400, 225, 0.0);
                });
            }
            Item("pan/crop with rotation", delegate
            {
                VideoTrack t = AddVTrack("Pan/crop rotated");
                VideoEvent ev = AddVideo(t, a, 0.0, 3.0);
                VideoMotionKeyframe k0 = PanKey(ev, 0.0);
                SetBox(k0, 320, 180, 640, 360, 0.0);
                VideoMotionKeyframe k1 = PanKey(ev, 2.0);
                SetBox(k1, 300, 200, 400, 225, 0.5);
                k1.Rotation = 0.5;
            });
            Item("pan/crop on a square picture, aspect kept and not", delegate
            {
                VideoTrack t = AddVTrack("Pan/crop square");
                VideoEvent ev = AddVideo(t, Pic("pic_square.png"), 0.0, 2.0);
                ev.MaintainAspectRatio = false;
                VideoMotionKeyframe k0 = PanKey(ev, 0.0);
                SetBox(k0, 200, 200, 300, 100, 0.0);
                VideoTrack t2 = AddVTrack("Scale to fill");
                VideoEvent ev2 = AddVideo(t2, Pic("pic_square.png"), 0.0, 2.0);
                ev2.VideoMotion.ScaleToFill = true;
            });
            Item("pan/crop move and scale helpers", delegate
            {
                VideoTrack t = AddVTrack("Pan/crop helpers");
                VideoEvent ev = AddVideo(t, a, 0.0, 2.0);
                VideoMotionKeyframe k0 = PanKey(ev, 0.0);
                k0.MoveBy(new VideoMotionVertex(20f, -10f));
                k0.ScaleBy(new VideoMotionVertex(0.5f, 0.5f));
                k0.RotateBy(0.2);
            });
        });
    }

    // ---------------------------------------------------------------------------------------- audio events
    void AudioEventScenarios()
    {
        Scenario("events_audio_fades", delegate
        {
            Media w = Wav("wav_stereo_48k.wav");
            for (int i = 0; i < FadeCurves.Length; i++)
            {
                CurveType cin = FadeCurves[i], cout = FadeCurves[(i + 2) % FadeCurves.Length];
                double fin = 0.3 + 0.1 * i, fout = 0.4 + 0.1 * i;
                Item("audio fades " + cin + " / " + cout, delegate
                {
                    AudioTrack t = AddATrack("Fades " + cin);
                    AudioEvent ev = AddAudio(t, w, 0.5, 4.0);
                    ev.FadeIn.Length = T(fin); ev.FadeIn.Curve = cin;
                    ev.FadeOut.Length = T(fout); ev.FadeOut.Curve = cout;
                });
            }
            Item("audio fade gains", delegate
            {
                AudioTrack t = AddATrack("Fade gain");
                AudioEvent ev = AddAudio(t, w, 0.0, 4.0);
                ev.FadeIn.Length = T(1.0); ev.FadeIn.Gain = 0.25f;
                ev.FadeOut.Length = T(1.0); ev.FadeOut.Gain = 0.5f;
            });
        });

        Scenario("events_audio_crossfades", delegate
        {
            Media w = Wav("wav_stereo_48k.wav");
            AudioTrack t = null;
            Item("track", delegate { t = AddATrack("Cross-fades"); });
            for (int i = 0; i < FadeCurves.Length; i++)
            {
                CurveType cin = FadeCurves[i], crec = FadeCurves[(FadeCurves.Length - 1 - i)];
                double t0 = i * 4.0;
                Item("audio cross-fade " + cin + " / " + crec, delegate
                {
                    AudioEvent e1 = AddAudio(t, w, t0, 2.0);
                    AudioEvent e2 = AddAudio(t, w, t0 + 1.5, 2.0);
                    e2.FadeIn.Length = T(0.5);
                    e2.FadeIn.Curve = cin;
                    e2.FadeIn.ReciprocalCurve = crec;
                });
            }
        });

        Scenario("events_audio_flags", delegate
        {
            Media w = Wav("wav_stereo_48k.wav");
            AudioTrack t = null;
            Item("track", delegate { t = AddATrack("Flags"); });
            double at = 0.0;
            Item("named and muted", delegate { AudioEvent ev = AddAudio(t, w, 0.0, 1.0); ev.Name = "Named sound"; ev.Mute = true; });
            Item("locked", delegate { AudioEvent ev = AddAudio(t, w, 1.5, 1.0); ev.Locked = true; });
            Item("loop off", delegate { AudioEvent ev = AddAudio(t, w, 3.0, 1.0); ev.Loop = false; });
            Item("snap offset", delegate { AudioEvent ev = AddAudio(t, w, 4.5, 1.0); ev.SnapOffset = T(0.25); });
            Item("invert phase", delegate { AudioEvent ev = AddAudio(t, w, 6.0, 1.0); ev.InvertPhase = true; });
            Item("normalize", delegate { AudioEvent ev = AddAudio(t, w, 7.5, 1.0); ev.Normalize = true; try { ev.RecalculateNorm(); } catch (Exception) { } });
            Item("normalize with a gain", delegate { AudioEvent ev = AddAudio(t, w, 9.0, 1.0); ev.SetNormalize(true, 2.0); });
            Item("looping past the media", delegate { AudioEvent ev = AddAudio(t, Wav("wav_mono_44k.wav"), 10.5, 6.0); ev.Loop = true; });
            Item("take offset", delegate { AudioEvent ev = AddAudio(t, w, 17.0, 1.0); ev.ActiveTake.Offset = T(2.0); });
            Item("take offset with a rate", delegate { AudioEvent ev = AddAudio(t, w, 18.5, 1.0); ev.PlaybackRate = 2.0; ev.ActiveTake.Offset = T(1.0); });
            double[] rates = { 0.5, 0.8, 1.25, 2.0, 3.0 };
            for (int i = 0; i < rates.Length; i++)
            {
                double r = rates[i], st = 20.0 + 1.5 * i;
                Item("playback rate " + r, delegate { AudioEvent ev = AddAudio(t, w, st, 1.0); ev.PlaybackRate = r; });
            }
            Item("adjust playback rate", delegate { AudioEvent ev = AddAudio(t, w, 28.0, 1.0); ev.AdjustPlaybackRate(1.5, true); });
            foreach (ChannelRemapping cr in Enum.GetValues(typeof(ChannelRemapping)))
            {
                ChannelRemapping c2 = cr;
                double st = 30.0 + 1.5 * (int)(c2 - ChannelRemapping.None);
                Item("channels " + c2, delegate { AudioEvent ev = AddAudio(t, w, st, 1.0); ev.Channels = c2; });
            }
        });

        Scenario("events_audio_stretch", delegate
        {
            Media w = Wav("wav_stereo_48k.wav");
            AudioTrack t = null;
            Item("track", delegate { t = AddATrack("Stretch"); });
            double at = 0.0;
            foreach (ClassicStretchAttributes ca in Enum.GetValues(typeof(ClassicStretchAttributes)))
            {
                ClassicStretchAttributes attr = ca;
                double st = at;
                Item("classic stretch " + attr, delegate
                {
                    AudioEvent ev = AddAudio(t, w, st, 1.0);
                    ev.PlaybackRate = 1.5;
                    ev.Method = TimeStretchPitchShift.Classic;
                    ev.ClassicAttribute = attr;
                    ev.PitchSemis = 2.0;
                });
                at += 1.2;
            }
            foreach (ElastiqueStretchAttributes ea in Enum.GetValues(typeof(ElastiqueStretchAttributes)))
            {
                ElastiqueStretchAttributes attr = ea;
                double st = at;
                // the first run failed on every attribute but Pro with E_FAIL from one of these setters (the log did not say which);
                // each is tried alone now, so the event keeps what VEGAS takes and the refusals are named in the log
                string en = "elastique stretch " + attr;
                AudioEvent eev = null;
                Item(en, delegate
                {
                    eev = AddAudio(t, w, st, 1.0);
                    eev.PlaybackRate = 0.75;
                    eev.Method = TimeStretchPitchShift.Elastique;
                });
                if (eev != null)
                {
                    AudioEvent ev = eev;
                    Try(en + ": attribute", delegate { ev.ElastiqueAttribute = attr; });
                    Try(en + ": pitch", delegate { ev.PitchSemis = -3.0; });
                    Try(en + ": pitch lock", delegate { ev.PitchLock = false; });
                    Try(en + ": formant lock", delegate { ev.FormantLock = true; });
                    Try(en + ": formant shift", delegate { ev.FormantShift = 1.5; });
                }
                at += 1.2;
            }
            double s1 = at;
            Item("pitch lock", delegate
            {
                AudioEvent ev = AddAudio(t, w, s1, 1.0);
                ev.PlaybackRate = 2.0;
                ev.Method = TimeStretchPitchShift.Elastique;
                ev.PitchLock = true;
            });
            at += 1.2;
            double s2 = at;
            // ACID style needs the media to carry ACID tempo information; a plain WAV gets E_FAIL, so this is asked, not required
            AudioEvent acidEv = null;
            Item("acid style event", delegate { acidEv = AddAudio(t, w, s2, 1.0); acidEv.PlaybackRate = 1.3; });
            if (acidEv != null) Try("acid style", delegate { acidEv.Method = TimeStretchPitchShift.AcidStyle; });
            at += 1.2;
            double s3 = at;
            Item("no stretch method", delegate
            {
                AudioEvent ev = AddAudio(t, w, s3, 1.0);
                ev.PlaybackRate = 1.3;
                ev.Method = TimeStretchPitchShift.None;
            });
            at += 1.2;
            double s4 = at;
            Item("pitch only", delegate
            {
                AudioEvent ev = AddAudio(t, w, s4, 1.0);
                ev.Method = TimeStretchPitchShift.Elastique;
                ev.PitchSemis = 7.0;
            });
        });

        Scenario("events_audio_media_takes", delegate
        {
            AudioTrack t = null;
            Item("track", delegate { t = AddATrack("Media kinds"); });
            string[] files = { "wav_mono_44k.wav", "wav_stereo_24bit.wav", "wav_surround_6ch.wav" };
            for (int i = 0; i < files.Length; i++)
            {
                string f = files[i];
                double st = i * 3.0;
                Item("sound " + f, delegate { AddAudio(t, Wav(f), st, 2.0); });
            }
            Item("takes", delegate
            {
                AudioEvent ev = AddAudio(t, Wav("wav_stereo_48k.wav"), 10.0, 2.0);
                ev.AddTake(Wav("wav_mono_44k.wav").GetAudioStreamByIndex(0), false, "Mono take");
                ev.AddTake(Wav("wav_stereo_24bit.wav").GetAudioStreamByIndex(0), false, "24 bit take");
                ev.ActiveTake = ev.Takes[2];
            });
            Item("reversed event (reversed subclip)", delegate
            {
                Subclip sc = new Subclip(proj, Path.Combine(mediaDir, "wav_stereo_48k.wav"), T(1.0), T(2.0), true, "Reversed");
                AudioEvent ev = AddAudio(t, sc, 13.0, 2.0);
            });
            Item("surround channel take", delegate
            {
                AudioEvent ev = AddAudio(t, Wav("wav_surround_6ch.wav"), 16.0, 2.0);
                Take tk = ev.ActiveTake;
                tk.SetAudioChannels(2, 2);
            });
        });
    }

    // ---------------------------------------------------------------------------------------- envelopes
    static readonly EnvelopeType[] AudioTrackEnvelopes = { EnvelopeType.Volume, EnvelopeType.Pan, EnvelopeType.Mute, EnvelopeType.PanY, EnvelopeType.PanCenter, EnvelopeType.PanSmoothness };
    static readonly EnvelopeType[] VideoTrackEnvelopes = { EnvelopeType.Composite, EnvelopeType.FadeToColor, EnvelopeType.Mute };

    // Six points, one for each kind of curve, over the range an envelope allows.
    static void FillEnvelope(Envelope env, double scale)
    {
        double lo = env.Min, hi = env.Max;
        if (double.IsNaN(lo) || double.IsInfinity(lo)) lo = 0;
        if (double.IsNaN(hi) || double.IsInfinity(hi)) hi = 1;
        double mid = env.Neutral;
        for (int i = 0; i < AllCurves.Length; i++)
        {
            double f = (i % 2 == 0) ? 0.25 : 0.75;
            double y = lo + (hi - lo) * f;
            if (i == 1) y = mid;
            EnvPoint(env, scale * (0.5 + i), y, AllCurves[i]);
        }
    }

    void EnvelopeScenarios()
    {
        Scenario("envelopes_tracks", delegate
        {
            Media w = Wav("wav_stereo_48k.wav"), a = Pic("pic_a.png");
            AudioTrack at = null;
            VideoTrack vt = null;
            Item("tracks", delegate
            {
                at = AddATrack("Audio envelopes"); AddAudio(at, w, 0, 8);
                vt = AddVTrack("Video envelopes"); AddVideo(vt, a, 0, 8);
            });
            foreach (EnvelopeType et in AudioTrackEnvelopes)
            {
                EnvelopeType type = et;
                Item("audio track envelope " + type, delegate
                {
                    bool can = at.CanAddEnvelope(type);
                    Log("  audio track can add " + type + ": " + can);
                    if (!can) return;
                    FillEnvelope(EnvOf(at.Envelopes, type), 1.0);
                });
            }
            foreach (EnvelopeType et in VideoTrackEnvelopes)
            {
                EnvelopeType type = et;
                Item("video track envelope " + type, delegate
                {
                    bool can = vt.CanAddEnvelope(type);
                    Log("  video track can add " + type + ": " + can);
                    if (!can) return;
                    FillEnvelope(EnvOf(vt.Envelopes, type), 1.0);
                });
            }
        });

        Scenario("envelopes_buses", delegate
        {
            Media w = Wav("wav_stereo_48k.wav");
            AudioTrack at = null;
            AudioBusTrack bus1 = null, bus2 = null, bus3 = null;
            Item("tracks and buses", delegate
            {
                at = AddATrack("Sends"); AddAudio(at, w, 0, 8);
                bus1 = proj.AddAudioBusTrack(); bus1.Description = "Bus one";
                bus2 = proj.AddAudioBusTrack(); bus2.Description = "Bus two";
                // a send envelope exists only for a bus that exists ("Invalid envelope type" for Bus C with two buses)
                bus3 = proj.AddAudioBusTrack(); bus3.Description = "Bus three";
                at.BusTrack = bus1;
            });
            Item("send envelope Bus A", delegate { FillEnvelope(EnvOf(at.Envelopes, EnvelopeType.BusA), 1.0); });
            Item("send envelope Bus B", delegate { FillEnvelope(EnvOf(at.Envelopes, EnvelopeType.BusB), 1.0); });
            Try("send envelope Bus C", delegate { FillEnvelope(EnvOf(at.Envelopes, EnvelopeType.BusC), 1.0); });
            Item("bus volume envelope", delegate { FillEnvelope(EnvOf(bus1.Envelopes, EnvelopeType.Volume), 1.0); });
            Item("bus pan envelope", delegate { FillEnvelope(EnvOf(bus2.Envelopes, EnvelopeType.Pan), 1.0); });
            Item("bus mute envelope", delegate { FillEnvelope(EnvOf(bus2.Envelopes, EnvelopeType.Mute), 1.0); });
            Item("master volume envelope", delegate { FillEnvelope(EnvOf(proj.MasterBus.Envelopes, EnvelopeType.Volume), 1.0); });
            Item("master mute envelope", delegate { FillEnvelope(EnvOf(proj.MasterBus.Envelopes, EnvelopeType.Mute), 1.0); });
            Item("video output fade to colour", delegate { FillEnvelope(EnvOf(proj.VideoBus.Envelopes, EnvelopeType.FadeToColor), 1.0); });
            Item("video output mute", delegate { FillEnvelope(EnvOf(proj.VideoBus.Envelopes, EnvelopeType.Mute), 1.0); });
            Item("video output motion blur length", delegate { FillEnvelope(EnvOf(proj.VideoBus.Envelopes, EnvelopeType.MotionBlurLength), 1.0); });
            Item("video output supersampling", delegate { FillEnvelope(EnvOf(proj.VideoBus.Envelopes, EnvelopeType.VideoSupersampling), 1.0); });
        });

        Scenario("envelopes_fades_and_effects", delegate
        {
            Media a = Pic("pic_a.png"), b = Pic("pic_b.png");
            VideoTrack t = null;
            VideoEvent e2 = null;
            Item("overlapping events", delegate
            {
                t = AddVTrack("Transition progress");
                AddVideo(t, a, 0, 3);
                e2 = AddVideo(t, b, 2, 3);
                e2.FadeIn.Length = T(1.0);
            });
            // the X of a transition-progress point must lie inside the transition (1 s here): 0.075 .. 0.825 s
            Item("transition progress envelope", delegate { FillEnvelope(EnvOf(e2.FadeIn.Envelopes, EnvelopeType.TransitionProgress), 0.15); });
            Item("automation envelope on an audio effect", delegate
            {
                PlugInNode node = FindAudioNode("Track EQ");
                if (node == null) node = FindAudioNode("Reverb");
                if (node == null) throw new Exception("no audio effect to automate");
                AudioTrack at = AddATrack("Automation");
                AddAudio(at, Wav("wav_stereo_48k.wav"), 0, 4);
                Effect fx = at.Effects.AddEffect(node);
                bool can = at.CanAddEnvelope(EnvelopeType.FX1);
                Log("  audio track can add FX1: " + can);
                if (can) FillEnvelope(EnvOf(at.Envelopes, EnvelopeType.FX1), 1.0);
            });
        });
    }

    // ---------------------------------------------------------------------------------------- markers
    void MarkerScenarios()
    {
        Scenario("markers_all", delegate
        {
            Item("a track so the project has a length", delegate { AddVideo(AddVTrack("Markers"), Pic("pic_a.png"), 0, 20); });
            for (int i = 0; i < 5; i++)
            {
                int k = i;
                Item("marker " + k, delegate { proj.Markers.Add(new Marker(T(1.0 + k * 1.5), "Marker " + k)); });
            }
            Item("marker without a name", delegate { proj.Markers.Add(new Marker(T(9.0))); });
            for (int i = 0; i < 3; i++)
            {
                int k = i;
                Item("region " + k, delegate { proj.Regions.Add(new Region(T(2.0 + k * 3.0), T(1.0 + k * 0.5), "Region " + k)); });
            }
            MarkerCommandType[] cmds = { MarkerCommandType.URL, MarkerCommandType.Text, MarkerCommandType.WMClosedCaption, MarkerCommandType.WMWMTextBodytext,
                MarkerCommandType.WMTextHeadline, MarkerCommandType.Title, MarkerCommandType.Author, MarkerCommandType.Copyright, MarkerCommandType.HotSpotPlay,
                MarkerCommandType.HotSpotBrowse, MarkerCommandType.HotSpotSeek, MarkerCommandType.ScottEOM, MarkerCommandType.ScottCueIn, MarkerCommandType.CEA608CC1,
                MarkerCommandType.CEA608CC2, MarkerCommandType.CEA608CC3, MarkerCommandType.CEA608CC4 };
            for (int i = 0; i < cmds.Length; i++)
            {
                int k = i;
                MarkerCommandType ct = cmds[i];
                Item("command marker " + ct, delegate { proj.CommandMarkers.Add(new CommandMarker(T(0.5 + k * 1.1), ct, "parameter " + k, "comment " + k)); });
            }
            for (int i = 0; i < 3; i++)
            {
                int k = i;
                Item("CD track " + k, delegate { proj.CDTracks.Add(new CDRegion(T(k * 4.0), T(3.5), "CD track " + k)); });
            }
            for (int i = 0; i < 2; i++)
            {
                int k = i;
                Item("CD index " + k, delegate { proj.CDIndices.Add(new CDMarker(T(1.0 + k * 4.0), "CD index " + k)); });
            }
        });
    }

    // ---------------------------------------------------------------------------------------- transitions
    void TransitionScenarios()
    {
        int chunk = 0;
        for (int start = 0; start < transitions.Count && !cancelled; start += ChunkTransitions)
        {
            int from = start, to = Math.Min(transitions.Count, start + ChunkTransitions);
            string name = "transitions_" + (++chunk).ToString("00", Inv);
            Scenario(name, delegate
            {
                VideoTrack t = null;
                Item("track", delegate { t = AddVTrack("Transitions"); });
                double at = 0.0;
                for (int i = from; i < to; i++)
                {
                    PlugInNode node = transitions[i];
                    List<string> presets = PresetNames(node, opt.Presets);
                    string id = "transition:" + PlugKey(node);
                    // the plain transition, then one pair for each preset, and one with its settings changed
                    List<string> variants = new List<string>();
                    variants.Add("");
                    variants.AddRange(presets);
                    variants.Add("\u0001tweak");
                    foreach (string variant in variants)
                    {
                        string v2 = variant;
                        double a0 = at;
                        at += 6.0;
                        Begin(id);
                        Item("transition " + node.Name + (v2.Length > 0 ? " [" + v2.Replace("\u0001", "") + "]" : ""), delegate
                        {
                            VideoEvent e1 = AddVideo(t, Pic("pic_a.png"), a0, 3.0);
                            VideoEvent e2 = AddVideo(t, Pic("pic_b.png"), a0 + 2.0, 3.0);
                            e2.FadeIn.Length = T(1.0);
                            Effect fx = new Effect(node);
                            e2.FadeIn.Transition = fx;
                            Effect applied = e2.FadeIn.Transition as Effect;
                            if (applied == null) applied = fx;
                            if (v2 == "\u0001tweak") TweakParams(applied, 0.7, true, 4);
                            else if (v2.Length > 0) applied.Preset = v2;
                        });
                        End(id);
                    }
                }
            });
        }
    }

    // ---------------------------------------------------------------------------------------- video effects
    void VideoFxScenarios()
    {
        int chunk = 0;
        for (int start = 0; start < videoFx.Count && !cancelled; start += ChunkVideoFx)
        {
            int from = start, to = Math.Min(videoFx.Count, start + ChunkVideoFx);
            string name = "videofx_" + (++chunk).ToString("00", Inv);
            Scenario(name, delegate
            {
                for (int i = from; i < to && !cancelled; i++)
                {
                    PlugInNode node = videoFx[i];
                    int idx = i;
                    string id = "video:" + PlugKey(node);
                    List<string> presets = PresetNames(node, opt.Presets);
                    Begin(id);
                    VideoTrack t = null;
                    Item("fx track " + node.Name, delegate { t = AddVTrack(node.Name); });
                    if (t == null) { End(id); continue; }
                    // on an event: the plug-in as it is, each preset, then settings changed
                    Item(node.Name + " on an event", delegate
                    {
                        VideoEvent ev = AddVideo(t, Pic("pic_a.png"), 0.0, 3.0);
                        ev.Effects.AddEffect(node);
                    });
                    foreach (string preset in presets)
                    {
                        string pn = preset;
                        Item(node.Name + " preset " + pn, delegate
                        {
                            VideoEvent ev = AddVideo(t, Pic("pic_b.png"), 3.5 + 3.5 * presets.IndexOf(pn), 3.0);
                            Effect fx = ev.Effects.AddEffect(node);
                            fx.Preset = pn;
                        });
                    }
                    double tweakAt = 3.5 + 3.5 * presets.Count;
                    Item(node.Name + " with settings changed", delegate
                    {
                        VideoEvent ev = AddVideo(t, Pic("pic_a.png"), tweakAt, 3.0);
                        Effect fx = ev.Effects.AddEffect(node);
                        TweakParams(fx, 0.3, false, 5);
                    });
                    Item(node.Name + " with keyframes", delegate
                    {
                        VideoEvent ev = AddVideo(t, Pic("pic_b.png"), tweakAt + 3.5, 3.0);
                        Effect fx = ev.Effects.AddEffect(node);
                        TweakParams(fx, 0.8, true, 5);
                        try { if (fx.Keyframes.Count == 0) fx.Keyframes.Add(new Keyframe(T(1.0))); } catch (Exception) { }
                    });
                    Item(node.Name + " bypassed with a before-pan/crop flag", delegate
                    {
                        VideoEvent ev = AddVideo(t, Pic("pic_a.png"), tweakAt + 7.0, 3.0);
                        Effect fx = ev.Effects.AddEffect(node);
                        fx.Bypass = true;
                        try { fx.ApplyBeforePanCrop = true; } catch (Exception) { }
                    });
                    Item(node.Name + " on the track", delegate { t.Effects.AddEffect(node); });
                    Item(node.Name + " on the media", delegate
                    {
                        string file = "fxpic_" + idx + ".png";
                        string path = Path.Combine(mediaDir, file);
                        if (!File.Exists(path)) SavePng(path, 320, 180, idx, false);
                        Media m = Pic(file);
                        VideoTrack t2 = AddVTrack(node.Name + " (media effect)");
                        AddVideo(t2, m, 0.0, 3.0);
                        m.Effects.AddEffect(node);
                    });
                    Item(node.Name + " on the video output", delegate { proj.VideoBus.Effects.AddEffect(node); });
                    End(id);
                }
            });
        }
    }

    // ---------------------------------------------------------------------------------------- generators
    void GeneratorScenarios()
    {
        int chunk = 0;
        for (int start = 0; start < generators.Count && !cancelled; start += ChunkGenerators)
        {
            int from = start, to = Math.Min(generators.Count, start + ChunkGenerators);
            string name = "generators_" + (++chunk).ToString("00", Inv);
            Scenario(name, delegate
            {
                for (int i = from; i < to && !cancelled; i++)
                {
                    PlugInNode node = generators[i];
                    string id = "generator:" + PlugKey(node);
                    List<string> presets = PresetNames(node, opt.Presets);
                    Begin(id);
                    VideoTrack t = null;
                    Item("generator track " + node.Name, delegate { t = AddVTrack(node.Name); });
                    if (t == null) { End(id); continue; }
                    double at = 0.0;
                    List<string> variants = new List<string>();
                    variants.Add("");
                    variants.AddRange(presets);
                    variants.Add("\u0001tweak");
                    variants.Add("\u0001keys");
                    foreach (string variant in variants)
                    {
                        string v2 = variant;
                        double a0 = at;
                        at += 3.5;
                        Item(node.Name + (v2.Length > 0 ? " [" + v2.Replace("\u0001", "") + "]" : ""), delegate
                        {
                            Media gm;
                            if (v2.Length > 0 && v2[0] != '\u0001') gm = Media.CreateInstance(proj, node, v2);
                            else gm = Media.CreateInstance(proj, node);
                            VideoEvent ev = t.AddVideoEvent(T(a0), T(3.0));
                            ev.AddTake(gm.GetVideoStreamByIndex(0), true);
                            if (v2.StartsWith("\u0001"))
                            {
                                Effect g = gm.Generator;
                                TweakParams(g, v2 == "\u0001keys" ? 0.8 : 0.3, v2 == "\u0001keys", 5);
                            }
                        });
                    }
                    End(id);
                }
            });
        }
    }

    // ---------------------------------------------------------------------------------------- audio effects
    void AudioFxScenarios()
    {
        // VST3 plug-ins get a project each (their saved state is what a .veg needs to be understood); the rest in chunks
        List<PlugInNode> vst3 = new List<PlugInNode>(), others = new List<PlugInNode>();
        foreach (PlugInNode n in audioFx) { if (IsVst3(n)) vst3.Add(n); else others.Add(n); }
        Log("audio plug-ins: " + vst3.Count + " VST3, " + others.Count + " others");
        foreach (PlugInNode n in vst3)
        {
            if (cancelled) break;
            PlugInNode node = n;
            Scenario("audiofx_vst3_" + SafeName(node.Name), delegate { AudioFxBody(new PlugInNode[] { node }); });
        }
        int chunk = 0;
        for (int start = 0; start < others.Count && !cancelled; start += ChunkAudioFx)
        {
            int from = start, to = Math.Min(others.Count, start + ChunkAudioFx);
            string name = "audiofx_" + (++chunk).ToString("00", Inv);
            List<PlugInNode> part = others.GetRange(from, to - from);
            Scenario(name, delegate { AudioFxBody(part.ToArray()); });
        }
    }

    void AudioFxBody(PlugInNode[] nodes)
    {
        foreach (PlugInNode n in nodes)
        {
            if (cancelled) break;
            PlugInNode node = n;
            string id = "audio:" + PlugKey(node);
            List<string> presets = PresetNames(node, opt.Presets);
            Begin(id);
            AudioTrack t = null;
            Item("audio fx track " + node.Name, delegate { t = AddATrack(node.Name); });
            if (t == null) { End(id); continue; }
            Item(node.Name + " on an event", delegate
            {
                AudioEvent ev = AddAudio(t, Wav("wav_stereo_48k.wav"), 0.0, 3.0);
                ev.Effects.AddEffect(node);
            });
            double at = 3.5;
            foreach (string preset in presets)
            {
                string pn = preset;
                double a0 = at;
                at += 3.5;
                Item(node.Name + " preset " + pn, delegate
                {
                    AudioEvent ev = AddAudio(t, Wav("wav_stereo_48k.wav"), a0, 3.0);
                    Effect fx = ev.Effects.AddEffect(node);
                    fx.Preset = pn;
                });
            }
            double a1 = at;
            Item(node.Name + " bypassed", delegate
            {
                AudioEvent ev = AddAudio(t, Wav("wav_stereo_48k.wav"), a1, 3.0);
                Effect fx = ev.Effects.AddEffect(node);
                fx.Bypass = true;
            });
            Item(node.Name + " on the track", delegate { t.Effects.AddEffect(node); });
            Item(node.Name + " on a bus", delegate
            {
                AudioBusTrack bus = proj.AddAudioBusTrack();
                bus.Description = node.Name;
                bus.Effects.AddEffect(node);
            });
            Item(node.Name + " on the master", delegate { proj.MasterBus.Effects.AddEffect(node); });
            Item(node.Name + " as an effects bus", delegate
            {
                AudioFXBusTrack fb = proj.AddAudioFXBusTrack(node);
                fb.Description = node.Name + " send";
            });
            End(id);
        }
    }

    // ======================================================================================== fuzz mode
    void FuzzMode()
    {
        MakeMediaFiles();
        for (int i = 0; i < opt.Count && !cancelled; i++)
        {
            int index = i;
            string name = "fuzz_s" + opt.Seed + "_" + index.ToString("000", Inv);
            Scenario(name, delegate { FuzzBody(new Random(opt.Seed * 100003 + index * 7919 + 17)); });
        }
        Progress("Done");
    }

    static readonly CompositeMode[] FuzzModes = { CompositeMode.SrcAlpha, CompositeMode.SrcAlpha, CompositeMode.SrcAlpha, CompositeMode.Add, CompositeMode.Multiply,
        CompositeMode.Screen, CompositeMode.Overlay, CompositeMode.Difference, CompositeMode.Darken, CompositeMode.Lighten, CompositeMode.Subtract, CompositeMode.Cut };
    static readonly string[] FuzzPics = { "pic_a.png", "pic_b.png", "pic_small.png", "pic_alpha.png", "pic_square.png", "pic_hd.png" };
    static readonly string[] FuzzWavs = { "wav_mono_44k.wav", "wav_stereo_48k.wav", "wav_stereo_24bit.wav" };
    static readonly double[] FuzzRates = { 1.0, 1.0, 1.0, 0.5, 2.0, 0.75, 1.5, 4.0 };

    void FuzzBody(Random rnd)
    {
        int[][] sizes = new int[][] { new int[] { 640, 360 }, new int[] { 1280, 720 }, new int[] { 720, 480 }, new int[] { 1920, 1080 }, new int[] { 320, 240 } };
        double[] fpsList = { 24.0, 25.0, 29.97, 30.0, 50.0, 59.94 };
        int sz = rnd.Next(sizes.Length), fr = rnd.Next(fpsList.Length), sr = rnd.Next(3);
        Item("settings", delegate
        {
            proj.Video.Width = sizes[sz][0]; proj.Video.Height = sizes[sz][1]; proj.Video.FrameRate = fpsList[fr];
            proj.Audio.SampleRate = sr == 0 ? 44100u : 48000u;
        });
        int nv = 1 + rnd.Next(3), na = 1 + rnd.Next(3);
        for (int v = 0; v < nv && !cancelled; v++)
        {
            int ci = rnd.Next(FuzzModes.Length);
            float level = (float)(0.3 + 0.7 * rnd.NextDouble());
            bool motion = rnd.Next(3) == 0;
            double mx = rnd.NextDouble() * 200 - 100, my = rnd.NextDouble() * 100 - 50, mw = 200 + rnd.NextDouble() * 400, mr = rnd.NextDouble() - 0.5;
            VideoTrack t = null;
            Item("video track " + v, delegate
            {
                t = AddVTrack("Fuzz video " + v);
                t.SetCompositeMode(FuzzModes[ci], false);
                t.CompositeLevel = level;
            });
            if (t == null) continue;
            if (motion)
                Item("track motion", delegate
                {
                    TrackMotionKeyframe k0 = MotionKey(t.TrackMotion, 0.0);
                    k0.PositionX = mx; k0.PositionY = my; k0.Width = mw; k0.Height = mw * 9 / 16; k0.RotationZ = mr;
                    TrackMotionKeyframe k1 = MotionKey(t.TrackMotion, 2.0);
                    k1.PositionX = -mx; k1.PositionY = -my; k1.Width = mw / 2; k1.Height = mw * 9 / 32; k1.Type = AllKeyTypes[(int)(Math.Abs(mr) * 12) % AllKeyTypes.Length];
                });
            int nev = 2 + rnd.Next(4);
            double pos = rnd.NextDouble();
            double prevEnd = -1, prevStart = 0;
            for (int e = 0; e < nev && !cancelled; e++)
            {
                int kind = rnd.Next(10), picIdx = rnd.Next(FuzzPics.Length), gi = generators.Count > 0 ? rnd.Next(generators.Count) : -1;
                double len = 0.6 + rnd.NextDouble() * 2.5, gap = rnd.NextDouble() * 1.6 - 0.6;
                int curveIn = rnd.Next(AllCurves.Length), curveOut = rnd.Next(AllCurves.Length), curveRec = rnd.Next(AllCurves.Length), rateIdx = rnd.Next(FuzzRates.Length);
                double fin = rnd.NextDouble() * 0.5, fout = rnd.NextDouble() * 0.5, takeOff = rnd.NextDouble() * 0.4;
                bool useFin = rnd.Next(2) == 0, useFout = rnd.Next(2) == 0, mute = rnd.Next(12) == 0, noLoop = rnd.Next(8) == 0, veloc = rnd.Next(6) == 0, pan = rnd.Next(5) == 0;
                int nfx = rnd.Next(3), fxIdx = videoFx.Count > 0 ? rnd.Next(videoFx.Count) : -1, fxIdx2 = videoFx.Count > 0 ? rnd.Next(videoFx.Count) : -1;
                int presetPick = rnd.Next(1000);
                double tweak = rnd.NextDouble();
                int trIdx = transitions.Count > 0 ? rnd.Next(transitions.Count) : -1;
                bool useTr = rnd.Next(3) == 0;
                double start = Math.Max(0.0, pos);
                if (prevEnd >= 0 && start < prevStart) start = prevStart;
                double overlap = prevEnd - start;
                pos = start + len + gap;
                int eNo = e;
                Item("video event " + v + "/" + eNo, delegate
                {
                    Media m;
                    if (kind == 6) m = Seq();
                    else if ((kind == 7 || kind == 8) && gi >= 0) m = Media.CreateInstance(proj, generators[gi]);
                    else m = Pic(FuzzPics[picIdx]);
                    VideoEvent ev = t.AddVideoEvent(T(start), T(len));
                    ev.AddTake(m.GetVideoStreamByIndex(0), true);
                    if (useFin) { ev.FadeIn.Length = T(Math.Min(fin, len / 2)); ev.FadeIn.Curve = FadeCurves[curveIn % FadeCurves.Length]; }
                    if (useFout) { ev.FadeOut.Length = T(Math.Min(fout, len / 2)); ev.FadeOut.Curve = FadeCurves[curveOut % FadeCurves.Length]; }
                    if (prevEnd > start + 0.05 && overlap < len - 0.05)
                    {
                        ev.FadeIn.Length = T(overlap);
                        ev.FadeIn.Curve = FadeCurves[curveIn % FadeCurves.Length];
                        ev.FadeIn.ReciprocalCurve = FadeCurves[curveRec % FadeCurves.Length];
                        if (useTr && trIdx >= 0)
                        {
                            Effect trfx = new Effect(transitions[trIdx]);
                            ev.FadeIn.Transition = trfx;
                            Effect applied = ev.FadeIn.Transition as Effect;
                            if (applied != null) TweakParams(applied, tweak, false, 3);
                        }
                    }
                    // what follows is optional: each part is tried alone, so that one refusal (a take offset on a still,
                    // a velocity envelope on a generator ...) does not drop everything after it from the project
                    ev.Mute = mute;
                    string en = "fuzz video " + v + "/" + eNo;
                    if (noLoop) Try(en + " loop off", delegate { ev.Loop = false; });
                    Try(en + " playback rate", delegate { ev.PlaybackRate = FuzzRates[rateIdx]; });
                    if (takeOff > 0.2) Try(en + " take offset", delegate { ev.ActiveTake.Offset = T(takeOff); });
                    if (veloc)
                        Try(en + " velocity", delegate
                        {
                            Envelope env = EnvOf(ev.Envelopes, EnvelopeType.Velocity);
                            EnvPoint(env, 0.0, 1.0, AllCurves[curveIn]);
                            EnvPoint(env, len / 2, 0.5 + tweak * 2, AllCurves[curveOut]);
                        });
                    if (pan)
                      Try(en + " pan/crop", delegate
                      {
                        VideoStream vs = m.GetVideoStreamByIndex(0);
                        double mw0 = vs.Width, mh0 = vs.Height;
                        VideoMotionKeyframe k0 = PanKey(ev, 0.0);
                        SetBox(k0, mw0 / 2, mh0 / 2, mw0, mh0, 0.0);
                        VideoMotionKeyframe k1 = PanKey(ev, len * 0.7);
                        SetBox(k1, mw0 * (0.3 + 0.4 * tweak), mh0 * 0.5, mw0 * 0.5, mh0 * 0.5, tweak - 0.5);
                        k1.Type = AllKeyTypes[curveIn % AllKeyTypes.Length];
                      });
                    for (int f = 0; f < nfx; f++)
                    {
                        int pick = f == 0 ? fxIdx : fxIdx2;
                        if (pick < 0) break;
                        int ff = f;
                        Try(en + " effect " + ff, delegate
                        {
                            Effect fx = ev.Effects.AddEffect(videoFx[pick]);
                            List<string> presets = PresetNames(videoFx[pick], 6);
                            if (presets.Count > 0 && presetPick % 3 == 0) fx.Preset = presets[presetPick % presets.Count];
                            else TweakParams(fx, tweak, ff == 0 && presetPick % 2 == 0, 3);
                        });
                    }
                });
                prevEnd = start + len;
                prevStart = start;
            }
        }
        for (int a = 0; a < na && !cancelled; a++)
        {
            float vol = (float)(0.2 + 1.5 * rnd.NextDouble()), panx = (float)(rnd.NextDouble() * 2 - 1);
            bool invert = rnd.Next(8) == 0, mute = rnd.Next(10) == 0;
            AudioTrack t = null;
            Item("audio track " + a, delegate
            {
                t = AddATrack("Fuzz audio " + a);
                t.Volume = vol; t.PanX = panx; t.InvertPhase = invert; t.Mute = mute;
            });
            if (t == null) continue;
            int nev = 2 + rnd.Next(4);
            double pos = rnd.NextDouble();
            double prevEnd = -1, prevStart = 0;
            for (int e = 0; e < nev && !cancelled; e++)
            {
                int wi = rnd.Next(FuzzWavs.Length), rateIdx = rnd.Next(FuzzRates.Length), curveIn = rnd.Next(AllCurves.Length), curveOut = rnd.Next(AllCurves.Length), curveRec = rnd.Next(AllCurves.Length);
                double len = 0.6 + rnd.NextDouble() * 2.5, gap = rnd.NextDouble() * 1.6 - 0.6, fin = rnd.NextDouble() * 0.5, fout = rnd.NextDouble() * 0.5, takeOff = rnd.NextDouble() * 2.0;
                double semis = Math.Round(rnd.NextDouble() * 12 - 6);
                bool useFin = rnd.Next(2) == 0, useFout = rnd.Next(2) == 0, stretch = rnd.Next(3) == 0, norm = rnd.Next(10) == 0, reverse = rnd.Next(12) == 0, inv = rnd.Next(10) == 0;
                int method = rnd.Next(3), attr = rnd.Next(4), chan = rnd.Next(7);
                int fxIdx = audioFx.Count > 0 ? rnd.Next(audioFx.Count) : -1, pick = rnd.Next(1000);
                bool useFx = rnd.Next(4) == 0;
                double start = Math.Max(0.0, pos);
                if (prevEnd >= 0 && start < prevStart) start = prevStart;
                double overlap = prevEnd - start;
                pos = start + len + gap;
                int eNo = e;
                Item("audio event " + a + "/" + eNo, delegate
                {
                    Media m = Wav(FuzzWavs[wi]);
                    if (reverse) m = new Subclip(proj, Path.Combine(mediaDir, FuzzWavs[wi]), T(0.5), T(2.5), true, "Reversed " + wi);
                    AudioEvent ev = AddAudio(t, m, start, len);
                    if (useFin) { ev.FadeIn.Length = T(Math.Min(fin, len / 2)); ev.FadeIn.Curve = FadeCurves[curveIn % FadeCurves.Length]; }
                    if (useFout) { ev.FadeOut.Length = T(Math.Min(fout, len / 2)); ev.FadeOut.Curve = FadeCurves[curveOut % FadeCurves.Length]; }
                    if (prevEnd > start + 0.05 && overlap < len - 0.05)
                    {
                        ev.FadeIn.Length = T(overlap);
                        ev.FadeIn.Curve = FadeCurves[curveIn % FadeCurves.Length];
                        ev.FadeIn.ReciprocalCurve = FadeCurves[curveRec % FadeCurves.Length];
                    }
                    string en = "fuzz audio " + a + "/" + eNo;
                    Try(en + " playback rate", delegate { ev.PlaybackRate = FuzzRates[rateIdx]; });
                    if (stretch)
                    {
                        // VEGAS takes the elastique attribute only for Pro on some setups (E_FAIL otherwise) and pitch
                        // shifting needs a method, so each setter is tried alone
                        Try(en + " stretch method", delegate
                        {
                            ev.Method = method == 0 ? TimeStretchPitchShift.Classic : method == 1 ? TimeStretchPitchShift.Elastique : TimeStretchPitchShift.None;
                        });
                        if (method != 2) Try(en + " pitch", delegate { ev.PitchSemis = semis; });
                        if (method == 0) Try(en + " classic attribute", delegate { ev.ClassicAttribute = (ClassicStretchAttributes)(pick % 19); });
                        if (method == 1) Try(en + " elastique attribute", delegate { ev.ElastiqueAttribute = (ElastiqueStretchAttributes)attr; });
                    }
                    if (norm) Try(en + " normalize", delegate { ev.SetNormalize(true, 1.0 + (pick % 5) * 0.2); });
                    ev.InvertPhase = inv;
                    if (chan > 0) Try(en + " channels", delegate { ev.Channels = (ChannelRemapping)(1000 + chan - 1); });
                    if (takeOff > 1.5) Try(en + " take offset", delegate { ev.ActiveTake.Offset = T(takeOff - 1.5); });
                    if (useFx && fxIdx >= 0) Try(en + " effect", delegate { ev.Effects.AddEffect(audioFx[fxIdx]); });
                });
                prevEnd = start + len;
                prevStart = start;
            }
        }
        int nm = rnd.Next(4);
        for (int k = 0; k < nm; k++)
        {
            double mt = rnd.NextDouble() * 6, ml = 0.3 + rnd.NextDouble() * 2;
            int kind = rnd.Next(3), kk = k;
            Item("marker " + k, delegate
            {
                if (kind == 0) proj.Markers.Add(new Marker(T(mt), "Fuzz marker " + kk));
                else if (kind == 1) proj.Regions.Add(new Region(T(mt), T(ml), "Fuzz region " + kk));
                else proj.CommandMarkers.Add(new CommandMarker(T(mt), MarkerCommandType.Text, "fuzz " + kk, "comment"));
            });
        }
    }

    // ======================================================================================== describe mode
    void DescribeFolderMode()
    {
        string[] files = Directory.GetFiles(opt.DescribeFolder, "*.veg");
        Array.Sort(files, StringComparer.OrdinalIgnoreCase);
        Log("describing " + files.Length + " files in " + opt.DescribeFolder);
        foreach (string file in files)
        {
            if (cancelled) break;
            string name = Path.GetFileNameWithoutExtension(file);
            string jsonPath = Path.Combine(projDir, name + ".vegas.json");
            if (opt.SkipDone && File.Exists(jsonPath)) { scenariosSkipped++; continue; }
            Progress("Describing " + name);
            bool ok = false;
            Item("describe " + name, delegate
            {
                if (!vegas.OpenProject(file)) throw new Exception("VEGAS did not open the file");
                proj = vegas.Project;
                JObj m = DescribeProject("vegas");
                m.Add("scenario", name);
                m.Add("describedFile", Path.GetFileName(file));
                File.WriteAllText(jsonPath, JW.Write(m), new UTF8Encoding(false));
                ok = true;
            });
            if (ok) scenariosDone++; else scenariosFailed++;
            try { vegas.NewProject(false, false); } catch (Exception) { }
        }
        Progress("Done");
    }
}
