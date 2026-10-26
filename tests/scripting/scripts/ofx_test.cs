// tests/scripting: VEGAS's OFXEffect (an OpenFX effect's parameters) against the made-up "Glow" plug-in of
// script_test.cpp (Amount: Double 0..1, default 0.5; Radius: Integer 0..100, default 10; Enable: Boolean; Mode: Choice
// Soft/Hard/Wild; Tint: RGBA; Center: Double2D; Caption: String; a Page). script_test checks the effect's settings after.
using System;
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
        VideoEvent ev = (VideoEvent)vegas.Project.Tracks[0].Events[0];
        PlugInNode glow = vegas.VideoFX.GetChildByName("Glow");
        Check(glow != null && glow.IsOFX, "the plug-in");
        Check(glow.OFXPlugIn != null && glow.OFXPlugIn.Label == "Glow" && glow.OFXPlugIn.EffectType == OFXEffectType.Filter, "OFXPlugIn");
        Effect fx = new Effect(glow);
        ev.Effects.Add(fx);
        OFXEffect o = fx.OFXEffect;
        Check(o.Label == "Glow" && o.Grouping == "Test" && o.EffectType == OFXEffectType.Filter, "OFXEffect");
        Check(o.Parameters.Count == 8, "parameters " + o.Parameters.Count);
        Check(o.Parameters[0].ParameterType == OFXParameterType.Double && o.Parameters[0] is OFXDoubleParameter, "Double");
        Check(o.Parameters[7].ParameterType == OFXParameterType.Page && o.Parameters[7].GetType() == typeof(OFXParameter), "Page");
        Check(o.FindParameterByName("nothing") == null, "no such parameter");

        OFXDoubleParameter amount = (OFXDoubleParameter)o["Amount"];
        Check(amount.Name == "Amount" && amount.Label == "Glow amount" && amount.Hint == "How much" && amount.Index == 0, "names");
        Check(amount.Enabled && !amount.Secret && amount.CanAnimate && amount.Persisted && !amount.IsAnimated, "flags");
        Near(amount.Default, 0.5, "default");
        Near(amount.Value, 0.5, "value is the default");
        Near(amount.Min, 0, "min");
        Near(amount.Max, 1, "max");
        Near(amount.DisplayMax, 1, "display max");
        amount.Value = 0.8;
        Near(amount.Value, 0.8, "value set");
        Check(amount == o["Amount"], "the same parameter object");

        OFXIntegerParameter radius = (OFXIntegerParameter)o["Radius"];
        Check(radius.Value == 10 && radius.Max == 100, "integer");
        radius.Value = 42;
        Check(radius.Value == 42, "integer set");

        OFXBooleanParameter enable = (OFXBooleanParameter)o["Enable"];
        Check(enable.Value == false, "boolean");
        enable.Value = true;
        Check(enable.Value, "boolean set");

        OFXChoiceParameter mode = (OFXChoiceParameter)o["Mode"];
        Check(mode.Choices.Length == 3 && mode.Choices[2].Name == "Wild" && mode.Value.Index == 0 && mode.Value.Name == "Soft", "choice");
        mode.Value = mode.Choices[2];
        Check(mode.Value.Index == 2 && mode.Value.Name == "Wild", "choice set");

        OFXRGBAParameter tint = (OFXRGBAParameter)o["Tint"];
        Check(tint.Value == new OFXColor(1, 1, 1, 1), "color default");
        tint.Value = new OFXColor(1, 0, 0.5, 0.25);
        Check(tint.Value.R == 1 && tint.Value.G == 0 && tint.Value.B == 0.5 && tint.Value.A == 0.25, "color set");

        OFXDouble2DParameter center = (OFXDouble2DParameter)o["Center"];
        OFXDouble2D c = center.Value;
        Near(c.X, 0.5, "2D default");
        c.X = 0.1; c.Y = 0.9;
        center.Value = c;
        Near(center.Value.Y, 0.9, "2D set");

        OFXStringParameter caption = (OFXStringParameter)o["Caption"];
        Check(caption.Value == "hello", "text default");
        caption.Value = "Title";
        Check(caption.Value == "Title", "text set");

        // keyframes: animating Amount from 0 at 0 s to 1 at 2 s
        amount.SetValueAtTime(Timecode.FromSeconds(0), 0.0);
        amount.SetValueAtTime(Timecode.FromSeconds(2), 1.0);
        Check(amount.IsAnimated && amount.Keyframes.Count == 2, "two keyframes " + amount.Keyframes.Count);
        Near(amount.GetValueAtTime(Timecode.FromSeconds(1)), 0.5, "halfway");
        Near(amount.Keyframes[1].Time.ToMilliseconds(), 2000, "key time");
        Near(amount.Keyframes[1].Value, 1.0, "key value");
        amount.Keyframes[1].Value = 0.75;
        Near(amount.GetValueAtTime(Timecode.FromSeconds(2)), 0.75, "key value set");
        amount.Keyframes[0].Interpolation = OFXInterpolationType.Hold;
        Check(amount.Keyframes[0].Interpolation == OFXInterpolationType.Hold, "interpolation");
        Near(amount.GetValueAtTime(Timecode.FromSeconds(1)), 0.0, "held");
        amount.Keyframes[0].Interpolation = OFXInterpolationType.Linear;
        // a key moved
        amount.SetValueAtTime(Timecode.FromSeconds(3), 0.3);
        Check(amount.Keyframes.Count == 3, "three keys");
        amount.Keyframes[2].Time = Timecode.FromSeconds(3.5);
        Near(amount.Keyframes[2].Time.ToMilliseconds(), 3500, "key moved");
        amount.Keyframes.Remove(amount.Keyframes[2]);
        Check(amount.Keyframes.Count == 2, "key removed");
        // the value of an animated parameter is its value at the effect's current time
        o.CurrentTime = Timecode.FromSeconds(1);
        Near(amount.Value, 0.375, "value at the current time");
        o.CurrentTime = Timecode.FromSeconds(0);
        // a keyframed choice, and turning the animation off keeps the value at the current time
        mode.SetValueAtTime(Timecode.FromSeconds(1), mode.Choices[1]);
        Check(mode.IsAnimated && mode.Keyframes.Count == 1 && mode.Keyframes[0].Value.Index == 1, "choice key");
        mode.IsAnimated = false;
        Check(!mode.IsAnimated && mode.Value.Index == 1, "animation off");
        // text keys
        caption.SetValueAtTime(Timecode.FromSeconds(2), "Later");
        Check(caption.Keyframes.Count == 1 && caption.Keyframes[0].Value == "Later", "text key");
        caption.IsAnimated = false;

        // what a parameter cannot do is named
        try { string d = amount.Definition; Check(false, "Definition " + d); }
        catch (NotSupportedException e) { Check(e.Message.Contains("Parameter.Definition is not supported by Reno"), "Definition named: " + e.Message); }

        // an effect that is not OpenFX
        Effect rev = new Effect(vegas.AudioFX.GetChildByName("Reverb"));
        ((AudioEvent)vegas.Project.Tracks[1].Events[0]).Effects.Add(rev);
        try { OFXEffect x = rev.OFXEffect; Check(false, "audio OFXEffect " + x); }
        catch (InvalidOperationException) { ++checks; }

        vegas.DebugOut("OFX TEST OK " + checks + " checks");
    }
}
