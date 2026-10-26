// Audio plug-ins' own news, passed on to whoever wants it (the effects window).
#pragma once
#include <functional>

namespace reno {

class AudioPlugin;

void setAudioPluginNews(std::function<void(AudioPlugin*, int index)> paramChanged, std::function<void(AudioPlugin*, int w, int h)> editorResized);

}  // namespace reno
