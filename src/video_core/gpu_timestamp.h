// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

#include "common/common_types.h"
#include "common/settings_enums.h"

namespace Tegra {

/// Host nanoseconds (the CoreTiming global clock) at which the guest submitted the newest command
/// list the GPU thread has started on; 0 before the first.
inline std::atomic<u64> g_nx2_submit_ns{0};
/// The latest submit-time timestamp handed out, so reports never go backwards.
inline std::atomic<u64> g_nx2_last_report_ns{0};

/// Called by the GPU thread as it starts on a submitted command list.
inline void NoteGpuSubmitTime(u64 submit_ns) {
    u64 current = g_nx2_submit_ns.load(std::memory_order_relaxed);
    while (submit_ns > current &&
           !g_nx2_submit_ns.compare_exchange_weak(current, submit_ns, std::memory_order_relaxed)) {
    }
}

/// Whether Switch 2 report timestamps are the commands' submit time. Games time their GPU work with
/// these (Splatoon Raiders' dynamic resolution, Mario Kart World's 30/60 fps choice). Read as the
/// time Eden's GPU thread got to a report, they also count the thread's own lag behind the game,
/// so the games saw a GPU slower than the console's and lowered their resolution every frame: the
/// GPU Clocks setting's Boost and Overclock (Boost is the default) report the submit time instead,
/// as if the GPU ran the commands at once, like they do for Switch 1 ticks. Normal keeps the GPU
/// thread's time. EDEN_NX2_GPU_TIME=now or =submit overrides the setting.
[[nodiscard]] inline bool Nx2SubmitTimeStamps(Settings::GpuClock clock) {
    static const int forced = [] {
        const char* value = std::getenv("EDEN_NX2_GPU_TIME");
        if (value == nullptr) {
            return -1;
        }
        return std::strcmp(value, "submit") == 0 ? 1 : std::strcmp(value, "now") == 0 ? 0 : -1;
    }();
    return forced >= 0 ? forced == 1 : clock != Settings::GpuClock::Normal;
}

/// A submit-time report timestamp: the newest submit time the GPU thread has started on, never
/// later than `now` and never before an earlier report; `now` before any command list.
[[nodiscard]] inline u64 Nx2ReportTimestamp(u64 now, u64 submit, std::atomic<u64>& last) {
    if (submit == 0) {
        return now;
    }
    const u64 value = (std::min)(submit, now);
    u64 previous = last.load(std::memory_order_relaxed);
    while (value > previous &&
           !last.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
    }
    return (std::max)(value, previous);
}

template <typename Timing>
[[nodiscard]] u64 ReadGpuTimestamp(const Timing& timing, bool ampere, Settings::GpuClock clock) {
    if (ampere) {
        const u64 now = static_cast<u64>(timing.GetGlobalTimeNs().count());
        if (!Nx2SubmitTimeStamps(clock)) {
            return now;
        }
        return Nx2ReportTimestamp(now, g_nx2_submit_ns.load(std::memory_order_relaxed),
                                  g_nx2_last_report_ns);
    }
    const u64 ticks = timing.GetGPUTicks();
    switch (clock) {
    case Settings::GpuClock::Boost:
        return ticks / 256;
    case Settings::GpuClock::Overclock:
        return ticks / 512;
    default:
        return ticks;
    }
}

} // namespace Tegra
