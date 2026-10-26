# xmlparams.py [OFX VIDEO PLUG-INS FOLDER]: the settings VEGAS's own resource files give a few effects, as JSON
# (the folder defaults to "OFX Video Plug-Ins" in $RENO_VEGAS_DIR)
import re,sys,glob,json,os
import xml.etree.ElementTree as ET
base=(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ.get("RENO_VEGAS_DIR", "."), "OFX Video Plug-Ins")).rstrip("/\\") + "/"
want={"de.magix:FlickerReducer","de.magix:DenoisingNLM","de.magix:MotionTracker","de.magix:Stabilize","de.magix:Colorization","de.magix:StyleTransfer","com.vegascreativesoftware:Stabilize"}
out={}
for f in ["MagixCVFx.ofx.bundle/Contents/Resources/MagixCVFx.xml","MagixAiFx.ofx.bundle/Contents/Resources/MagixAiFx.xml","Stabilize.ofx.bundle/Contents/Resources/Stabilize.xml"]:
    t=open(base+f,encoding='utf-8-sig').read()
    root=ET.fromstring(t)
    for pl in root.iter('OfxPlugin'):
        n=pl.get('name')
        if n not in want: continue
        ps=[]
        for ctx in pl.iter('OfxImageEffectContext'):
            if ctx.get('name')!='OfxImageEffectContextFilter': continue
            for e in ctx:
                if not e.tag.startswith('OfxParamType'): continue
                d={'type':e.tag[12:],'name':e.get('name'),'label':(e.findtext('OfxPropLabel') or ''),
                   'choices':[c.text for c in e.findall('OfxParamPropChoiceOption')],
                   'parent':e.findtext('OfxParamPropParent') or '', 'secret':e.findtext('OfxParamPropSecret'),
                   'extra':{c.tag:c.text for c in e if c.tag not in('OfxPropLabel','OfxPropShortLabel','OfxPropLongLabel','OfxParamPropChoiceOption','OfxParamPropHint')}}
                ps.append(d)
        out[n]=ps
json.dump(out,open('xml.json','w'),indent=1)
for n,ps in out.items(): print(n,len(ps))
