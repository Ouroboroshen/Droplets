// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"

// GameChat, the Switch 2's C Menu chat, on a private online server (Reservoir's /v1/chat on the
// friends API, npln_chat.py). A player calls friends into a GameChat from Droplet's C Menu (up to
// 12 people); a call rings for the friend, who answers or declines in theirs. The members talk
// (nx2_chat_voice.h: muted when they join unless gamechat_join_muted is off). Games read the room
// through chat:u (friend.cpp) and its cameras through the chat graphics session (nx2_gsv.h). That
// is how Mario Kart World shows the other players' faces online (CameraPlay).
//
// Camera pictures are personal data, so:
// - This player's picture goes out only while they turned on gamechat_share_camera (off by
//   default) and the camera itself (camera_enabled), they are in a GameChat, and the game shows
//   their camera there. Each picture is shrunk to PictureWidth x PictureHeight, compressed and
//   sent over TLS to the server, which passes it only to the other members of that GameChat.
// - Pictures are never written to a file or the log, here or on the server. Received ones stay
//   in memory until their sender stops sharing or leaves, and are wiped then.
// - Everything received is checked: a picture must decompress to exactly its fixed size before
//   it is converted, and games' images are bounds-checked before it is written to them.
namespace Service::Nx2Chat {

inline constexpr u32 PictureWidth = 192;
inline constexpr u32 PictureHeight = 108;
// nn::chat::RoomInfo, what nn::chat::GetRoomInfo copies out (chat:u 11)
inline constexpr size_t RoomInfoSize = 0xF0;

// A member's microphone, as the server last said: talking, muted, or no voice (no microphone, or
// a Droplet without voice).
enum class MicState : u8 { Off, Muted, On };

// The GameChat's voice for this player (nx2_chat_voice.h's State).
enum class VoiceState : u8 { Off, Connecting, On, NoVoice, Failed };

struct Member {
    std::string code;   // friend code, 12 digits
    std::string name;   // the in-game name the server last saw
    u64 account_id{};   // the network service account id the member's game uses this run
    bool camera{};      // sharing their camera with the GameChat
    bool self{};
    MicState microphone{};
    bool listening{};   // hearing the GameChat
    bool talking{};     // heard right now (this player: their voice is going out)
    int volume{100};    // how loud this player hears them, in percent
};

struct Invitation {
    std::string room;
    std::string code;   // who called
    std::string name;
    u32 age{};          // seconds since the call, as the server last said
    bool ringing{};     // the call still rings (for a while after it came, until the C Menu opens)
};

// A friend this GameChat called who hasn't answered, or who declined a moment ago.
struct PendingCall {
    std::string code;
    std::string name;
    bool declined{};
};

struct Friend {
    std::string code;
    std::string name;
    bool online{};
};

struct View {
    std::string server;            // the private server online play uses; empty: none
    std::string error;             // why the last call failed, until one succeeds
    std::string room;              // empty: not in a GameChat
    size_t max_members{};
    size_t max_cameras{};          // people who can show their camera at once
    std::vector<Member> members;   // this player included, in joining order
    std::vector<PendingCall> calls;
    std::vector<Invitation> invites;
    std::vector<Friend> friends;
    bool busy{};                   // a request is on its way
    bool sharing{};                // this player's picture is going to the GameChat right now
    bool ringing{};                // a call rings for this player
    // Voice
    VoiceState voice{};
    std::string voice_error;       // why the voice connection last failed
    bool muted{};                  // this player's microphone is muted in the GameChat
    bool microphone{};             // the host microphone is open for the GameChat right now
    bool microphone_failed{};      // the chosen microphone didn't open
    bool no_microphone{};          // Configure > Audio > Input Device is None
};

// --- The C Menu's side. These return at once; GameChat's own thread does the calls. ----------
View Snapshot();
void Refresh();
void Start();
// Calls friends into this player's GameChat, starting one first when they aren't in one.
void CallFriends(const std::vector<std::string>& codes);
void Join(const std::string& room);
void Decline(const std::string& room);
void Leave();
// Mutes or unmutes this player's microphone in the GameChat. Muting closes it at once.
void SetMuted(bool muted);
// How loud this player hears a member, in percent (0-200).
void SetMemberVolume(const std::string& code, int percent);
// The C Menu opened: the player has seen the calls, which stop ringing.
void MenuShown();
// Whether the host microphone is open for the GameChat (the status bar's "Microphone on").
bool MicrophoneOn();

// The frontend's callbacks (any thread): the view changed; the game asked for the C Menu
// (nn::chat::ShowChatStart). Empty functions clear them.
void SetViewChanged(std::function<void()> callback);
void SetMenuRequested(std::function<void()> callback);
void RequestMenu();

// Leaves the GameChat (briefly waiting for the server) and stops GameChat's thread. The frontend
// calls it when it closes.
void Exit();

// Picks up a changed gamechat_share_camera, camera or audio device setting
// (Core::System::ApplySettings).
void ApplySettings();

// --- The game's side (chat:u, the chat graphics session) -------------------------------------
bool IsChatting();
// What the game's nn::chat::GetRoomInfo gets: `self` is the game's network service account id,
// `user` its account's uid. Layout read from Mario Kart World 1.8.0: +0x18 uid, +0x28 u64 own
// account id, +0x30 u8 own camera on (bit 0), +0x38 s32 count, +0x40 the other members
// (up to 11, 16 bytes each: u64 account id, u8 camera on).
std::array<u8, RoomInfoSize> RoomInfo(u64 self, const std::array<u8, 16>& user);
// Whether this player's camera is on in the GameChat: in one, sharing turned on and the camera
// on and available. The room info's own camera flag.
bool LocalCameraOn();
// The game starts or stops streaming this player's camera (StartLocalCameraStreaming).
void SetLocalStreaming(bool on);
// This player's newest camera picture for the game's own GameChat image, and the newest one of
// the member with this account id: RGBA8 rows of width * 4 bytes at the game image's size, when
// newer than `seen` (which becomes its number).
bool ReadLocal(u32 width, u32 height, u64& seen, std::vector<u8>& out);
bool ReadRemote(u64 account_id, u32 width, u32 height, u64& seen, std::vector<u8>& out);

// The game's streams of other members' cameras (StartRemoteCameraStreaming): up to
// RemoteStreams at once, each shown through one of the chat graphics session's displays.
// StartRemoteStream returns the stream's slot (the same one again for the same account id), or
// -1 when all are taken.
inline constexpr u32 RemoteStreams = 4;
int StartRemoteStream(u64 account_id);
void StopRemoteStream(u32 slot);
u64 RemoteStreamAccount(u32 slot); // 0 when the slot streams nothing

// Called (on GameChat's thread or the caller's) when IsChatting or RoomInfo may answer
// differently: chat:u signals the game's events from it. RemoveListener returns after any call
// in progress.
int AddListener(std::function<void()> callback);
void RemoveListener(int id);

// A game opened GameChat (chat:u) / the running game stopped: its camera stream and account id
// go then, the GameChat stays.
void GameStarted();
void GameStopped();

// --- Pictures (exposed for tests) ------------------------------------------------------------
// PictureWidth x PictureHeight RGBA8 -> the bytes sent; empty on failure.
std::vector<u8> EncodePicture(std::span<const u8> rgba);
// The bytes received -> PictureWidth x PictureHeight RGBA8; false if they aren't exactly one
// picture.
bool DecodePicture(std::span<const u8> data, std::vector<u8>& rgba);

} // namespace Service::Nx2Chat
