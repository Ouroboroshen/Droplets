// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

// The host camera that games use (Settings::values.camera_*): a webcam, a virtual camera or the
// built-in test pattern. Every user of this file keeps these rules:
// - Only a Stream turns a camera on. Games' camera services hold one only while the game streams,
//   and the camera stays off unless the user turned it on in Settings. Listing cameras never
//   turns one on.
// - Frames stay in this process and only ever get copied into the game's memory. They are never
//   written to a file or logged, and never sent over the network, with one exception the user
//   turns on separately: GameChat's camera sharing (nx2_chat.h), which sends small compressed
//   pictures to the other members of the player's GameChat.
// - When the last Stream goes, the camera turns off and the frame buffers are wiped.
namespace Service::Nx2Camera {

// The built-in picture (colour bars and a moving face) that needs no camera.
inline constexpr std::string_view TestPatternDevice = "Test pattern";

// The host's cameras, by name. Turns none of them on.
std::vector<std::string> ListDevices();

// Whether the user turned the camera on in Settings and chose one.
bool Enabled();

// Enabled, and the chosen camera can deliver: the test pattern, or a camera that is plugged in
// and has not failed to open. What games see as "a camera is connected".
bool Available();

// Whether a camera (or the test pattern) is on for a game right now.
bool IsActive();

// Picks up changed camera settings (Core::System::ApplySettings).
void ApplySettings();

// A game's use of the camera. While at least one Stream exists and the camera is on in Settings,
// the chosen camera captures.
class Stream {
public:
    Stream();
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    // Writes the newest frame, centre-cropped and scaled to width x height, as RGBA8 rows of
    // width * 4 bytes into out (resized to fit). Returns false and leaves out alone when there is
    // no frame newer than `seen`; otherwise `seen` becomes the frame's number.
    bool Read(u32 width, u32 height, u64& seen, std::vector<u8>& out) const;
};

} // namespace Service::Nx2Camera
