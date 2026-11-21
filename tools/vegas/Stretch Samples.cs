// Stretch Samples: renders a stretch of a sound file through VEGAS's élastique time stretching at many lengths, one WAV
// per case, so that Reno's stretchers (src/engine/stretch.cpp) can be matched to VEGAS's sample for sample.
//
// Run it with Tools > Scripting > Run Script in VEGAS Pro 18. It asks for a sound file (or a video with sound), starts
// a new project (asking to save the open one), and writes into "<file name> - stretch samples" beside it:
//   reference.wav                         the excerpt unstretched (method None, rate 1): the level and timing to compare with
//   efficient_rate_0.750.wav ...           élastique Efficient at each playback rate in Rates
//   efficient_len_2.250s.wav ...           élastique Efficient with the excerpt stretched to each event length in Lengths
//   pro_rate_... / pro_len_...             the same in élastique Pro
//   index.csv                             one line per file: mode, playback rate, event length, the excerpt, the format
// Each file is exactly the event's span on the timeline, rendered as 32-bit float PCM at the project's sample rate, so
// the event starts at the file's first sample. Pitch is not changed (rate only, pitch not locked to it, 0 semitones).
//
// What to change: the excerpt (ExcerptStart, ExcerptLength), the modes, the rates and lengths below.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Windows.Forms;
using ScriptPortal.Vegas;

public class EntryPoint
{
    // the part of the file that is stretched (seconds)
    const double ExcerptStart = 0.0;
    const double ExcerptLength = 2.0;
    // the project's sample rate (the renders' too)
    const int SampleRate = 48000;

    static readonly ElastiqueStretchAttributes[] Modes = { ElastiqueStretchAttributes.Efficient, ElastiqueStretchAttributes.Pro };

    // playback rates (VEGAS allows 0.25 to 4): fine steps near 1, where the differences are hardest to hear
    static readonly double[] Rates = {
        0.25, 0.3, 0.35, 0.4, 0.45, 0.5, 0.55, 0.6, 0.65, 0.7, 0.75, 0.8, 0.85,
        0.9, 0.91, 0.92, 0.93, 0.94, 0.95, 0.96, 0.97, 0.98, 0.99, 1.0, 1.01, 1.02, 1.03, 1.04, 1.05, 1.06, 1.07, 1.08, 1.09, 1.1,
        1.15, 1.2, 1.25, 1.3, 1.4, 1.5, 1.6, 1.75, 2.0, 2.25, 2.5, 3.0, 3.5, 4.0 };

    // event lengths the excerpt is stretched to (seconds): 0.5 to 6 in steps of 0.25 (rate = excerpt length / event length)
    static double[] Lengths()
    {
        List<double> l = new List<double>();
        for (double s = 0.5; s <= 6.0 + 1e-9; s += 0.25) l.Add(Math.Round(s, 2));
        return l.ToArray();
    }

    const double Gap = 0.5;     // between events on the timeline (each is rendered on its own)

    Vegas vegas;
    StringBuilder index = new StringBuilder();
    string outDir;
    Renderer renderer;
    RenderTemplate template;

    public void FromVegas(Vegas v)
    {
        vegas = v;
        try
        {
            OpenFileDialog dlg = new OpenFileDialog();
            dlg.Title = "Stretch Samples: the sound to stretch";
            dlg.Filter = "Sound or video files|*.wav;*.w64;*.flac;*.mp3;*.m4a;*.aac;*.ogg;*.mp4;*.mov;*.mkv;*.avi|All files|*.*";
            if (dlg.ShowDialog() != DialogResult.OK) return;
            string source = dlg.FileName;
            outDir = Path.Combine(Path.GetDirectoryName(source), Path.GetFileNameWithoutExtension(source) + " - stretch samples");
            Directory.CreateDirectory(outDir);

            if (!vegas.NewProject(true, false)) return;
            Project p = vegas.Project;
            p.Audio.SampleRate = SampleRate;
            if (!FindWaveTemplate()) return;

            Media m = Media.CreateInstance(p, source);
            MediaStream stream = m.GetAudioStreamByIndex(0);
            if (stream == null) { MessageBox.Show(source + " has no sound.", "Stretch Samples"); return; }
            double have = m.Length.ToMilliseconds() / 1000.0;
            if (have < ExcerptStart + ExcerptLength)
            {
                MessageBox.Show(string.Format(CultureInfo.InvariantCulture, "The file is {0:0.###} s long; the excerpt needs {1:0.###} s from {2:0.###} s.",
                    have, ExcerptLength, ExcerptStart), "Stretch Samples");
                return;
            }

            AudioTrack track = new AudioTrack(p, 0, "Stretch Samples");
            p.Tracks.Add(track);
            index.AppendLine("file,method,mode,playback_rate,event_length_s,excerpt_start_s,excerpt_length_s,sample_rate,renderer,template");

            // lay out every case first, then render each event's span on its own
            List<KeyValuePair<string, AudioEvent>> cases = new List<KeyValuePair<string, AudioEvent>>();
            double at = 0;
            {
                AudioEvent e = AddEvent(track, stream, at, ExcerptLength, "reference");
                e.Method = TimeStretchPitchShift.None;
                e.PlaybackRate = 1.0;
                cases.Add(new KeyValuePair<string, AudioEvent>("reference", e));
                at += ExcerptLength + Gap;
            }
            foreach (ElastiqueStretchAttributes mode in Modes)
            {
                string tag = mode == ElastiqueStretchAttributes.Pro ? "pro" : mode == ElastiqueStretchAttributes.Efficient ? "efficient" : mode.ToString().ToLowerInvariant();
                foreach (double rate in Rates)
                {
                    double len = ExcerptLength / rate;
                    string name = tag + "_rate_" + rate.ToString("0.000", CultureInfo.InvariantCulture);
                    cases.Add(new KeyValuePair<string, AudioEvent>(name, Stretched(track, stream, at, len, rate, mode, name)));
                    at += len + Gap;
                }
                foreach (double len in Lengths())
                {
                    double rate = ExcerptLength / len;
                    if (rate < 0.25 - 1e-9 || rate > 4.0 + 1e-9) continue;
                    string name = tag + "_len_" + len.ToString("0.000", CultureInfo.InvariantCulture) + "s";
                    cases.Add(new KeyValuePair<string, AudioEvent>(name, Stretched(track, stream, at, len, rate, mode, name)));
                    at += len + Gap;
                }
            }

            int done = 0;
            foreach (KeyValuePair<string, AudioEvent> c in cases)
            {
                AudioEvent e = c.Value;
                string file = c.Key + ".wav";
                if (!RenderSpan(e.Start, e.Length, Path.Combine(outDir, file)))
                {
                    MessageBox.Show("Stopped at " + file + " (" + done + " of " + cases.Count + " rendered).", "Stretch Samples");
                    WriteIndex();
                    return;
                }
                bool stretched = e.Method == TimeStretchPitchShift.Elastique;
                index.AppendLine(string.Join(",", new string[] {
                    file, e.Method.ToString(), stretched ? e.ElastiqueAttribute.ToString() : "",
                    e.PlaybackRate.ToString("R", CultureInfo.InvariantCulture),
                    (e.Length.ToMilliseconds() / 1000.0).ToString("R", CultureInfo.InvariantCulture),
                    ExcerptStart.ToString("R", CultureInfo.InvariantCulture), ExcerptLength.ToString("R", CultureInfo.InvariantCulture),
                    SampleRate.ToString(CultureInfo.InvariantCulture), Quote(renderer.Name), Quote(template.Name) }));
                done++;
            }
            WriteIndex();
            MessageBox.Show(done + " files written to\n" + outDir, "Stretch Samples");
        }
        catch (Exception ex)
        {
            if (outDir != null) WriteIndex();
            MessageBox.Show("Stretch Samples stopped:\n" + ex.ToString(), "Stretch Samples");
        }
    }

    AudioEvent Stretched(AudioTrack t, MediaStream s, double at, double len, double rate, ElastiqueStretchAttributes mode, string name)
    {
        AudioEvent e = AddEvent(t, s, at, len, name);
        e.Method = TimeStretchPitchShift.Elastique;
        e.ElastiqueAttribute = mode;
        e.PitchSemis = 0;
        e.PitchLock = false;
        e.PlaybackRate = rate;
        // (setting the rate can move the event's end: its length is put back)
        e.Length = Timecode.FromSeconds(len);
        return e;
    }

    static AudioEvent AddEvent(AudioTrack t, MediaStream s, double start, double length, string name)
    {
        AudioEvent e = new AudioEvent(Timecode.FromSeconds(start), Timecode.FromSeconds(length), name);
        t.Events.Add(e);
        Take k = new Take(s, true);
        e.Takes.Add(k);
        k.Offset = Timecode.FromSeconds(ExcerptStart);
        e.Normalize = false;
        e.FadeIn.Length = Timecode.FromSeconds(0);
        e.FadeOut.Length = Timecode.FromSeconds(0);
        return e;
    }

    // Wave (Microsoft) at 32-bit float, stereo, at the project's rate; else the template VEGAS matches to the project
    bool FindWaveTemplate()
    {
        foreach (Renderer r in vegas.Renderers)
        {
            string ext = (r.FileExtension ?? "").TrimStart('*', '.').ToLowerInvariant();
            if (ext != "wav") continue;
            string rate = SampleRate.ToString("N0", CultureInfo.InvariantCulture);       // "48,000"
            foreach (RenderTemplate t in r.Templates)
            {
                string n = t.Name;
                if (n.IndexOf("32", StringComparison.Ordinal) >= 0 && n.IndexOf("Float", StringComparison.OrdinalIgnoreCase) >= 0 &&
                    n.IndexOf("Stereo", StringComparison.OrdinalIgnoreCase) >= 0 &&
                    (n.IndexOf(rate, StringComparison.Ordinal) >= 0 || n.IndexOf((SampleRate / 1000).ToString(CultureInfo.InvariantCulture) + " kHz", StringComparison.OrdinalIgnoreCase) >= 0) &&
                    t.IsValid())
                {
                    renderer = r;
                    template = t;
                    return true;
                }
            }
            try
            {
                RenderTemplate[] m = r.Templates.FindProjectMatches(vegas.Project);
                if (m != null && m.Length > 0) { renderer = r; template = m[0]; }
            }
            catch { }
        }
        if (template != null)
        {
            DialogResult go = MessageBox.Show("No 32-bit float stereo " + SampleRate + " Hz Wave template was found; the renders will use \"" + template.Name +
                "\" (" + renderer.Name + "), which may round the samples. Go on?", "Stretch Samples", MessageBoxButtons.OKCancel);
            return go == DialogResult.OK;
        }
        MessageBox.Show("VEGAS has no Wave (.wav) renderer.", "Stretch Samples");
        return false;
    }

    bool RenderSpan(Timecode start, Timecode length, string path)
    {
        if (File.Exists(path)) File.Delete(path);
        RenderArgs args = new RenderArgs(vegas.Project);
        args.OutputFile = path;
        args.RenderTemplate = template;
        args.Start = start;
        args.Length = length;
        args.UseSelection = false;
        try { args.ShowOpenButtonsOnComplete = false; } catch { }
        RenderStatus st = vegas.Render(args);
        // (VEGAS writes a peak file beside it: not wanted)
        try { if (File.Exists(path + ".sfk")) File.Delete(path + ".sfk"); } catch { }
        return st == RenderStatus.Complete;
    }

    void WriteIndex()
    {
        File.WriteAllText(Path.Combine(outDir, "index.csv"), index.ToString(), new UTF8Encoding(false));
    }

    static string Quote(string s)
    {
        return "\"" + (s ?? "").Replace("\"", "\"\"") + "\"";
    }
}
