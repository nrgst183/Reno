// Loud Tests (for the Reno Bridge): four audio tracks of the same 440 Hz tone (req\tone440.wav, amplitude 0.5) that
// come in one after another (track k from k seconds to 4 s), all faders at 0 dB and pans centred as VEGAS makes new
// tracks - so the render shows how VEGAS adds tracks up (no limiter: 0.5, 1.0, 1.5, 2.0 if it simply sums).
// Setup: the bridge folder is RENO_BRIDGE_DIR (default Documents\Reno Bridge).
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

    public void FromVegas(Vegas vegas)
    {
        Project p = vegas.Project;
        MediaStream s = Media.CreateInstance(p, Path.Combine(Path.Combine(Bridge, "req"), "tone440.wav")).GetAudioStreamByIndex(0);
        for (int k = 0; k < 4; ++k)
        {
            AudioTrack t = new AudioTrack(p, k, "Tone " + (k + 1));
            p.Tracks.Add(t);
            AudioEvent e = new AudioEvent(Timecode.FromSeconds(k), Timecode.FromSeconds(4 - k), "");
            t.Events.Add(e);
            Take tk = new Take(s, true);
            e.Takes.Add(tk);
            tk.Offset = Timecode.FromSeconds(1);
            e.Normalize = false;
        }
    }
}
