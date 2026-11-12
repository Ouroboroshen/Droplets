// SPDX-FileCopyrightText: Copyright 2026 Droplet
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "core/hle/service/sockets/online_profile.h"

// ZELDA NOTES for The Legend of Zelda: Ocarina of Time (Nintendo Switch 2 Edition). On a console
// the Nintendo Switch App reaches the game through Nintendo's servers; with a private server,
// Reservoir plays the app's server (npln_notes.py) and Droplet's ZELDA NOTES window shows what the
// app showed: Link on a live map, what the player has collected, and the enemies around Link.
//
// The window asks the server, on the friends API's port, signed with the profile's friend code and
// secret like GameChat. The game sends the server its map and save flags only while the window is
// open (Start ... Stop), and the server keeps them in memory for that player alone. The calls
// block (a few seconds at most), so the window makes them off its thread.
namespace Service::Sockets::OnlineRedirect {

// Ocarina of Time (Nintendo Switch 2 Edition): the only game with ZELDA NOTES here.
constexpr u64 OcarinaOfTimeProgramId = 0x0400486024DEC000ULL;

struct NotesPlayer {
    double pos[3]{};
    double front[3]{};
    std::string stage;
    std::string zone;
    bool child = false;
    bool stopped = false;
    bool indoor = false;
    double age = 0; // seconds since the game sent it
};

struct NotesCollected {
    std::string name; // the game's name for the save flags (heart_pieces, gold_skulltula, ...)
    int found = 0;    // bits the game has set
    int bits = 0;
};

struct NotesNearby {
    int type = -1; // the game's actor type number
    double distance = -1;
    int life = -1;
    int max_life = -1;
    bool battle = false;
};

struct NotesView {
    bool reached = false; // the server answered (an error then is its refusal)
    std::string error;
    std::string game;     // "connected" (Ocarina of Time with ZELDA NOTES on), "offline", "other_game"
    bool open = false;    // the server has this window's session
    bool map_started = false;
    std::optional<NotesPlayer> player;
    std::vector<NotesCollected> collected;
    std::string live; // the live channel: "connecting", "connected" or "off"
    std::vector<NotesNearby> nearby;
};

// What the window shows now (GET /v1/notes).
NotesView FetchNotes(const Profile& profile);
// "start" when the window opens, "stop" when it closes; the view as it is after.
NotesView NotesAction(const Profile& profile, const std::string& action);

} // namespace Service::Sockets::OnlineRedirect
