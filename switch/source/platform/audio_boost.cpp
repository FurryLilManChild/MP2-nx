// mp2-nx (0.5.1, GitHub issue #4: the sound is quiet with every volume at maximum): one volume boost over
// every audio stream the game opens. The game plays through two SDL streams (MusyX's mix in
// extern/musyx-port hw_pc.c, and the AI DMA path for streamed music and movies in platform/ai_dma.cpp);
// SDL_OpenAudioDeviceStream and SDL_DestroyAudioStream are wrapped (-Wl,--wrap=) so each gets the boost as its
// gain, and the Switch menu's "Volume boost" row changes it live. SDL mixes in float and converts to the
// device's format at the end, so a boost above what the loudest moments leave room for clips there.

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <vector>

extern "C" SDL_AudioStream* __real_SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec* spec,
                                                             SDL_AudioStreamCallback callback, void* userdata);

extern "C" void __real_SDL_DestroyAudioStream(SDL_AudioStream* stream);

namespace {
std::mutex g_mutex;
std::vector<SDL_AudioStream*> g_streams;
int g_percent = 200;
}  // namespace

extern "C" SDL_AudioStream* __wrap_SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec* spec,
                                                             SDL_AudioStreamCallback callback, void* userdata) {
    SDL_AudioStream* stream = __real_SDL_OpenAudioDeviceStream(devid, spec, callback, userdata);
    if (stream != nullptr) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_streams.push_back(stream);
        SDL_SetAudioStreamGain(stream, float(g_percent) / 100.f);
        std::fprintf(stderr, "mp2-nx: audio: stream %zu opened, volume boost %d%%\n", g_streams.size(), g_percent);
    }
    return stream;
}

extern "C" void __wrap_SDL_DestroyAudioStream(SDL_AudioStream* stream) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_streams.erase(std::remove(g_streams.begin(), g_streams.end(), stream), g_streams.end());
    }
    __real_SDL_DestroyAudioStream(stream);
}

// The Switch menu (switch_menu.cpp): the boost in percent, 100 = the game's own level
extern "C" void mp2nx_set_volume_boost(int percent) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_percent = std::clamp(percent, 100, 300);
    for (SDL_AudioStream* s : g_streams) SDL_SetAudioStreamGain(s, float(g_percent) / 100.f);
}

extern "C" int mp2nx_volume_boost(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_percent;
}
