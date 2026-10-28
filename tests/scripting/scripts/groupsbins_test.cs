// tests/scripting: HDR and stereoscopic 3D project settings, track groups and media bins (the project of script_test.cpp:
// tracks V1, A1, V2; media clip.mp4 and still.png). script_test checks the project after.
using System;
using ScriptPortal.Vegas;

public class EntryPoint
{
    int checks = 0;

    void Check(bool ok, string what)
    {
        ++checks;
        if (!ok) throw new ApplicationException("CHECK FAILED: " + what);
    }

    public void FromVegas(Vegas vegas)
    {
        ProjectVideoProperties vp = vegas.Project.Video;
        vp.HDRMode = HDRMode.HLG;
        Check(vp.HDRMode == HDRMode.HLG, "HDR mode");
        vp.Stereo3DMode = Stereo3DOutputMode.SideBySideFull;
        Check(vp.Stereo3DMode == Stereo3DOutputMode.SideBySideFull, "stereo mode");
        vp.SetStereo3DSwap(Stereo3DOutputMode.SideBySideFull, true);
        Check(vp.GetStereo3DSwap(Stereo3DOutputMode.SideBySideFull) && !vp.GetStereo3DSwap(Stereo3DOutputMode.TopBottomFull), "swap");
        vp.SetStereo3DCrosstalkCancelation(Stereo3DOutputMode.SideBySideFull, 0.5f);
        Check(Math.Abs(vp.GetStereo3DCrosstalkCancelation(Stereo3DOutputMode.SideBySideFull) - 0.5f) < 1e-6, "crosstalk");

        Check(vegas.Project.TrackGroups.Count == 0, "no track groups");
        vegas.Project.Tracks[0].Selected = true;
        vegas.Project.Tracks[2].Selected = true;
        TrackGroup g = vegas.Project.GroupSelectedTracks();
        Check(g != null && g.IsValid() && g.Count == 2, "grouped");
        Check(vegas.Project.TrackGroups.Count == 1 && vegas.Project.GetTrackGroupOfTrack(0) == g, "the group of a track");
        Check(vegas.Project.GetTrackGroupOfTrack(1) == null, "the audio track is in none");
        g.Mute = true;
        Check(vegas.Project.Tracks[0].Mute && vegas.Project.Tracks[2].Mute && g.Mute, "group mute");
        g.InsertTrack(vegas.Project.Tracks[1], 1);
        Check(g.Count == 3, "a track is inserted");

        MediaBin root = vegas.Project.MediaPool.RootMediaBin;
        Check(root.IsRoot() && root.IsValid(), "root bin");
        MediaBin b = new MediaBin(vegas.Project, "Footage");
        Check(b.Name == "Footage" && b.Parent == root && !b.IsRoot(), "a bin in the root");
        MediaBin c = new MediaBin("Day 1");
        b.Add(c);
        Check(c.IsValid() && c.Parent == b, "a bin in a bin");
        Media m = vegas.Project.MediaPool.Find("/media/clip.mp4");
        if (m != null)
        {
            c.Add(m);
            Check(c.Count == 1 && c[0] == m, "media in a bin");
        }
        b.Name = "Raw";
        Check(c.Parent.Name == "Raw", "renamed");
        // render formats and templates (rendering itself needs a made file: the render of an empty range is NothingToDo)
        Check(vegas.Renderers.Count > 0, "render formats are listed");
        Renderer r = vegas.Renderers[0];
        Check(r.Name.Length > 0 && r.FileExtension.Length > 0 && r.Templates.Count > 0, "a format has a name, an extension and templates");
        RenderTemplate t = r.Templates[0];
        Check(t.Name.Length > 0 && t.RendererID == r.ID && vegas.Renderers.FindByRendererID(r.ID) == r, "a template knows its format");
        Check(r.Templates.FindByName(t.Name) == t, "a template is found by name");
        Check(vegas.Render("unused." + r.FileExtension, t, new Timecode(), new Timecode()) == RenderStatus.NothingToDo, "a render of nothing");
        vegas.DebugOut("GROUPS AND BINS OK " + checks + " checks");
    }
}
