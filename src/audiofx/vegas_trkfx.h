// The track effects VEGAS puts on every track and bus (sftrkfx1_x64.dll: Track Compressor, Track EQ, Track Noise Gate,
// Dither; sffrgpnv_x64.dll: Volume, Pan), as Reno's built-in effects reproduce them. This header only collects them and
// tells which built-in effect stands for which DirectX class (used by audioplugin.cpp and vegwrite_fx.cpp).
#pragma once
#include "vegas_forge.h"
#include "vegas_gate.h"
#include "vegas_comp.h"
#include "vegas_dither.h"
#include "vegas_eq.h"
#include <cstring>
#include <string>

namespace reno {
namespace vegtrk {

struct Map {
    const char* builtinUid;
    const char* clsid;       // lower case, as it appears in a DirectX uid
    const char* dxUid;
};

inline const Map* maps(int* count)
{
    static const Map list[] = {
        {"reno.trkvolume", "ee38ca88-d78e-4bfb-b05e-577892730c83", "dshow:{EE38CA88-D78E-4BFB-B05E-577892730C83}"},
        {"reno.trkpan", "eb6213db-08ff-4510-9f8d-3058b0ece4c6", "dshow:{EB6213DB-08FF-4510-9F8D-3058B0ECE4C6}"},
        {"reno.trkgate", "869419dd-501f-11d3-8cdc-00c04f6b8e4c", "dshow:{869419DD-501F-11D3-8CDC-00C04F6B8E4C}"},
        {"reno.trkcomp", "23c9f225-40ec-11d2-9d36-00c04f8edc1e", "dshow:{23C9F225-40EC-11D2-9D36-00C04F8EDC1E}"},
        {"reno.trkdither", "260df3e1-ac77-11d2-9e93-00c04f68be44", "dshow:{260DF3E1-AC77-11D2-9E93-00C04F68BE44}"},
        {"reno.trkeq", "8cb69a0a-10e8-11d2-9b89-00104b8d13c2", "dshow:{8CB69A0A-10E8-11D2-9B89-00104B8D13C2}"},
    };
    *count = (int)(sizeof list / sizeof list[0]);
    return list;
}

// The built-in effect for a DirectX uid ("dshow:{...}", any case), or "".
inline std::string builtinUidForDx(const std::string& dxUid)
{
    std::string low = dxUid;
    for (char& c : low) c = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    int n = 0;
    const Map* m = maps(&n);
    for (int i = 0; i < n; ++i)
        if (low.find(m[i].clsid) != std::string::npos) return m[i].builtinUid;
    return std::string();
}

inline std::string dxUidForBuiltin(const std::string& builtinUid)
{
    int n = 0;
    const Map* m = maps(&n);
    for (int i = 0; i < n; ++i)
        if (builtinUid == m[i].builtinUid) return m[i].dxUid;
    return std::string();
}

// The built-in effects of these that have a page (fxpages_vegastrk.inc)
inline bool hasPage(const std::string& uid)
{
    return false
           || uid == "reno.trkvolume"
           || uid == "reno.trkpan"
           || uid == "reno.trkgate"
           || uid == "reno.trkcomp"
           || uid == "reno.trkdither"
           || uid == "reno.trkeq"
        ;
}

}  // namespace vegtrk
}  // namespace reno
