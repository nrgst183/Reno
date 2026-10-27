// Which of VEGAS's sfppack2 / sfresfilter DirectX effects Reno reproduces (the built-in effect's uid and the VEGAS
// effect's DirectX uid), for the fall back when the DirectX plug-in is missing and for saving as VEGAS's effect.
#pragma once
#include "vegasgdyn.h"
#include "vegasgraphiceq.h"
#include "vegasmbdyn.h"
#include "vegaspgeq.h"
#include "vegaspmeq.h"
#include "vegasresfilter.h"
#include "vegasnoisegate.h"
#include <cstring>
#include <string>

namespace reno {

// The built-in effect for a DirectX uid ("dshow:{CLSID}", any case), or empty
inline std::string pack2BuiltinForDx(const std::string& dxUidLower)
{
    if (dxUidLower.find(vegng::kClsid) != std::string::npos) return "reno.noisegate";
    if (dxUidLower.find(veggeq::kClsid) != std::string::npos) return "reno.graphiceq";
    if (dxUidLower.find(vegpm::kClsid) != std::string::npos) return "reno.parametriceq";
    if (dxUidLower.find(vegpg::kClsid) != std::string::npos) return "reno.paragraphiceq";
    if (dxUidLower.find(veggd::kClsid) != std::string::npos) return "reno.graphicdynamics";
    if (dxUidLower.find(vegmb::kClsid) != std::string::npos) return "reno.multibanddynamics";
    if (dxUidLower.find(vegrf::kClsid) != std::string::npos) return "reno.resonantfilter";
    return std::string();
}

// The DirectX uid VEGAS knows a built-in effect by, or empty
inline std::string pack2DxForBuiltin(const std::string& builtinUid)
{
    if (builtinUid == "reno.noisegate") return vegng::kDxUid;
    if (builtinUid == "reno.graphiceq") return veggeq::kDxUid;
    if (builtinUid == "reno.parametriceq") return vegpm::kDxUid;
    if (builtinUid == "reno.paragraphiceq") return vegpg::kDxUid;
    if (builtinUid == "reno.graphicdynamics") return veggd::kDxUid;
    if (builtinUid == "reno.multibanddynamics") return vegmb::kDxUid;
    if (builtinUid == "reno.resonantfilter") return vegrf::kDxUid;
    return std::string();
}

// The name of the VEGAS effect a built-in effect reproduces, or empty (for the effect window's About box)
inline std::string pack2VegasName(const std::string& builtinUid)
{
    if (builtinUid == "reno.noisegate") return "VEGAS Noise Gate";
    if (builtinUid == "reno.graphiceq") return "VEGAS Graphic EQ";
    if (builtinUid == "reno.parametriceq") return "VEGAS Parametric EQ";
    if (builtinUid == "reno.paragraphiceq") return "VEGAS Paragraphic EQ";
    if (builtinUid == "reno.graphicdynamics") return "VEGAS Graphic Dynamics";
    if (builtinUid == "reno.multibanddynamics") return "VEGAS Multi-Band Dynamics";
    if (builtinUid == "reno.resonantfilter") return "VEGAS Resonant Filter";
    return std::string();
}

}  // namespace reno
