/**
 * Reno Effect Reference: saves VEGAS's own pictures of the effects only VEGAS can run (Rays, Glint, Soft Contrast,
 * Starburst, Fill Light, Defocus, Light Rays), for matching Reno's versions against.
 * Run in VEGAS Pro (Tools > Scripting > Run Script). Asks for a folder containing reno_test_picture.png, makes a new
 * 320x180 project (it will not start if the open project has unsaved changes), applies each effect to a copy of the
 * picture at its defaults, with each preset and, for OpenFX effects, each setting at minimum / middle / maximum, and
 * saves each frame as a PNG. Writes manifest.txt, effects.txt and ref_<index>.png to the folder.
 * C# 5. Anything missing from the scripting API is reached by reflection, so a gap costs one manifest line.
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
    Vegas vegas;
    string folder;
    StreamWriter manifest, effects;
    int index = 0;
    static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

    // VEGAS effect names; the first exact match wins, then the first that contains the name
    static readonly string[] Wanted = { "Rays", "Glint", "Soft Contrast", "Starburst", "Fill Light", "Defocus", "Light Rays" };

    public void FromVegas(Vegas v)
    {
        vegas = v;
        try { Run(); }
        catch (Exception e) { MessageBox.Show("Reno Effect Reference stopped: " + e.Message + "\n\n" + e.StackTrace, "Reno Effect Reference"); }
        finally
        {
            if (manifest != null) manifest.Close();
            if (effects != null) effects.Close();
        }
    }

    void Run()
    {
        string guess = Path.Combine(Path.GetDirectoryName(Path.GetDirectoryName(Path.GetDirectoryName(ScriptPath()))), "effectref");
        if (!File.Exists(Path.Combine(guess, "reno_test_picture.png")))
            guess = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "Reno Effect Reference");
        FolderBrowserDialog dlg = new FolderBrowserDialog();
        dlg.Description = "Folder with reno_test_picture.png (the pictures are written there)";
        dlg.SelectedPath = guess;
        if (dlg.ShowDialog() != DialogResult.OK) return;
        folder = dlg.SelectedPath;
        string picture = Path.Combine(folder, "reno_test_picture.png");
        if (!File.Exists(picture)) { MessageBox.Show("reno_test_picture.png is not in " + folder, "Reno Effect Reference"); return; }

        if (!vegas.NewProject(true, false)) return;     // false: cancelled
        Project p = vegas.Project;
        p.Video.Width = 320;
        p.Video.Height = 180;
        p.Video.FrameRate = 30;
        p.Video.PixelAspectRatio = 1.0;
        p.Video.FieldOrder = VideoFieldOrder.ProgressiveScan;
        TrySet(p.Video, "RenderQuality", "Best");
        TrySet(p.Video, "PixelFormat", "Normal");      // 8-bit
        TrySet(p.Preview, "RenderQuality", "Best");
        TrySet(p.Preview, "FullSize", true);
        TrySet(p.Preview, "PreviewMode", "Full");

        manifest = new StreamWriter(Path.Combine(folder, "manifest.txt"), false, new UTF8Encoding(false));
        effects = new StreamWriter(Path.Combine(folder, "effects.txt"), false, new UTF8Encoding(false));
        manifest.WriteLine("# index\teffect\tkind\twhat\tpng\tresult");

        Media media = new Media(picture);
        VideoTrack track = new VideoTrack(0, "Reno test");
        p.Tracks.Add(track);

        List<PlugInNode> all = new List<PlugInNode>();
        Collect(vegas.VideoFX, all, 0);
        foreach (string name in Wanted)
        {
            PlugInNode plug = Find(all, name);
            if (plug == null) { effects.WriteLine(name + ": NOT FOUND"); continue; }
            bool ofx = false;
            try { ofx = Convert.ToBoolean(Get(plug, "IsOFX")); } catch (Exception) { }
            effects.WriteLine(name + ": \"" + plug.Name + "\" " + (ofx ? "OpenFX" : "not OpenFX (managed)") + "  " + SafeString(Get(plug, "UniqueID")));

            Test(track, media, plug, name, "default", "", null);
            List<string> presets = PresetNames(plug);
            effects.WriteLine("  presets: " + string.Join(" | ", presets.ToArray()));
            foreach (string pr in presets) Test(track, media, plug, name, "preset", pr, delegate(Effect fx) { fx.Preset = pr; });
            // OpenFX: each setting at its minimum / middle / maximum
            if (ofx) ParamTests(track, media, plug, name);
            // settings together, where one setting at a time shows nothing (Starburst's single-setting cases run at
            // Strength 0, so they draw no glow)
            if (ofx) ComboTests(track, media, plug, name);
            effects.Flush();
            manifest.Flush();
        }
        manifest.Close(); manifest = null;
        effects.Close(); effects = null;
        MessageBox.Show(index + " pictures written to " + folder + ".", "Reno Effect Reference");
    }

    delegate void Setup(Effect fx);

    // One test: the picture at 'index' seconds with the effect applied, then the frame saved.
    void Test(VideoTrack track, Media media, PlugInNode plug, string name, string kind, string what, Setup setup)
    {
        int i = index++;
        string png = "ref_" + i.ToString("0000") + ".png";
        string result = "ok";
        try
        {
            Timecode at = Timecode.FromSeconds(i);
            VideoEvent ev = track.AddVideoEvent(at, Timecode.FromFrames(1));
            ev.AddTake(media.GetVideoStreamByIndex(0));
            Effect fx = new Effect(plug);
            ev.Effects.Add(fx);
            if (setup != null) setup(fx);
            vegas.Transport.CursorPosition = at;
            vegas.UpdateUI();
            Application.DoEvents();
            object r = CallSnapshot(Path.Combine(folder, png), at);
            if (r != null && r.ToString() != "Complete") result = "snapshot: " + r;
        }
        catch (Exception e) { result = "FAILED: " + e.Message; }
        manifest.WriteLine(i + "\t" + name + "\t" + kind + "\t" + what + "\t" + png + "\t" + result);
    }

    void ParamTests(VideoTrack track, Media media, PlugInNode plug, string name)
    {
        Effect probe = new Effect(plug);
        object ofxEffect = null;
        try { ofxEffect = Get(probe, "OFXEffect"); } catch (Exception) { }
        if (ofxEffect == null) { effects.WriteLine("  (no OFXEffect: no parameter tests)"); return; }
        IEnumerable pars = Get(ofxEffect, "Parameters") as IEnumerable;
        if (pars == null) return;
        foreach (object par in pars)
        {
            string pname = SafeString(Get(par, "Name"));
            string ptype = par.GetType().Name;
            object mn = null, mx = null, def = null;
            try { mn = Get(par, "Min"); } catch (Exception) { }
            try { mx = Get(par, "Max"); } catch (Exception) { }
            try { def = Get(par, "Default"); } catch (Exception) { }
            effects.WriteLine("  param " + pname + " (" + ptype + ") min " + SafeString(mn) + " max " + SafeString(mx) + " default " + SafeString(def));
            if (ptype.IndexOf("Double", StringComparison.Ordinal) >= 0 && mn is double && mx is double && !(ptype.IndexOf("2D", StringComparison.Ordinal) >= 0 || ptype.IndexOf("3D", StringComparison.Ordinal) >= 0))
            {
                double a = (double)mn, b = (double)mx;
                foreach (double v in new double[] { a, (a + b) / 2, b })
                {
                    double val = v;
                    string pn = pname;
                    Test(track, media, plug, name, "param", pn + "=" + val.ToString("R", Inv), delegate(Effect fx) { SetOfx(fx, pn, val); });
                }
            }
            else if (ptype.IndexOf("Boolean", StringComparison.Ordinal) >= 0)
            {
                foreach (bool v in new bool[] { false, true })
                {
                    bool val = v;
                    string pn = pname;
                    Test(track, media, plug, name, "param", pn + "=" + (val ? "1" : "0"), delegate(Effect fx) { SetOfx(fx, pn, val); });
                }
            }
            else if (ptype.IndexOf("Choice", StringComparison.Ordinal) >= 0)
            {
                IEnumerable choices = null;
                try { choices = Get(par, "Choices") as IEnumerable; } catch (Exception) { }
                int n = 0;
                if (choices != null) foreach (object c in choices) n++;
                for (int k = 0; k < n && k < 12; ++k)
                {
                    int val = k;
                    string pn = pname;
                    Test(track, media, plug, name, "param", pn + "=" + val, delegate(Effect fx) { SetOfxChoice(fx, pn, val); });
                }
            }
        }
    }

    // One picture with several settings at once ("Name=value" pairs; integers as int, switches as bool).
    void Combo(VideoTrack track, Media media, PlugInNode plug, string name, params object[] pairs)
    {
        StringBuilder what = new StringBuilder();
        for (int k = 0; k + 1 < pairs.Length; k += 2)
        {
            if (what.Length > 0) what.Append(' ');
            object v = pairs[k + 1];
            what.Append(pairs[k]).Append('=').Append(v is bool ? ((bool)v ? "1" : "0") : Convert.ToString(v, Inv));
        }
        object[] set = pairs;
        Test(track, media, plug, name, "combo", what.ToString(), delegate(Effect fx)
        {
            for (int k = 0; k + 1 < set.Length; k += 2) SetOfx(fx, (string)set[k], set[k + 1]);
        });
    }

    void ComboTests(VideoTrack track, Media media, PlugInNode plug, string name)
    {
        if (name == "Starburst")
        {
            // the star itself: its arms and how they fade
            foreach (int pts in new int[] { 2, 4, 6 })
                Combo(track, media, plug, name, "Threshold", 0.0, "Brilliance", 100.0, "Strength", 100.0, "Radius", 10.0, "Points", pts, "Orientation", 0.0);
            // which way Orientation turns, and Aspect against Orientation
            foreach (double o in new double[] { 30.0, 330.0 })
                Combo(track, media, plug, name, "Threshold", 0.0, "Brilliance", 100.0, "Strength", 100.0, "Radius", 30.0, "Points", 3, "Orientation", o, "Aspect", 1.0);
            foreach (double a in new double[] { 0.5, 2.0 })
                foreach (double o in new double[] { 0.0, 30.0, 90.0 })
                    Combo(track, media, plug, name, "Threshold", 0.0, "Brilliance", 100.0, "Strength", 100.0, "Radius", 30.0, "Points", 2, "Orientation", o, "Aspect", a);
            // Soften edges: a small glow, so the mask shows at the picture's borders
            foreach (double se in new double[] { 0.0, 50.0, 100.0 })
                Combo(track, media, plug, name, "Threshold", 0.0, "Brilliance", 100.0, "Strength", 100.0, "Radius", 2.0, "SoftenEdges", se);
            foreach (bool rf in new bool[] { false, true })
                Combo(track, media, plug, name, "Threshold", 0.0, "Brilliance", 100.0, "Strength", 100.0, "Radius", 2.0, "ReduceFlicker", rf);
            foreach (double cb in new double[] { 0.0, 50.0, 100.0 })
                Combo(track, media, plug, name, "Threshold", 0.0, "Brilliance", 100.0, "Strength", 100.0, "Radius", 30.0, "Points", 4, "CenterBurst", cb, "EffectOnly", true);
        }
        else if (name == "Fill Light")
        {
            // how wide its blur is against Softness
            foreach (double sf in new double[] { 10.0, 30.0, 60.0, 100.0 })
                Combo(track, media, plug, name, "Softness", sf);
        }
    }

    void SetOfx(Effect fx, string pname, object value)
    {
        object o = Get(fx, "OFXEffect");
        object par = Call(o, "FindParameterByName", pname);
        if (par == null) throw new Exception("no parameter " + pname);
        Set(par, "Value", value);
    }

    void SetOfxChoice(Effect fx, string pname, int k)
    {
        object o = Get(fx, "OFXEffect");
        object par = Call(o, "FindParameterByName", pname);
        if (par == null) throw new Exception("no parameter " + pname);
        IEnumerable choices = Get(par, "Choices") as IEnumerable;
        int i = 0;
        foreach (object c in choices) { if (i == k) { Set(par, "Value", c); return; } i++; }
    }

    List<string> PresetNames(PlugInNode plug)
    {
        List<string> names = new List<string>();
        try
        {
            IEnumerable presets = Get(plug, "Presets") as IEnumerable;
            if (presets != null) foreach (object pr in presets) { string n = SafeString(Get(pr, "Name")); if (n.Length > 0) names.Add(n); }
        }
        catch (Exception) { }
        return names;
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

    static PlugInNode Find(List<PlugInNode> all, string name)
    {
        foreach (PlugInNode n in all) if (string.Equals(n.Name, name, StringComparison.OrdinalIgnoreCase) || string.Equals(n.Name, "VEGAS " + name, StringComparison.OrdinalIgnoreCase)) return n;
        foreach (PlugInNode n in all) if (n.Name != null && n.Name.IndexOf(name, StringComparison.OrdinalIgnoreCase) >= 0 && !(name == "Rays" && n.Name.IndexOf("Light", StringComparison.OrdinalIgnoreCase) >= 0)) return n;
        return null;
    }

    static void Collect(PlugInNode node, List<PlugInNode> into, int depth)
    {
        if (node == null || depth > 6) return;
        bool container = depth == 0;
        try { container = Convert.ToBoolean(Get(node, "IsContainer")); } catch (Exception) { }
        if (!container) { if (depth > 0) into.Add(node); return; }
        try { foreach (PlugInNode child in node) Collect(child, into, depth + 1); } catch (Exception) { }
    }

    string ScriptPath()
    {
        try { object s = Get(vegas, "Script"); object f = Get(s, "File"); if (f != null) return f.ToString(); } catch (Exception) { }
        return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "x", "x", "x");
    }

    // ---- reflection helpers
    static object Get(object o, string prop)
    {
        if (o == null) return null;
        PropertyInfo pi = o.GetType().GetProperty(prop);
        return pi == null ? null : pi.GetValue(o, null);
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
    static string SafeString(object o) { return o == null ? "" : Convert.ToString(o, Inv); }
}
