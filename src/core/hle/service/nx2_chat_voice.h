// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"

// GameChat voice (nx2_chat.h): the members talk through the GameChat server's two voice streams
// (Reservoir's /v1/chat/voice on the friends API, TLS; npln_chat.py has the format). The microphone
// is the one chosen in Configure > Audio > Input Device; voices play on the Output Device, beside
// the games' sound.
//
// A microphone is personal data, so:
// - It is open only while this player is in a GameChat with their microphone unmuted (GameChats
//   start muted unless gamechat_join_muted is off), and closes as soon as either ends.
// - Its sound goes only to the server, over TLS that trusts only the server's own certificate, in
//   20 ms Opus frames and only while the player talks (a noise gate holds back the rest). The
//   server passes them to the GameChat's other members and keeps nothing.
// - Nothing here writes sound to a file or the log, and buffers are wiped when they are let go.
// - Everything received is checked before it is decoded and mixed: message and frame sizes,
//   friend codes, the decoder's output.
namespace Service::Nx2Chat::Voice {

// Where the voice streams go: the friends API of the GameChat's server, and this player's sign-in.
struct Endpoint {
    std::string host;
    int port{};
    std::string ca_pem;
    std::string code;
    std::string secret;
    bool operator==(const Endpoint&) const = default;
};

enum class State : u8 {
    Off,          // not in a GameChat
    Connecting,   // reaching the GameChat's voice
    On,           // hearing the GameChat
    NoVoice,      // the server has no GameChat voice (an older Reservoir)
    Failed,       // the last try failed; trying again
};

struct Status {
    State state{State::Off};
    std::string error;                // why the last try failed
    bool microphone{};                // the host microphone is open for the GameChat
    bool microphone_failed{};         // the chosen microphone didn't open
    bool no_microphone{};             // none chosen (Configure > Audio > Input Device is None)
    bool self_talking{};              // this player's voice is going out right now
    std::vector<std::string> talking; // codes of the members heard just now
    bool operator==(const Status&) const = default;
};

// Joins the GameChat's voice, as the player `endpoint.code`: hears the other members. Leave stops
// everything (talking too) and wipes what was buffered; leave before joining another GameChat.
void Join(const Endpoint& endpoint);
void Leave();
// Whether this player talks: the microphone and the stream to the server, only while joined and
// once the server's voice answered. A microphone that didn't open is tried again after talking
// stops and starts.
void SetTalking(bool talking);
// The GameChat's other members by code, whenever the room changes: whoever left is forgotten, with
// their sound, so newcomers always find a place.
void SetMembers(const std::vector<std::string>& codes);
// A call rings for this player: a ring on the output device.
void SetRinging(bool ringing);
// A member's voice volume in percent (0-200, 100 by default).
void SetVolume(const std::string& code, int percent);
// The Input or Output Device setting changed: a microphone or output in use opens again.
void DevicesChanged();
Status GetStatus();
// Called on a voice thread: the server says the GameChat changed (members, microphones, cameras,
// calls), or Status changed. Empty functions clear them.
void SetCallbacks(std::function<void()> room_changed, std::function<void()> status_changed);

// A TLS write to a connection the server (or a stop) closed raises SIGPIPE, which would end the
// program: GameChat's network threads block it in themselves, so such a write just fails.
void BlockBrokenPipeSignal();

// --- Exposed for tests ----------------------------------------------------------------------
// Whether a 20 ms frame of microphone sound goes out: louder than the noise floor by a margin,
// and for a moment after, so words aren't cut. One gate per talk.
class Gate {
public:
    bool Pass(std::span<const s16> frame);

private:
    double floor_db{-60.0};
    int hold{};
};

} // namespace Service::Nx2Chat::Voice
