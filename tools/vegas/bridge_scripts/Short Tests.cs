// Short Tests (for the Reno Bridge): builds, in the open (new) project, one of the very-short-event test projects the
// Reno "shortclips" work compares with Reno. Which one: "short_test_mode.txt" in the bridge folder.
//  1a/1b/1c/1d: one audio event of the speech clip (44.1 kHz AAC) at offset O = 435.4702743 s: 1a 0.0044352 s
//               long; 1b 1 s long; 1c offset O + 0.00001 s; 1d offset O + 0.00002 s.
//  2butt/2gap/2over: three copies of 1a's event back to back; the second 1 sample (1/48000 s) later / earlier.
//  4: video, 20 events 0.0044352 s long butted from 0, each from clip_a.avi (frame numbers drawn) 0.2 s further on.
//  5: video, events 0.5 frame long starting 0.1 frame after frame 2k and 0.9 frame after frame 2k+1 (k = 0..9).
//  6: audio and video, one event of tone.wav 33756.686 samples (at 48 kHz) long.
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
    // Your own media: the folder RENO_TEST_MEDIA (default <bridge folder>\media) holds speech.mp4 (a clip with sound longer than 437 s).
    static string Speech
    {
        get
        {
            string d = System.Environment.GetEnvironmentVariable("RENO_TEST_MEDIA");
            if (string.IsNullOrEmpty(d)) d = System.IO.Path.Combine(Bridge, "media");
            return System.IO.Path.Combine(d, "speech.mp4");
        }
    }
    const double O = 435.4702743, L = 0.0044352;

    public void FromVegas(Vegas vegas)
    {
        string mode = File.ReadAllText(Path.Combine(Bridge, "short_test_mode.txt")).Trim();
        Project p = vegas.Project;
        if (mode.StartsWith("1") || mode.StartsWith("2"))
        {
            AudioTrack t = new AudioTrack(p, 0, "Short " + mode);
            p.Tracks.Add(t);
            MediaStream s = Media.CreateInstance(p, Speech).GetAudioStreamByIndex(0);
            double gap = 1.0 / 48000.0;
            switch (mode)
            {
                case "1a": Audio(t, s, 0, L, O); break;
                case "1b": Audio(t, s, 0, 1.0, O); break;
                case "1c": Audio(t, s, 0, L, O + 0.00001); break;
                case "1d": Audio(t, s, 0, L, O + 0.00002); break;
                case "2butt": Audio(t, s, 0, L, O); Audio(t, s, L, L, O); Audio(t, s, 2 * L, L, O); break;
                case "2gap": Audio(t, s, 0, L, O); Audio(t, s, L + gap, L, O); Audio(t, s, 2 * L + gap, L, O); break;
                case "2over": Audio(t, s, 0, L, O); Audio(t, s, L - gap, L, O); Audio(t, s, 2 * L - gap, L, O); break;
            }
        }
        else if (mode.StartsWith("4") || mode.StartsWith("5") || mode.StartsWith("7") || mode.StartsWith("8"))
        {
            // (a mode ending in "m" uses the H.264 clip_h264.mp4 instead of clip_a.avi; both have frame numbers drawn)
            VideoTrack t = new VideoTrack(p, 0, "Short " + mode);
            p.Tracks.Add(t);
            string clip = mode.EndsWith("m") ? "clip_h264.mp4" : "clip_a.avi";
            MediaStream s = Media.CreateInstance(p, Path.Combine(Path.Combine(Bridge, "suite"), clip)).GetVideoStreamByIndex(0);
            double frame = 1.0 / p.Video.FrameRate;
            if (mode.StartsWith("7"))
                Video(t, s, 0, 1.0, 0);                                         // the control: one plain 1 s event
            else if (mode.StartsWith("8"))
                for (int k = 0; k < 10; ++k) Video(t, s, (4 * k + 0.5) * frame, 1.5 * frame, 0.4 * k);   // 1.5 frames from half a frame in
            else if (mode.StartsWith("4"))
                for (int k = 0; k < 20; ++k) Video(t, s, k * L, L, 0.2 * k);
            else
                for (int k = 0; k < 10; ++k)
                {
                    Video(t, s, (4 * k + 0.1) * frame, 0.5 * frame, 0.4 * k);
                    Video(t, s, (4 * k + 2 + 0.9) * frame, 0.5 * frame, 0.4 * k + 0.2);
                }
        }
        else if (mode == "6")
        {
            AudioTrack a = new AudioTrack(p, 0, "Audio");
            p.Tracks.Add(a);
            Audio(a, Media.CreateInstance(p, Path.Combine(Path.Combine(Bridge, "suite"), "tone.wav")).GetAudioStreamByIndex(0), 0, 33756.686 / 48000.0, 0);
        }
    }

    static void Audio(AudioTrack t, MediaStream s, double start, double length, double offset)
    {
        AudioEvent e = new AudioEvent(Timecode.FromSeconds(start), Timecode.FromSeconds(length), "");
        t.Events.Add(e);
        Take k = new Take(s, true);
        e.Takes.Add(k);
        k.Offset = Timecode.FromSeconds(offset);
        e.Normalize = false;
        e.Method = TimeStretchPitchShift.None;
        e.FadeIn.Length = Timecode.FromSeconds(0);
        e.FadeOut.Length = Timecode.FromSeconds(0);
    }

    static void Video(VideoTrack t, MediaStream s, double start, double length, double offset)
    {
        VideoEvent e = new VideoEvent(Timecode.FromSeconds(start), Timecode.FromSeconds(length), "");
        t.Events.Add(e);
        Take k = new Take(s, true);
        e.Takes.Add(k);
        k.Offset = Timecode.FromSeconds(offset);
        e.FadeIn.Length = Timecode.FromSeconds(0);
        e.FadeOut.Length = Timecode.FromSeconds(0);
    }
}
