// Reno Bridge: run inside VEGAS Pro 18 (Tools > Scripting > Run Script...). Shows a small window and does what job files
// dropped in its folder ask: open a project, render, take snapshots, list render templates and plug-ins, export a Reno
// project, build a project from a .reno file and save it as .veg, run a script. Stop (or a file named STOP in the jobs
// folder) ends it. Reno's tests use it to drive real VEGAS on a machine nobody is sitting at.
//
// The folder (default Documents\Reno Bridge; "Reno Bridge.ini" next to this script can name
// another, see BridgeConfig) holds:
//   jobs\<id>.job.json       a job, taken in name order, one at a time
//   jobs\done\               finished jobs
//   results\<id>.result.json outcome: ok or the error, time per action, files made, log
//   heartbeat.json           rewritten every few seconds (and during renders) to show the bridge is alive
//   bridge.log               everything the bridge did
// A job (every action and field is in tools\vegas\BRIDGE.txt):
//   { "timeout": 600, "project": "suite\\cuts.veg",
//     "actions": [ { "do": "render", "out": "renders\\cuts.avi", "renderer": "Video for Windows",
//                    "template": "Default Template (uncompressed)" },
//                  { "do": "snapshot", "times": [0, 1.5], "out": "snaps\\cuts_{t}.png" } ] }
// Relative paths are in the bridge folder, and files are only written there. A project a job opens is closed without saving.
//
// No message boxes: errors go to the result file and the log. VEGAS's own error windows can still wait for a click,
// which stops the heartbeat. A job past its time limit is stopped (a render is cancelled).
//
// The parts that do not need VEGAS (job files, JSON, folder handling) compile on their own with BRIDGE_TEST defined
// (tests/compare/bridge_test.cs); everything that talks to VEGAS is in VegasHost and BridgeForm at the end.
// C# 5 against ScriptPortal.Vegas.

using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text;
#if !BRIDGE_TEST
using System.CodeDom.Compiler;
using System.Runtime.InteropServices;
using System.Windows.Forms;
using ScriptPortal.Vegas;
#endif

#if !BRIDGE_TEST
public class EntryPoint
{
    public void FromVegas(Vegas vegas)
    {
        BridgeConfig config = null;
        try
        {
            string scriptPath = null;
            try { scriptPath = Script.File; } catch { }
            config = BridgeConfig.ForScript(scriptPath);
            if (vegas.Project != null && vegas.Project.IsModified)
            {
                // the only question the bridge ever asks: the user is there when it starts
                DialogResult r = MessageBox.Show("The project that is open has changes that are not saved.\n\n" +
                    "Reno Bridge closes it without saving when a job opens another project. Save it first (No), " +
                    "or go on and lose the changes (Yes)?", "Reno Bridge", MessageBoxButtons.YesNo, MessageBoxIcon.Warning);
                if (r != DialogResult.Yes) return;
            }
            VegasHost host = new VegasHost(vegas, config);
            BridgeCore core = new BridgeCore(config, host);
            host.Core = core;
            using (BridgeForm form = new BridgeForm(core, config))
            {
                form.ShowDialog();
            }
        }
        catch (Exception e)
        {
            MessageBox.Show("Reno Bridge could not start:\n" + e.ToString(), "Reno Bridge");
        }
    }
}
#endif

// ---------------------------------------------------------------------------------------------- JSON
// A small JSON reader and writer: objects are Dictionary<string, object> (written in the order their keys were
// added), arrays List<object>, numbers double, true/false bool, null null.
public class BJson
{
    string s;
    int p;
    int depth;

    public static object Parse(string text)
    {
        if (text == null) throw new FormatException("no text");
        BJson j = new BJson();
        j.s = text;
        j.p = 0;
        if (j.s.Length > 0 && j.s[0] == '\uFEFF') j.p = 1;
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
        if (p + w.Length > s.Length || string.CompareOrdinal(s, p, w, 0, w.Length) != 0) return false;
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
        while (true)
        {
            if (p >= s.Length) throw new FormatException("a string does not end");
            char c = s[p++];
            if (c == '"') return sb.ToString();
            if (c != '\\') { sb.Append(c); continue; }
            if (p >= s.Length) throw new FormatException("a string does not end");
            char e = s[p++];
            switch (e)
            {
                case 'n': sb.Append('\n'); break;
                case 'r': sb.Append('\r'); break;
                case 't': sb.Append('\t'); break;
                case 'b': sb.Append('\b'); break;
                case 'f': sb.Append('\f'); break;
                case 'u':
                    if (p + 4 > s.Length) throw new FormatException("a bad \\u escape");
                    sb.Append((char)int.Parse(s.Substring(p, 4), NumberStyles.HexNumber, CultureInfo.InvariantCulture));
                    p += 4;
                    break;
                default: sb.Append(e); break;
            }
        }
    }

    object Number()
    {
        int b = p;
        while (p < s.Length && "+-0123456789.eE".IndexOf(s[p]) >= 0) p++;
        double d;
        if (p == b || !double.TryParse(s.Substring(b, p - b), NumberStyles.Float, CultureInfo.InvariantCulture, out d))
            throw new FormatException("a value was expected at " + b);
        return d;
    }

    // ---- reading values out of parsed JSON (missing or of the wrong kind: the default)
    public static object Get(object o, string key)
    {
        Dictionary<string, object> d = o as Dictionary<string, object>;
        object v;
        return d != null && d.TryGetValue(key, out v) ? v : null;
    }
    public static bool Has(object o, string key) { Dictionary<string, object> d = o as Dictionary<string, object>; return d != null && d.ContainsKey(key); }
    public static double Num(object o, string key, double def)
    {
        object v = Get(o, key);
        if (v is double) return (double)v;
        double d;
        if (v is string && double.TryParse((string)v, NumberStyles.Float, CultureInfo.InvariantCulture, out d)) return d;
        return def;
    }
    public static bool Bool(object o, string key, bool def) { object v = Get(o, key); return v is bool ? (bool)v : def; }
    public static string Text(object o, string key, string def) { object v = Get(o, key); return v is string ? (string)v : def; }
    public static List<object> Items(object o, string key) { return Get(o, key) as List<object> ?? new List<object>(); }

    // ---- writing
    public static string Write(object v)
    {
        StringBuilder sb = new StringBuilder();
        Put(sb, v, 0);
        sb.Append('\n');
        return sb.ToString();
    }

    static void Indent(StringBuilder sb, int depth) { sb.Append('\n'); sb.Append(' ', depth * 2); }

    static void Put(StringBuilder sb, object v, int depth)
    {
        if (v == null) { sb.Append("null"); return; }
        if (v is string) { Quote(sb, (string)v); return; }
        if (v is bool) { sb.Append((bool)v ? "true" : "false"); return; }
        IDictionary<string, object> o = v as IDictionary<string, object>;
        if (o != null)
        {
            sb.Append('{');
            int i = 0;
            foreach (KeyValuePair<string, object> kv in o)
            {
                if (i++ > 0) sb.Append(',');
                Indent(sb, depth + 1);
                Quote(sb, kv.Key);
                sb.Append(": ");
                Put(sb, kv.Value, depth + 1);
            }
            if (i > 0) Indent(sb, depth);
            sb.Append('}');
            return;
        }
        if (v is IList)
        {
            IList a = (IList)v;
            // short lists of numbers on one line
            bool flat = a.Count <= 16;
            foreach (object x in a) if (!(x is double || x is int || x is long || x is float)) { flat = false; break; }
            sb.Append('[');
            for (int i = 0; i < a.Count; i++)
            {
                if (i > 0) sb.Append(flat ? ", " : ",");
                if (!flat) Indent(sb, depth + 1);
                Put(sb, a[i], depth + 1);
            }
            if (a.Count > 0 && !flat) Indent(sb, depth);
            sb.Append(']');
            return;
        }
        if (v is Enum) { Quote(sb, v.ToString()); return; }
        double d;
        try { d = Convert.ToDouble(v, CultureInfo.InvariantCulture); }
        catch { Quote(sb, v.ToString()); return; }
        if (double.IsNaN(d) || double.IsInfinity(d)) { sb.Append("null"); return; }
        if (d == Math.Floor(d) && Math.Abs(d) < 1e15) sb.Append(((long)d).ToString(CultureInfo.InvariantCulture));
        else sb.Append(d.ToString("R", CultureInfo.InvariantCulture));
    }

    public static void Quote(StringBuilder sb, string s)
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

    // An ordered object for results (Dictionary keeps the order keys were added in as long as none is removed).
    public static Dictionary<string, object> Obj(params object[] kv)
    {
        Dictionary<string, object> d = new Dictionary<string, object>();
        for (int i = 0; i + 1 < kv.Length; i += 2) d[(string)kv[i]] = kv[i + 1];
        return d;
    }
}

// ---------------------------------------------------------------------------------------------- settings
// "Reno Bridge.ini" next to the script (all optional):
//   folder = D:\vegas_bridge                                    the bridge folder
//   poll_seconds = 1                                            how often the jobs folder is looked at
//   heartbeat_seconds = 5                                       how often heartbeat.json is rewritten
//   job_timeout_seconds = 1800                                  a job's time limit when it gives none
//   scripts = D:\more scripts;E:\other                          more folders to look for Export to Reno.cs etc. in
//   keep_awake = 1                                              keep Windows from sleeping while the bridge runs
public class BridgeConfig
{
    public static readonly string DefaultFolder = System.IO.Path.Combine(System.Environment.GetFolderPath(System.Environment.SpecialFolder.MyDocuments), "Reno Bridge");
    public string Folder = DefaultFolder;
    public double PollSeconds = 1;
    public double HeartbeatSeconds = 5;
    public double JobTimeoutSeconds = 1800;
    public bool KeepAwake = true;
    public List<string> ScriptDirs = new List<string>();
    public string IniPath;
    public string ScriptPath;
    public List<string> Notes = new List<string>();

    public static BridgeConfig ForScript(string scriptPath)
    {
        BridgeConfig c = new BridgeConfig();
        c.ScriptPath = scriptPath;
        if (!string.IsNullOrEmpty(scriptPath))
        {
            string dir = Path.GetDirectoryName(scriptPath);
            c.ScriptDirs.Add(dir);
            c.IniPath = Path.Combine(dir, Path.GetFileNameWithoutExtension(scriptPath) + ".ini");
            if (File.Exists(c.IniPath)) c.Read(File.ReadAllText(c.IniPath));
        }
        c.AddDefaultScriptDirs();
        return c;
    }

    // the folders Export to Reno.cs and Import Reno Project.cs are looked for in, after the script's own and the ini's
    public void AddDefaultScriptDirs()
    {
        ScriptDirs.Add(Path.Combine(Folder, "scripts"));
        try
        {
            string parent = Path.GetDirectoryName(Folder.TrimEnd('\\', '/'));
            if (!string.IsNullOrEmpty(parent)) ScriptDirs.Add(Path.Combine(Path.Combine(parent, "tools"), "vegas"));
        }
        catch { }
    }

    public void Read(string text)
    {
        foreach (string raw in text.Split('\n'))
        {
            string line = raw.Trim();
            if (line.Length == 0 || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
            int eq = line.IndexOf('=');
            if (eq <= 0) { Notes.Add("ini: line not understood: " + line); continue; }
            string key = line.Substring(0, eq).Trim().ToLowerInvariant();
            string val = line.Substring(eq + 1).Trim();
            if (val.Length >= 2 && val[0] == '"' && val[val.Length - 1] == '"') val = val.Substring(1, val.Length - 2);
            double d;
            bool num = double.TryParse(val, NumberStyles.Float, CultureInfo.InvariantCulture, out d);
            switch (key)
            {
                case "folder": if (val.Length > 0) Folder = val; break;
                case "poll_seconds": if (num && d >= 0.1 && d <= 600) PollSeconds = d; else Notes.Add("ini: bad poll_seconds"); break;
                case "heartbeat_seconds": if (num && d >= 1 && d <= 3600) HeartbeatSeconds = d; else Notes.Add("ini: bad heartbeat_seconds"); break;
                case "job_timeout_seconds": if (num && d >= 1) JobTimeoutSeconds = d; else Notes.Add("ini: bad job_timeout_seconds"); break;
                case "keep_awake": KeepAwake = val != "0" && val.ToLowerInvariant() != "false" && val.ToLowerInvariant() != "no"; break;
                case "scripts":
                    foreach (string s in val.Split(';')) if (s.Trim().Length > 0) ScriptDirs.Add(s.Trim());
                    break;
                default: Notes.Add("ini: unknown setting " + key); break;
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------- what a host does
// The actions that need VEGAS. The core hands each action to the host; the tests give it a fake.
public interface IBridgeHost
{
    string Describe();                                  // "VEGAS Pro 18.0 (build 527)"
    // Does one action. Throws on failure; puts what it made in ctx (outputs, info).
    void Run(string what, Dictionary<string, object> action, BridgeJobContext ctx);
    // Called after each job, whatever happened (the VEGAS host closes the project so nothing is left half done).
    void AfterJob(BridgeJobContext ctx);
}

public class BridgeException : Exception
{
    public BridgeException(string message) : base(message) { }
}

// One job as it runs: where its files go, its time limit, its log, what it made.
public class BridgeJobContext
{
    public string Id;
    public string Root;                 // the bridge folder (full path)
    public DateTime Started;
    public DateTime Deadline;
    public Func<DateTime> Clock;
    public List<object> Log = new List<object>();
    public List<object> Outputs = new List<object>();
    public Dictionary<string, object> Info = new Dictionary<string, object>();     // the current action's
    public Action<string> LogSink;
    public Action Alive;                // the host calls it during long work (renders): heartbeat
    public Dictionary<string, object> Job;

    public void Write(string line)
    {
        string t = (Clock() - Started).TotalSeconds.ToString("0.000", CultureInfo.InvariantCulture);
        Log.Add(t + " " + line);
        if (LogSink != null) LogSink("[" + Id + "] " + line);
    }

    public bool PastDeadline { get { return Clock() > Deadline; } }
    public double SecondsLeft { get { return (Deadline - Clock()).TotalSeconds; } }

    // A path to read: relative ones are in the bridge folder.
    public string In(string path)
    {
        if (string.IsNullOrEmpty(path)) throw new BridgeException("a path is missing");
        string p = path.Replace('/', Path.DirectorySeparatorChar);
        return Path.GetFullPath(Path.IsPathRooted(p) ? p : Path.Combine(Root, p));
    }

    // A path to write: it must be inside the bridge folder (nothing outside it is ever written).
    public string Out(string path)
    {
        string full = In(path);
        if (!BridgeCore.IsInside(Root, full)) throw new BridgeException("'" + path + "' is outside the bridge folder; the bridge only writes inside " + Root);
        string dir = Path.GetDirectoryName(full);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        return full;
    }

    public void Made(string full)
    {
        long size = -1;
        try { if (File.Exists(full)) size = new FileInfo(full).Length; } catch { }
        Outputs.Add(BJson.Obj("path", Rel(full), "bytes", (double)size));
    }

    public string Rel(string full)
    {
        string r = Root.TrimEnd('\\', '/') + Path.DirectorySeparatorChar;
        return full.StartsWith(r, StringComparison.OrdinalIgnoreCase) ? full.Substring(r.Length) : full;
    }
}

// ---------------------------------------------------------------------------------------------- the core
// Looks for jobs, runs them through the host one action at a time, writes results, the heartbeat and the log.
public class BridgeCore
{
    public const string Version = "1";
    public BridgeConfig Config;
    public IBridgeHost Host;
    public Func<DateTime> Clock = delegate { return DateTime.Now; };
    public string Root, JobsDir, DoneDir, ResultsDir, LogPath, HeartbeatPath;
    public bool StopRequested;
    public string State = "starting";
    public string CurrentJob = "";
    public int JobsDone, JobsFailed;
    public string LastJob = "", LastError = "";
    public Action<string> StatusChanged;            // the window's status line
    DateTime lastHeartbeat = DateTime.MinValue;
    DateTime started;
    bool busy;
    // jobs whose file could not be read yet (still being written): first seen at
    Dictionary<string, DateTime> unreadable = new Dictionary<string, DateTime>();
    public double UnreadableGraceSeconds = 30;

    public BridgeCore(BridgeConfig config, IBridgeHost host)
    {
        Config = config;
        Host = host;
    }

    public static bool IsInside(string root, string full)
    {
        string r = Path.GetFullPath(root).TrimEnd('\\', '/') + Path.DirectorySeparatorChar;
        string f = Path.GetFullPath(full);
        return f.StartsWith(r, StringComparison.OrdinalIgnoreCase);
    }

    public void Start()
    {
        started = Clock();
        Root = Path.GetFullPath(Config.Folder);
        JobsDir = Path.Combine(Root, "jobs");
        DoneDir = Path.Combine(JobsDir, "done");
        ResultsDir = Path.Combine(Root, "results");
        LogPath = Path.Combine(Root, "bridge.log");
        HeartbeatPath = Path.Combine(Root, "heartbeat.json");
        Directory.CreateDirectory(JobsDir);
        Directory.CreateDirectory(DoneDir);
        Directory.CreateDirectory(ResultsDir);
        // a STOP left over from last time would stop the bridge at once
        try { File.Delete(Path.Combine(JobsDir, "STOP")); } catch { }
        Log("Reno Bridge " + Version + " started on " + SafeDescribe() + ", watching " + JobsDir);
        foreach (string n in Config.Notes) Log(n);
        // a job that was running when the bridge last stopped (VEGAS closed or crashed during it) is not tried again
        try
        {
            foreach (string marker in Directory.GetFiles(JobsDir, "*.running"))
            {
                string id = Path.GetFileName(marker);
                id = id.Substring(0, id.Length - ".running".Length);
                string why = "the bridge stopped while doing this job last time (VEGAS was closed or crashed); it is not tried again";
                Log("job " + id + ": " + why);
                WriteAtomic(Path.Combine(ResultsDir, id + ".result.json"), BJson.Write(BJson.Obj("id", id, "ok", false, "error", why, "crashed", true)));
                string job = Path.Combine(JobsDir, id + ".job.json");
                try
                {
                    if (File.Exists(job))
                    {
                        string dest = Path.Combine(DoneDir, id + ".job.json");
                        if (File.Exists(dest)) File.Delete(dest);
                        File.Move(job, dest);
                    }
                }
                catch { }
                try { File.Delete(marker); } catch { }
                JobsFailed++;
                LastError = id + ": " + why;
            }
        }
        catch (Exception e) { Log("looking for interrupted jobs: " + e.Message); }
        State = "idle";
        Heartbeat(true);
    }

    string SafeDescribe()
    {
        try { return Host.Describe(); } catch (Exception e) { return "(host: " + e.Message + ")"; }
    }

    public void Log(string line)
    {
        string text = Clock().ToString("yyyy-MM-dd HH:mm:ss.fff", CultureInfo.InvariantCulture) + "  " + line;
        try
        {
            if (File.Exists(LogPath) && new FileInfo(LogPath).Length > 5 * 1024 * 1024)
            {
                string old = LogPath + ".1";
                if (File.Exists(old)) File.Delete(old);
                File.Move(LogPath, old);
            }
            File.AppendAllText(LogPath, text + Environment.NewLine, Encoding.UTF8);
        }
        catch { }
    }

    void Status(string s)
    {
        if (StatusChanged != null) { try { StatusChanged(s); } catch { } }
    }

    // Rewrites heartbeat.json if it is due (or 'force').
    public void Heartbeat(bool force)
    {
        DateTime now = Clock();
        if (!force && (now - lastHeartbeat).TotalSeconds < Config.HeartbeatSeconds) return;
        lastHeartbeat = now;
        Dictionary<string, object> hb = BJson.Obj(
            "time", now.ToString("yyyy-MM-ddTHH:mm:ss", CultureInfo.InvariantCulture),
            "utc", now.ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ", CultureInfo.InvariantCulture),
            "state", State,
            "job", CurrentJob,
            "jobs_done", (double)JobsDone,
            "jobs_failed", (double)JobsFailed,
            "last_job", LastJob,
            "last_error", LastError,
            "running_since", started.ToString("yyyy-MM-ddTHH:mm:ss", CultureInfo.InvariantCulture),
            "bridge_version", Version,
            "host", SafeDescribe(),
            "poll_seconds", Config.PollSeconds);
        WriteAtomic(HeartbeatPath, BJson.Write(hb));
    }

    public static void WriteAtomic(string path, string text)
    {
        try
        {
            string tmp = path + ".tmp";
            File.WriteAllText(tmp, text, new UTF8Encoding(false));
            if (File.Exists(path)) File.Delete(path);
            File.Move(tmp, path);
        }
        catch
        {
            try { File.WriteAllText(path, text, new UTF8Encoding(false)); } catch { }
        }
    }

    // The job files waiting, in name order.
    public List<string> PendingJobs()
    {
        List<string> list = new List<string>();
        try { list.AddRange(Directory.GetFiles(JobsDir, "*.job.json")); } catch { }
        list.Sort(StringComparer.OrdinalIgnoreCase);
        return list;
    }

    // Called by the timer: heartbeat, STOP, and at most one job. Returns false once the bridge should stop.
    public bool Tick()
    {
        if (busy) return !StopRequested;        // (a timer tick while a job runs, e.g. during a render)
        busy = true;
        try
        {
            Heartbeat(false);
            if (File.Exists(Path.Combine(JobsDir, "STOP")))
            {
                Log("STOP file found: stopping");
                try { File.Delete(Path.Combine(JobsDir, "STOP")); } catch { }
                StopRequested = true;
            }
            if (StopRequested) return false;
            if (File.Exists(Path.Combine(JobsDir, "PAUSE")))
            {
                if (State != "paused") { State = "paused"; Status("paused (PAUSE file in the jobs folder)"); Heartbeat(true); }
                return true;
            }
            if (State == "paused") { State = "idle"; Heartbeat(true); }
            foreach (string f in PendingJobs())
            {
                if (StopRequested) break;
                if (RunJobFile(f)) break;               // one job per tick, so STOP and the window stay responsive
            }
            return !StopRequested;
        }
        catch (Exception e)
        {
            Log("error in the bridge itself: " + e);
            return !StopRequested;
        }
        finally
        {
            busy = false;
        }
    }

    public void Stop()
    {
        StopRequested = true;
        State = "stopped";
        Log("stopped");
        Heartbeat(true);
    }

    static string IdOf(string file)
    {
        string n = Path.GetFileName(file);
        return n.EndsWith(".job.json", StringComparison.OrdinalIgnoreCase) ? n.Substring(0, n.Length - 9) : Path.GetFileNameWithoutExtension(n);
    }

    // Runs one job file. Returns false when it was left for later (still being written).
    public bool RunJobFile(string file)
    {
        string id = IdOf(file);
        Dictionary<string, object> job = null;
        string readError = null;
        try
        {
            object parsed = BJson.Parse(File.ReadAllText(file, Encoding.UTF8));
            job = parsed as Dictionary<string, object>;
            if (job == null) readError = "the job is not a JSON object";
        }
        catch (Exception e) { readError = e.Message; }
        if (readError != null)
        {
            // perhaps it is still being written: give it a while before calling it bad
            DateTime first;
            DateTime now = Clock();
            if (!unreadable.TryGetValue(file, out first)) { unreadable[file] = now; first = now; }
            if ((now - first).TotalSeconds < UnreadableGraceSeconds) return false;
        }
        unreadable.Remove(file);

        BridgeJobContext ctx = new BridgeJobContext();
        ctx.Id = id;
        ctx.Root = Root;
        ctx.Clock = Clock;
        ctx.Started = Clock();
        ctx.Job = job;
        ctx.LogSink = Log;
        ctx.Alive = delegate { Heartbeat(false); };
        double timeout = job != null ? BJson.Num(job, "timeout", Config.JobTimeoutSeconds) : Config.JobTimeoutSeconds;
        if (timeout <= 0) timeout = Config.JobTimeoutSeconds;
        ctx.Deadline = ctx.Started.AddSeconds(timeout);

        // marks the job as running until its result is written (see Start)
        string runningMarker = Path.Combine(JobsDir, id + ".running");
        try { File.WriteAllText(runningMarker, Clock().ToString("yyyy-MM-ddTHH:mm:ss", CultureInfo.InvariantCulture)); } catch { }
        CurrentJob = id;
        State = "working";
        Status("job " + id);
        Heartbeat(true);
        Log("job " + id + " started");

        List<object> actionResults = new List<object>();
        string error = readError != null ? "the job file could not be read: " + readError : null;
        if (job != null)
        {
            bool keepGoing = BJson.Bool(job, "continue_on_error", false);
            List<Dictionary<string, object>> actions = new List<Dictionary<string, object>>();
            // "project" at the top level: open it first
            if (BJson.Has(job, "project")) actions.Add(BJson.Obj("do", "open", "project", BJson.Get(job, "project")));
            foreach (object a in BJson.Items(job, "actions"))
            {
                Dictionary<string, object> d = a as Dictionary<string, object>;
                if (d == null) { error = "an action is not a JSON object"; break; }
                actions.Add(d);
            }
            if (error == null && actions.Count == 0) error = "the job has no actions";
            if (error == null)
            {
                foreach (Dictionary<string, object> action in actions)
                {
                    string what = BJson.Text(action, "do", "");
                    Dictionary<string, object> ar = BJson.Obj("do", what);
                    actionResults.Add(ar);
                    if (ctx.PastDeadline)
                    {
                        ar["ok"] = false;
                        ar["error"] = "skipped: the job ran past its time limit (" + timeout + " s)";
                        if (error == null) error = "the job ran past its time limit (" + timeout + " s)";
                        continue;
                    }
                    if (error != null && !keepGoing) { ar["ok"] = false; ar["error"] = "skipped after an earlier error"; continue; }
                    DateTime t0 = Clock();
                    ctx.Info = new Dictionary<string, object>();
                    int outputsBefore = ctx.Outputs.Count;
                    ctx.Write("action " + what);
                    try
                    {
                        RunAction(what, action, ctx);
                        if (ctx.PastDeadline) throw new BridgeException("it ran past the job's time limit (" + timeout + " s)");
                        ar["ok"] = true;
                    }
                    catch (Exception e)
                    {
                        string msg = Describe(e);
                        ar["ok"] = false;
                        ar["error"] = msg;
                        ctx.Write("action " + what + " failed: " + msg);
                        if (error == null) error = what + ": " + msg;
                    }
                    ar["seconds"] = Math.Round((Clock() - t0).TotalSeconds, 3);
                    if (ctx.Outputs.Count > outputsBefore) ar["outputs"] = ctx.Outputs.GetRange(outputsBefore, ctx.Outputs.Count - outputsBefore);
                    if (ctx.Info.Count > 0) ar["info"] = ctx.Info;
                    Heartbeat(false);
                }
            }
        }
        try { Host.AfterJob(ctx); }
        catch (Exception e) { ctx.Write("after the job: " + Describe(e)); }

        DateTime end = Clock();
        Dictionary<string, object> result = BJson.Obj(
            "id", id,
            "ok", error == null,
            "error", error,
            "started", ctx.Started.ToString("yyyy-MM-ddTHH:mm:ss.fff", CultureInfo.InvariantCulture),
            "finished", end.ToString("yyyy-MM-ddTHH:mm:ss.fff", CultureInfo.InvariantCulture),
            "seconds", Math.Round((end - ctx.Started).TotalSeconds, 3),
            "host", SafeDescribe(),
            "bridge_version", Version,
            "actions", actionResults,
            "outputs", ctx.Outputs,
            "log", ctx.Log);
        if (job != null && BJson.Has(job, "tag")) result["tag"] = BJson.Get(job, "tag");
        WriteAtomic(Path.Combine(ResultsDir, id + ".result.json"), BJson.Write(result));
        // the job goes to done\ (replacing one of the same name done before)
        try
        {
            string dest = Path.Combine(DoneDir, Path.GetFileName(file));
            if (File.Exists(dest)) File.Delete(dest);
            File.Move(file, dest);
        }
        catch (Exception e)
        {
            Log("could not move " + file + " to done: " + e.Message + "; deleting it");
            try { File.Delete(file); } catch { }
        }
        try { File.Delete(runningMarker); } catch { }
        if (error == null) JobsDone++; else { JobsFailed++; LastError = id + ": " + error; }
        LastJob = id;
        Log("job " + id + (error == null ? " done" : " FAILED: " + error) + " in " + Math.Round((end - ctx.Started).TotalSeconds, 1) + " s");
        CurrentJob = "";
        State = "idle";
        Status("last job: " + id + (error == null ? " (ok)" : " (failed)") + " - " + JobsDone + " done, " + JobsFailed + " failed");
        Heartbeat(true);
        return true;
    }

    static string Describe(Exception e)
    {
        if (e is TargetInvocationException && e.InnerException != null) e = e.InnerException;
        if (e is BridgeException) return e.Message;
        return e.GetType().Name + ": " + e.Message + (e.StackTrace != null ? " | " + e.StackTrace.Replace("\r", "").Replace("\n", " | ") : "");
    }

    // The actions the core does itself; everything else goes to the host.
    void RunAction(string what, Dictionary<string, object> action, BridgeJobContext ctx)
    {
        switch (what)
        {
            case "":
                throw new BridgeException("the action has no \"do\"");
            case "ping":
                ctx.Info["host"] = SafeDescribe();
                ctx.Info["root"] = Root;
                return;
            case "wait":
                {
                    // waits (in short steps, keeping the heartbeat) - for tests of the time limit
                    double secs = Math.Min(BJson.Num(action, "seconds", 1), 3600);
                    DateTime until = Clock().AddSeconds(secs);
                    while (Clock() < until && !ctx.PastDeadline) { System.Threading.Thread.Sleep(50); ctx.Alive(); }
                    return;
                }
            case "write_text":
                {
                    string path = ctx.Out(BJson.Text(action, "out", null));
                    File.WriteAllText(path, BJson.Text(action, "text", ""), new UTF8Encoding(false));
                    ctx.Made(path);
                    return;
                }
            case "list_files":
                {
                    // what is in a folder of the bridge (to check that copied media arrived)
                    string dir = ctx.In(BJson.Text(action, "dir", "."));
                    List<object> files = new List<object>();
                    if (Directory.Exists(dir))
                        foreach (string f in Directory.GetFiles(dir, BJson.Text(action, "pattern", "*"), BJson.Bool(action, "recursive", false) ? SearchOption.AllDirectories : SearchOption.TopDirectoryOnly))
                            files.Add(BJson.Obj("path", ctx.Rel(f), "bytes", (double)new FileInfo(f).Length));
                    ctx.Info["files"] = files;
                    return;
                }
            default:
                Host.Run(what, action, ctx);
                return;
        }
    }
}

#if !BRIDGE_TEST
// ---------------------------------------------------------------------------------------------- VEGAS
public class VegasHost : IBridgeHost
{
    Vegas vegas;
    BridgeConfig config;
    public BridgeCore Core;
    // scripts compiled once (by path and time), and their assemblies
    Dictionary<string, Assembly> compiled = new Dictionary<string, Assembly>();
    BridgeJobContext current;

    public VegasHost(Vegas v, BridgeConfig c)
    {
        vegas = v;
        config = c;
        // a project with missing media would otherwise ask what to do
        vegas.MissingMedia += OnMissingMedia;
    }

    public void Detach()
    {
        try { vegas.MissingMedia -= OnMissingMedia; } catch { }
    }

    void OnMissingMedia(object sender, MissingMediaEventArgs args)
    {
        try
        {
            args.IgnoreMissingMedia = true;
            if (current != null)
            {
                current.Write("missing media (left offline): " + args.MissingMedia);
                List<object> l = current.Info.ContainsKey("missing_media") ? (List<object>)current.Info["missing_media"] : new List<object>();
                l.Add(args.MissingMedia);
                current.Info["missing_media"] = l;
            }
        }
        catch { }
    }

    public string Describe()
    {
        string v = "";
        try { v = vegas.Version; } catch { }
        string n = "VEGAS";
        try { n = vegas.LongAppName; } catch { }
        return n + " " + v;
    }

    public void AfterJob(BridgeJobContext ctx)
    {
        current = null;
        if (BJson.Bool(ctx.Job, "keep_open", false)) return;
        // nothing a job did stays open (and nothing is saved)
        try { vegas.NewProject(false, false); } catch (Exception e) { ctx.Write("closing the project: " + e.Message); }
    }

    public void Run(string what, Dictionary<string, object> a, BridgeJobContext ctx)
    {
        current = ctx;
        switch (what)
        {
            case "open": Open(a, ctx); break;
            case "new": NewProject(a, ctx); break;
            case "close": vegas.NewProject(false, false); break;
            case "render": Render(a, ctx); break;
            case "snapshot": Snapshot(a, ctx); break;
            case "project_info": ProjectInfo(a, ctx); break;
            case "export_reno": ExportReno(a, ctx); break;
            case "build": Build(a, ctx); break;
            case "save_as": SaveAs(a, ctx); break;
            case "list_templates": ListTemplates(a, ctx); break;
            case "list_plugins": ListPlugins(a, ctx); break;
            case "run_script": RunScript(a, ctx); break;
            case "media_info": MediaInfo(a, ctx); break;
            default: throw new BridgeException("unknown action '" + what + "'");
        }
    }

    // ---- projects
    void Open(Dictionary<string, object> a, BridgeJobContext ctx)
    {
        string path = ctx.In(BJson.Text(a, "project", null));
        if (!File.Exists(path)) throw new BridgeException("no such project: " + path);
        vegas.NewProject(false, false);         // the open project is closed without saving
        if (!vegas.OpenProject(path)) throw new BridgeException("VEGAS did not open " + path);
        try { vegas.WaitForIdle(); } catch { }
        ctx.Info["project"] = path;
        ctx.Info["length"] = vegas.Project.Length.ToMilliseconds() / 1000.0;
        ctx.Write("opened " + path);
    }

    void NewProject(Dictionary<string, object> a, BridgeJobContext ctx)
    {
        vegas.NewProject(false, false);
        Project p = vegas.Project;
        object s = BJson.Get(a, "settings");
        if (s == null) return;
        if (BJson.Has(s, "width")) p.Video.Width = (int)BJson.Num(s, "width", 1920);
        if (BJson.Has(s, "height")) p.Video.Height = (int)BJson.Num(s, "height", 1080);
        if (BJson.Has(s, "fps")) p.Video.FrameRate = BJson.Num(s, "fps", 30);
        if (BJson.Has(s, "sample_rate")) p.Audio.SampleRate = (uint)BJson.Num(s, "sample_rate", 48000);
    }

    void SaveAs(Dictionary<string, object> a, BridgeJobContext ctx)
    {
        string path = ctx.Out(BJson.Text(a, "out", null));
        if (!path.EndsWith(".veg", StringComparison.OrdinalIgnoreCase)) throw new BridgeException("save_as: the file must end in .veg");
        if (File.Exists(path)) File.Delete(path);
        if (!vegas.SaveProject(path)) throw new BridgeException("VEGAS did not save " + path);
        ctx.Made(path);
    }

    // ---- rendering
    static bool NameMatch(string have, string want, bool exact)
    {
        if (have == null) return false;
        return exact ? string.Equals(have, want, StringComparison.OrdinalIgnoreCase) : have.IndexOf(want, StringComparison.OrdinalIgnoreCase) >= 0;
    }

    Renderer FindRenderer(string want)
    {
        for (int pass = 0; pass < 2; pass++)
            foreach (Renderer r in vegas.Renderers)
            {
                if (NameMatch(r.Name, want, pass == 0) || NameMatch(r.FileTypeName, want, pass == 0)) return r;
                if (pass == 0 && r.FileExtension != null && string.Equals(r.FileExtension.TrimStart('*', '.'), want.TrimStart('*', '.'), StringComparison.OrdinalIgnoreCase)) return r;
            }
        return null;
    }

    RenderTemplate FindTemplate(Renderer r, string want)
    {
        if (want == null || want.Length == 0 || want == "project" || want == "match project")
        {
            // VEGAS's own choice of templates that match the project
            try
            {
                RenderTemplate[] m = r.Templates.FindProjectMatches(vegas.Project);
                if (m != null && m.Length > 0) return m[0];
            }
            catch { }
            if (want != null && want.Length > 0) return null;
        }
        if (want == null || want.Length == 0) return r.Templates.Count > 0 ? r.Templates[0] : null;
        for (int pass = 0; pass < 2; pass++)
            foreach (RenderTemplate t in r.Templates)
                if (NameMatch(t.Name, want, pass == 0)) return t;
        return null;
    }

    static List<string> Names(Dictionary<string, object> a, string key, string def)
    {
        List<string> l = new List<string>();
        object v = BJson.Get(a, key);
        if (v is string) l.Add((string)v);
        foreach (object o in BJson.Items(a, key)) if (o is string) l.Add((string)o);
        if (l.Count == 0) l.Add(def);
        return l;
    }

    void Render(Dictionary<string, object> a, BridgeJobContext ctx)
    {
        // "renderer" and "template" may each be a name or a list of names to try in turn
        List<string> rnames = Names(a, "renderer", "Video for Windows");
        List<string> tnames = Names(a, "template", "Default Template (uncompressed)");
        Renderer r = null;
        foreach (string rn in rnames) if ((r = FindRenderer(rn)) != null) break;
        if (r == null) throw new BridgeException("no renderer '" + string.Join("' or '", rnames.ToArray()) + "' (a list_templates job lists them)");
        RenderTemplate t = null;
        foreach (string tn in tnames) if ((t = FindTemplate(r, tn)) != null) break;
        if (t == null) throw new BridgeException("renderer '" + r.Name + "' has no template '" + string.Join("' or '", tnames.ToArray()) + "' (a list_templates job lists them)");
        if (!t.IsValid()) throw new BridgeException("template '" + t.Name + "' is not valid in this VEGAS");
        string outPath = ctx.Out(BJson.Text(a, "out", null));
        if (File.Exists(outPath))
        {
            if (!BJson.Bool(a, "overwrite", true)) throw new BridgeException(outPath + " is there already");
            File.Delete(outPath);
        }
        RenderArgs args = new RenderArgs(vegas.Project);
        args.OutputFile = outPath;
        args.RenderTemplate = t;
        double start = BJson.Num(a, "start", 0);
        double projLen = vegas.Project.Length.ToMilliseconds() / 1000.0;
        double length = BJson.Has(a, "length") ? BJson.Num(a, "length", 0) : BJson.Has(a, "end") ? BJson.Num(a, "end", 0) - start : projLen - start;
        if (length <= 0) throw new BridgeException("nothing to render (length " + length + " s; the project is " + projLen + " s long)");
        args.Start = Timecode.FromSeconds(start);
        args.Length = Timecode.FromSeconds(length);
        args.UseSelection = false;
        args.StretchToFill = BJson.Bool(a, "stretch", false);
        try { args.ShowOpenButtonsOnComplete = false; } catch { }
        if (BJson.Has(a, "preview_quality")) SetPreviewQuality(BJson.Text(a, "preview_quality", "best"));
        ctx.Info["renderer"] = r.Name;
        ctx.Info["template"] = t.Name;
        ctx.Info["template_size"] = new List<object> { (double)t.VideoWidth, (double)t.VideoHeight };
        ctx.Info["template_fps"] = t.VideoFrameRate;
        ctx.Info["start"] = start;
        ctx.Info["length"] = length;
        ctx.Write("render " + r.Name + " / " + t.Name + " -> " + outPath);

        int lastPercent = -1;
        RenderStatusEventHandler progress = delegate(object sender, RenderStatusEventArgs e)
        {
            try
            {
                if (e.PercentComplete != lastPercent) { lastPercent = e.PercentComplete; ctx.Alive(); }
                if (ctx.PastDeadline && !args.CancelRender) { ctx.Write("time limit reached: cancelling the render"); args.CancelRender = true; }
            }
            catch { }
        };
        vegas.RenderProgress += progress;
        RenderStatus st;
        try { st = vegas.Render(args); }
        finally { vegas.RenderProgress -= progress; }
        ctx.Info["status"] = st.ToString();
        if (st != RenderStatus.Complete)
            throw new BridgeException("the render ended " + st + (ctx.PastDeadline ? " (time limit)" : ""));
        ctx.Made(outPath);
        // VEGAS writes a peak file (.sfk) beside rendered sound: not an output
    }

    void SetPreviewQuality(string q)
    {
        PreviewVideoProperties pv = vegas.Project.Preview;
        pv.RenderQuality = q == "draft" ? VideoRenderQuality.Draft : q == "preview" ? VideoRenderQuality.Preview : q == "good" ? VideoRenderQuality.Good : VideoRenderQuality.Best;
        pv.FullSize = true;
        pv.PreviewSize = VideoPreviewSize.Full;
    }

    void Snapshot(Dictionary<string, object> a, BridgeJobContext ctx)
    {
        // the preview's picture: at Best, full size, unless the job says otherwise
        SetPreviewQuality(BJson.Text(a, "quality", "best"));
        string pattern = BJson.Text(a, "out", "snapshots/" + ctx.Id + "_{i}.png");
        List<object> times = BJson.Items(a, "times");
        if (times.Count == 0) throw new BridgeException("snapshot: no times");
        int i = 0;
        foreach (object o in times)
        {
            double t = o is double ? (double)o : 0;
            string name = pattern.Replace("{i}", i.ToString("000", CultureInfo.InvariantCulture))
                                 .Replace("{t}", t.ToString("0.000", CultureInfo.InvariantCulture))
                                 .Replace("{f}", ((long)Math.Round(t * vegas.Project.Video.FrameRate)).ToString("000000", CultureInfo.InvariantCulture));
            string path = ctx.Out(name);
            if (File.Exists(path)) File.Delete(path);
            Timecode at = Timecode.FromSeconds(t);
            vegas.Transport.CursorPosition = at;
            RenderStatus st = vegas.SaveSnapshot(path, ImageFileFormat.PNG, at);
            if (st != RenderStatus.Complete || !File.Exists(path)) throw new BridgeException("snapshot at " + t + " s: " + st);
            ctx.Made(path);
            i++;
            ctx.Alive();
            if (ctx.PastDeadline) throw new BridgeException("time limit reached");
        }
    }

    // ---- information
    static string Secs(Timecode t) { return t == null ? "" : (t.ToMilliseconds() / 1000.0).ToString("0.######", CultureInfo.InvariantCulture); }
    static double S(Timecode t) { return t == null ? 0 : t.ToMilliseconds() / 1000.0; }

    void ProjectInfo(Dictionary<string, object> a, BridgeJobContext ctx)
    {
        Project p = vegas.Project;
        Dictionary<string, object> o = BJson.Obj(
            "path", p.FilePath,
            "length", S(p.Length),
            "width", (double)p.Video.Width, "height", (double)p.Video.Height, "fps", p.Video.FrameRate,
            "field_order", p.Video.FieldOrder.ToString(), "pixel_aspect", p.Video.PixelAspectRatio,
            "pixel_format", p.Video.PixelFormat.ToString(), "render_quality", p.Video.RenderQuality.ToString(),
            "sample_rate", (double)p.Audio.SampleRate, "bit_depth", (double)p.Audio.BitDepth);
        List<object> media = new List<object>();
        foreach (object mo0 in p.MediaPool)
        {
            // (the media pool is a dictionary: its items may come as entries)
            Media m = mo0 as Media;
            if (m == null && mo0 is DictionaryEntry) m = ((DictionaryEntry)mo0).Value as Media;
            if (m == null) continue;
            Dictionary<string, object> mo = BJson.Obj("path", m.FilePath, "length", S(m.Length), "offline", m.IsOffline(), "generated", m.IsGenerated());
            if (m.IsGenerated() && m.Generator != null && m.Generator.PlugIn != null) mo["generator"] = m.Generator.PlugIn.UniqueID;
            media.Add(mo);
        }
        o["media"] = media;
        List<object> tracks = new List<object>();
        foreach (Track t in p.Tracks)
        {
            Dictionary<string, object> to = BJson.Obj("type", t.IsVideo() ? "video" : "audio", "name", t.Name, "mute", t.Mute, "solo", t.Solo);
            if (t.IsVideo())
            {
                VideoTrack vt = (VideoTrack)t;
                to["composite_mode"] = vt.CompositeMode.ToString();
                to["composite_level"] = (double)vt.CompositeLevel;
            }
            else
            {
                AudioTrack at = (AudioTrack)t;
                to["volume"] = (double)at.Volume;
                to["pan"] = (double)at.PanX;
            }
            List<object> fx = new List<object>();
            foreach (Effect e in t.Effects) fx.Add(EffectInfo(e));
            to["effects"] = fx;
            List<object> evs = new List<object>();
            foreach (TrackEvent ev in t.Events)
            {
                Dictionary<string, object> eo = BJson.Obj("start", S(ev.Start), "length", S(ev.Length), "name", ev.Name,
                    "mute", ev.Mute, "loop", ev.Loop, "rate", ev.PlaybackRate,
                    "fade_in", S(ev.FadeIn.Length), "fade_in_curve", ev.FadeIn.Curve.ToString(),
                    "fade_out", S(ev.FadeOut.Length), "fade_out_curve", ev.FadeOut.Curve.ToString());
                Take tk = ev.ActiveTake;
                if (tk != null)
                {
                    eo["offset"] = S(tk.Offset);
                    try { eo["media"] = tk.Media != null ? tk.Media.FilePath : null; } catch { }
                }
                Effect tr = ev.FadeIn.Transition as Effect;       // (VEGAS gives the transition as an object)
                if (tr != null) eo["transition"] = EffectInfo(tr);
                VideoEvent ve = ev as VideoEvent;
                if (ve != null)
                {
                    List<object> vfx = new List<object>();
                    foreach (Effect e in ve.Effects) vfx.Add(EffectInfo(e));
                    eo["effects"] = vfx;
                    eo["pan_crop_keys"] = (double)ve.VideoMotion.Keyframes.Count;
                }
                evs.Add(eo);
            }
            to["events"] = evs;
            tracks.Add(to);
        }
        o["tracks"] = tracks;
        Finish(a, ctx, o, "info");
    }

    static Dictionary<string, object> EffectInfo(Effect e)
    {
        Dictionary<string, object> d = BJson.Obj("name", e.PlugIn != null ? e.PlugIn.Name : e.Description, "id", e.PlugIn != null ? e.PlugIn.UniqueID : "", "bypass", e.Bypass);
        if (e.IsOFX && e.OFXEffect != null)
        {
            Dictionary<string, object> ps = new Dictionary<string, object>();
            foreach (OFXParameter p in e.OFXEffect.Parameters) ps[p.Name] = ParamValue(p);
            d["params"] = ps;
        }
        return d;
    }

    // An OpenFX parameter's value, through reflection (each parameter class has its own Value type).
    static object ParamValue(OFXParameter p)
    {
        try
        {
            PropertyInfo best = null;
            foreach (PropertyInfo pi in p.GetType().GetProperties(BindingFlags.Public | BindingFlags.Instance))
                if (pi.Name == "Value" && pi.CanRead && pi.GetIndexParameters().Length == 0 && (best == null || pi.DeclaringType.IsSubclassOf(best.DeclaringType))) best = pi;
            if (best == null) return null;
            object v = best.GetValue(p, null);
            if (v == null) return null;
            if (v is double || v is int || v is bool || v is string || v is float) return v is float ? (double)(float)v : v is int ? (double)(int)v : v;
            OFXChoice c = v as OFXChoice;
            if (c != null) return c.Name;
            // structures (colours, 2D/3D points): their public fields
            Dictionary<string, object> fields = new Dictionary<string, object>();
            foreach (FieldInfo f in v.GetType().GetFields(BindingFlags.Public | BindingFlags.Instance))
            {
                object fv = f.GetValue(v);
                fields[f.Name] = (fv is double || fv is int || fv is float || fv is bool) ? fv : (fv == null ? null : fv.ToString());
            }
            foreach (PropertyInfo pi in v.GetType().GetProperties(BindingFlags.Public | BindingFlags.Instance))
            {
                if (fields.ContainsKey(pi.Name) || pi.GetIndexParameters().Length > 0) continue;
                object fv = pi.GetValue(v, null);
                fields[pi.Name] = (fv is double || fv is int || fv is float || fv is bool) ? fv : (fv == null ? null : fv.ToString());
            }
            return fields.Count > 0 ? (object)fields : v.ToString();
        }
        catch (Exception e) { return "(" + e.Message + ")"; }
    }

    // Writes 'o' to the action's "out" file if it names one, else puts it in the result.
    static void Finish(Dictionary<string, object> a, BridgeJobContext ctx, object o, string key)
    {
        string outName = BJson.Text(a, "out", null);
        if (outName == null) { ctx.Info[key] = o; return; }
        string path = ctx.Out(outName);
        BridgeCore.WriteAtomic(path, BJson.Write(o));
        ctx.Made(path);
    }

    void MediaInfo(Dictionary<string, object> a, BridgeJobContext ctx)
    {
        string path = ctx.In(BJson.Text(a, "file", null));
        ctx.Info["media_info"] = vegas.MediaInfo(path);
    }

    void ListTemplates(Dictionary<string, object> a, BridgeJobContext ctx)
    {
        string filter = BJson.Text(a, "filter", "");
        List<object> list = new List<object>();
        foreach (Renderer r in vegas.Renderers)
        {
            List<object> ts = new List<object>();
            try
            {
                foreach (RenderTemplate t in r.Templates)
                {
                    if (filter.Length > 0 && !NameMatch(r.Name + " " + t.Name, filter, false)) continue;
                    ts.Add(BJson.Obj("name", t.Name, "description", t.Description, "guid", t.TemplateGuid.ToString(),
                        "width", (double)t.VideoWidth, "height", (double)t.VideoHeight, "fps", t.VideoFrameRate,
                        "video_streams", (double)t.VideoStreamCount, "audio_streams", (double)t.AudioStreamCount,
                        "sample_rate", (double)t.AudioSampleRate, "channels", (double)t.AudioChannelCount, "bits", (double)t.AudioBitsPerSample,
                        "valid", t.IsValid()));
                }
            }
            catch (Exception e) { ts.Add(BJson.Obj("error", e.Message)); }
            if (filter.Length > 0 && ts.Count == 0) continue;
            list.Add(BJson.Obj("name", r.Name, "file_type", r.FileTypeName, "extension", r.FileExtension, "class_id", r.ClassID.ToString(),
                "audio", r.SupportsAudio, "video", r.SupportsVideo, "templates", ts));
        }
        Finish(a, ctx, list, "renderers");
    }

    // ---- plug-ins
    void ListPlugins(Dictionary<string, object> a, BridgeJobContext ctx)
    {
        bool withParams = BJson.Bool(a, "params", false);
        Dictionary<string, object> o = new Dictionary<string, object>();
        o["video_fx"] = Nodes(vegas.VideoFX, withParams ? "fx" : null, ctx);
        o["audio_fx"] = Nodes(vegas.AudioFX, null, ctx);
        o["transitions"] = Nodes(vegas.Transitions, null, ctx);
        o["generators"] = Nodes(vegas.Generators, withParams ? "generator" : null, ctx);
        o["compositors"] = Nodes(vegas.VideoCompositeFX, null, ctx);
        Finish(a, ctx, o, "plugins");
    }

    List<object> Nodes(PlugInNode root, string paramsHow, BridgeJobContext ctx)
    {
        List<object> list = new List<object>();
        if (root == null) return list;
        foreach (PlugInNode n in root)
        {
            Dictionary<string, object> d = BJson.Obj("name", n.Name);
            try
            {
                if (n.IsContainer) { d["children"] = Nodes(n, paramsHow, ctx); list.Add(d); continue; }
                d["id"] = n.UniqueID;
                d["class_id"] = n.ClassID.ToString();
                d["group"] = n.Group;
                d["ofx"] = n.IsOFX;
                d["disabled"] = n.IsDisabled;
                List<object> presets = new List<object>();
                try { foreach (EffectPreset p in n.Presets) presets.Add(p.Name); } catch { }
                d["presets"] = presets;
                if (paramsHow != null && n.IsOFX && !n.IsDisabled) d["params"] = Params(n, paramsHow);
                ctx.Alive();
            }
            catch (Exception e) { d["error"] = e.Message; }
            list.Add(d);
        }
        return list;
    }

    // An OpenFX plug-in's parameters, read from an instance made for the purpose (on the video output, or as
    // generated media) and taken away again.
    List<object> Params(PlugInNode n, string how)
    {
        List<object> list = new List<object>();
        Project p = vegas.Project;
        Effect fx = null;
        Media m = null;
        try
        {
            if (how == "generator") { m = Media.CreateInstance(p, n); fx = m.Generator; }
            else fx = p.VideoBus.Effects.AddEffect(n);
            if (fx == null || fx.OFXEffect == null) return list;
            foreach (OFXParameter q in fx.OFXEffect.Parameters)
            {
                Dictionary<string, object> d = BJson.Obj("name", q.Name, "label", q.Label, "type", q.ParameterType.ToString(), "default", ParamValue(q));
                OFXChoiceParameter cq = q as OFXChoiceParameter;
                if (cq != null && cq.Choices != null)
                {
                    List<object> cs = new List<object>();
                    foreach (OFXChoice c in cq.Choices) cs.Add(c.Name);
                    d["choices"] = cs;
                }
                if (q.Secret) d["secret"] = true;
                list.Add(d);
            }
        }
        catch (Exception e) { list.Add(BJson.Obj("error", e.Message)); }
        finally
        {
            try { if (fx != null && m == null) p.VideoBus.Effects.Remove(fx); } catch { }
            try { if (m != null) p.MediaPool.Remove(m.KeyString); } catch { }
        }
        return list;
    }

    // ---- other scripts: compiled as VEGAS compiles scripts (C#, in memory)
    string FindScript(string name, BridgeJobContext ctx)
    {
        if (Path.IsPathRooted(name) && File.Exists(name)) return name;
        string inRoot = Path.Combine(ctx.Root, name);
        if (File.Exists(inRoot)) return inRoot;
        foreach (string d in config.ScriptDirs)
        {
            try
            {
                string p = Path.Combine(d, name);
                if (File.Exists(p)) return p;
            }
            catch { }
        }
        throw new BridgeException("the script '" + name + "' was not found (looked in the bridge folder and " + string.Join("; ", config.ScriptDirs.ToArray()) + ")");
    }

    Assembly Compile(string path, BridgeJobContext ctx)
    {
        string key = path + "|" + File.GetLastWriteTimeUtc(path).Ticks;
        Assembly asm;
        if (compiled.TryGetValue(key, out asm)) return asm;
        CompilerParameters cp = new CompilerParameters();
        cp.GenerateInMemory = true;
        cp.GenerateExecutable = false;
        cp.TreatWarningsAsErrors = false;
        foreach (string r in new string[] { "System.dll", "System.Core.dll", "System.Data.dll", "System.Drawing.dll", "System.Windows.Forms.dll", "System.Xml.dll" })
            cp.ReferencedAssemblies.Add(r);
        cp.ReferencedAssemblies.Add(typeof(Vegas).Assembly.Location);
        CompilerResults res;
        using (CodeDomProvider provider = CodeDomProvider.CreateProvider("CSharp"))
        {
            res = provider.CompileAssemblyFromFile(cp, path);
        }
        if (res.Errors.HasErrors)
        {
            StringBuilder sb = new StringBuilder();
            int n = 0;
            foreach (CompilerError e in res.Errors)
                if (!e.IsWarning && n++ < 20) sb.Append("(" + e.Line + "," + e.Column + "): " + e.ErrorText + "\n");
            throw new BridgeException(Path.GetFileName(path) + " did not compile:\n" + sb.ToString());
        }
        asm = res.CompiledAssembly;
        compiled[key] = asm;
        ctx.Write("compiled " + path);
        return asm;
    }

    void RunScript(Dictionary<string, object> a, BridgeJobContext ctx)
    {
        string path = FindScript(BJson.Text(a, "file", null), ctx);
        string args = BJson.Text(a, "args", "");
        if (BJson.Text(a, "engine", "") == "vegas")
        {
            // VEGAS's own way (it may refuse while this script runs; then use the default)
            vegas.RunScriptFile(path, args);
            return;
        }
        Assembly asm = Compile(path, ctx);
        foreach (Type t in asm.GetTypes())
        {
            MethodInfo mi = t.GetMethod("FromVegas", new Type[] { typeof(Vegas) });
            if (mi == null) continue;
            object inst = mi.IsStatic ? null : Activator.CreateInstance(t);
            mi.Invoke(inst, new object[] { vegas });
            ctx.Info["entry"] = t.FullName;
            return;
        }
        throw new BridgeException(Path.GetFileName(path) + " has no FromVegas(Vegas) method");
    }

    // ---- Reno projects, through the two scripts beside this one (Export to Reno.cs, Import Reno Project.cs)
    void ExportReno(Dictionary<string, object> a, BridgeJobContext ctx)
    {
        Assembly asm = Compile(FindScript(BJson.Text(a, "script", "Export to Reno.cs"), ctx), ctx);
        Type t = asm.GetType("RenoExporter");
        if (t == null) throw new BridgeException("Export to Reno.cs has no RenoExporter");
        object ex = Activator.CreateInstance(t, new object[] { vegas });
        string outName = BJson.Text(a, "out", null);
        string target = outName != null ? ctx.Out(outName) : null;
        if (target == null)
        {
            string fp = vegas.Project.FilePath;
            if (string.IsNullOrEmpty(fp)) throw new BridgeException("export_reno: give \"out\" (the project has no file)");
            target = ctx.Out(ctx.Rel(Path.ChangeExtension(fp, ".reno")));
        }
        FieldInfo tf = t.GetField("Target");
        if (tf == null) throw new BridgeException("this Export to Reno.cs cannot be told where to write (no Target field): use the one from Reno's tools\\vegas");
        tf.SetValue(ex, target);
        object written = t.GetMethod("Run").Invoke(ex, null);
        if (written == null) throw new BridgeException("Export to Reno wrote nothing");
        ctx.Made((string)written);
        Problems(t, ex, ctx);
    }

    static void Problems(Type t, object o, BridgeJobContext ctx)
    {
        FieldInfo pf = t.GetField("Problems");
        IList probs = pf != null ? pf.GetValue(o) as IList : null;
        if (probs == null || probs.Count == 0) return;
        List<object> l = new List<object>();
        foreach (object p in probs) l.Add(p == null ? "" : p.ToString());
        ctx.Info["problems"] = l;
    }

    // Builds a project from a Reno project file (.reno: Project::toJson) in a new VEGAS project and saves it as a .veg
    // in the bridge folder: VEGAS writes the file itself.
    void Build(Dictionary<string, object> a, BridgeJobContext ctx)
    {
        string reno = ctx.In(BJson.Text(a, "reno", null));
        if (!File.Exists(reno)) throw new BridgeException("no such file: " + reno);
        string save = BJson.Text(a, "save_as", null);
        string savePath = save != null ? ctx.Out(save) : null;
        if (savePath != null && !savePath.EndsWith(".veg", StringComparison.OrdinalIgnoreCase)) throw new BridgeException("save_as must end in .veg");
        Assembly asm = Compile(FindScript(BJson.Text(a, "script", "Import Reno Project.cs"), ctx), ctx);
        Type jt = asm.GetType("Json"), it = asm.GetType("RenoImporter");
        if (jt == null || it == null) throw new BridgeException("Import Reno Project.cs has no Json / RenoImporter");
        object parsed = jt.GetMethod("Parse").Invoke(null, new object[] { File.ReadAllText(reno, Encoding.UTF8) });
        vegas.NewProject(false, false);
        object im = Activator.CreateInstance(it, new object[] { vegas, Path.GetDirectoryName(reno) });
        using (UndoBlock undo = new UndoBlock("Reno Bridge: build"))
        {
            it.GetMethod("Run").Invoke(im, new object[] { parsed });
        }
        try { ctx.Info["summary"] = it.GetMethod("Summary").Invoke(im, null); } catch { }
        Problems(it, im, ctx);
        if (savePath != null)
        {
            if (File.Exists(savePath)) File.Delete(savePath);
            if (!vegas.SaveProject(savePath)) throw new BridgeException("VEGAS did not save " + savePath);
            ctx.Made(savePath);
        }
        ctx.Info["length"] = S(vegas.Project.Length);
    }
}

// ---------------------------------------------------------------------------------------------- the window
// A small modal window whose timer drives the bridge: VEGAS keeps running its own work (renders show their
// progress window) while the script waits here. Stop ends it; so does a STOP file in the jobs folder.
public class BridgeForm : Form
{
    BridgeCore core;
    BridgeConfig config;
    Label title, status;
    Button stopButton, openButton;
    Timer timer;
    bool closing;

    [DllImport("kernel32.dll")]
    static extern uint SetThreadExecutionState(uint flags);
    const uint ES_CONTINUOUS = 0x80000000, ES_SYSTEM_REQUIRED = 0x00000001;

    public BridgeForm(BridgeCore c, BridgeConfig cfg)
    {
        core = c;
        config = cfg;
        Text = "Reno Bridge";
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        MinimizeBox = true;
        ShowInTaskbar = true;
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new System.Drawing.Size(520, 130);
        title = new Label();
        title.AutoSize = false;
        title.SetBounds(12, 10, 496, 40);
        title.Text = "Reno Bridge running - watching " + Path.Combine(config.Folder, "jobs");
        status = new Label();
        status.AutoSize = false;
        status.SetBounds(12, 52, 496, 36);
        status.Text = "starting...";
        stopButton = new Button();
        stopButton.Text = "Stop";
        stopButton.SetBounds(420, 94, 88, 26);
        stopButton.Click += delegate { StopBridge(); };
        openButton = new Button();
        openButton.Text = "Open Folder";
        openButton.SetBounds(312, 94, 100, 26);
        openButton.Click += delegate { try { Process.Start("explorer.exe", "\"" + core.Root + "\""); } catch { } };
        Controls.Add(title);
        Controls.Add(status);
        Controls.Add(stopButton);
        Controls.Add(openButton);
        CancelButton = stopButton;
        core.StatusChanged = delegate(string s) { status.Text = s; status.Refresh(); };
        timer = new Timer();
        timer.Interval = Math.Max(100, (int)(config.PollSeconds * 1000));
        timer.Tick += OnTick;
        Load += delegate
        {
            try { core.Start(); status.Text = "waiting for jobs"; }
            catch (Exception e) { status.Text = "could not start: " + e.Message; return; }
            if (config.KeepAwake) { try { SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED); } catch { } }
            timer.Start();
        };
        FormClosing += delegate
        {
            if (!closing) { closing = true; timer.Stop(); try { core.Stop(); } catch { } }
            if (config.KeepAwake) { try { SetThreadExecutionState(ES_CONTINUOUS); } catch { } }
            VegasHost h = core.Host as VegasHost;
            if (h != null) h.Detach();
        };
    }

    void OnTick(object sender, EventArgs e)
    {
        timer.Stop();
        bool go = true;
        try { go = core.Tick(); }
        catch (Exception ex) { try { core.Log("tick: " + ex); } catch { } }
        if (!go || core.StopRequested) { StopBridge(); return; }
        if (!closing) timer.Start();
    }

    void StopBridge()
    {
        if (closing) return;
        closing = true;
        timer.Stop();
        try { core.Stop(); } catch { }
        Close();
    }
}
#endif
