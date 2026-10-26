// Bypass All Effects (for the Reno Bridge): every effect in the open project is bypassed - event, track, bus,
// master, video output and media effects - so that a render can be compared with Reno's for timing alone.
// The project is not saved.
using System;
using ScriptPortal.Vegas;

public class EntryPoint
{
    public void FromVegas(Vegas vegas)
    {
        Project p = vegas.Project;
        foreach (Track t in p.Tracks)
        {
            Off(t.Effects);
            foreach (TrackEvent e in t.Events)
            {
                AudioEvent ae = e as AudioEvent;
                if (ae != null) Off(ae.Effects);
                VideoEvent ve = e as VideoEvent;
                if (ve != null) Off(ve.Effects);
            }
        }
        foreach (BusTrack bt in p.BusTracks) { AudioBusTrack b = bt as AudioBusTrack; if (b != null) Off(b.Effects); }
        foreach (Media m in p.MediaPool) { try { Off(m.Effects); } catch (Exception) { } }
        try { Off(p.VideoBus.Effects); } catch (Exception) { }
    }

    static void Off(Effects fx)
    {
        if (fx == null) return;
        foreach (Effect e in fx) { try { e.Bypass = true; } catch (Exception) { } }
    }
}
