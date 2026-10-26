// Lists the public API of a .NET assembly from its metadata (without loading it): used by dump_api.ps1 to compare
// Reno's ScriptPortal.Vegas with VEGAS's (tests/scripting/gen_unsupported.py).
using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.Reflection;
using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;
using System.Reflection.PortableExecutable;

public class TP : ISignatureTypeProvider<string, object> {
    MetadataReader r; public TP(MetadataReader r){this.r=r;}
    public string GetArrayType(string e, ArrayShape s){return e+"[,]";}
    public string GetByReferenceType(string e){return "ref "+e;}
    public string GetFunctionPointerType(MethodSignature<string> s){return "fnptr";}
    public string GetGenericInstantiation(string g, ImmutableArray<string> a){return g+"<"+string.Join(",",a)+">";}
    public string GetGenericMethodParameter(object c,int i){return "!!"+i;}
    public string GetGenericTypeParameter(object c,int i){return "!"+i;}
    public string GetModifiedType(string m,string u,bool req){return u;}
    public string GetPinnedType(string e){return e;}
    public string GetPointerType(string e){return e+"*";}
    public string GetPrimitiveType(PrimitiveTypeCode t){return t.ToString();}
    public string GetSZArrayType(string e){return e+"[]";}
    public string GetTypeFromDefinition(MetadataReader rr, TypeDefinitionHandle h, byte k){var t=rr.GetTypeDefinition(h);return rr.GetString(t.Name);}
    public string GetTypeFromReference(MetadataReader rr, TypeReferenceHandle h, byte k){var t=rr.GetTypeReference(h);return rr.GetString(t.Namespace)+"."+rr.GetString(t.Name);}
    public string GetTypeFromSpecification(MetadataReader rr, object c, TypeSpecificationHandle h, byte k){return rr.GetTypeSpecification(h).DecodeSignature(this,c);}
}
public static class Dumper {
  public static string Run(string path) {
    var sb=new StringBuilder();
    using(var fs=File.OpenRead(path)) using(var pe=new PEReader(fs)) {
      var r=pe.GetMetadataReader(); var tp=new TP(r);
      var asm=r.GetAssemblyDefinition(); sb.AppendLine("assembly "+r.GetString(asm.Name)+" "+asm.Version);
      foreach(var th in r.TypeDefinitions){
        var t=r.GetTypeDefinition(th);
        var vis=t.Attributes & TypeAttributes.VisibilityMask;
        if(vis!=TypeAttributes.Public && vis!=TypeAttributes.NestedPublic) continue;
        string bt="";
        if(!t.BaseType.IsNil){ if(t.BaseType.Kind==HandleKind.TypeReference) bt=tp.GetTypeFromReference(r,(TypeReferenceHandle)t.BaseType,0); else if(t.BaseType.Kind==HandleKind.TypeDefinition) bt=tp.GetTypeFromDefinition(r,(TypeDefinitionHandle)t.BaseType,0); else bt=tp.GetTypeFromSpecification(r,null,(TypeSpecificationHandle)t.BaseType,0);}
        string kind=(t.Attributes&TypeAttributes.Interface)!=0?"interface":bt=="System.Enum"?"enum":bt=="System.ValueType"?"struct":"class";
        var ifs=string.Join(",",t.GetInterfaceImplementations().Select(ih=>{var ii=r.GetInterfaceImplementation(ih);var h=ii.Interface; return h.Kind==HandleKind.TypeReference?tp.GetTypeFromReference(r,(TypeReferenceHandle)h,0):h.Kind==HandleKind.TypeDefinition?tp.GetTypeFromDefinition(r,(TypeDefinitionHandle)h,0):tp.GetTypeFromSpecification(r,null,(TypeSpecificationHandle)h,0);}));
        string abs=(t.Attributes&TypeAttributes.Abstract)!=0?" abstract":"";
        sb.AppendLine(kind+abs+" "+r.GetString(t.Namespace)+"."+r.GetString(t.Name)+(t.GetDeclaringType().IsNil?"":" (nested in "+r.GetString(r.GetTypeDefinition(t.GetDeclaringType()).Name)+")")+" : "+bt+(ifs.Length>0?" ["+ifs+"]":""));
        if(kind=="enum"){
          foreach(var fh in t.GetFields()){var f=r.GetFieldDefinition(fh); if((f.Attributes&FieldAttributes.Static)==0) continue; var c=f.GetDefaultValue(); object v=null; if(!c.IsNil){var br=r.GetBlobReader(r.GetConstant(c).Value); v=br.ReadConstant(r.GetConstant(c).TypeCode);} sb.AppendLine("  "+r.GetString(f.Name)+" = "+v);}
          continue;
        }
        var accessors=new HashSet<MethodDefinitionHandle>();
        foreach(var ph in t.GetProperties()){var p=r.GetPropertyDefinition(ph); var a=p.GetAccessors(); bool g=false,s=false; string st="";
          if(!a.Getter.IsNil){var m=r.GetMethodDefinition(a.Getter); accessors.Add(a.Getter); if((m.Attributes&MethodAttributes.MemberAccessMask)==MethodAttributes.Public){g=true; if((m.Attributes&MethodAttributes.Static)!=0) st="static ";}}
          if(!a.Setter.IsNil){var m=r.GetMethodDefinition(a.Setter); accessors.Add(a.Setter); if((m.Attributes&MethodAttributes.MemberAccessMask)==MethodAttributes.Public){s=true;if((m.Attributes&MethodAttributes.Static)!=0) st="static ";}}
          if(!g&&!s) continue;
          var sig=p.DecodeSignature(tp,null);
          string idx=sig.ParameterTypes.Length>0?"["+string.Join(",",sig.ParameterTypes)+"]":"";
          sb.AppendLine("  prop "+st+sig.ReturnType+" "+r.GetString(p.Name)+idx+" {"+(g?" get;":"")+(s?" set;":"")+" }");}
        foreach(var eh in t.GetEvents()){var e=r.GetEventDefinition(eh); var a=e.GetAccessors(); accessors.Add(a.Adder); accessors.Add(a.Remover); sb.AppendLine("  event "+r.GetString(e.Name));}
        foreach(var mh in t.GetMethods()){ if(accessors.Contains(mh)) continue; var m=r.GetMethodDefinition(mh);
          var acc=m.Attributes&MethodAttributes.MemberAccessMask; if(acc!=MethodAttributes.Public && acc!=MethodAttributes.Family) continue;
          var sig=m.DecodeSignature(tp,null); var names=m.GetParameters().Select(x=>r.GetParameter(x)).Where(x=>x.SequenceNumber>0).OrderBy(x=>x.SequenceNumber).Select(x=>r.GetString(x.Name)).ToList();
          var outs=m.GetParameters().Select(x=>r.GetParameter(x)).Where(x=>x.SequenceNumber>0).OrderBy(x=>x.SequenceNumber).Select(x=>(x.Attributes&ParameterAttributes.Out)!=0&&(x.Attributes&ParameterAttributes.In)==0).ToList();
          var ps=new List<string>(); for(int i=0;i<sig.ParameterTypes.Length;i++){var pt=sig.ParameterTypes[i]; if(i<outs.Count&&outs[i]&&pt.StartsWith("ref ")) pt="out "+pt.Substring(4); ps.Add(pt+" "+(i<names.Count?names[i]:"p"+i));}
          string mods=((m.Attributes&MethodAttributes.Static)!=0?"static ":"")+(acc==MethodAttributes.Family?"protected ":"")+((m.Attributes&MethodAttributes.Virtual)!=0?"virtual ":"");
          sb.AppendLine("  "+mods+sig.ReturnType+" "+r.GetString(m.Name)+"("+string.Join(", ",ps)+")");}
        foreach(var fh in t.GetFields()){var f=r.GetFieldDefinition(fh); if((f.Attributes&FieldAttributes.FieldAccessMask)!=FieldAttributes.Public) continue; var c=f.GetDefaultValue(); object v=null; if(!c.IsNil){var br=r.GetBlobReader(r.GetConstant(c).Value); v=br.ReadConstant(r.GetConstant(c).TypeCode);} sb.AppendLine("  field "+((f.Attributes&FieldAttributes.Static)!=0?"static ":"")+f.DecodeSignature(tp,null)+" "+r.GetString(f.Name)+(v!=null?" = "+v:""));}
      }
      sb.AppendLine("references:"); foreach(var ah in r.AssemblyReferences){var a=r.GetAssemblyReference(ah); sb.AppendLine("  "+r.GetString(a.Name)+" "+a.Version);}
    }
    return sb.ToString();
  }
}
