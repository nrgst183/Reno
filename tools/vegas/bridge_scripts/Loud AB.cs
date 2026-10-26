// Loud AB (for the Reno Bridge): the level tests of Reno's "loud" work. "loud_mode.txt" in the bridge folder holds A or B.
//  A: élastique Efficient / Pro / Soloist (Speech) on speech (the speech clip at 14.2 s), noise (loud\noise_st.wav) and a
//     tone (req\tone440.wav), each at rates 1.25 1.5 2 3 4 0.5 and pitches -12 -7 -3 -1 +1 +3 +7 +12; then a plain event
//     of each source and two Efficient events at rate 1, pitch 0. Events 1 s long, 1.5 s apart, one track.
//  B: channel, pan and level cases of loud\lr.wav (L 440 Hz 0.5, R 880 Hz 0.25) and loud\mono440.wav: one track per
//     case, events 1 s long, 1.5 s apart (case k from 1.5 (k - 1) s).
// Setup: the bridge folder is RENO_BRIDGE_DIR (default Documents\Reno Bridge); your own speech clip is speech.mp4 in the folder RENO_TEST_MEDIA (default <bridge folder>\media).
using System;
using System.Globalization;
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
    // Your own media: the folder RENO_TEST_MEDIA (default <bridge folder>\media) holds speech.mp4 (any clip with speech sound, at least 20 s long).
    static string Speech
    {
        get
        {
            string d = System.Environment.GetEnvironmentVariable("RENO_TEST_MEDIA");
            if (string.IsNullOrEmpty(d)) d = System.IO.Path.Combine(Bridge, "media");
            return System.IO.Path.Combine(d, "speech.mp4");
        }
    }
    double at = 0;

    public void FromVegas(Vegas vegas)
    {
        string mode = File.ReadAllText(Path.Combine(Bridge, "loud_mode.txt")).Trim().ToUpperInvariant();
        Project p = vegas.Project;
        if (mode == "A")
        {
            AudioTrack t = new AudioTrack(p, 0, "Loud A");
            p.Tracks.Add(t);
            MediaStream speech = Media.CreateInstance(p, Speech).GetAudioStreamByIndex(0);
            MediaStream noise = Media.CreateInstance(p, Path.Combine(Path.Combine(Bridge, "loud"), "noise_st.wav")).GetAudioStreamByIndex(0);
            MediaStream tone = Media.CreateInstance(p, Path.Combine(Path.Combine(Bridge, "req"), "tone440.wav")).GetAudioStreamByIndex(0);
            string[] srcNames = { "SPEECH", "NOISE", "TONE" };
            MediaStream[] srcs = { speech, noise, tone };
            double[] offs = { 14.2, 1.0, 1.0 };
            ElastiqueStretchAttributes[] modes = { ElastiqueStretchAttributes.Efficient, ElastiqueStretchAttributes.Pro, ElastiqueStretchAttributes.Soloist_Speech };
            double[] rates = { 1.25, 1.5, 2, 3, 4, 0.5 };
            double[] semis = { -12, -7, -3, -1, 1, 3, 7, 12 };
            for (int s = 0; s < 3; ++s)
                foreach (ElastiqueStretchAttributes m in modes)
                {
                    foreach (double r in rates)
                        Stretch(Add(t, srcs[s], offs[s], srcNames[s] + "_" + m + "_r" + r.ToString(CultureInfo.InvariantCulture)), m, r, 0);
                    foreach (double q in semis)
                        Stretch(Add(t, srcs[s], offs[s], srcNames[s] + "_" + m + "_p" + (q > 0 ? "+" : "") + q.ToString(CultureInfo.InvariantCulture)), m, 1, q);
                }
            for (int s = 0; s < 3; ++s) { AudioEvent e = Add(t, srcs[s], offs[s], srcNames[s] + "_none"); e.Method = TimeStretchPitchShift.None; }
            for (int k = 0; k < 2; ++k) Stretch(Add(t, speech, offs[0], "SPEECH_Efficient_r1_p0"), ElastiqueStretchAttributes.Efficient, 1, 0);
        }
        else
        {
            MediaStream lr = Media.CreateInstance(p, Path.Combine(Path.Combine(Bridge, "loud"), "lr.wav")).GetAudioStreamByIndex(0);
            MediaStream mono = Media.CreateInstance(p, Path.Combine(Path.Combine(Bridge, "loud"), "mono440.wav")).GetAudioStreamByIndex(0);
            Case(p, 1, lr, ChannelRemapping.None, 0, 1, 1);
            Case(p, 2, lr, ChannelRemapping.Swap, 0, 1, 1);
            Case(p, 3, lr, ChannelRemapping.Mono, 0, 1, 1);
            Case(p, 4, lr, ChannelRemapping.DisableLeft, 0, 1, 1);
            Case(p, 5, lr, ChannelRemapping.DisableRight, 0, 1, 1);
            Case(p, 6, mono, ChannelRemapping.None, 0, 1, 1);
            Case(p, 7, lr, ChannelRemapping.None, -1, 1, 1);
            Case(p, 8, lr, ChannelRemapping.None, -0.5f, 1, 1);
            Case(p, 9, lr, ChannelRemapping.None, 0.5f, 1, 1);
            Case(p, 10, mono, ChannelRemapping.None, -0.5f, 1, 1);
            Case(p, 11, mono, ChannelRemapping.None, -1, 1, 1);
            Case(p, 12, lr, ChannelRemapping.None, 0, 1.9952623f, 1);
            Case(p, 13, lr, ChannelRemapping.None, 0, 1, 0.50118723f);     // (event gain -6 dB: VEGAS refuses more than 0 dB)
        }
    }

    AudioEvent Add(AudioTrack t, MediaStream s, double offset, string name)
    {
        AudioEvent e = new AudioEvent(Timecode.FromSeconds(at), Timecode.FromSeconds(1.0), name);
        t.Events.Add(e);
        Take k = new Take(s, true);
        e.Takes.Add(k);
        k.Offset = Timecode.FromSeconds(offset);
        e.Normalize = false;
        e.FadeIn.Length = Timecode.FromSeconds(0);
        e.FadeOut.Length = Timecode.FromSeconds(0);
        at += 1.5;
        return e;
    }

    static void Stretch(AudioEvent e, ElastiqueStretchAttributes m, double rate, double semis)
    {
        e.Method = TimeStretchPitchShift.Elastique;
        e.ElastiqueAttribute = m;
        e.PitchSemis = semis;
        e.PitchLock = false;
        e.PlaybackRate = rate;
    }

    void Case(Project p, int k, MediaStream s, ChannelRemapping ch, float pan, float volume, float gain)
    {
        AudioTrack t = new AudioTrack(p, k - 1, "Case " + k);
        p.Tracks.Add(t);
        t.PanX = pan;
        t.Volume = volume;
        at = 1.5 * (k - 1);
        AudioEvent e = Add(t, s, 1.0, "case " + k);
        e.Channels = ch;
        e.FadeIn.Gain = gain;
    }
}
