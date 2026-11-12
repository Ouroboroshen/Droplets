// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <span>
#include <string>

#include "common/common_types.h"

namespace AudioCore::Sink {

/// The Input Device setting's value for no microphone, and its default: games hear silence until
/// the player picks a microphone in Configure > Audio.
constexpr char none_device_name[] = "None";

/// Captures mono 48 kHz 16-bit sound from a host microphone through SDL, for the Switch 2 input
/// streams that games read the console microphone with. Samples stay in memory: nothing here logs,
/// saves or sends them.
class Microphone {
public:
    Microphone();
    ~Microphone();
    Microphone(const Microphone&) = delete;
    Microphone& operator=(const Microphone&) = delete;

    /// Opens the named recording device ("auto": the default one). "None", an empty name or a device
    /// that isn't connected opens nothing. Returns whether a device is open.
    bool Open(const std::string& device_name);
    void Close();
    bool IsOpen() const;

    /// Takes up to out.size() captured samples, oldest first, and returns how many it wrote. At
    /// most about 0.1 s stays queued, so a reader that falls behind hears the latest sound.
    size_t Read(std::span<s16> out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace AudioCore::Sink
