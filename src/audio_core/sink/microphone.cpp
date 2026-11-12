// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>

#include <SDL3/SDL.h>

#include "audio_core/sink/microphone.h"
#include "common/logging.h"

namespace AudioCore::Sink {

namespace {
constexpr int SampleRate = 48000;
constexpr int MaxQueuedBytes = SampleRate / 10 * static_cast<int>(sizeof(s16));
} // Anonymous namespace

struct Microphone::Impl {
    SDL_AudioStream* stream{};
};

Microphone::Microphone() : impl{std::make_unique<Impl>()} {}

Microphone::~Microphone() {
    Close();
}

bool Microphone::Open(const std::string& device_name) {
    Close();
    if (device_name.empty() || device_name == none_device_name) {
        return false;
    }
    // SDL counts subsystem starts, so this pairs with the quit in Close.
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOG_ERROR(Audio, "Microphone: SDL audio failed to start: {}", SDL_GetError());
        return false;
    }
    SDL_AudioDeviceID device = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    if (device_name != "auto") {
        int count = 0;
        SDL_AudioDeviceID* devices = SDL_GetAudioRecordingDevices(&count);
        bool found = false;
        for (int i = 0; devices != nullptr && i < count; ++i) {
            const char* name = SDL_GetAudioDeviceName(devices[i]);
            if (name != nullptr && device_name == name) {
                device = devices[i];
                found = true;
                break;
            }
        }
        SDL_free(devices);
        if (!found) {
            // Never another microphone than the one the player chose (as the camera).
            LOG_WARNING(Audio, "Microphone: the chosen input device isn't connected; games hear "
                               "silence");
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            return false;
        }
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 1, SampleRate};
    impl->stream = SDL_OpenAudioDeviceStream(device, &spec, nullptr, nullptr);
    if (impl->stream == nullptr) {
        LOG_ERROR(Audio, "Microphone: the input device didn't open: {}", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    SDL_ResumeAudioStreamDevice(impl->stream);
    LOG_INFO(Audio, "Microphone on");
    return true;
}

void Microphone::Close() {
    if (impl->stream == nullptr) {
        return;
    }
    SDL_DestroyAudioStream(impl->stream);
    impl->stream = nullptr;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    LOG_INFO(Audio, "Microphone off");
}

bool Microphone::IsOpen() const {
    return impl->stream != nullptr;
}

size_t Microphone::Read(std::span<s16> out) {
    if (impl->stream == nullptr || out.empty()) {
        return 0;
    }
    int excess = SDL_GetAudioStreamAvailable(impl->stream) - MaxQueuedBytes;
    std::array<u8, 4096> scratch;
    while (excess > 0) {
        const int got = SDL_GetAudioStreamData(
            impl->stream, scratch.data(),
            std::min(excess - excess % 2, static_cast<int>(scratch.size())));
        if (got <= 0) {
            break;
        }
        excess -= got;
    }
    const int got = SDL_GetAudioStreamData(impl->stream, out.data(),
                                           static_cast<int>(out.size_bytes()));
    return got > 0 ? static_cast<size_t>(got) / sizeof(s16) : 0;
}

} // namespace AudioCore::Sink
