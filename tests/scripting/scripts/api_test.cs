// tests/scripting: goes through much of the VEGAS scripting API against the project script_test.cpp builds
// (see makeProject there) and checks what it reads back. Written as a VEGAS script would be (C# 5).
using System;
using System.Collections.Generic;
using ScriptPortal.Vegas;

public class EntryPoint
{
    Vegas vegas;
    int checks = 0;

    void Check(bool ok, string what)
    {
        ++checks;
        if (!ok) throw new ApplicationException("CHECK FAILED: " + what);
    }
    void Near(double a, double b, string what) { Check(Math.Abs(a - b) < 1e-6, what + " (" + a + " vs " + b + ")"); }

    public void FromVegas(Vegas v)
    {
        vegas = v;
        using (UndoBlock undo = new UndoBlock("API Test"))
        {
            Basics();
            TimecodeTests();
            Editing();
            Dialogs();
        }
        vegas.DebugOut("API TEST OK " + checks + " checks");
    }

    // Vegas.FileUtilities (the test's Windows Forms answers from RENO_TEST_FOLDER / RENO_TEST_SAVE_PATH)
    void Dialogs()
    {
        string dir;
        vegas.FileUtilities.SelectDirectoryDlg(vegas.MainWindow.Handle, "Folder", "", false, out dir);
        Check(dir == Environment.GetEnvironmentVariable("RENO_TEST_FOLDER"), "SelectDirectoryDlg " + dir);
        string file;
        int index = 1;
        vegas.FileUtilities.CreateFileDlg(vegas.MainWindow.Handle, "Save", "Text (*.txt)|*.txt|", "", out file, ref index);
        Check(file == Environment.GetEnvironmentVariable("RENO_TEST_SAVE_PATH"), "CreateFileDlg " + file);
        Check(vegas.FileUtilities.GetDiskTotalSpace("/", 1024 * 1024) > 0, "GetDiskTotalSpace");
    }

    void Basics()
    {
        Check(vegas.Version.Contains("Reno"), "Version");
        Project p = vegas.Project;
        Check(p.Video.Width == 1920 && p.Video.Height == 1080, "frame size");
        Near(p.Video.FrameRate, 30, "frame rate");
        Check(p.Audio.SampleRate == 48000, "sample rate");
        Check(p.Tracks.Count == 3, "three tracks");
        Check(p.Tracks[0].IsVideo() && p.Tracks[1].IsAudio() && p.Tracks[2].IsVideo(), "track kinds");
        Check(p.Tracks[0] is VideoTrack && p.Tracks[1] is AudioTrack, "track classes");
        Check(p.Tracks[0].Name == "V1", "track name");
        Check(p.Tracks[0] == p.Tracks[0] && ReferenceEquals(p.Tracks[0], p.Tracks[0]), "the same track object each time");
        Check(p.Tracks[0].Index == 0 && p.Tracks[2].Index == 2, "track index");
        TrackEvent a = p.Tracks[0].Events[0];
        TrackEvent b = p.Tracks[1].Events[0];
        Check(a is VideoEvent && b is AudioEvent, "event classes");
        Check(a.MediaType == MediaType.Video && b.MediaType == MediaType.Audio, "event media types");
        Near(a.Start.ToMilliseconds(), 0, "event start");
        Near(a.Length.ToMilliseconds(), 4000, "event length");
        Near(a.End.ToMilliseconds(), 4000, "event end");
        Check(a.Selected && b.Selected, "selection");
        Check(a.IsGrouped && a.Group == b.Group && a.Group.Count == 2 && a.Group.Contains(b), "group");
        Check(a.ActiveTake != null && a.Takes.Count == 1 && a.ActiveTake.IsActive, "take");
        Check(a.ActiveTake.Media.FilePath.EndsWith("clip.mp4"), "take media");
        Check(a.ActiveTake.Media == b.ActiveTake.Media, "same media object");
        Check(a.ActiveTake.MediaStream is VideoStream && b.ActiveTake.MediaStream is AudioStream, "streams");
        Media m = a.ActiveTake.Media;
        Check(m.HasVideo() && m.HasAudio() && !m.IsGenerated(), "media kinds");
        Near(m.Length.ToMilliseconds(), 10000, "media length");
        VideoStream vs = m.GetVideoStreamByIndex(0);
        Check(vs != null && vs.Width == 1280 && vs.Height == 720, "video stream size");
        Near(vs.FrameRate, 25, "stream fps");
        Check(m.GetAudioStreamByIndex(0).Channels == 2, "audio channels");
        Check(m.Streams.Count == 2, "stream count");
        Check(p.MediaPool.Count == 2, "media pool count");
        int n = 0;
        foreach (Media mm in p.MediaPool) { Check(mm != null, "media pool enumerates Media"); ++n; }
        Check(n == 2, "media pool enumeration");
        Check(p.MediaPool.Find(m.FilePath) == m, "MediaPool.Find");
        Check(p.Markers.Count == 1 && p.Markers[0].Label == "m1", "markers");
        Check(p.Regions.Count == 1 && p.Regions[0].Label == "r1", "regions");
        Near(p.Regions[0].Length.ToMilliseconds(), 1000, "region length");
        Near(vegas.Transport.CursorPosition.ToMilliseconds(), 1500, "cursor");
        Check(vegas.VideoFX.GetChildByName("VEGAS Gaussian Blur") != null, "plug-in by its VEGAS name");
        Check(vegas.VideoFX.GetChildByName("Gaussian Blur") == vegas.VideoFX.GetChildByName("VEGAS Gaussian Blur"), "plug-in without the maker");
        Check(vegas.VideoFX.GetChildByName("No Such Effect") == null, "missing plug-in");
        Check(vegas.AudioFX.Count == 1 && vegas.AudioFX[0].IsAudio, "audio plug-ins");
        PlugInNode blur = vegas.VideoFX.GetChildByName("Gaussian Blur");
        Check(vegas.VideoFX.GetChildByClassID(blur.ClassID) == blur, "plug-in by class id");
        Check(vegas.VideoFX.GetChildByUniqueID(blur.UniqueID) == blur, "plug-in by unique id");
        Check(vegas.PlugIns.Count == 4, "plug-in folders");
    }

    void TimecodeTests()
    {
        Near(Timecode.FromFrames(30).ToMilliseconds(), 1000, "FromFrames");
        Check(Timecode.FromSeconds(1.5).FrameCount == 45, "FrameCount");
        Check(Timecode.FromSeconds(1).ToString() == "00:00:01:00", "ToString: " + Timecode.FromSeconds(1).ToString());
        Check(Timecode.FromString("00:00:02:15").FrameCount == 75, "FromString frames");
        Near(Timecode.FromString("00:01:02.500", RulerFormat.Time).ToMilliseconds(), 62500, "FromString time");
        Near(Timecode.FromString("2.25", RulerFormat.Seconds).ToMilliseconds(), 2250, "FromString seconds");
        Check(Timecode.FromSeconds(1) + Timecode.FromSeconds(2) == Timecode.FromSeconds(3), "Timecode +");
        Check(Timecode.FromSeconds(3) - Timecode.FromSeconds(1) > Timecode.FromSeconds(1.5), "Timecode - and >");
        Check(new Timecode(500).Nanos == 5000000, "Nanos are 100 ns");
        Timecode t = Timecode.FromFrames(10);
        t++;
        Check(t.FrameCount == 11, "Timecode ++");
        Check(Timecode.FromSeconds(1).ToString(RulerFormat.Time) == "00:00:01.000", "ToString(Time)");
    }

    void Editing()
    {
        Project p = vegas.Project;
        VideoTrack v1 = (VideoTrack)p.Tracks[0];
        AudioTrack a1 = (AudioTrack)p.Tracks[1];
        VideoEvent a = (VideoEvent)v1.Events[0];
        AudioEvent b = (AudioEvent)a1.Events[0];

        // split
        TrackEvent right = a.Split(Timecode.FromSeconds(1));
        Near(a.Length.ToMilliseconds(), 1000, "left part after split");
        Near(right.Start.ToMilliseconds(), 1000, "right part start");
        Near(right.ActiveTake.Offset.ToMilliseconds(), 1500, "right part's offset (the event started 0.5 s in)");
        Check(v1.Events.Count == 2 && v1.Events[1] == right, "split adds an event");

        // copy
        VideoTrack v2 = (VideoTrack)p.Tracks[2];
        TrackEvent copy = right.Copy(v2, Timecode.FromSeconds(10));
        Check(v2.Events.Count == 1 && copy.Track == v2, "copy");
        Near(copy.Start.ToMilliseconds(), 10000, "copy start");
        bool threw = false;
        try { right.Copy(a1, Timecode.FromSeconds(0)); } catch (InvalidOperationException) { threw = true; }
        Check(threw, "copying video onto an audio track fails");

        // move to a track
        copy.Track = v1;
        Check(copy.Track == v1 && v2.Events.Count == 0, "TrackEvent.Track moves it");

        // properties
        right.PlaybackRate = 2;
        Near(right.PlaybackRate, 2, "rate");
        right.Mute = true; right.Loop = false; right.Locked = false;
        Check(right.Mute && !right.Loop, "switches");
        right.FadeIn.Length = Timecode.FromFrames(10);
        right.FadeIn.Curve = CurveType.Slow;
        right.FadeOut.Length = Timecode.FromFrames(5);
        right.FadeIn.Gain = 0.5f;
        Check(right.FadeIn.Length.FrameCount == 10 && right.FadeIn.Curve == CurveType.Slow, "fade in");
        Near(right.FadeIn.Gain, 0.5, "event level (FadeIn.Gain)");
        Check(right.FadeOut.Length.FrameCount == 5, "fade out");
        right.Name = "renamed";
        Check(right.Name == "renamed" && right.ActiveTake.Name == "renamed", "name");
        a.Selected = false;
        Check(!a.Selected, "deselect");

        // audio
        b.PitchLock = false;
        b.PitchSemis = 3;
        b.Method = TimeStretchPitchShift.Classic;
        b.ClassicAttribute = ClassicStretchAttributes.A05;
        Check(b.PitchSemis == 3 && b.Method == TimeStretchPitchShift.Classic && b.ClassicAttribute == ClassicStretchAttributes.A05 && !b.PitchLock, "audio stretch settings");
        b.Channels = ChannelRemapping.Mono;
        Check(b.Channels == ChannelRemapping.Mono, "channels");
        b.InvertPhase = true;
        Check(b.InvertPhase, "invert phase");

        // effects
        PlugInNode blur = vegas.VideoFX.GetChildByName("Gaussian Blur");
        Effect fx = a.Effects.AddEffect(blur);
        Check(a.Effects.Count == 1 && a.Effects[0] == fx && fx.PlugIn == blur, "event effect");
        Check(fx.Description == blur.Name, "effect name");
        fx.Bypass = true;
        Check(fx.Bypass, "bypass");
        v1.Effects.Add(new Effect(vegas.VideoFX.GetChildByName("Glow")));
        Check(v1.Effects.Count == 1, "track effect");
        b.Effects.AddEffect(vegas.AudioFX[0]);
        Check(b.Effects.Count == 1, "audio event effect");
        threw = false;
        try { a.Effects.AddEffect(vegas.AudioFX[0]); } catch (InvalidOperationException) { threw = true; }
        Check(threw, "audio effect on video fails");
        a.Effects.Remove(fx);
        Check(a.Effects.Count == 0, "remove effect");

        // transition
        right.FadeIn.Transition = new Effect(vegas.Transitions.GetChildByName("Cross Fade"));
        Check(right.FadeIn.Transition is Effect, "transition");

        // envelopes
        Envelope vel = new Envelope(EnvelopeType.Velocity);
        a.Envelopes.Add(vel);
        Check(a.Envelopes.Count == 1 && a.Envelopes.HasEnvelope(EnvelopeType.Velocity), "velocity envelope");
        Check(vel.Points.Count == 1, "a new envelope has one point");
        vel.Points.Add(new EnvelopePoint(Timecode.FromSeconds(0.5), 2.0, CurveType.Smooth));
        Check(vel.Points.Count == 2 && vel.Points[1].Curve == CurveType.Smooth, "point added");
        Near(vel.ValueAt(Timecode.FromSeconds(0.5)), 2.0, "ValueAt");
        vel.Points[0].Y = -1;
        Near(vel.Points[0].Y, -1, "point value");
        Envelope vol = new Envelope(EnvelopeType.Volume);
        a1.Envelopes.Add(vol);
        vol.Points.Add(new EnvelopePoint(Timecode.FromSeconds(2), 0.25));
        Check(a1.Envelopes.FindByType(EnvelopeType.Volume) == vol && vol.Points.Count == 2, "track volume envelope");
        Check(vol.Points.GetPointAtX(Timecode.FromSeconds(2)) != null, "GetPointAtX");
        Near(vol.Max, 4, "volume envelope's top");
        threw = false;
        try { a1.Envelopes.Add(new Envelope(EnvelopeType.Composite)); } catch (InvalidOperationException) { threw = true; }
        Check(threw, "composite envelope on an audio track fails");

        // a new track, event and take
        Media other = p.MediaPool.AddMedia("/media/other.mp4");
        Check(p.MediaPool.Count == 3 && other.HasVideo(), "AddMedia");
        Check(p.MediaPool.AddMedia("/media/other.mp4") == other, "AddMedia twice gives the same media");
        VideoTrack nt = new VideoTrack(1, "New");
        p.Tracks.Add(nt);
        Check(p.Tracks.Count == 4 && p.Tracks[1] == nt && nt.Name == "New", "new track at index 1");
        VideoEvent ne = new VideoEvent(Timecode.FromSeconds(2), Timecode.FromSeconds(3));
        nt.Events.Add(ne);
        Check(ne.Track == nt && nt.Events.Count == 1, "new event");
        Check(ne.Takes.Count == 0 && ne.ActiveTake == null, "an empty event has no take");
        Take tk = ne.AddTake(other.GetVideoStreamByIndex(0));
        Check(ne.Takes.Count == 1 && ne.ActiveTake == tk && tk.Media == other, "AddTake");
        tk.Offset = Timecode.FromSeconds(1);
        Near(ne.ActiveTake.Offset.ToMilliseconds(), 1000, "take offset");
        Take second = new Take(m(a).GetVideoStreamByIndex(0), false, "second");
        ne.Takes.Add(second);
        Check(ne.Takes.Count == 2 && ne.ActiveTake == tk, "a second take, not active");
        ne.ActiveTake = ne.Takes[1];
        Check(ne.ActiveTake.Name == "second", "switch take");
        AudioEvent ae = a1.AddAudioEvent(Timecode.FromSeconds(20), Timecode.FromSeconds(1));
        Check(ae.Track == a1 && a1.Events.Count == 2, "AddAudioEvent");

        // pan/crop
        VideoMotionKeyframe k0 = a.VideoMotion.Keyframes[0];
        Check(a.VideoMotion.Keyframes.Count == 1, "one pan/crop key");
        Near(k0.Center.X, 640, "pan/crop centre in pixels");
        k0.Bounds = new VideoMotionBounds(new VideoMotionVertex(0, 0), new VideoMotionVertex(640, 0), new VideoMotionVertex(640, 360), new VideoMotionVertex(0, 360));
        Near(k0.Center.X, 320, "pan/crop box moved");
        VideoMotionKeyframe k1 = new VideoMotionKeyframe(Timecode.FromSeconds(0.5));
        a.VideoMotion.Keyframes.Add(k1);
        k1.Center = new VideoMotionVertex(900, 500);
        Check(a.VideoMotion.Keyframes.Count == 2, "second pan/crop key");
        k1.Rotation = Math.PI / 2;
        Near(k1.Rotation, Math.PI / 2, "rotation");

        // groups
        TrackEventGroup g = new TrackEventGroup();
        p.Groups.Add(g);
        g.Add(ne);
        g.Add(ae);
        Check(ne.IsGrouped && ne.Group == g && g.Count == 2, "new group");

        // markers
        p.Markers.Add(new Marker(Timecode.FromSeconds(5), "five"));
        p.Regions.Add(new Region(Timecode.FromSeconds(6), Timecode.FromSeconds(2), "six"));
        Check(p.Markers.Count == 2 && p.Regions.Count == 2, "markers added");
        Check(p.Markers[1].Label == "five", "markers in time order");
        Marker mk = p.Markers[0];
        mk.Position = Timecode.FromSeconds(7);
        Check(p.Markers[1] == mk, "moved marker");
        threw = false;
        try { p.Markers.Add(new Marker(Timecode.FromSeconds(5), "again")); } catch (InvalidOperationException) { threw = true; }
        Check(threw, "two markers at one place fail");
        p.Markers.Remove(mk);
        Check(p.Markers.Count == 1, "remove marker");

        // project settings and the rest
        p.Summary.Title = "Script title";
        Check(p.Summary.Title == "Script title", "summary");
        vegas.Transport.SelectionStart = Timecode.FromSeconds(1);
        vegas.Transport.SelectionLength = Timecode.FromSeconds(2);
        vegas.Cursor = Timecode.FromSeconds(3);
        Near(vegas.SelectionLength.ToMilliseconds(), 2000, "selection");
        v1.Mute = true;
        Check(v1.Mute, "track mute");
        ((AudioTrack)a1).Volume = 0.5f;
        Near(a1.Volume, 0.5, "track volume");
        v1.CompositeMode = CompositeMode.Add;
        Check(v1.CompositeMode == CompositeMode.Add, "composite mode");
        Check(p.MasterBus.IsMaster() && p.MasterBus.Effects.Count == 0, "master bus");

        // what Reno cannot do says so
        threw = false;
        try { a.SnapOffset = Timecode.FromSeconds(1); }
        catch (NotSupportedException e) { threw = e.Message.Contains("SnapOffset"); }
        Check(threw, "NotSupportedException names the member");

        // removing
        nt.Events.Remove(ne);
        Check(nt.Events.Count == 0 && !ne.IsValid(), "remove event");
        p.Tracks.Remove(nt);
        Check(p.Tracks.Count == 3 && !nt.IsValid(), "remove track");
    }

    static Media m(TrackEvent e) { return e.ActiveTake.Media; }
}
