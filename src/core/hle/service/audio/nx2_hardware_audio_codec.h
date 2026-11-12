// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>

#include "core/hle/service/service.h"

namespace Core::Timing {
struct EventType;
}

namespace Service::Audio {

// Switch 2's hardware audio codec (nn::codec::hwacod), reached through hwopus command 100. The
// SDK's hardware Opus decoders (nn::codec::opus::hw) run on it: a game queues Nintendo-framed Opus
// packets in memory it shares with the audio DSP, which decodes them into that memory and signals
// an event. Droplet decodes with libopus; nx2_hardware_audio_codec.cpp has the layout, read from
// the SDK's client (nn::codec::hwacod::shim).

class Nx2HardwareAudioCodecManager;

class INx2HardwareAudioCodecManagerSession final
    : public ServiceFramework<INx2HardwareAudioCodecManagerSession> {
public:
    explicit INx2HardwareAudioCodecManagerSession(
        Core::System& system_, std::shared_ptr<Nx2HardwareAudioCodecManager> manager_);
    ~INx2HardwareAudioCodecManagerSession() override;

private:
    void InitializeInstance(HLERequestContext& ctx);
    void FinalizeInstance(HLERequestContext& ctx);
    void Execute(HLERequestContext& ctx);

    std::shared_ptr<Nx2HardwareAudioCodecManager> manager;
    std::shared_ptr<Core::Timing::EventType> process_event;
};

class INx2HardwareAudioCodecClientSession final
    : public ServiceFramework<INx2HardwareAudioCodecClientSession> {
public:
    explicit INx2HardwareAudioCodecClientSession(Core::System& system_);
    ~INx2HardwareAudioCodecClientSession() override;

private:
    void GetInstanceWorkBufferSize(HLERequestContext& ctx);
    void GetManagerWorkBufferSize(HLERequestContext& ctx);
    void OpenManagerSession(HLERequestContext& ctx);
};

} // namespace Service::Audio
