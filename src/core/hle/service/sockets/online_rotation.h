// SPDX-FileCopyrightText: Copyright 2026 Droplet
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <span>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Kernel {
class KProcess;
}

// Mario Kart World course rotations from a private server: the server's admin decides what the
// course votes offer (Reservoir's GET /v1/rotation on the friends API's port). A vote is built by
// the lobby's host, which sends its four choices to the others, so the host's emulator replaces
// them just before they go out; a Knockout Tour has no vote, and its host draws the rally the same
// way. Three instructions in that code become an SVC of Droplet's own (CourseRotationSvc), which
// rewrites the choices or the rally and does what the instruction did. The rotation is fetched
// on a thread of its own and kept, so a vote never waits for the network.
namespace Service::Sockets::OnlineRedirect {

constexpr u32 CourseRotationSvc = 0xD701;

// At load: puts the hooks into a module of the build they were made for (Mario Kart World 1.8.0's
// main), unless the CPU runs natively (NCE patches SVCs itself). `image` is the module's code
// and data, `base` its address.
void InstallCourseRotation(std::span<u8> image, const std::array<u8, 0x20>& build_id, VAddr base);

// The SVC: the vote's choices as the rotation has them, then the replaced instruction.
void RunCourseRotationHook(Core::System& system, Kernel::KProcess& process);

} // namespace Service::Sockets::OnlineRedirect
