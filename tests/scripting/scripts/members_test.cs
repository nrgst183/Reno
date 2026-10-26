// tests/scripting: goes through EVERY public property (and every parameterless Is/Has/Can/Get method) of every object
// a script can reach from Vegas, on a project with something of each kind in it, through reflection:
//  - reading it must work, or throw a NotSupportedException that names the member ("Track.X is not supported by Reno");
//    any other exception is a failure;
//  - a property that can be set is set to the value just read, which must work (or be NotSupported) and leave the
//    same value.
// The unsupported members met are listed in the output (script_test prints them).
using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using ScriptPortal.Vegas;

public class EntryPoint
{
    Vegas vegas;
    int props = 0, methods = 0, sets = 0, objects = 0;
    List<string> failures = new List<string>();
    SortedDictionary<string, int> unsupported = new SortedDictionary<string, int>();
    HashSet<object> seen = new HashSet<object>(new RefEq());

    class RefEq : IEqualityComparer<object>
    {
        public new bool Equals(object a, object b) { return ReferenceEquals(a, b); }
        public int GetHashCode(object o) { return System.Runtime.CompilerServices.RuntimeHelpers.GetHashCode(o); }
    }

    public void FromVegas(Vegas v)
    {
        vegas = v;
        Prepare();
        Sweep(vegas, "Vegas", 0);
        foreach (KeyValuePair<string, int> kv in unsupported) vegas.DebugOut("unsupported: " + kv.Key);
        vegas.DebugOut("MEMBERS: " + objects + " objects, " + props + " properties read, " + sets + " set back, " + methods + " methods called, " +
                       unsupported.Count + " members not supported");
        if (failures.Count > 0)
        {
            foreach (string f in failures) vegas.DebugOut("FAIL " + f);
            throw new ApplicationException(failures.Count + " member checks failed, the first: " + failures[0]);
        }
        vegas.DebugOut("MEMBERS OK");
    }

    // something of every kind: effects (with OpenFX parameters), envelopes, pan/crop and track motion keys, markers of
    // each kind, a group, a subclip, a take, a transition
    void Prepare()
    {
        Project p = vegas.Project;
        VideoTrack v1 = (VideoTrack)p.Tracks[0];
        AudioTrack a1 = (AudioTrack)p.Tracks[1];
        VideoEvent ve = (VideoEvent)v1.Events[0];
        AudioEvent ae = (AudioEvent)a1.Events[0];
        PlugInNode glow = vegas.VideoFX.GetChildByName("Glow");
        Effect fx = ve.Effects.AddEffect(glow);
        ((OFXDoubleParameter)fx.OFXEffect["Amount"]).SetValueAtTime(Timecode.FromSeconds(1), 0.25);
        v1.Effects.AddEffect(glow);
        ae.Effects.AddEffect(vegas.AudioFX.GetChildByName("Reverb"));
        Envelope env = new Envelope(EnvelopeType.Velocity);
        ve.Envelopes.Add(env);
        env.Points.Add(new EnvelopePoint(Timecode.FromSeconds(1), 1.5));
        Envelope vol = new Envelope(EnvelopeType.Volume);
        a1.Envelopes.Add(vol);
        ve.VideoMotion.Keyframes.Add(new VideoMotionKeyframe(Timecode.FromSeconds(1)));
        v1.TrackMotion.InsertMotionKeyframe(Timecode.FromSeconds(1));
        p.Markers.Add(new Marker(Timecode.FromSeconds(3), "m"));
        p.CommandMarkers.Add(new CommandMarker(Timecode.FromSeconds(3), MarkerCommandType.URL, "http://x"));
        p.CDTracks.Add(new CDRegion(Timecode.FromSeconds(0), Timecode.FromSeconds(1)));
        p.CDIndices.Add(new CDMarker(Timecode.FromSeconds(0.5)));
        TrackEvent copy = ve.Copy(v1, Timecode.FromSeconds(3.5));
        copy.FadeIn.Transition = new Effect(vegas.Transitions.GetChildByName("VEGAS Cross Fade"));
        Subclip clip = new Subclip(p, ve.ActiveTake.Media.FilePath, Timecode.FromSeconds(1), Timecode.FromSeconds(1), true, "rev");
        ve.AddTake(clip.GetVideoStreamByIndex(0), false, "rev");
        new Media(vegas.Generators.GetChildByName("VEGAS Solid Color"));
        p.BusTracks.Count.ToString();
    }

    static bool IsApiType(Type t) { return t.Namespace == "ScriptPortal.Vegas" && !t.IsEnum && !t.IsPrimitive; }

    void Sweep(object o, string path, int depth)
    {
        if (o == null || depth > 5 || !seen.Add(o)) return;
        ++objects;
        Type t = o.GetType();
        foreach (PropertyInfo pi in t.GetProperties(BindingFlags.Public | BindingFlags.Instance))
        {
            if (pi.GetIndexParameters().Length > 0) continue;
            MethodInfo getter = pi.GetGetMethod();
            if (getter == null) continue;
            string name = t.Name + "." + pi.Name;
            object val;
            if (!Try(delegate { return pi.GetValue(o, null); }, name, out val)) continue;
            ++props;
            MethodInfo setter = pi.GetSetMethod();
            if (setter != null && !(o is OFXParameter && pi.Name == "IsAnimated") && SetBack(o, pi, val, name)) ++sets;
            Follow(val, path + "." + pi.Name, depth);
        }
        foreach (MethodInfo mi in t.GetMethods(BindingFlags.Public | BindingFlags.Instance))
        {
            if (mi.GetParameters().Length != 0 || mi.ReturnType == typeof(void) || mi.IsSpecialName || mi.ContainsGenericParameters) continue;
            if (!(mi.Name.StartsWith("Is") || mi.Name.StartsWith("Has") || mi.Name.StartsWith("Can") || mi.Name.StartsWith("Get"))) continue;
            if (mi.Name == "GetType" || mi.Name == "GetHashCode" || mi.Name == "GetEnumerator") continue;
            object val;
            MethodInfo m = mi;
            if (!Try(delegate { return m.Invoke(o, null); }, t.Name + "." + mi.Name + "()", out val)) continue;
            ++methods;
            Follow(val, path + "." + mi.Name + "()", depth);
        }
        // the items of a list (the first few)
        IEnumerable list = o as IEnumerable;
        if (list != null && !(o is string))
        {
            int n = 0;
            object[] items;
            if (!TryItems(list, t.Name, out items)) return;
            foreach (object item in items)
            {
                if (++n > 4) break;
                Sweep(item, path + "[" + (n - 1) + "]", depth + 1);
            }
        }
    }

    void Follow(object val, string path, int depth)
    {
        if (val == null) return;
        if (IsApiType(val.GetType())) Sweep(val, path, depth + 1);
    }

    delegate object Getter();

    bool Try(Getter g, string name, out object val)
    {
        val = null;
        try
        {
            val = g();
            return true;
        }
        catch (TargetInvocationException e)
        {
            Exception x = e.InnerException;
            if (x is NotSupportedException)
            {
                string bare = name.EndsWith("()") ? name.Substring(0, name.Length - 2) : name;
                string member = bare.Substring(bare.IndexOf('.') + 1);
                if (x.Message.IndexOf("not supported by Reno", StringComparison.Ordinal) < 0 || x.Message.IndexOf(member, StringComparison.Ordinal) < 0)
                    failures.Add(name + ": a NotSupportedException that does not name it: " + x.Message);
                unsupported[bare] = 1;
            }
            else
            {
                failures.Add(name + ": " + x.GetType().Name + ": " + x.Message);
            }
            return false;
        }
    }

    bool TryItems(IEnumerable list, string name, out object[] items)
    {
        items = null;
        try
        {
            List<object> l = new List<object>();
            foreach (object o in list) l.Add(o);
            items = l.ToArray();
            return true;
        }
        catch (NotSupportedException x)
        {
            if (x.Message.IndexOf("not supported by Reno", StringComparison.Ordinal) < 0) failures.Add(name + " (its items): " + x.Message);
            unsupported[name + " (items)"] = 1;
            return false;
        }
        catch (Exception x)
        {
            failures.Add(name + " (its items): " + x.GetType().Name + ": " + x.Message);
            return false;
        }
    }

    bool SetBack(object o, PropertyInfo pi, object val, string name)
    {
        try
        {
            pi.SetValue(o, val, null);
        }
        catch (TargetInvocationException e)
        {
            Exception x = e.InnerException;
            if (x is NotSupportedException) { unsupported[name + " (set)"] = 1; return false; }
            failures.Add(name + " = (its own value): " + x.GetType().Name + ": " + x.Message);
            return false;
        }
        object again;
        if (!Try(delegate { return pi.GetValue(o, null); }, name, out again)) return false;
        if (!Same(val, again)) failures.Add(name + ": set to " + Show(val) + ", reads back " + Show(again));
        return true;
    }

    static bool Same(object a, object b)
    {
        if (a == null || b == null) return a == null && b == null;
        if (a is double || a is float) return Math.Abs(Convert.ToDouble(a) - Convert.ToDouble(b)) < 1e-6;
        if (a is Timecode) return Math.Abs(((Timecode)a).ToMilliseconds() - ((Timecode)b).ToMilliseconds()) < 1e-3;
        if (a.GetType().IsPrimitive || a is string || a.GetType().IsEnum) return a.Equals(b);
        return true;    // (objects: the same Reno object is checked elsewhere)
    }

    static string Show(object o) { return o == null ? "null" : o.ToString(); }
}
