// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <span>
#include <string>

#include "common/common_types.h"

namespace AudioCore::Sink {

/// Plays 48 kHz 16-bit stereo sound on a host output device beside the games' own sound: GameChat's
/// voices and its ring. SDL calls the source on its audio thread for the samples it needs
/// (interleaved left and right), so the source must be quick and must not block for long.
class Speaker {
public:
    using Source = std::function<void(std::span<s16> samples)>;

    Speaker();
    ~Speaker();
    Speaker(const Speaker&) = delete;
    Speaker& operator=(const Speaker&) = delete;

    /// Opens the named playback device ("auto", or one that isn't connected: the default one).
    /// Returns whether a device is open.
    bool Open(const std::string& device_name, Source source);
    void Close();
    bool IsOpen() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace AudioCore::Sink
