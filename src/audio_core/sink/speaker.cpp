// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <vector>

#include <SDL3/SDL.h>

#include "audio_core/sink/speaker.h"
#include "common/logging.h"

namespace AudioCore::Sink {

namespace {
constexpr int SampleRate = 48000;
constexpr int Channels = 2;
} // Anonymous namespace

struct Speaker::Impl {
    SDL_AudioStream* stream{};
    Source source;
    std::vector<s16> samples;

    static void SDLCALL Feed(void* userdata, SDL_AudioStream* stream, int additional_amount,
                             int /*total_amount*/) {
        auto* const self = static_cast<Impl*>(userdata);
        const size_t count = static_cast<size_t>(additional_amount > 0 ? additional_amount : 0) /
                             (sizeof(s16) * Channels) * Channels;
        if (count == 0) {
            return;
        }
        self->samples.resize(count);
        self->source(self->samples);
        SDL_PutAudioStreamData(stream, self->samples.data(), static_cast<int>(count * sizeof(s16)));
    }
};

Speaker::Speaker() : impl{std::make_unique<Impl>()} {}

Speaker::~Speaker() {
    Close();
}

bool Speaker::Open(const std::string& device_name, Source source) {
    Close();
    // SDL counts subsystem starts, so this pairs with the quit in Close.
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOG_ERROR(Audio, "Speaker: SDL audio failed to start: {}", SDL_GetError());
        return false;
    }
    SDL_AudioDeviceID device = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    if (!device_name.empty() && device_name != "auto") {
        int count = 0;
        SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
        for (int i = 0; devices != nullptr && i < count; ++i) {
            const char* name = SDL_GetAudioDeviceName(devices[i]);
            if (name != nullptr && device_name == name) {
                device = devices[i];
                break;
            }
        }
        SDL_free(devices);
    }
    impl->source = std::move(source);
    impl->samples.reserve(SampleRate / 10 * Channels);
    const SDL_AudioSpec spec{SDL_AUDIO_S16, Channels, SampleRate};
    impl->stream = SDL_OpenAudioDeviceStream(device, &spec, &Impl::Feed, impl.get());
    if (impl->stream == nullptr) {
        LOG_ERROR(Audio, "Speaker: the output device didn't open: {}", SDL_GetError());
        impl->source = {};
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    SDL_ResumeAudioStreamDevice(impl->stream);
    return true;
}

void Speaker::Close() {
    if (impl->stream == nullptr) {
        return;
    }
    SDL_DestroyAudioStream(impl->stream);
    impl->stream = nullptr;
    impl->source = {};
    std::fill(impl->samples.begin(), impl->samples.end(), s16{0});
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool Speaker::IsOpen() const {
    return impl->stream != nullptr;
}

} // namespace AudioCore::Sink
