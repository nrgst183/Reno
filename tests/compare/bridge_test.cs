// Tests of Reno Bridge's core (tools/vegas/Reno Bridge.cs compiled with BRIDGE_TEST: no VEGAS, no window): the
// JSON, the ini, and the job handling with a fake host - results, done\, errors, time limits, unreadable and
// half-written jobs, paths outside the bridge folder, STOP and PAUSE, the heartbeat and the log.
// Run by tests/compare/bridge_test.sh (Roslyn in PowerShell 7 compiles it; BridgeTests.Run() returns the failures).

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

public class FakeHost : IBridgeHost
{
    public List<string> Ran = new List<string>();
    public int AfterJobs;
    public BridgeTests T;
    public string Describe() { return "fake host"; }
    public void AfterJob(BridgeJobContext ctx) { AfterJobs++; }
    public void Run(string what, Dictionary<string, object> a, BridgeJobContext ctx)
    {
        Ran.Add(what);
        switch (what)
        {
            case "open":
                ctx.Info["project"] = BJson.Text(a, "project", "");
                return;
            case "render":
                {
                    string p = ctx.Out(BJson.Text(a, "out", null));
                    File.WriteAllText(p, "frames");
                    ctx.Made(p);
                    return;
                }
            case "boom": throw new InvalidOperationException("boom");
            case "slow":
                // takes 'seconds' of the (fake) clock
                T.Now = T.Now.AddSeconds(BJson.Num(a, "seconds", 1));
                ctx.Alive();
                return;
            default: throw new BridgeException("unknown action '" + what + "'");
        }
    }
}

public class BridgeTests
{
    public DateTime Now = new DateTime(2026, 10, 16, 12, 0, 0);
    int failures, checks;
    StringBuilder report = new StringBuilder();

    void Check(string name, bool ok, string detail)
    {
        checks++;
        if (ok) report.Append("PASS " + name + "\n");
        else { failures++; report.Append("FAIL " + name + (detail != null ? ": " + detail : "") + "\n"); }
    }
    void Check(string name, bool ok) { Check(name, ok, null); }

    string root;
    BridgeCore core;
    FakeHost host;

    void Fresh()
    {
        root = Path.Combine(Path.GetTempPath(), "renobridge_test_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        BridgeConfig c = new BridgeConfig();
        c.Folder = root;
        host = new FakeHost();
        host.T = this;
        core = new BridgeCore(c, host);
        core.Clock = delegate { return Now; };
        core.Start();
    }

    void Job(string id, string json) { File.WriteAllText(Path.Combine(core.JobsDir, id + ".job.json"), json); }

    Dictionary<string, object> Result(string id)
    {
        string p = Path.Combine(core.ResultsDir, id + ".result.json");
        if (!File.Exists(p)) return null;
        return BJson.Parse(File.ReadAllText(p)) as Dictionary<string, object>;
    }

    static object At(object list, int i) { List<object> l = list as List<object>; return l != null && i < l.Count ? l[i] : null; }

    public static int Run()
    {
        BridgeTests t = new BridgeTests();
        try { t.All(); }
        catch (Exception e) { t.failures++; t.report.Append("FAIL (exception) " + e + "\n"); }
        Console.Write(t.report.ToString());
        Console.WriteLine(t.checks + " checks, " + t.failures + " failed");
        return t.failures;
    }

    void All()
    {
        JsonTests();
        IniTests();
        JobTests();
    }

    void JsonTests()
    {
        object o = BJson.Parse("{\"a\": [1, 2.5, -3e2, true, false, null], \"s\": \"x\\\"y\\\\z\\n\\u00e9\", \"o\": {}}");
        Check("json: array numbers", (double)At(BJson.Get(o, "a"), 2) == -300.0);
        Check("json: escapes", BJson.Text(o, "s", "") == "x\"y\\z\n\u00e9");
        Check("json: empty object", BJson.Get(o, "o") is Dictionary<string, object>);
        string w = BJson.Write(o);
        object back = BJson.Parse(w);
        Check("json: written and read back", BJson.Write(back) == w, w);
        Check("json: Num of a string number", BJson.Num(BJson.Parse("{\"t\": \"2.5\"}"), "t", 0) == 2.5);
        Check("json: Num default", BJson.Num(o, "nothere", 7) == 7);
        foreach (string bad in new string[] { "{", "{\"a\" 1}", "[1,]", "{\"a\": 1} x", "", "\"open" })
        {
            bool threw = false;
            try { BJson.Parse(bad); } catch (FormatException) { threw = true; }
            Check("json: rejects '" + bad + "'", threw);
        }
        Check("json: NaN written as null", BJson.Write(double.NaN).Trim() == "null");
        Check("json: BOM skipped", BJson.Parse("\uFEFF[1]") is List<object>);
    }

    void IniTests()
    {
        BridgeConfig c = new BridgeConfig();
        c.Read("; a comment\n[bridge]\nfolder = \"D:\\Bridge Folder\"\npoll_seconds = 2.5\nheartbeat_seconds = 0\njob_timeout_seconds=60\nkeep_awake = no\nscripts = A;B\nfrobnicate = 1\nnonsense\n");
        Check("ini: folder (quoted)", c.Folder == "D:\\Bridge Folder", c.Folder);
        Check("ini: poll_seconds", c.PollSeconds == 2.5);
        Check("ini: bad heartbeat_seconds kept default", c.HeartbeatSeconds == 5);
        Check("ini: job_timeout_seconds", c.JobTimeoutSeconds == 60);
        Check("ini: keep_awake no", !c.KeepAwake);
        Check("ini: scripts", c.ScriptDirs.Count == 2 && c.ScriptDirs[1] == "B");
        Check("ini: notes for unknown and bad lines", c.Notes.Count == 3, string.Join(" | ", c.Notes.ToArray()));
        BridgeConfig d = new BridgeConfig();
        Check("ini: default folder", d.Folder == BridgeConfig.DefaultFolder && d.Folder.EndsWith("Reno Bridge"));
    }

    void JobTests()
    {
        // ---- a job that works
        Fresh();
        Check("start: folders made", Directory.Exists(core.JobsDir) && Directory.Exists(core.DoneDir) && Directory.Exists(core.ResultsDir));
        Check("start: heartbeat written", File.Exists(core.HeartbeatPath));
        Job("a1", "{\"project\": \"suite/x.veg\", \"tag\": \"t1\", \"actions\": [{\"do\": \"render\", \"out\": \"renders/sub/x.avi\"}, {\"do\": \"ping\"}]}");
        Check("tick returns true", core.Tick());
        Dictionary<string, object> r = Result("a1");
        Check("ok job: result written", r != null);
        Check("ok job: ok", r != null && BJson.Bool(r, "ok", false), r == null ? null : BJson.Write(r));
        Check("ok job: project opened first", host.Ran.Count == 2 && host.Ran[0] == "open" && host.Ran[1] == "render", string.Join(",", host.Ran.ToArray()));
        Check("ok job: three action results (open, render, ping)", BJson.Items(r, "actions").Count == 3);
        Check("ok job: output listed", BJson.Items(r, "outputs").Count == 1 && BJson.Text(At(BJson.Items(r, "outputs"), 0), "path", "").Replace('\\', '/') == "renders/sub/x.avi",
              BJson.Write(BJson.Items(r, "outputs")));
        Check("ok job: output made in a new folder", File.Exists(Path.Combine(Path.Combine(Path.Combine(root, "renders"), "sub"), "x.avi")));
        Check("ok job: tag kept", BJson.Text(r, "tag", "") == "t1");
        Check("ok job: moved to done", !File.Exists(Path.Combine(core.JobsDir, "a1.job.json")) && File.Exists(Path.Combine(core.DoneDir, "a1.job.json")));
        Check("ok job: log in the result", BJson.Items(r, "log").Count >= 3);
        Check("ok job: AfterJob called", host.AfterJobs == 1);
        Check("ok job: no running marker left", Directory.GetFiles(core.JobsDir, "*.running").Length == 0);
        Check("ok job: counted", core.JobsDone == 1 && core.JobsFailed == 0);
        Dictionary<string, object> hb = BJson.Parse(File.ReadAllText(core.HeartbeatPath)) as Dictionary<string, object>;
        Check("heartbeat: idle after the job", BJson.Text(hb, "state", "") == "idle" && BJson.Text(hb, "last_job", "") == "a1");
        Check("log file written", File.Exists(core.LogPath) && File.ReadAllText(core.LogPath).Contains("job a1 done"));

        // ---- the same id again replaces the done job and the result
        Job("a1", "{\"actions\": [{\"do\": \"ping\"}]}");
        core.Tick();
        Check("same id again: result replaced", BJson.Items(Result("a1"), "actions").Count == 1);

        // ---- one job per tick, in name order
        Job("c", "{\"actions\": [{\"do\": \"ping\"}]}");
        Job("b", "{\"actions\": [{\"do\": \"ping\"}]}");
        core.Tick();
        Check("name order: b first", Result("b") != null && Result("c") == null);
        core.Tick();
        Check("name order: then c", Result("c") != null);

        // ---- a failing action: later ones skipped, the next job still runs
        host.Ran.Clear();
        int after = host.AfterJobs;
        Job("e1", "{\"actions\": [{\"do\": \"boom\"}, {\"do\": \"render\", \"out\": \"r.avi\"}]}");
        Job("e2", "{\"actions\": [{\"do\": \"ping\"}]}");
        core.Tick();
        r = Result("e1");
        Check("error: not ok", !BJson.Bool(r, "ok", true));
        Check("error: names the action and the exception", BJson.Text(r, "error", "").StartsWith("boom: InvalidOperationException: boom"), BJson.Text(r, "error", ""));
        Check("error: the next action skipped", host.Ran.Count == 1 && BJson.Text(At(BJson.Items(r, "actions"), 1), "error", "").Contains("skipped"));
        Check("error: AfterJob still called", host.AfterJobs == after + 1);
        Check("error: moved to done too", File.Exists(Path.Combine(core.DoneDir, "e1.job.json")));
        core.Tick();
        Check("error: the next job runs", BJson.Bool(Result("e2"), "ok", false));
        Check("error: counted", core.JobsFailed == 1);
        hb = BJson.Parse(File.ReadAllText(core.HeartbeatPath)) as Dictionary<string, object>;
        Check("heartbeat: last error", BJson.Text(hb, "last_error", "").StartsWith("e1:"));

        // ---- continue_on_error
        host.Ran.Clear();
        Job("e3", "{\"continue_on_error\": true, \"actions\": [{\"do\": \"boom\"}, {\"do\": \"render\", \"out\": \"r.avi\"}]}");
        core.Tick();
        r = Result("e3");
        Check("continue_on_error: both ran, job still failed", host.Ran.Count == 2 && !BJson.Bool(r, "ok", true));
        Check("continue_on_error: second ok", BJson.Bool(At(BJson.Items(r, "actions"), 1), "ok", false));

        // ---- unknown action, no actions, no "do", an action that is not an object
        Job("u1", "{\"actions\": [{\"do\": \"frobnicate\"}]}");
        core.Tick();
        Check("unknown action: error", BJson.Text(Result("u1"), "error", "").Contains("unknown action 'frobnicate'"));
        Job("u2", "{\"actions\": []}");
        core.Tick();
        Check("no actions: error", BJson.Text(Result("u2"), "error", "") == "the job has no actions");
        Job("u3", "{\"actions\": [{\"out\": \"x\"}]}");
        core.Tick();
        Check("no do: error", BJson.Text(Result("u3"), "error", "").Contains("no \\\"do\\\"") || BJson.Text(Result("u3"), "error", "").Contains("no \"do\""));
        Job("u4", "{\"actions\": [3]}");
        core.Tick();
        Check("action not an object: error", BJson.Text(Result("u4"), "error", "") == "an action is not a JSON object");
        Job("u5", "[1, 2]");
        core.Tick();
        Check("job not an object: waits first", Result("u5") == null);

        // ---- writing outside the bridge folder is refused
        string outside = Path.Combine(Path.GetTempPath(), "renobridge_outside_" + Guid.NewGuid().ToString("N") + ".txt");
        Job("p1", "{\"actions\": [{\"do\": \"write_text\", \"out\": \"../escape.txt\", \"text\": \"no\"}]}");
        Job("p2", "{\"actions\": [{\"do\": \"write_text\", \"out\": " + Q(outside) + ", \"text\": \"no\"}]}");
        Job("p3", "{\"actions\": [{\"do\": \"render\", \"out\": \"renders/../../x.avi\"}]}");
        Job("p4", "{\"actions\": [{\"do\": \"write_text\", \"out\": \"notes/fine.txt\", \"text\": \"yes\"}]}");
        Now = Now.AddSeconds(31);             // (u5 is past its grace now and fails)
        for (int i = 0; i < 6; i++) core.Tick();
        Check("half-written job: fails after the grace time", Result("u5") != null && BJson.Text(Result("u5"), "error", "").Contains("could not be read"));
        Check("outside: ../ refused", !BJson.Bool(Result("p1"), "ok", true) && BJson.Text(Result("p1"), "error", "").Contains("outside the bridge folder"));
        Check("outside: nothing written above the folder", !File.Exists(Path.Combine(Path.GetDirectoryName(root), "escape.txt")));
        Check("outside: absolute path refused", !BJson.Bool(Result("p2"), "ok", true) && !File.Exists(outside));
        Check("outside: through a subfolder refused", !BJson.Bool(Result("p3"), "ok", true));
        Check("inside: written", BJson.Bool(Result("p4"), "ok", false) && File.ReadAllText(Path.Combine(Path.Combine(root, "notes"), "fine.txt")) == "yes");

        // ---- a job file still being written: left alone, then read once it is whole
        File.WriteAllText(Path.Combine(core.JobsDir, "w1.job.json"), "{\"actions\": [{\"do\": \"pi");
        core.Tick();
        Check("partial job: no result yet", Result("w1") == null && File.Exists(Path.Combine(core.JobsDir, "w1.job.json")));
        Now = Now.AddSeconds(5);
        File.WriteAllText(Path.Combine(core.JobsDir, "w1.job.json"), "{\"actions\": [{\"do\": \"ping\"}]}");
        core.Tick();
        Check("partial job: done once complete", BJson.Bool(Result("w1"), "ok", false));
        // a partial job does not hold up the jobs after it
        File.WriteAllText(Path.Combine(core.JobsDir, "w2.job.json"), "{\"act");
        Job("w3", "{\"actions\": [{\"do\": \"ping\"}]}");
        core.Tick();
        Check("partial job: later jobs go on", Result("w2") == null && BJson.Bool(Result("w3"), "ok", false));
        File.Delete(Path.Combine(core.JobsDir, "w2.job.json"));

        // ---- time limits
        host.Ran.Clear();
        Job("t1", "{\"timeout\": 5, \"actions\": [{\"do\": \"slow\", \"seconds\": 10}, {\"do\": \"render\", \"out\": \"t.avi\"}]}");
        core.Tick();
        r = Result("t1");
        Check("timeout: failed", !BJson.Bool(r, "ok", true), BJson.Write(r));
        Check("timeout: the slow action fails", BJson.Text(At(BJson.Items(r, "actions"), 0), "error", "").Contains("time limit"));
        Check("timeout: the next action skipped", host.Ran.Count == 1 && BJson.Text(At(BJson.Items(r, "actions"), 1), "error", "").Contains("time limit"));
        Job("t2", "{\"timeout\": 5, \"actions\": [{\"do\": \"slow\", \"seconds\": 2}, {\"do\": \"slow\", \"seconds\": 2}]}");
        core.Tick();
        Check("timeout: within the limit is fine", BJson.Bool(Result("t2"), "ok", false));
        Check("timeout: seconds recorded", Math.Abs(BJson.Num(Result("t2"), "seconds", 0) - 4) < 0.01, BJson.Num(Result("t2"), "seconds", 0).ToString());
        core.Config.JobTimeoutSeconds = 3;
        Job("t3", "{\"actions\": [{\"do\": \"slow\", \"seconds\": 4}]}");
        core.Tick();
        Check("timeout: the ini's default applies", !BJson.Bool(Result("t3"), "ok", true));
        core.Config.JobTimeoutSeconds = 1800;

        // ---- list_files and wait
        Job("l1", "{\"actions\": [{\"do\": \"list_files\", \"dir\": \"notes\"}]}");
        core.Tick();
        Check("list_files", BJson.Items(BJson.Get(At(BJson.Items(Result("l1"), "actions"), 0), "info"), "files").Count == 1);

        // ---- PAUSE and STOP
        File.WriteAllText(Path.Combine(core.JobsDir, "PAUSE"), "");
        Job("z1", "{\"actions\": [{\"do\": \"ping\"}]}");
        Check("pause: still running", core.Tick());
        Check("pause: job waits", Result("z1") == null && core.State == "paused");
        File.Delete(Path.Combine(core.JobsDir, "PAUSE"));
        core.Tick();
        Check("pause: resumes", Result("z1") != null && core.State == "idle");
        File.WriteAllText(Path.Combine(core.JobsDir, "STOP"), "");
        Job("z2", "{\"actions\": [{\"do\": \"ping\"}]}");
        Check("stop: tick says stop", !core.Tick());
        Check("stop: STOP removed, job left", !File.Exists(Path.Combine(core.JobsDir, "STOP")) && Result("z2") == null);
        core.Stop();
        hb = BJson.Parse(File.ReadAllText(core.HeartbeatPath)) as Dictionary<string, object>;
        Check("stop: heartbeat says stopped", BJson.Text(hb, "state", "") == "stopped");

        // ---- heartbeat timing: not rewritten before it is due
        Fresh();
        DateTime t0 = File.GetLastWriteTimeUtc(core.HeartbeatPath);
        string before = File.ReadAllText(core.HeartbeatPath);
        Now = Now.AddSeconds(1);
        core.Tick();
        Check("heartbeat: not due after 1 s", File.ReadAllText(core.HeartbeatPath) == before);
        Now = Now.AddSeconds(5);
        core.Tick();
        Check("heartbeat: rewritten after 6 s", File.ReadAllText(core.HeartbeatPath) != before);
        // a job that was running when the bridge stopped (VEGAS crashed) is not run again at the next start
        Job("k1", "{\"actions\": [{\"do\": \"ping\"}]}");
        File.WriteAllText(Path.Combine(core.JobsDir, "k1.running"), "");
        core.Start();
        Check("crashed job: failed result", Result("k1") != null && BJson.Bool(Result("k1"), "crashed", false));
        Check("crashed job: moved to done, marker gone", File.Exists(Path.Combine(core.DoneDir, "k1.job.json")) && !File.Exists(Path.Combine(core.JobsDir, "k1.running")));
        // a leftover STOP from an earlier run is removed at start
        File.WriteAllText(Path.Combine(core.JobsDir, "STOP"), "");
        core.Start();
        Check("start: old STOP removed", core.Tick());

        try { Directory.Delete(root, true); } catch { }
    }

    static string Q(string s) { StringBuilder sb = new StringBuilder(); BJson.Quote(sb, s); return sb.ToString(); }
}
