// tests/scripting: a script that uses something Reno cannot do (the error must come back naming it)
using ScriptPortal.Vegas;
public class EntryPoint
{
    public void FromVegas(Vegas vegas)
    {
        Effect e = vegas.Project.Tracks[0].Events[0] is VideoEvent ? ((VideoEvent)vegas.Project.Tracks[0].Events[0]).Effects.AddEffect(vegas.VideoFX[0]) : null;
        e.Preset = "Strong";
    }
}
