#!/usr/bin/env python3
# Makes src/vegas/vegtemplates.cpp: the records VEGAS Pro 18 wrote that Reno's .veg writer starts from when a
# project has no record of a kind to copy. Each is taken unchanged from the smallest sample project that has one:
#
#   python3 tests/vegas/maketemplates.py <folder of sample projects, $RENO_SAMPLES> > src/vegas/vegtemplates.cpp
import sys, os, struct, uuid

LIST = "7473696c-912f-11cf-a5d6-28db04c10000"
RIFF = "66666972-912e-11cf-a5d6-28db04c10000"

def g(b, o): return str(uuid.UUID(bytes_le=bytes(b[o:o + 16])))

def parse(b, off, end, out):
    while off + 24 <= end:
        gid = g(b, off); sz = struct.unpack_from('<Q', b, off + 16)[0]
        if sz < 24 or sz > end - off: break
        n = {'id': gid, 'off': off, 'size': sz, 'kids': []}
        if gid in (LIST, RIFF):
            n['type'] = g(b, off + 24); parse(b, off + 40, off + sz, n['kids'])
        out.append(n)
        off += (sz + 7) & ~7
    return out

def load(p):
    b = open(p, 'rb').read()
    return b, parse(b, 40, struct.unpack_from('<Q', b, 16)[0], [])

def walk(nodes):
    for n in nodes:
        yield n
        yield from walk(n['kids'])

def key(n): return n.get('type') or n['id']

root = sys.argv[1]
# every .veg under the folder, smallest first (parsed when first needed)
paths = sorted((os.path.join(d, f) for d, _, fs in os.walk(root) for f in fs if f.lower().endswith('.veg')),
               key=lambda p: (os.path.getsize(p), p))
cache = {}
def sample(path):
    if path not in cache: cache[path] = load(path)
    return cache[path]

out = {}   # name -> (bytes, where from)

def find(pred):
    for path in paths:
        b, t = sample(path)
        for n in walk(t):
            if pred(b, n): return path, b, n
    return None

def take(name, pred, what):
    hit = find(pred)
    if not hit: raise SystemExit('template %s not found in any sample project' % name)
    _, b, n = hit
    out[name] = (b[n['off']:n['off'] + n['size']], 'a sample project: ' + what)

data = lambda b, n: b[n['off'] + 24:n['off'] + n['size']]
u32 = lambda r, o: struct.unpack_from('<I', r, o)[0]

def media_path(r):
    return r[0x80:0x80 + u32(r, 0x24)].decode('utf-16le').rstrip('\0')

TRACK = '4d6c0748-2316-11d2-86b0-00c04f8edb8a'
is_video_track = lambda b, n: n.get('type') == TRACK and u32(data(b, n['kids'][0]), 0xc) == 3
is_audio_track = lambda b, n: n.get('type') == TRACK and u32(data(b, n['kids'][0]), 0xc) == 2
is_fx = lambda b, n: n['id'] == '1af0e4e6-6e7f-11d2-8705-00c04f8edb8a'
is_pancrop = lambda b, n: n['id'] == '884bacff-74eb-456c-88f3-739e8390bd07'
is_group = lambda b, n: n['id'] == 'e901e132-c677-4604-ac9d-db0b87b4eebd'

# the whole project template: the smallest sample with a video track, an audio track, an effect, a pan/crop and a group
whole = None
for path in paths:
    b, t = sample(path)
    nodes = list(walk(t))
    if all(any(pred(b, n) for n in nodes) for pred in (is_video_track, is_audio_track, is_fx, is_pancrop, is_group)):
        whole = path
        break
if not whole: raise SystemExit('no sample project has a video track, an audio track, an effect, a pan/crop and a group')
out['project'] = (open(whole, 'rb').read(), 'a sample project: the whole project (its media and tracks are not used)')
take('videoTrack', is_video_track, 'its video track')
take('audioTrack', is_audio_track, 'its audio track')
take('fx', is_fx, 'an OpenFX effect (Brightness and Contrast)')
take('panCrop', is_pancrop, 'an event pan/crop')
M = 'b28f2d5d-230f-11d2-86af-00c04f8edb8a'
take('mediaAV', lambda b, n: n['id'] == M and media_path(data(b, n)).lower().endswith('.mp4'), 'a video file with sound (.mp4)')
take('mediaVideo', lambda b, n: n['id'] == M and media_path(data(b, n)).lower().endswith('.jpg'), 'a picture (.jpg)')
take('mediaAudio', lambda b, n: n['id'] == M and media_path(data(b, n)).lower().endswith('.mp3') and u32(data(b, n), 0x48) == 0, 'a sound file (.mp3)')
take('mediaVeg', lambda b, n: n['id'] == M and media_path(data(b, n)).lower().endswith('.veg'), 'another VEGAS project used as media')
take('mediaSubclip', lambda b, n: n['id'] == M and media_path(data(b, n)).startswith('META:\\SubClip\\'), 'a subclip')
take('mediaGenerated', lambda b, n: n['id'] == M and media_path(data(b, n)).startswith('META:\\Video Generator\\'), 'generated media (Titles & Text)')
take('eventAudioChain', lambda b, n: n['id'] == 'b66a95ce-5946-48ab-a4f4-f88c95acdc1d' and 'type' not in n, 'the audio effects of an event (one VST plug-in)')
take('trackAudioChain', lambda b, n: n['id'] == 'f79298f4-5695-4b17-9ddb-19a0912d39f3' and 'type' not in n, 'the audio effects of a track')
take('trackEnvelopes', lambda b, n: n.get('type') == '84701b5d-6863-11d2-86fd-00c04f8edb8a', 'a track volume envelope (4 points)')
take('eventEnvelopes', lambda b, n: n.get('type') == '84701b5a-6863-11d2-86fd-00c04f8edb8a', 'an event velocity envelope (1 point)')
take('marker', lambda b, n: n['id'] == 'abf76256-392d-11d2-86c7-00c04f8edb8a', 'a marker')
take('group', is_group, 'a group of two events')
take('bus', lambda b, n: n['id'] == '72b8749d-14ab-4588-94a9-39343aeef7cc' and u32(data(b, n), 0xc) == 3 and data(b, n).find(bytes.fromhex('018612b42e70d1118253000092 90dd44'.replace(' ', ''))) < 0,
     'an audio bus (Bus A) without effects')

# VST plug-ins as VEGAS names them: the VST id, then four bytes VEGAS works out from something on the machine
# (not from the name), then a fixed tail. Taken from every sample, so that a VST plug-in the sample projects
# already use can be written into a project that does not have it yet.
CH = bytes.fromhex('018612b42e70d1118253000092 90dd44'.replace(' ', ''))
vsts = {}
for dirpath, _, files in os.walk(root):
    for f in sorted(files):
        if not f.lower().endswith('.veg'): continue
        b = open(os.path.join(dirpath, f), 'rb').read()
        i = b.find(CH)
        while i >= 0:
            try:
                nl = u32(b, i + 36); q = i + 40 + nl + 8 + 18 + 4
                cnt = u32(b, q); q += 8
                for k in range(cnt):
                    clsid = b[q:q + 16]; nl2 = u32(b, q + 20); name = b[q + 24:q + 24 + nl2].decode('utf-16le').rstrip('\0')
                    rr = q + 24 + nl2; dl = u32(b, rr + 16); d = b[rr + 24:rr + 24 + dl]
                    if clsid[8:] == bytes.fromhex('02690007e95d36fa') and d[:4] == b'CcnK':
                        vid = struct.unpack_from('>I', d, 16)[0]
                        vsts.setdefault((vid, name), clsid)
                    q = rr + 24 + dl
            except Exception:
                pass
            i = b.find(CH, i + 1)

def carr(name, bts):
    s = 'static const unsigned char k_%s[%d] = {\n' % (name, len(bts))
    for i in range(0, len(bts), 24):
        s += '    ' + ''.join('0x%02x,' % x for x in bts[i:i + 24]) + '\n'
    return s + '};\n'

print('// Made by tests/vegas/maketemplates.py from the sample projects; do not edit. What each one is, and where it')
print('// comes from, is listed in kTemplates below.')
print('#include "vegtemplates.h"')
print('#include <cstring>')
print()
print('namespace reno {')
print('namespace veg {')
print()
for name, (bts, _) in out.items():
    print(carr(name, bts))
print('static const struct { const char* name; const unsigned char* data; size_t size; const char* from; } kTemplates[] = {')
for name, (bts, frm) in out.items():
    print('    {"%s", k_%s, sizeof k_%s, "%s"},' % (name, name, name, frm.replace('\\', '\\\\').replace('"', '\\"')))
print('};')
print()
print('std::string templateBytes(const char* name)')
print('{')
print('    for (const auto& t : kTemplates)')
print('        if (std::strcmp(t.name, name) == 0) return std::string((const char*)t.data, t.size);')
print('    return std::string();')
print('}')
print()
print('const std::vector<KnownVst>& knownVsts()')
print('{')
print('    static const std::vector<KnownVst> v = {')
for (vid, name), clsid in sorted(vsts.items()):
    print('        {0x%08xu, "%s", {%s}},' % (vid, name.replace('"', '\\"'), ', '.join('0x%02x' % x for x in clsid)))
print('    };')
print('    return v;')
print('}')
print()
print('}  // namespace veg')
print('}  // namespace reno')
