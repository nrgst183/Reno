// The names effects are shown by. A slot read from a VEGAS project has only its plug-in's identifier
// ("com.vegascreativesoftware:gaussianblur"); what VEGAS shows is the plug-in's label ("VEGAS Gaussian Blur"), so every
// place that shows an effect asks fxSlotLabel / videoFxLabel instead of showing the identifier.
#pragma once
#include <string>

namespace reno {

struct FxSlot;

// The label of the OpenFX effect with this identifier, as VEGAS shows it.
std::string videoFxLabel(const std::string& pluginId);
// The name an effect in a chain is shown by: its own name when it has one (one given when it was added), else
// videoFxLabel of its plug-in (OpenFX) or the audio plug-in's name.
std::string fxSlotLabel(const FxSlot& slot);
// The label VEGAS Pro 18 gives one of its own OpenFX effects ("" for other identifiers).
std::string vegasStockFxLabel(const std::string& pluginId);
// For an effect whose plug-in cannot be found: its identifier's last part, made readable
// ("com.NewBlue.STCartoonrPlus" -> "ST Cartoonr Plus").
std::string readableFxId(const std::string& pluginId);

}  // namespace reno
