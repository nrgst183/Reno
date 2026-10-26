// Pitch Tests (for the Reno Bridge): builds, in the open (new) project, one of two test tracks the Reno "pitch" work
// compares with Reno. Which one: the file "pitch_test_mode.txt" beside this script holds A or B.
//  A: Classic presets A01..A19, each at rate 1.5 and 0.6, 1 s events 1.3 s apart, on the speech clip's sound from 30 s.
//  B: the pitch cases (lock to rate with a stale value, method None, fractional and large pitch, reverse, the
//     élastique modes, very short events), 1 s events 1.2 s apart, on a 440 Hz tone from 1 s.
// Setup: the bridge folder is RENO_BRIDGE_DIR (default Documents\Reno Bridge); your own speech clip is speech.mp4 in the folder RENO_TEST_MEDIA (default <bridge folder>\media).
using System;
using System.IO;
using ScriptPortal.Vegas;

public class EntryPoint
{
    // The bridge folder: the environment variable RENO_BRIDGE_DIR, else Documents\Reno Bridge (as Reno Bridge.cs).
    static readonly string Bridge = BridgeFolder();
    static string BridgeFolder()
    {
        string e = System.Environment.GetEnvironmentVariable("RENO_BRIDGE_DIR");
        if (!string.IsNullOrEmpty(e)) return e;
        return System.IO.Path.Combine(System.Environment.GetFolderPath(System.Environment.SpecialFolder.MyDocuments), "Reno Bridge");
    }
    // Your own media: the folder RENO_TEST_MEDIA (default <bridge folder>\media) holds speech.mp4 (any clip with speech sound, at least 40 s long).
    static string Speech
    {
        get
        {
            string d = System.Environment.GetEnvironmentVariable("RENO_TEST_MEDIA");
            if (string.IsNullOrEmpty(d)) d = System.IO.Path.Combine(Bridge, "media");
            return System.IO.Path.Combine(d, "speech.mp4");
        }
    }

    public void FromVegas(Vegas vegas)
    {
        string mode = File.ReadAllText(Path.Combine(Bridge, "pitch_test_mode.txt")).Trim().ToUpperInvariant();
        Project p = vegas.Project;
        AudioTrack t = new AudioTrack(p, 0, "Pitch Test " + mode);
        p.Tracks.Add(t);
        if (mode == "A")
        {
            Media m = Media.CreateInstance(p, Speech);
            MediaStream s = m.GetAudioStreamByIndex(0);
            double at = 0;
            foreach (ClassicStretchAttributes a in Enum.GetValues(typeof(ClassicStretchAttributes)))
                foreach (double rate in new double[] { 1.5, 0.6 })
                {
                    AudioEvent e = Add(t, s, at, 1.0, 30.0, a.ToString() + " r" + rate.ToString(System.Globalization.CultureInfo.InvariantCulture));
                    e.Method = TimeStretchPitchShift.Classic;
                    e.ClassicAttribute = a;
                    e.PitchSemis = 0;
                    e.PitchLock = false;
                    e.PlaybackRate = rate;
                    at += 1.3;
                }
        }
        else
        {
            Media m = Media.CreateInstance(p, Path.Combine(Path.Combine(Bridge, "req"), "tone440.wav"));
            MediaStream s = m.GetAudioStreamByIndex(0);
            double at = 0;
            int n = 0;
            Func<double, AudioEvent> next = (len) => { n++; AudioEvent ev = Add(t, s, at, len, 1.0, "case " + n); at += 1.2; return ev; };
            Case(next(1), TimeStretchPitchShift.Elastique, ElastiqueStretchAttributes.Efficient, 5, false, 1);
            Case(next(1), TimeStretchPitchShift.Elastique, ElastiqueStretchAttributes.Efficient, 5, false, 1.5);
            Case(next(1), TimeStretchPitchShift.Elastique, ElastiqueStretchAttributes.Efficient, 5, true, 1);
            Case(next(1), TimeStretchPitchShift.Elastique, ElastiqueStretchAttributes.Efficient, 5, true, 1.5);
            Case(next(1), TimeStretchPitchShift.Elastique, ElastiqueStretchAttributes.Efficient, 0, true, 0.6);
            { AudioEvent e = next(1); e.Method = TimeStretchPitchShift.Elastique; e.PitchSemis = 5; e.Method = TimeStretchPitchShift.None; e.PlaybackRate = 1; }
            { AudioEvent e = next(1); e.Method = TimeStretchPitchShift.None; e.PlaybackRate = 1.5; }
            CaseClassic(next(1), 5, false, 1);
            CaseClassic(next(1), 0, false, 1.5);
            CaseClassic(next(1), 5, true, 1.5);
            Case(next(1), TimeStretchPitchShift.Elastique, ElastiqueStretchAttributes.Efficient, 0.5, false, 1);
            Case(next(1), TimeStretchPitchShift.Elastique, ElastiqueStretchAttributes.Efficient, 2.37, false, 1);
            Case(next(1), TimeStretchPitchShift.Elastique, ElastiqueStretchAttributes.Efficient, 24, false, 1);
            Case(next(1), TimeStretchPitchShift.Elastique, ElastiqueStretchAttributes.Efficient, -24, false, 1);
            {
                // reversed: a take of a reversed subclip of the tone (how VEGAS's Reverse does it)
                Subclip rev = new Subclip(p, Path.Combine(Path.Combine(Bridge, "req"), "tone440.wav"), Timecode.FromSeconds(0), Timecode.FromSeconds(30), true, "tone440 reversed");
                n++;
                AudioEvent e = Add(t, rev.GetAudioStreamByIndex(0), at, 1.0, 1.0, "case " + n + " reversed");
                at += 1.2;
                Case(e, TimeStretchPitchShift.Elastique, ElastiqueStretchAttributes.Efficient, 5, false, 1);
            }
            Case(next(1), TimeStretchPitchShift.Elastique, ElastiqueStretchAttributes.Pro, 7, false, 1);
            Case(next(1), TimeStretchPitchShift.Elastique, ElastiqueStretchAttributes.Efficient, 7, false, 1);
            Case(next(1), TimeStretchPitchShift.Elastique, ElastiqueStretchAttributes.Soloist_Monophonic, 7, false, 1);
            Case(next(1), TimeStretchPitchShift.Elastique, ElastiqueStretchAttributes.Soloist_Speech, 7, false, 1);
            Case(next(1), TimeStretchPitchShift.Elastique, ElastiqueStretchAttributes.Pro, 0, false, 0.6);
            Case(next(1), TimeStretchPitchShift.Elastique, ElastiqueStretchAttributes.Pro, 0, false, 3.0);
            Case(next(0.006), TimeStretchPitchShift.Elastique, ElastiqueStretchAttributes.Efficient, 5, false, 1);
            Case(next(0.020), TimeStretchPitchShift.Elastique, ElastiqueStretchAttributes.Efficient, 5, false, 1);
        }
    }

    static AudioEvent Add(AudioTrack t, MediaStream s, double start, double length, double offset, string name)
    {
        AudioEvent e = new AudioEvent(Timecode.FromSeconds(start), Timecode.FromSeconds(length), name);
        t.Events.Add(e);
        Take k = new Take(s, true);
        e.Takes.Add(k);
        k.Offset = Timecode.FromSeconds(offset);
        e.Normalize = false;
        return e;
    }

    // (the pitch is set before the lock, so that a locked event keeps a stale value, as the test asks)
    static void Case(AudioEvent e, TimeStretchPitchShift method, ElastiqueStretchAttributes attr, double semis, bool locked, double rate)
    {
        e.Method = method;
        e.ElastiqueAttribute = attr;
        e.PitchSemis = semis;
        e.PitchLock = locked;
        e.PlaybackRate = rate;
    }

    static void CaseClassic(AudioEvent e, double semis, bool locked, double rate)
    {
        e.Method = TimeStretchPitchShift.Classic;
        e.ClassicAttribute = ClassicStretchAttributes.A03;
        e.PitchSemis = semis;
        e.PitchLock = locked;
        e.PlaybackRate = rate;
    }

}
