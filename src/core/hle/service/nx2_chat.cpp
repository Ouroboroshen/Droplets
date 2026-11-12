// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <thread>

#include <nlohmann/json.hpp>

#include "common/httplib.h"
#include "common/logging.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "common/thread.h"
#include "common/zstd_compression.h"
#include "core/hle/service/acc/acc.h"
#include "core/hle/service/nx2_camera.h"
#include "core/hle/service/nx2_chat.h"
#include "core/hle/service/nx2_chat_voice.h"
#include "core/hle/service/sockets/online_friends.h"
#include "core/hle/service/sockets/online_profile.h"
#include "core/hle/service/sockets/online_redirect.h"

namespace Service::Nx2Chat {
namespace {

namespace Online = Service::Sockets::OnlineRedirect;
using Clock = std::chrono::steady_clock;

// --- Pictures ---------------------------------------------------------------------------------
// "DCF1", u16 width, u16 height, then Zstandard of the YUV 4:2:0 planes (full-range BT.601). Each
// sample keeps 6 bits and is stored as its difference from the one to its left (the first of a
// row: from the first of the row above), which repeats enough to compress a face to a few KB.
constexpr std::array<u8, 4> PictureMagic{'D', 'C', 'F', '1'};
constexpr size_t PictureHeader = 8;
constexpr size_t LumaSize = size_t{PictureWidth} * PictureHeight;
constexpr size_t ChromaWidth = PictureWidth / 2;
constexpr size_t ChromaHeight = PictureHeight / 2;
constexpr size_t ChromaSize = ChromaWidth * ChromaHeight;
constexpr size_t PlanesSize = LumaSize + 2 * ChromaSize;
constexpr size_t MaxPictureBytes = 32 * 1024; // the server's cap too (npln_chat.MAX_FRAME)
constexpr int PictureLevel = 6;

u8 Clamp(int value) {
    return static_cast<u8>(std::clamp(value, 0, 255));
}

void DeltaPlane(u8* plane, size_t width, size_t height) {
    u8 row_start = 0;
    for (size_t y = 0; y < height; ++y) {
        u8* const row = plane + y * width;
        u8 previous = row_start;
        row_start = static_cast<u8>(row[0] >> 2);
        for (size_t x = 0; x < width; ++x) {
            const u8 value = static_cast<u8>(row[x] >> 2);
            row[x] = static_cast<u8>(value - previous);
            previous = value;
        }
    }
}

void UndeltaPlane(u8* plane, size_t width, size_t height) {
    u8 row_start = 0;
    for (size_t y = 0; y < height; ++y) {
        u8* const row = plane + y * width;
        u8 previous = row_start;
        for (size_t x = 0; x < width; ++x) {
            const u8 value = static_cast<u8>((previous + row[x]) & 0x3F);
            if (x == 0) {
                row_start = value;
            }
            row[x] = static_cast<u8>(value << 2 | 2);
            previous = value;
        }
    }
}

// Bilinear scale of a sw x sh RGBA8 picture to dw x dh.
void ScaleRgba(const u8* src, u32 sw, u32 sh, u32 dw, u32 dh, u8* dst) {
    for (u32 y = 0; y < dh; ++y) {
        const u64 fy = (u64{y} * 2 + 1) * sh * 128 / dh; // source y in 1/256 pixels, centred
        const u32 sy = static_cast<u32>(std::min<u64>(fy >= 128 ? (fy - 128) >> 8 : 0, sh - 1));
        const u32 sy1 = std::min(sy + 1, sh - 1);
        const u32 wy = fy >= 128 ? static_cast<u32>((fy - 128) & 0xFF) : 0;
        for (u32 x = 0; x < dw; ++x) {
            const u64 fx = (u64{x} * 2 + 1) * sw * 128 / dw;
            const u32 sx = static_cast<u32>(std::min<u64>(fx >= 128 ? (fx - 128) >> 8 : 0, sw - 1));
            const u32 sx1 = std::min(sx + 1, sw - 1);
            const u32 wx = fx >= 128 ? static_cast<u32>((fx - 128) & 0xFF) : 0;
            const u8* a = src + (size_t{sy} * sw + sx) * 4;
            const u8* b = src + (size_t{sy} * sw + sx1) * 4;
            const u8* c = src + (size_t{sy1} * sw + sx) * 4;
            const u8* d = src + (size_t{sy1} * sw + sx1) * 4;
            u8* out = dst + (size_t{y} * dw + x) * 4;
            for (int i = 0; i < 4; ++i) {
                const u32 top = a[i] * (256 - wx) + b[i] * wx;
                const u32 bottom = c[i] * (256 - wx) + d[i] * wx;
                out[i] = static_cast<u8>((top * (256 - wy) + bottom * wy) >> 16);
            }
        }
    }
}

// Overwrites and frees a picture buffer (camera pictures never linger in memory).
void Wipe(std::vector<u8>& pixels) {
    std::fill(pixels.begin(), pixels.end(), u8{0});
    pixels.clear();
    pixels.shrink_to_fit();
}

// --- The server's answers -------------------------------------------------------------------

bool IsCode(const std::string& code) {
    return code.size() == 12 && std::all_of(code.begin(), code.end(), [](char c) {
               return c >= '0' && c <= '9';
           });
}

// Room ids as Reservoir makes them (16 hex digits), within a safe set: they go back into requests.
bool IsRoomId(const std::string& id) {
    return !id.empty() && id.size() <= 32 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               c == '_' || c == '-';
    });
}

u64 ParseAccountId(const std::string& text) {
    if (text.size() != 16 || !std::all_of(text.begin(), text.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        })) {
        return 0;
    }
    return std::strtoull(text.c_str(), nullptr, 16);
}

std::string Text(const nlohmann::json& object, const char* key, size_t limit) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return {};
    }
    std::string text = it->get<std::string>();
    if (text.size() > limit) {
        text.resize(limit);
    }
    return text;
}

bool Flag(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() && it->get<bool>();
}

u64 Number(const nlohmann::json& object, const char* key, u64 limit) {
    const auto it = object.find(key);
    return it != object.end() && it->is_number_unsigned() ? std::min(it->get<u64>(), limit) : 0;
}

MicState ParseMicrophone(const std::string& text) {
    return text == "on" ? MicState::On : text == "muted" ? MicState::Muted : MicState::Off;
}

// GET /v1/chat's answer into `view` (room and invitations); false if it isn't one.
bool ParseRoom(const std::string& body, const std::string& own_code, View& view) {
    const auto answer = nlohmann::json::parse(body, nullptr, false);
    if (answer.is_discarded() || !answer.is_object()) {
        return false;
    }
    view.room.clear();
    view.members.clear();
    view.calls.clear();
    view.invites.clear();
    view.max_members = 0;
    view.max_cameras = 0;
    if (const auto room = answer.find("room"); room != answer.end() && room->is_object() &&
                                                IsRoomId(Text(*room, "id", 64))) {
        view.room = Text(*room, "id", 64);
        view.max_members = Number(*room, "max", 32);
        view.max_cameras = Number(*room, "cameras", 32);
        if (const auto members = room->find("members");
            members != room->end() && members->is_array()) {
            for (const auto& item : *members) {
                if (!item.is_object() || view.members.size() >= 32) {
                    continue;
                }
                Member member;
                member.code = Text(item, "code", 12);
                member.name = Text(item, "name", 64);
                member.account_id = ParseAccountId(Text(item, "nsa", 16));
                member.camera = Flag(item, "camera");
                member.self = member.code == own_code;
                member.microphone = ParseMicrophone(Text(item, "mic", 8));
                member.listening = Flag(item, "listening");
                if (IsCode(member.code)) {
                    view.members.push_back(std::move(member));
                }
            }
        }
        // Friends this GameChat called who haven't answered, then those who declined
        for (const char* key : {"calling", "declined"}) {
            const auto list = room->find(key);
            if (list == room->end() || !list->is_array()) {
                continue;
            }
            for (const auto& item : *list) {
                if (!item.is_object() || view.calls.size() >= 32) {
                    continue;
                }
                PendingCall call{Text(item, "code", 12), Text(item, "name", 64),
                                 std::string_view{key} == "declined"};
                if (IsCode(call.code)) {
                    view.calls.push_back(std::move(call));
                }
            }
        }
    }
    if (const auto invites = answer.find("invites"); invites != answer.end() && invites->is_array()) {
        for (const auto& item : *invites) {
            if (!item.is_object() || view.invites.size() >= 16) {
                continue;
            }
            Invitation invite{Text(item, "room", 64), Text(item, "code", 12), Text(item, "name", 64),
                              static_cast<u32>(Number(item, "age", 24 * 60 * 60))};
            if (IsRoomId(invite.room) && IsCode(invite.code)) {
                view.invites.push_back(std::move(invite));
            }
        }
    }
    return true;
}

struct Picture {
    std::vector<u8> rgba; // PictureWidth x PictureHeight
    u64 number{};
};

// What the game sees of GameChat: when it changes, its events fire.
struct GameView {
    bool chatting{};
    bool camera{};
    std::vector<std::pair<u64, bool>> members;
    bool operator==(const GameView&) const = default;
};

struct Action {
    std::string op;   // start, invite, join, decline, leave
    std::string arg;  // a friend code or a room
};

class Client {
public:
    static Client& Get() {
        static Client client;
        return client;
    }

    Client() {
        // The camera's and the voice's own singletons must outlive this one, which may still let a
        // camera or microphone go when the program ends: made first, they are destroyed last.
        static_cast<void>(Nx2Camera::Enabled());
        static_cast<void>(Voice::GetStatus());
        Voice::SetCallbacks(
            [this] {
                {
                    std::scoped_lock lk{m_mutex};
                    m_room_changed = true;
                }
                Wake();
            },
            [this] { Wake(); });
    }

    ~Client() {
        Stop(false);
        Voice::SetCallbacks({}, {});
    }

    View Snapshot() {
        const Voice::Status voice = Voice::GetStatus();
        std::scoped_lock lk{m_mutex};
        View view = m_view;
        view.busy = !m_actions.empty();
        view.sharing = m_sending;
        view.muted = m_muted;
        view.voice = static_cast<VoiceState>(voice.state);
        view.voice_error = voice.error;
        view.microphone = voice.microphone;
        view.microphone_failed = voice.microphone_failed;
        view.no_microphone = voice.no_microphone;
        for (auto& member : view.members) {
            member.talking = member.self ? voice.self_talking
                                         : std::find(voice.talking.begin(), voice.talking.end(),
                                                     member.code) != voice.talking.end();
            if (const auto it = m_volumes.find(member.code); it != m_volumes.end()) {
                member.volume = it->second;
            }
        }
        return view;
    }

    void Queue(Action action) {
        EnsureThread();
        {
            std::scoped_lock lk{m_mutex};
            if (m_actions.size() < 32) {
                m_actions.push_back(std::move(action));
            }
            m_menu_active = Clock::now();
        }
        Wake();
    }

    // Starts a GameChat when this player isn't in one, then calls each friend into it.
    void CallFriends(const std::vector<std::string>& codes) {
        EnsureThread();
        {
            std::scoped_lock lk{m_mutex};
            if (m_view.room.empty() && m_actions.size() < 32) {
                m_actions.push_back({"start", {}});
            }
            for (const auto& code : codes) {
                if (IsCode(code) && m_actions.size() < 32) {
                    m_actions.push_back({"invite", code});
                }
            }
            m_menu_active = Clock::now();
        }
        Wake();
    }

    // Muting closes the microphone here at once; the server hears of it right after, and of
    // unmuting before the microphone opens.
    void SetMuted(bool muted) {
        {
            std::scoped_lock lk{m_mutex};
            m_muted = muted;
        }
        if (muted) {
            Voice::SetTalking(false);
        }
        Wake();
    }

    void SetMemberVolume(const std::string& code, int percent) {
        percent = std::clamp(percent, 0, 200);
        {
            std::scoped_lock lk{m_mutex};
            m_volumes[code] = percent;
        }
        Voice::SetVolume(code, percent);
    }

    void MenuShown() {
        {
            std::scoped_lock lk{m_mutex};
            for (const auto& [key, ring] : m_rings) {
                m_rings_seen.insert(key);
            }
        }
        Voice::SetRinging(false);
        Wake();
    }

    void Refresh() {
        EnsureThread();
        {
            std::scoped_lock lk{m_mutex};
            m_menu_active = Clock::now();
            m_refresh = true;
        }
        Wake();
    }

    void SetViewChanged(std::function<void()> callback) {
        std::scoped_lock lk{m_callback_mutex};
        m_view_changed = std::move(callback);
    }

    void SetMenuRequested(std::function<void()> callback) {
        std::scoped_lock lk{m_callback_mutex};
        m_menu_requested = std::move(callback);
    }

    void RequestMenu() {
        std::scoped_lock lk{m_callback_mutex};
        if (m_menu_requested) {
            m_menu_requested();
        }
    }

    int AddListener(std::function<void()> callback) {
        std::scoped_lock lk{m_callback_mutex};
        const int id = ++m_next_listener;
        m_listeners.emplace(id, std::move(callback));
        return id;
    }

    void RemoveListener(int id) {
        std::scoped_lock lk{m_callback_mutex};
        m_listeners.erase(id);
    }

    void ApplySettings() {
        m_share = Settings::values.gamechat_share_camera.GetValue();
        UpdateCamera();
        std::string devices = Settings::values.audio_input_device_id.GetValue() + '\n' +
                              Settings::values.audio_output_device_id.GetValue();
        bool changed;
        {
            std::scoped_lock lk{m_mutex};
            changed = devices != m_devices;
            m_devices = std::move(devices);
        }
        if (changed) {
            Voice::DevicesChanged();
        }
        Wake();
    }

    void GameStarted() {
        {
            std::scoped_lock lk{m_mutex};
            m_game_running = true;
        }
        EnsureThread();
        Wake();
    }

    void GameStopped() {
        {
            std::scoped_lock lk{m_mutex};
            m_game_running = false;
            m_local_streaming = false;
            m_remote_streams = {};
        }
        UpdateCamera();
        ForgetPictures();
        Wake();
    }

    void SetLocalStreaming(bool on) {
        {
            std::scoped_lock lk{m_mutex};
            m_local_streaming = on;
        }
        UpdateCamera();
        Wake();
    }

    bool IsChatting() {
        std::scoped_lock lk{m_mutex};
        return !m_view.room.empty();
    }

    // Sharing turned on, the camera on and available, in a GameChat.
    bool LocalCameraOn() {
        if (!m_share.load() || !Nx2Camera::Available()) {
            return false;
        }
        std::scoped_lock lk{m_mutex};
        return !m_view.room.empty();
    }

    std::array<u8, RoomInfoSize> RoomInfo(u64 self, const std::array<u8, 16>& user) {
        std::array<u8, RoomInfoSize> info{};
        const bool camera = LocalCameraOn();
        std::scoped_lock lk{m_mutex};
        if (m_view.room.empty()) {
            return info;
        }
        // +0x00: the room, as a number the game can tell apart (not read by Mario Kart World)
        const u64 room = std::hash<std::string>{}(m_view.room);
        std::memcpy(info.data(), &room, sizeof(room));
        std::memcpy(info.data() + 0x18, user.data(), user.size());
        std::memcpy(info.data() + 0x28, &self, sizeof(self));
        info[0x30] = camera ? 1 : 0;
        s32 count = 0;
        for (const auto& member : m_view.members) {
            if (member.self || member.account_id == 0 || member.account_id == self) {
                continue;
            }
            if (count == MaxRoomInfoMembers) {
                break;
            }
            u8* const entry = info.data() + 0x40 + size_t(count) * 0x10;
            std::memcpy(entry, &member.account_id, sizeof(member.account_id));
            entry[8] = member.camera ? 1 : 0;
            ++count;
        }
        std::memcpy(info.data() + 0x38, &count, sizeof(count));
        return info;
    }

    bool ReadLocal(u32 width, u32 height, u64& seen, std::vector<u8>& out) {
        std::scoped_lock lk{m_camera_mutex};
        return m_camera && m_camera->Read(width, height, seen, out);
    }

    bool ReadRemote(u64 account_id, u32 width, u32 height, u64& seen, std::vector<u8>& out) {
        if (account_id == 0 || width == 0 || height == 0 || width > 4096 || height > 4096) {
            return false;
        }
        std::scoped_lock lk{m_picture_mutex};
        const auto it = m_pictures.find(account_id);
        if (it == m_pictures.end() || it->second.number == seen ||
            it->second.rgba.size() != LumaSize * 4) {
            return false;
        }
        out.resize(size_t{width} * height * 4);
        ScaleRgba(it->second.rgba.data(), PictureWidth, PictureHeight, width, height, out.data());
        seen = it->second.number;
        return true;
    }

    void Exit() {
        Stop(true);
    }

    int StartRemoteStream(u64 account_id) {
        std::scoped_lock lk{m_mutex};
        int free = -1;
        for (u32 slot = 0; slot < RemoteStreams; ++slot) {
            if (m_remote_streams[slot] == account_id) {
                return static_cast<int>(slot);
            }
            if (m_remote_streams[slot] == 0 && free < 0) {
                free = static_cast<int>(slot);
            }
        }
        if (free >= 0 && account_id != 0) {
            m_remote_streams[free] = account_id;
            return free;
        }
        return -1;
    }

    void StopRemoteStream(u32 slot) {
        std::scoped_lock lk{m_mutex};
        if (slot < RemoteStreams) {
            m_remote_streams[slot] = 0;
        }
    }

    u64 RemoteStreamAccount(u32 slot) {
        std::scoped_lock lk{m_mutex};
        return slot < RemoteStreams ? m_remote_streams[slot] : 0;
    }

private:
    static constexpr s32 MaxRoomInfoMembers = 11;
    static constexpr auto FrameInterval = std::chrono::milliseconds{150};
    static constexpr auto VoiceInterval = std::chrono::milliseconds{200};
    static constexpr auto RingFor = std::chrono::seconds{30};
    static constexpr auto RoomCheckInterval = std::chrono::seconds{10};
    static constexpr auto IdleCheckInterval = std::chrono::seconds{4};
    static constexpr auto FriendsInterval = std::chrono::seconds{10};
    static constexpr auto RefreshInterval = std::chrono::seconds{3};
    static constexpr auto NoGameChatInterval = std::chrono::seconds{600};
    static constexpr auto MenuActiveFor = std::chrono::seconds{30};
    static constexpr size_t MaxAnswerBytes = 256 * 1024;

    void EnsureThread() {
        std::scoped_lock life{m_life_mutex};
        if (m_thread.joinable() && m_thread_done.load()) {
            m_thread.join();
            m_thread = {};
        }
        if (!m_thread.joinable()) {
            m_thread_done = false;
            m_thread = std::jthread{[this](std::stop_token stop) { Run(stop); }};
        }
    }

    void Wake() {
        {
            std::scoped_lock lk{m_mutex};
            ++m_wakes;
        }
        m_cv.notify_all();
    }

    void Stop(bool leave) {
        std::scoped_lock life{m_life_mutex};
        if (!m_thread.joinable()) {
            return;
        }
        {
            std::scoped_lock lk{m_mutex};
            m_leave_on_exit = leave;
        }
        m_thread.request_stop();
        m_cv.notify_all();
        m_thread.join();
        m_thread = {};
    }

    // Holds the camera while the game streams it and the player shares it, else lets it go.
    void UpdateCamera() {
        bool want;
        {
            std::scoped_lock lk{m_mutex};
            want = m_local_streaming && !m_view.room.empty() && m_share.load();
        }
        want = want && Nx2Camera::Available();
        std::scoped_lock lk{m_camera_mutex};
        if (want && !m_camera) {
            m_camera.emplace();
        } else if (!want && m_camera) {
            m_camera.reset();
            Wipe(m_send_pixels);
        }
    }

    void ForgetPictures() {
        std::scoped_lock lk{m_picture_mutex};
        for (auto& [id, picture] : m_pictures) {
            Wipe(picture.rgba);
        }
        m_pictures.clear();
    }

    // Keeps the pictures of members still sharing, wipes the rest.
    void KeepPicturesOf(const std::vector<Member>& members) {
        std::scoped_lock lk{m_picture_mutex};
        for (auto it = m_pictures.begin(); it != m_pictures.end();) {
            const bool sharing = std::any_of(members.begin(), members.end(), [&](const Member& m) {
                return !m.self && m.camera && m.account_id == it->first;
            });
            if (sharing) {
                ++it;
            } else {
                Wipe(it->second.rgba);
                it = m_pictures.erase(it);
            }
        }
    }

    GameView CurrentGameView() {
        GameView game;
        game.camera = LocalCameraOn();
        std::scoped_lock lk{m_mutex};
        game.chatting = !m_view.room.empty();
        for (const auto& member : m_view.members) {
            if (!member.self) {
                game.members.emplace_back(member.account_id, member.camera);
            }
        }
        return game;
    }

    void Notify(bool view_changed) {
        const GameView game = CurrentGameView();
        std::scoped_lock lk{m_callback_mutex};
        if (!(game == m_game_view)) {
            m_game_view = game;
            for (auto& [id, listener] : m_listeners) {
                listener();
            }
        }
        if (view_changed && m_view_changed) {
            m_view_changed();
        }
    }

    // --- the server ---------------------------------------------------------------------------

    struct Connection {
        std::string server;
        std::string code;
        std::string secret;
        std::unique_ptr<httplib::SSLClient> client;
        Voice::Endpoint voice; // the same server and sign-in for the GameChat's voice
    };

    // Signs this player's calls; false (and `error` why) when there is no server or profile yet.
    bool Connect(const std::string& server, std::string& error) {
        if (m_connection.client && m_connection.server == server) {
            return true;
        }
        m_connection = {};
        const auto profile = Online::LoadProfile(server, false);
        if (!profile || profile->code.empty() || profile->secret.empty()) {
            error = "play online once on this server first";
            return false;
        }
        std::string host;
        int port = 0;
        if (!Online::FriendsApiAddress(server, host, port)) {
            error = "the server address isn't name:port";
            return false;
        }
        const std::string pem = Online::ServerCaPem();
        if (pem.empty()) {
            error = "the server's certificate isn't kept yet: play online once first";
            return false;
        }
        auto client = std::make_unique<httplib::SSLClient>(host, port);
        client->set_connection_timeout(5);
        client->set_read_timeout(5);
        client->set_write_timeout(5);
        client->set_keep_alive(true);
        client->set_payload_max_length(MaxAnswerBytes);
        client->load_ca_cert_store(pem.data(), pem.size());
        client->enable_server_certificate_verification(true);
        // The certificate names the hosts games connect to, not this address: the server's own
        // CA, the only one trusted here, is the check (as for friend requests)
        client->enable_server_hostname_verification(false);
        m_connection = {server, profile->code, profile->secret, std::move(client),
                        Voice::Endpoint{host, port, pem, profile->code, profile->secret}};
        return true;
    }

    httplib::Headers Headers() const {
        return {{"X-Droplet-Code", m_connection.code},
                {"X-Droplet-Secret", m_connection.secret},
                {"X-Droplet-Version", Common::g_build_version}};
    }

    // A JSON call; the answer's body, or nullopt (and `error`) when it failed.
    std::optional<std::string> Call(const std::string& path, const std::string* body,
                                    std::string& error) {
        auto result = body ? m_connection.client->Post(path, Headers(), *body, "application/json")
                           : m_connection.client->Get(path, Headers());
        m_last_status = result ? result->status : 0;
        LOG_DEBUG(Service, "GameChat {} {}: {}", body ? "POST" : "GET", path, m_last_status);
        if (!result) {
            error = "couldn't reach the server (" + httplib::to_string(result.error()) + ")";
            m_connection.client.reset();
            return std::nullopt;
        }
        if (result->status == 200) {
            return result->body;
        }
        if (result->status == 404) {
            error = "this server has no GameChat (an older server)";
            return std::nullopt;
        }
        const auto answer = nlohmann::json::parse(result->body, nullptr, false);
        error = answer.is_object() ? Text(answer, "error", 200) : std::string{};
        if (error.empty()) {
            error = "the server answered " + std::to_string(result->status);
        }
        return std::nullopt;
    }

    // Takes a room answer: the view changes, and pictures of members gone or not sharing go. The
    // voice follows the room: a GameChat joined starts muted unless gamechat_join_muted is off.
    void TakeRoom(const std::string& body) {
        View view;
        if (!ParseRoom(body, m_connection.code, view)) {
            return;
        }
        bool left;
        bool joined;
        {
            std::scoped_lock lk{m_mutex};
            left = !m_view.room.empty() && view.room != m_view.room;
            joined = !view.room.empty() && view.room != m_view.room;
            m_view.room = view.room;
            m_view.max_members = view.max_members;
            m_view.max_cameras = view.max_cameras;
            m_view.members = view.members;
            m_view.calls = view.calls;
            m_view.invites = view.invites;
            m_view.error.clear();
            if (left) {
                m_reported = {};
            }
            if (joined) {
                m_muted = Settings::values.gamechat_join_muted.GetValue();
            }
        }
        if (left) {
            Voice::Leave();
        }
        if (joined) {
            Voice::Join(m_connection.voice);
        }
        std::vector<std::string> others;
        for (const auto& member : view.members) {
            if (!member.self) {
                others.push_back(member.code);
            }
        }
        Voice::SetMembers(others);
        KeepPicturesOf(view.members);
        UpdateCamera();
        UpdateRinging();
    }

    // Calls ring for RingFor after they were made, until the player opens the C Menu, answers or
    // declines. A friend calling again rings again.
    bool UpdateRinging() {
        const auto now = Clock::now();
        bool ringing = false;
        bool changed = false;
        {
            std::scoped_lock lk{m_mutex};
            std::map<std::string, Ring> rings;
            for (auto& invite : m_view.invites) {
                const std::string key = invite.room + ':' + invite.code;
                const auto old = m_rings.find(key);
                Ring ring{now + std::chrono::seconds{RingFor.count() -
                                                     std::min<s64>(invite.age, RingFor.count())},
                          invite.age};
                if (old != m_rings.end() && invite.age >= old->second.age) {
                    ring.until = old->second.until;
                } else {
                    m_rings_seen.erase(key);
                }
                rings.emplace(key, ring);
                const bool rings_now = now < ring.until && !m_rings_seen.contains(key);
                changed = changed || invite.ringing != rings_now;
                invite.ringing = rings_now;
                ringing = ringing || rings_now;
            }
            m_rings = std::move(rings);
            std::erase_if(m_rings_seen, [&](const std::string& key) { return !m_rings.contains(key); });
            changed = changed || m_view.ringing != ringing;
            m_view.ringing = ringing;
        }
        Voice::SetRinging(ringing);
        return changed;
    }

    void FetchFriends() {
        const auto profile = Online::LoadProfile(m_connection.server, false);
        if (!profile) {
            return;
        }
        const auto lists = Online::FetchFriends(*profile);
        if (!lists.reached) {
            return;
        }
        std::vector<Friend> friends;
        for (const auto& entry : lists.friends) {
            if (IsCode(entry.code) && friends.size() < 512) {
                friends.push_back({entry.code, entry.name.substr(0, 64), entry.online});
            }
        }
        std::scoped_lock lk{m_mutex};
        m_view.friends = std::move(friends);
    }

    // What the server should know of this player: the game's account id this run (when a game
    // runs), whether their camera goes to the GameChat and their microphone's state.
    struct Me {
        u64 account_id{};
        bool camera{};
        MicState microphone{};
        bool known{}; // false: nothing reported yet in this GameChat
        bool operator==(const Me&) const = default;
    };

    Me WantedMe() {
        Me me{0, false, MicState::Off, true};
        const Voice::Status voice = Voice::GetStatus();
        bool streaming;
        {
            std::scoped_lock lk{m_mutex};
            streaming = m_local_streaming;
            if (m_game_running) {
                me.account_id = Service::Account::LastNetworkServiceAccountId();
            }
            if (voice.state != Voice::State::NoVoice && voice.state != Voice::State::Off &&
                !voice.no_microphone) {
                me.microphone = m_muted ? MicState::Muted : MicState::On;
            }
        }
        me.camera = streaming && LocalCameraOn();
        return me;
    }

    bool ReportMe(const Me& me, std::string& error) {
        static constexpr std::array<const char*, 3> microphones{"off", "muted", "on"};
        const std::string body =
            nlohmann::json{{"nsa", me.account_id ? fmt::format("{:016x}", me.account_id) : ""},
                           {"camera", me.camera},
                           {"mic", microphones[static_cast<size_t>(me.microphone)]}}
                .dump();
        const auto answer = Call("/v1/chat/me", &body, error);
        if (!answer) {
            return false;
        }
        TakeRoom(*answer);
        std::scoped_lock lk{m_mutex};
        m_reported = me;
        return true;
    }

    // Sends this player's newest picture (when they share) and takes the others' new ones.
    // Returns the room version the server reports, or nullopt when the call failed.
    std::optional<u64> ExchangePictures(bool share, std::string& error) {
        std::string body;
        if (share) {
            std::scoped_lock lk{m_camera_mutex};
            if (m_camera && m_camera->Read(PictureWidth, PictureHeight, m_send_seen, m_send_pixels)) {
                const auto encoded = EncodePicture(m_send_pixels);
                body.assign(encoded.begin(), encoded.end());
            }
        }
        auto result = m_connection.client->Post("/v1/chat/frame", Headers(), body,
                                                "application/octet-stream");
        std::fill(body.begin(), body.end(), '\0');
        if (!result) {
            error = "couldn't reach the server (" + httplib::to_string(result.error()) + ")";
            m_connection.client.reset();
            return std::nullopt;
        }
        if (result->status == 409) {
            // Not in a GameChat any more (it ended, or this player timed out)
            return u64{0};
        }
        if (result->status != 200) {
            error = "the server answered " + std::to_string(result->status);
            return std::nullopt;
        }
        TakePictures(result->body);
        std::fill(result->body.begin(), result->body.end(), '\0');
        const std::string version = result->get_header_value("X-Chat-Version");
        return std::strtoull(version.c_str(), nullptr, 10);
    }

    // The server's bundle: "DCB1", u8 count, then per picture code[12], u32 number, u32 size, data.
    void TakePictures(const std::string& bundle) {
        const auto* data = reinterpret_cast<const u8*>(bundle.data());
        const size_t size = bundle.size();
        if (size < 5 || std::memcmp(data, "DCB1", 4) != 0) {
            return;
        }
        std::map<std::string, u64> sharing;
        {
            std::scoped_lock lk{m_mutex};
            for (const auto& member : m_view.members) {
                if (!member.self && member.camera && member.account_id != 0) {
                    sharing.emplace(member.code, member.account_id);
                }
            }
        }
        const size_t count = data[4];
        size_t at = 5;
        std::vector<u8> rgba;
        for (size_t i = 0; i < count; ++i) {
            if (size - at < 20) {
                return;
            }
            const std::string code(reinterpret_cast<const char*>(data + at), 12);
            u32 picture_size;
            std::memcpy(&picture_size, data + at + 16, sizeof(picture_size));
            at += 20;
            if (picture_size > MaxPictureBytes || size - at < picture_size) {
                return;
            }
            const std::span<const u8> picture{data + at, picture_size};
            at += picture_size;
            const auto member = sharing.find(code);
            if (member == sharing.end() || !DecodePicture(picture, rgba)) {
                continue;
            }
            std::scoped_lock lk{m_picture_mutex};
            auto& stored = m_pictures[member->second];
            Wipe(stored.rgba);
            stored.rgba = std::move(rgba);
            stored.number = ++m_picture_numbers;
            rgba = {};
        }
    }

    void Run(std::stop_token stop) {
        Common::SetCurrentThreadName("nx2:chat");
        Voice::BlockBrokenPipeSignal();
        // Nothing may escape this thread: an uncaught exception would end the emulator.
        try {
            Loop(stop);
        } catch (const std::exception& e) {
            LOG_ERROR(Service, "GameChat stopped: {}", e.what());
        } catch (...) {
            LOG_ERROR(Service, "GameChat stopped");
        }
        bool leave;
        {
            std::scoped_lock lk{m_mutex};
            leave = m_leave_on_exit && !m_view.room.empty() && m_connection.client;
        }
        if (leave) {
            try {
                m_connection.client->set_read_timeout(2);
                std::string error;
                const std::string body = "{}";
                Call("/v1/chat/leave", &body, error);
            } catch (...) {
            }
        }
        m_connection = {};
        Voice::Leave();
        Voice::SetRinging(false);
        {
            std::scoped_lock lk{m_camera_mutex};
            m_camera.reset();
            Wipe(m_send_pixels);
        }
        ForgetPictures();
        m_thread_done = true;
    }

    void Loop(std::stop_token stop) {
        auto next_room_check = Clock::now();
        Clock::time_point last_room_call{};
        auto next_friends = Clock::now();
        auto next_exchange = Clock::now();
        auto backoff = std::chrono::milliseconds{0};
        u64 room_version = 0;
        while (!stop.stop_requested()) {
            bool refresh;
            {
                std::unique_lock lk{m_mutex};
                const bool in_room = !m_view.room.empty();
                // In a GameChat the loop also shows who talks, so it looks often
                auto wait = in_room ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                          m_game_running ? FrameInterval : VoiceInterval)
                                    : std::chrono::milliseconds{1000};
                wait = std::max(wait, backoff);
                const u64 wakes = m_wakes;
                if (m_actions.empty() && !m_refresh && !m_room_changed) {
                    m_cv.wait_for(lk, stop, wait, [&] { return m_wakes != wakes; });
                }
                if (stop.stop_requested()) {
                    break;
                }
                refresh = std::exchange(m_refresh, false);
            }
            const std::string server = Online::ServerAddress();
            std::string error;
            bool changed = false;
            bool server_changed = false;
            {
                std::scoped_lock lk{m_mutex};
                if (m_view.server != server) {
                    m_view = {};
                    m_view.server = server;
                    m_reported = {};
                    changed = true;
                    server_changed = true;
                }
            }
            if (server_changed) {
                Voice::Leave();
                Voice::SetRinging(false);
            }
            if (m_connection.server != server) {
                m_connection = {};
                next_room_check = Clock::now();
                last_room_call = {};
                m_no_gamechat = 0;
                ForgetPictures();
                UpdateCamera();
            }
            if (server.empty() || !Connect(server, error)) {
                {
                    std::scoped_lock lk{m_mutex};
                    const bool dropped = !m_actions.empty();
                    m_actions.clear();
                    if (m_view.error != error || dropped) {
                        m_view.error = error;
                        changed = true;
                    }
                }
                backoff = std::chrono::seconds{5};
                Notify(changed);
                continue;
            }
            bool failed = Step(refresh, next_room_check, last_room_call, next_friends, next_exchange,
                               room_version, changed, error);
            changed = UpdateRinging() || changed;
            if (const Voice::Status voice = Voice::GetStatus(); !(voice == m_voice_shown)) {
                m_voice_shown = voice;
                changed = true;
            }
            if (failed) {
                std::scoped_lock lk{m_mutex};
                m_view.error = error;
                m_sending = false;
                changed = true;
                backoff = std::min<std::chrono::milliseconds>(
                    std::max<std::chrono::milliseconds>(backoff * 2, std::chrono::seconds{1}),
                    std::chrono::seconds{15});
            } else {
                backoff = std::chrono::milliseconds{0};
            }
            Notify(changed);
        }
    }

    // One round with the server: the C Menu's requests, the room when it's due, then this
    // player's state and the pictures. True if a call failed (`error` says why, and the loop
    // backs off); a refused request of the player's only shows its reason.
    bool Step(bool refresh, Clock::time_point& next_room_check, Clock::time_point& last_room_call,
              Clock::time_point& next_friends, Clock::time_point& next_exchange,
              u64& room_version, bool& changed, std::string& error) {
        for (;;) {
            std::optional<Action> action;
            {
                std::scoped_lock lk{m_mutex};
                if (!m_actions.empty()) {
                    action = m_actions.front();
                }
            }
            if (!action) {
                break;
            }
            const nlohmann::json body = action->op == "invite" ? nlohmann::json{{"code", action->arg}}
                                        : action->op == "join" || action->op == "decline"
                                            ? nlohmann::json{{"room", action->arg}}
                                            : nlohmann::json::object();
            const std::string text = body.dump();
            const auto answer = Call("/v1/chat/" + action->op, &text, error);
            {
                std::scoped_lock lk{m_mutex};
                m_actions.pop_front();
                m_view.error = answer ? std::string{} : error;
            }
            changed = true;
            if (answer) {
                m_no_gamechat = 0;
                TakeRoom(*answer);
                room_version = 0;
            } else if (!m_connection.client) {
                return true;
            }
        }
        bool in_room;
        bool game;
        bool menu;
        bool room_changed;
        {
            std::scoped_lock lk{m_mutex};
            in_room = !m_view.room.empty();
            game = m_game_running;
            menu = Clock::now() - m_menu_active < MenuActiveFor;
            room_changed = std::exchange(m_room_changed, false);
        }
        const auto now = Clock::now();
        // The room (and invitations) while a game runs, the C Menu is open or this player is in a
        // GameChat (which also keeps them in it), and when the voice stream says it changed. The C
        // Menu asks at most every RefreshInterval.
        const bool due = ((game || menu || in_room) && now >= next_room_check) ||
                         (room_changed && in_room);
        if (due || (refresh && now - last_room_call >= RefreshInterval)) {
            next_room_check = now + (in_room ? RoomCheckInterval : IdleCheckInterval);
            last_room_call = now;
            const auto answer = Call("/v1/chat", nullptr, error);
            if (!answer) {
                if (m_last_status == 404 && !in_room) {
                    // A server without GameChat (an older Reservoir): look again later and later,
                    // up to every NoGameChatInterval
                    static constexpr std::array<std::chrono::seconds, 5> waits{
                        std::chrono::seconds{30}, std::chrono::seconds{60}, std::chrono::seconds{120},
                        std::chrono::seconds{300}, NoGameChatInterval};
                    next_room_check = now + waits[std::min<size_t>(m_no_gamechat, waits.size() - 1)];
                    ++m_no_gamechat;
                }
                return true;
            }
            m_no_gamechat = 0;
            TakeRoom(*answer);
            changed = true;
        }
        if (refresh && now >= next_friends) {
            FetchFriends();
            next_friends = now + FriendsInterval;
            changed = true;
        }
        {
            std::scoped_lock lk{m_mutex};
            in_room = !m_view.room.empty();
        }
        if (!in_room) {
            return false;
        }
        if (Voice::GetStatus().state == Voice::State::Off) {
            // In a GameChat whose voice this thread hasn't joined yet (it started again)
            Voice::Join(m_connection.voice);
        }
        const Me me = WantedMe();
        Me reported;
        {
            std::scoped_lock lk{m_mutex};
            reported = m_reported;
        }
        if (!(me == reported)) {
            if (me.microphone != MicState::On) {
                Voice::SetTalking(false);
            }
            if (!ReportMe(me, error)) {
                return true;
            }
            changed = true;
        }
        // The microphone opens only once the server knows it is on
        Voice::SetTalking(me.microphone == MicState::On);
        bool others_share = false;
        {
            std::scoped_lock lk{m_mutex};
            others_share = std::any_of(m_view.members.begin(), m_view.members.end(),
                                       [](const Member& m) { return !m.self && m.camera; });
        }
        // Pictures only matter while a game shows them; quickly while anyone shares
        if (!game || Clock::now() < next_exchange) {
            return false;
        }
        next_exchange = Clock::now() + (me.camera || others_share
                                            ? std::chrono::duration_cast<Clock::duration>(FrameInterval)
                                            : std::chrono::duration_cast<Clock::duration>(
                                                  std::chrono::seconds{1}));
        const auto version = ExchangePictures(me.camera, error);
        if (!version) {
            return true;
        }
        if (*version != room_version) {
            // Members, cameras or ids changed (or the GameChat ended): look again
            room_version = *version;
            const auto answer = Call("/v1/chat", nullptr, error);
            if (!answer) {
                return true;
            }
            TakeRoom(*answer);
            changed = true;
        }
        std::scoped_lock lk{m_mutex};
        if (m_sending != me.camera) {
            m_sending = me.camera;
            changed = true;
        }
        return false;
    }

    std::mutex m_life_mutex;
    std::jthread m_thread;
    std::atomic<bool> m_thread_done{}; // Run returned (EnsureThread starts it again)

    std::mutex m_mutex; // the view and the game's state
    std::condition_variable_any m_cv;
    u64 m_wakes{};
    View m_view;
    std::deque<Action> m_actions;
    bool m_refresh{};
    Clock::time_point m_menu_active{};
    bool m_game_running{};
    bool m_local_streaming{};
    bool m_sending{};
    bool m_leave_on_exit{};
    std::array<u64, RemoteStreams> m_remote_streams{}; // account ids by slot
    Me m_reported{};
    std::atomic<bool> m_share{Settings::values.gamechat_share_camera.GetValue()};
    bool m_muted{true};            // this player's microphone in the GameChat
    bool m_room_changed{};         // the voice stream said the room changed
    std::map<std::string, int> m_volumes; // members' voice volumes by code, in percent
    std::string m_devices;         // the audio devices last applied
    struct Ring {
        Clock::time_point until;
        u32 age{};
    };
    std::map<std::string, Ring> m_rings;  // calls to this player by room:code
    std::set<std::string> m_rings_seen;   // calls the player has seen in the C Menu
    Voice::Status m_voice_shown;          // GameChat's thread only

    Connection m_connection; // GameChat's thread only
    int m_last_status{};     // the last answer's HTTP status (0: none), GameChat's thread only
    size_t m_no_gamechat{};  // room checks in a row the server answered "no GameChat" (404)

    std::mutex m_camera_mutex;
    std::optional<Nx2Camera::Stream> m_camera; // on while the game streams it and it's shared
    std::vector<u8> m_send_pixels;
    u64 m_send_seen{};

    std::mutex m_picture_mutex;
    std::map<u64, Picture> m_pictures; // by account id
    u64 m_picture_numbers{};

    std::mutex m_callback_mutex;
    std::function<void()> m_view_changed;
    std::function<void()> m_menu_requested;
    std::map<int, std::function<void()>> m_listeners;
    int m_next_listener{};
    GameView m_game_view{};
};

} // namespace

std::vector<u8> EncodePicture(std::span<const u8> rgba) {
    if (rgba.size() != LumaSize * 4) {
        return {};
    }
    std::vector<u8> planes(PlanesSize);
    u8* const luma = planes.data();
    u8* const cb = luma + LumaSize;
    u8* const cr = cb + ChromaSize;
    for (size_t y = 0; y < PictureHeight; ++y) {
        for (size_t x = 0; x < PictureWidth; ++x) {
            const u8* p = rgba.data() + (y * PictureWidth + x) * 4;
            luma[y * PictureWidth + x] = Clamp((77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8);
        }
    }
    for (size_t y = 0; y < ChromaHeight; ++y) {
        for (size_t x = 0; x < ChromaWidth; ++x) {
            int r = 0, g = 0, b = 0;
            for (size_t dy = 0; dy < 2; ++dy) {
                for (size_t dx = 0; dx < 2; ++dx) {
                    const u8* p = rgba.data() + ((y * 2 + dy) * PictureWidth + x * 2 + dx) * 4;
                    r += p[0];
                    g += p[1];
                    b += p[2];
                }
            }
            r /= 4;
            g /= 4;
            b /= 4;
            cb[y * ChromaWidth + x] = Clamp(((-43 * r - 85 * g + 128 * b) >> 8) + 128);
            cr[y * ChromaWidth + x] = Clamp(((128 * r - 107 * g - 21 * b) >> 8) + 128);
        }
    }
    DeltaPlane(luma, PictureWidth, PictureHeight);
    DeltaPlane(cb, ChromaWidth, ChromaHeight);
    DeltaPlane(cr, ChromaWidth, ChromaHeight);
    const auto packed = Common::Compression::CompressDataZSTD(planes.data(), planes.size(),
                                                              PictureLevel);
    Wipe(planes);
    if (packed.empty() || PictureHeader + packed.size() > MaxPictureBytes) {
        return {};
    }
    std::vector<u8> out(PictureHeader + packed.size());
    std::memcpy(out.data(), PictureMagic.data(), PictureMagic.size());
    const u16 width = PictureWidth;
    const u16 height = PictureHeight;
    std::memcpy(out.data() + 4, &width, sizeof(width));
    std::memcpy(out.data() + 6, &height, sizeof(height));
    std::memcpy(out.data() + PictureHeader, packed.data(), packed.size());
    return out;
}

bool DecodePicture(std::span<const u8> data, std::vector<u8>& rgba) {
    if (data.size() <= PictureHeader || data.size() > MaxPictureBytes ||
        std::memcmp(data.data(), PictureMagic.data(), PictureMagic.size()) != 0) {
        return false;
    }
    u16 width;
    u16 height;
    std::memcpy(&width, data.data() + 4, sizeof(width));
    std::memcpy(&height, data.data() + 6, sizeof(height));
    if (width != PictureWidth || height != PictureHeight) {
        return false;
    }
    std::vector<u8> planes(PlanesSize);
    // Exactly one picture's planes, never more: Zstandard stops at the buffer's end
    if (Common::Compression::DecompressDataZSTD(data.subspan(PictureHeader), planes) != PlanesSize) {
        Wipe(planes);
        return false;
    }
    u8* const luma = planes.data();
    u8* const cb = luma + LumaSize;
    u8* const cr = cb + ChromaSize;
    UndeltaPlane(luma, PictureWidth, PictureHeight);
    UndeltaPlane(cb, ChromaWidth, ChromaHeight);
    UndeltaPlane(cr, ChromaWidth, ChromaHeight);
    rgba.resize(LumaSize * 4);
    for (size_t y = 0; y < PictureHeight; ++y) {
        for (size_t x = 0; x < PictureWidth; ++x) {
            const int l = luma[y * PictureWidth + x] * 256;
            const int u = cb[(y / 2) * ChromaWidth + x / 2] - 128;
            const int v = cr[(y / 2) * ChromaWidth + x / 2] - 128;
            u8* out = rgba.data() + (y * PictureWidth + x) * 4;
            out[0] = Clamp((l + 359 * v) >> 8);
            out[1] = Clamp((l - 88 * u - 183 * v) >> 8);
            out[2] = Clamp((l + 454 * u) >> 8);
            out[3] = 255;
        }
    }
    Wipe(planes);
    return true;
}

View Snapshot() {
    return Client::Get().Snapshot();
}

void Refresh() {
    Client::Get().Refresh();
}

void Start() {
    Client::Get().Queue({"start", {}});
}

void CallFriends(const std::vector<std::string>& codes) {
    Client::Get().CallFriends(codes);
}

void Join(const std::string& room) {
    Client::Get().Queue({"join", room});
}

void Decline(const std::string& room) {
    Client::Get().Queue({"decline", room});
}

void Leave() {
    Client::Get().Queue({"leave", {}});
}

void SetMuted(bool muted) {
    Client::Get().SetMuted(muted);
}

void SetMemberVolume(const std::string& code, int percent) {
    Client::Get().SetMemberVolume(code, percent);
}

void MenuShown() {
    Client::Get().MenuShown();
}

bool MicrophoneOn() {
    return Voice::GetStatus().microphone;
}

void SetViewChanged(std::function<void()> callback) {
    Client::Get().SetViewChanged(std::move(callback));
}

void SetMenuRequested(std::function<void()> callback) {
    Client::Get().SetMenuRequested(std::move(callback));
}

void RequestMenu() {
    Client::Get().RequestMenu();
}

void Exit() {
    Client::Get().Exit();
}

void ApplySettings() {
    Client::Get().ApplySettings();
}

bool IsChatting() {
    return Client::Get().IsChatting();
}

std::array<u8, RoomInfoSize> RoomInfo(u64 self, const std::array<u8, 16>& user) {
    return Client::Get().RoomInfo(self, user);
}

bool LocalCameraOn() {
    return Client::Get().LocalCameraOn();
}

void SetLocalStreaming(bool on) {
    Client::Get().SetLocalStreaming(on);
}

bool ReadLocal(u32 width, u32 height, u64& seen, std::vector<u8>& out) {
    return Client::Get().ReadLocal(width, height, seen, out);
}

bool ReadRemote(u64 account_id, u32 width, u32 height, u64& seen, std::vector<u8>& out) {
    return Client::Get().ReadRemote(account_id, width, height, seen, out);
}

int AddListener(std::function<void()> callback) {
    return Client::Get().AddListener(std::move(callback));
}

void RemoveListener(int id) {
    Client::Get().RemoveListener(id);
}

int StartRemoteStream(u64 account_id) {
    return Client::Get().StartRemoteStream(account_id);
}

void StopRemoteStream(u32 slot) {
    Client::Get().StopRemoteStream(slot);
}

u64 RemoteStreamAccount(u32 slot) {
    return Client::Get().RemoteStreamAccount(slot);
}

void GameStarted() {
    Client::Get().GameStarted();
}

void GameStopped() {
    Client::Get().GameStopped();
}

} // namespace Service::Nx2Chat
