// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "audio_core/sink/microphone.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/hle/service/audio/nx2_ocarina_songs.h"

namespace Service::Audio {

// The Switch 2 console microphone, which games read through sysmix input streams on the
// TrueRawAudioIn device (Ocarina of Time listens there for hummed or played songs). Two sources,
// mixed:
// - the host microphone chosen in Configure > Audio > Input Device. "None", the default, captures
//   nothing; a chosen one is open only while a game's microphone stream runs;
// - the ocarina songs the Microphone menu plays, as ZELDA NOTES or HORI's Electric Ocarina would.
//   EDEN_NX2_MIC_SONGS=<song>@<seconds>[,...] queues them that long after a game first listens
//   (for tests).
// Samples stay in memory: nothing here logs, saves or sends them.
class Nx2Microphone {
public:
    static Nx2Microphone& Get() {
        static Nx2Microphone instance;
        return instance;
    }

    /// Plays one of Nx2OcarinaSongs into the microphone the next time a game listens (within
    /// 10 s; it's dropped after that). Any thread.
    void QueueSong(size_t index) {
        if (index >= Nx2OcarinaSongs.size()) {
            return;
        }
        std::scoped_lock lk{mutex};
        queued.push_back({index, -1});
        LOG_INFO(Service_Audio, "Microphone: queued {}", Nx2OcarinaSongs[index].title);
    }

    /// Whether the host microphone is capturing, for the frontend's "Microphone on" notice.
    bool HostMicrophoneOn() const {
        return host_on.load(std::memory_order_relaxed);
    }

    /// Whether a game's microphone stream is running.
    bool Listening() const {
        return listening_now.load(std::memory_order_relaxed);
    }

    /// Called by the sysmix timer every tick with whether any microphone stream runs.
    void Update(bool listening, s64 now_ns) {
        std::scoped_lock lk{mutex};
        listening_now.store(listening, std::memory_order_relaxed);
        RunSchedule(listening, now_ns);
        for (auto& song : queued) {
            if (song.queued_ns < 0) {
                song.queued_ns = now_ns;
            }
        }
        // Songs wait for a listener for at most 10 s.
        while (!queued.empty() && !listening && now_ns - queued.front().queued_ns > 10'000'000'000) {
            LOG_INFO(Service_Audio, "Microphone: no game listened for {}",
                     Nx2OcarinaSongs[queued.front().index].title);
            queued.pop_front();
        }
        if (listening) {
            last_listening_ns = now_ns;
            // One try per device and listening spell: a device that won't open isn't retried
            // every tick.
            const std::string device = Settings::values.audio_input_device_id.GetValue();
            if (device != tried_device) {
                tried_device = device;
                host.Open(device);
            }
        } else if (now_ns - last_listening_ns > 1'000'000'000) {
            host.Close();
            tried_device.clear();
        }
        host_on.store(host.IsOpen(), std::memory_order_relaxed);
    }

    /// Closes the host microphone and forgets queued songs; the game's audio session ended.
    void Stop() {
        std::scoped_lock lk{mutex};
        host.Close();
        tried_device.clear();
        queued.clear();
        player.Start({});
        host_on.store(false, std::memory_order_relaxed);
        listening_now.store(false, std::memory_order_relaxed);
    }

    /// Fills out with the next mono 48 kHz samples: the host microphone (silence without one) plus
    /// the playing song.
    void Fill(std::span<s16> out) {
        std::scoped_lock lk{mutex};
        std::fill(out.begin(), out.end(), s16{0});
        if (host.IsOpen()) {
            host.Read(out);
        }
        if (!player.Playing() && !queued.empty()) {
            const auto& song = Nx2OcarinaSongs[queued.front().index];
            queued.pop_front();
            player.Start(song.notes);
            LOG_INFO(Service_Audio, "Microphone: playing {}", song.title);
        }
        player.Mix(out);
    }

private:
    struct Queued {
        size_t index;
        s64 queued_ns;
    };

    void RunSchedule(bool listening, s64 now_ns) {
        if (!schedule_parsed) {
            schedule_parsed = true;
            if (const char* text = std::getenv("EDEN_NX2_MIC_SONGS")) {
                ParseSchedule(text);
            }
        }
        if (listening && first_listening_ns < 0) {
            first_listening_ns = now_ns;
        }
        while (first_listening_ns >= 0 && !schedule.empty() &&
               first_listening_ns + schedule.front().queued_ns <= now_ns) {
            queued.push_back({schedule.front().index, now_ns});
            schedule.pop_front();
        }
    }

    void ParseSchedule(std::string_view text) {
        while (!text.empty()) {
            const auto comma = text.find(',');
            const auto item = text.substr(0, comma);
            text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
            const auto at = item.find('@');
            const auto key = item.substr(0, at);
            const auto it = std::find_if(Nx2OcarinaSongs.begin(), Nx2OcarinaSongs.end(),
                                         [&](const auto& song) { return song.key == key; });
            if (it == Nx2OcarinaSongs.end() || at == std::string_view::npos) {
                LOG_WARNING(Service_Audio, "EDEN_NX2_MIC_SONGS: skipping '{}'", item);
                continue;
            }
            const double seconds = std::strtod(std::string{item.substr(at + 1)}.c_str(), nullptr);
            schedule.push_back({static_cast<size_t>(it - Nx2OcarinaSongs.begin()),
                                static_cast<s64>(seconds * 1e9)});
        }
        std::stable_sort(schedule.begin(), schedule.end(),
                         [](const auto& a, const auto& b) { return a.queued_ns < b.queued_ns; });
    }

    std::mutex mutex;
    AudioCore::Sink::Microphone host;
    std::string tried_device; ///< the device last opened (or tried) while listening
    std::atomic<bool> host_on{};
    std::atomic<bool> listening_now{};
    s64 last_listening_ns{};
    Nx2OcarinaPlayer player;
    std::deque<Queued> queued;
    std::deque<Queued> schedule; ///< queued_ns: when to queue, after a game first listens
    bool schedule_parsed{};
    s64 first_listening_ns{-1};
};

} // namespace Service::Audio
