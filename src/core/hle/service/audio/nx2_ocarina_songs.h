// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <string_view>

namespace Service::Audio {

// Ocarina of Time's twelve songs, played as sound into the console microphone. On a Switch 2 the
// Nintendo Switch App's ZELDA NOTES (Play Ocarina, auto-play) and HORI's Electric Ocarina reach the
// game this way: the game listens for the melody and Link plays the song he knows.
struct Nx2OcarinaSong {
    std::string_view key;   ///< short name, for EDEN_NX2_MIC_SONGS
    std::string_view title;
    std::string_view notes; ///< D = D4 (A), F = F4 (C-down), A = A4 (C-right), B = B4 (C-left), d = D5 (C-up)
};

inline constexpr std::array<Nx2OcarinaSong, 12> Nx2OcarinaSongs{{
    {"zelda", "Zelda's Lullaby", "BdABdA"},
    {"epona", "Epona's Song", "dBAdBA"},
    {"saria", "Saria's Song", "FABFAB"},
    {"sun", "Sun's Song", "AFdAFd"},
    {"time", "Song of Time", "ADFADF"},
    {"storms", "Song of Storms", "DFdDFd"},
    {"minuet", "Minuet of Forest", "DdBABA"},
    {"bolero", "Bolero of Fire", "FDFDAFAF"},
    {"serenade", "Serenade of Water", "DFAAB"},
    {"requiem", "Requiem of Spirit", "DFDAFD"},
    {"nocturne", "Nocturne of Shadow", "BAADBAF"},
    {"prelude", "Prelude of Light", "dAdABd"},
}};

/// Renders one song at 48 kHz, a sample at a time: a short lead-in, then each note for 0.45 s with
/// 0.1 s between notes, as a soft ocarina-like tone (a sine with a little of its overtones).
class Nx2OcarinaPlayer {
public:
    static constexpr std::uint32_t SampleRate = 48000;
    static constexpr std::uint32_t LeadIn = SampleRate / 4;
    static constexpr std::uint32_t NoteLength = SampleRate * 45 / 100;
    static constexpr std::uint32_t GapLength = SampleRate / 10;

    static double Frequency(char note) {
        switch (note) {
        case 'D':
            return 293.66;
        case 'F':
            return 349.23;
        case 'A':
            return 440.00;
        case 'B':
            return 493.88;
        case 'd':
            return 587.33;
        default:
            return 0.0;
        }
    }

    void Start(std::string_view song_notes) {
        notes = song_notes;
        position = 0;
        phase = 0.0;
    }

    bool Playing() const {
        return position < Length();
    }

    std::uint32_t Length() const {
        return notes.empty() ? 0
                             : LeadIn + static_cast<std::uint32_t>(notes.size()) *
                                            (NoteLength + GapLength);
    }

    /// Adds the next out.size() samples to out (saturating); stays silent once the song is over.
    void Mix(std::span<std::int16_t> out) {
        constexpr double Amplitude = 0.4 * 32767.0 / 1.2;
        constexpr std::uint32_t Attack = SampleRate * 15 / 1000;
        constexpr std::uint32_t Release = SampleRate * 30 / 1000;
        for (auto& sample : out) {
            if (!Playing()) {
                return;
            }
            const std::uint32_t at = position++;
            if (at < LeadIn) {
                continue;
            }
            const std::uint32_t in_song = at - LeadIn;
            const std::uint32_t in_note = in_song % (NoteLength + GapLength);
            if (in_note >= NoteLength) {
                phase = 0.0;
                continue;
            }
            const double frequency = Frequency(notes[in_song / (NoteLength + GapLength)]);
            double envelope = 1.0;
            if (in_note < Attack) {
                envelope = static_cast<double>(in_note) / Attack;
            } else if (in_note >= NoteLength - Release) {
                envelope = static_cast<double>(NoteLength - in_note) / Release;
            }
            const double tone =
                std::sin(phase) + 0.15 * std::sin(2.0 * phase) + 0.05 * std::sin(3.0 * phase);
            phase += 2.0 * std::numbers::pi * frequency / SampleRate;
            if (phase > 2.0 * std::numbers::pi) {
                phase -= 2.0 * std::numbers::pi;
            }
            const double mixed = sample + Amplitude * envelope * tone;
            sample = static_cast<std::int16_t>(mixed > 32767.0    ? 32767.0
                                               : mixed < -32768.0 ? -32768.0
                                                                  : mixed);
        }
    }

private:
    std::string_view notes;
    std::uint32_t position{};
    double phase{};
};

} // namespace Service::Audio
