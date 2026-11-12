// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace VideoCommon {

/// False while a thread outside the GPU threads calls into the caches (ThreadManager::
/// InvalidateRegion runs on its caller's thread, an HLE service that wrote guest memory):
/// BufferCache::WriteMemory then leaves the page's other GPU-written bytes alone instead of
/// downloading them, which only the GPU thread may do.
inline thread_local bool buffer_downloads_allowed = true;

} // namespace VideoCommon
