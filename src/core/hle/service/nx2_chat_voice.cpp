// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>

#include <opus.h>

#ifndef _WIN32
#include <csignal>
#include <pthread.h>
#endif

#include "audio_core/sink/microphone.h"
#include "audio_core/sink/sink_stream.h"
#include "audio_core/sink/speaker.h"
#include "common/httplib.h"
#include "common/logging.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/hle/service/nx2_chat_voice.h"

namespace Service::Nx2Chat::Voice {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr int SampleRate = 48000;
constexpr size_t FrameSamples = 960;                     // 20 ms of mono sound: one Opus frame
constexpr size_t MaxOpusBytes = 400;                     // the server's VOICE_MAX
constexpr size_t MaxMessage = 512;                       // the server's VOICE_MESSAGE_MAX
constexpr size_t MaxPending = 64 * 1024;                 // unfinished messages from the server
constexpr size_t MaxDecoded = 5760;                      // 120 ms, Opus's longest frame
constexpr size_t MaxTalkers = 11;                        // the others in a full GameChat
constexpr size_t QueueSamples = SampleRate * 300 / 1000; // a member's sound waiting to play, at most
constexpr size_t PrimeSamples = SampleRate * 60 / 1000;  // waiting before a member is heard
constexpr size_t TrimSamples = SampleRate * 100 / 1000;  // kept when a member's sound piles up
constexpr size_t MaxUpQueue = 50;                        // frames waiting for the server: 1 s
constexpr size_t MixBlock = 512;
constexpr int Bitrate = 24000;
constexpr double VoicedDb = -45.0;                       // a decoded frame this loud is talking
constexpr auto TalkingFor = 400ms;
constexpr auto NewTalkerGrace = 3s; // a member may be heard before the room says they joined
constexpr auto QuietBeforeReuse = 1s; // a full room reuses the slot of the longest-quiet member
constexpr auto KeepAliveEvery = 4s;
constexpr u8 KindKeepAlive = 0;
constexpr u8 KindFrame = 1;
constexpr u8 KindRoom = 2;
constexpr float GameVolumeWhileTalking = 0.4f;

template <typename T, size_t N>
void Wipe(std::array<T, N>& data) {
    data.fill(T{});
}

template <typename T>
void Wipe(std::vector<T>& data) {
    std::fill(data.begin(), data.end(), T{});
    data.clear();
}

void Wipe(std::string& data) {
    std::fill(data.begin(), data.end(), '\0');
    data.clear();
}

bool IsCode(std::string_view code) {
    return code.size() == 12 &&
           std::all_of(code.begin(), code.end(), [](char c) { return c >= '0' && c <= '9'; });
}

double LevelDb(std::span<const s16> samples) {
    double sum = 0.0;
    for (const s16 sample : samples) {
        sum += double(sample) * double(sample);
    }
    const double rms = samples.empty() ? 0.0 : std::sqrt(sum / double(samples.size()));
    return 20.0 * std::log10(std::max(rms, 1.0) / 32768.0);
}

// EDEN_NX2_TEST_VOICE=<hz>: a tone in bursts takes the microphone's place, so tests never open a
// real one, and each listener logs what it heard every 5 s (counts and pitch, never the sound).
int TestVoiceHz() {
    static const int hz = [] {
        const char* text = std::getenv("EDEN_NX2_TEST_VOICE");
        const int value = text != nullptr ? std::atoi(text) : 0;
        return value >= 50 && value <= 8000 ? value : 0;
    }();
    return hz;
}

// 0.8 s of the tone, then 0.4 s of quiet, from sample `position` on.
void TestTone(int hz, u64 position, std::span<s16> out) {
    for (size_t i = 0; i < out.size(); ++i) {
        const double t = double(position + i) / SampleRate;
        const double cycle = std::fmod(t, 1.2);
        const double fade = std::min({1.0, cycle / 0.01, (0.8 - cycle) / 0.01});
        out[i] = cycle < 0.8 ? static_cast<s16>(8000.0 * fade *
                                                 std::sin(2.0 * std::numbers::pi * hz * t))
                             : s16{0};
    }
}

struct EncoderDeleter {
    void operator()(OpusEncoder* encoder) const {
        opus_encoder_destroy(encoder);
    }
};

struct DecoderDeleter {
    void operator()(OpusDecoder* decoder) const {
        opus_decoder_destroy(decoder);
    }
};

// A member heard through the server: their decoder and their sound waiting to be played.
struct Talker {
    std::unique_ptr<OpusDecoder, DecoderDeleter> decoder;
    std::vector<s16> queue = std::vector<s16>(QueueSamples); // a ring, `size` samples from `head`
    size_t head{};
    size_t size{};
    bool primed{};
    bool have_seq{};
    u16 next_seq{};
    float volume{1.0f};
    Clock::time_point created{};
    Clock::time_point heard{};      // their last frame
    Clock::time_point last_voice{}; // their last frame with talking in it
    // The test voice's statistics
    u64 frames{};
    u64 crossings{};
    u64 voiced_samples{};
    s16 last_sample{};
};

struct UpFrame {
    u16 seq{};
    std::vector<u8> data;
};

class Engine {
public:
    static Engine& Get() {
        static Engine engine;
        return engine;
    }

    ~Engine() {
        std::scoped_lock life{m_life_mutex};
        m_joined = false;
        StopThreads();
        ForgetTalkers();
        CloseSpeaker();
    }

    void Join(const Endpoint& endpoint) {
        std::scoped_lock life{m_life_mutex};
        if (m_joined && m_endpoint == endpoint) {
            return;
        }
        StopThreads();
        ForgetTalkers();
        m_endpoint = endpoint;
        m_joined = true;
        m_talking = false;
        {
            std::scoped_lock lk{m_state_mutex};
            m_confirmed = false;
        }
        SetState(State::Connecting, {});
        LOG_INFO(Service, "GameChat voice: joining");
        m_down = std::jthread{[this](std::stop_token stop) { HearLoop(stop); }};
        UpdateSpeaker();
    }

    void Leave() {
        std::scoped_lock life{m_life_mutex};
        if (!m_joined) {
            return;
        }
        m_joined = false;
        m_talking = false;
        StopThreads();
        if (TestVoiceHz() != 0) {
            std::scoped_lock lk{m_audio_mutex};
            ReportTest(true);
        }
        ForgetTalkers();
        SetState(State::Off, {});
        LOG_INFO(Service, "GameChat voice: left");
        UpdateSpeaker();
    }

    void SetTalking(bool talking) {
        std::scoped_lock life{m_life_mutex};
        m_talking = talking;
        if (talking && m_joined && !m_capture.joinable()) {
            StartTalking();
        } else if (!talking) {
            StopTalking();
        }
    }

    void SetRinging(bool ringing) {
        std::scoped_lock life{m_life_mutex};
        {
            std::scoped_lock lk{m_audio_mutex};
            if (m_ringing == ringing) {
                return;
            }
            m_ringing = ringing;
            m_ring_position = 0;
        }
        UpdateSpeaker();
    }

    // Members who left are forgotten with their sound, unless they're newer than the room the
    // player last heard about.
    void SetMembers(const std::vector<std::string>& codes) {
        const auto now = Clock::now();
        std::scoped_lock lk{m_audio_mutex};
        for (auto it = m_talkers.begin(); it != m_talkers.end();) {
            const bool member = std::find(codes.begin(), codes.end(), it->first) != codes.end();
            it = member || now - it->second.created < NewTalkerGrace ? std::next(it)
                                                                     : Forget(it, "left");
        }
    }

    void SetVolume(const std::string& code, int percent) {
        const float volume = static_cast<float>(std::clamp(percent, 0, 200)) / 100.0f;
        std::scoped_lock lk{m_audio_mutex};
        m_volumes[code] = volume;
        if (const auto it = m_talkers.find(code); it != m_talkers.end()) {
            it->second.volume = volume;
        }
    }

    void DevicesChanged() {
        std::scoped_lock life{m_life_mutex};
        if (m_capture.joinable()) {
            StopTalking();
            StartTalking();
        }
        CloseSpeaker();
        UpdateSpeaker();
    }

    Status GetStatus() {
        Status status;
        {
            std::scoped_lock lk{m_state_mutex};
            status = m_status;
        }
        status.self_talking = status.microphone && m_self_talking.load(std::memory_order_relaxed);
        status.no_microphone = TestVoiceHz() == 0 && Settings::values.audio_input_device_id.GetValue() ==
                                                         AudioCore::Sink::none_device_name;
        const auto now = Clock::now();
        std::scoped_lock lk{m_audio_mutex};
        for (const auto& [code, talker] : m_talkers) {
            if (now - talker.last_voice < TalkingFor) {
                status.talking.push_back(code);
            }
        }
        return status;
    }

    void SetCallbacks(std::function<void()> room_changed, std::function<void()> status_changed) {
        std::scoped_lock lk{m_callback_mutex};
        m_room_changed = std::move(room_changed);
        m_status_changed = std::move(status_changed);
    }

private:
    // --- threads (m_life_mutex held) ----------------------------------------------------------

    void StartTalking() {
        {
            std::scoped_lock lk{m_up_mutex};
            WipeUpQueue();
        }
        {
            std::scoped_lock lk{m_state_mutex};
            m_status.microphone_failed = false;
        }
        m_capture = std::jthread{[this](std::stop_token stop) { CaptureLoop(stop); }};
        m_up = std::jthread{[this](std::stop_token stop) { TalkLoop(stop); }};
    }

    void StopTalking() {
        m_capture.request_stop();
        m_up.request_stop();
        {
            std::scoped_lock lk{m_client_mutex};
            if (m_up_client != nullptr) {
                m_up_client->stop();
            }
        }
        if (m_capture.joinable()) {
            m_capture.join();
        }
        if (m_up.joinable()) {
            m_up.join();
        }
        m_capture = {};
        m_up = {};
        std::scoped_lock lk{m_up_mutex};
        WipeUpQueue();
    }

    void StopThreads() {
        StopTalking();
        m_down.request_stop();
        {
            std::scoped_lock lk{m_client_mutex};
            if (m_down_client != nullptr) {
                m_down_client->stop();
            }
        }
        if (m_down.joinable()) {
            m_down.join();
        }
        m_down = {};
    }

    // The output plays while in a GameChat or while a call rings.
    void UpdateSpeaker() {
        bool ringing;
        {
            std::scoped_lock lk{m_audio_mutex};
            ringing = m_ringing;
        }
        const bool want = m_joined || ringing;
        if (want && !m_speaker.IsOpen()) {
            m_speaker.Open(Settings::values.audio_output_device_id.GetValue(),
                           [this](std::span<s16> samples) { Mix(samples); });
        } else if (!want) {
            CloseSpeaker();
        }
    }

    void CloseSpeaker() {
        m_speaker.Close();
        AudioCore::Sink::SetGameVolumeScale(1.0f);
        m_game_volume = 1.0f;
    }

    void ForgetTalkers() {
        std::scoped_lock lk{m_audio_mutex};
        for (auto& [code, talker] : m_talkers) {
            Wipe(talker.queue);
        }
        m_talkers.clear();
    }

    void WipeUpQueue() {
        for (auto& frame : m_up_queue) {
            Wipe(frame.data);
        }
        m_up_queue.clear();
    }

    // --- status -------------------------------------------------------------------------------

    void NotifyStatus() {
        std::scoped_lock lk{m_callback_mutex};
        if (m_status_changed) {
            m_status_changed();
        }
    }

    void SetState(State state, std::string error) {
        {
            std::scoped_lock lk{m_state_mutex};
            if (m_status.state == state && m_status.error == error) {
                return;
            }
            m_status.state = state;
            m_status.error = std::move(error);
        }
        m_state_cv.notify_all();
        NotifyStatus();
    }

    void SetMicrophone(bool on, bool failed) {
        {
            std::scoped_lock lk{m_state_mutex};
            m_status.microphone = on;
            m_status.microphone_failed = failed;
        }
        NotifyStatus();
    }

    // Talking waits for the server's voice to answer once: no microphone for a server without it.
    bool WaitForVoice(std::stop_token stop) {
        std::unique_lock lk{m_state_mutex};
        return m_state_cv.wait(lk, stop, [this] { return m_confirmed; });
    }

    // --- the server ---------------------------------------------------------------------------

    std::unique_ptr<httplib::SSLClient> MakeClient(time_t read_timeout) const {
        auto client = std::make_unique<httplib::SSLClient>(m_endpoint.host, m_endpoint.port);
        client->set_connection_timeout(5);
        client->set_read_timeout(read_timeout);
        client->set_write_timeout(5);
        client->set_tcp_nodelay(true);
        client->load_ca_cert_store(m_endpoint.ca_pem.data(), m_endpoint.ca_pem.size());
        client->enable_server_certificate_verification(true);
        // The certificate names the hosts games connect to, not this address: the server's own CA,
        // the only one trusted here, is the check (as for GameChat's other calls)
        client->enable_server_hostname_verification(false);
        return client;
    }

    httplib::Headers Headers() const {
        return {{"X-Droplet-Code", m_endpoint.code},
                {"X-Droplet-Secret", m_endpoint.secret},
                {"X-Droplet-Version", Common::g_build_version}};
    }

    bool Pause(std::stop_token stop, std::chrono::milliseconds wait) {
        std::unique_lock lk{m_state_mutex};
        m_state_cv.wait_for(lk, stop, wait, [] { return false; });
        return !stop.stop_requested();
    }

    static std::string Why(int status, const httplib::Result& result) {
        if (status == 403) {
            return "the server refused this player's sign-in";
        }
        if (status == 409) {
            return "not in this GameChat any more";
        }
        if (status != 0 && status != 200) {
            return "the server answered " + std::to_string(status);
        }
        if (!result && result.error() != httplib::Error::Canceled) {
            return "couldn't reach the server (" + httplib::to_string(result.error()) + ")";
        }
        return "the voice connection ended";
    }

    // The other members' voices, as the server sends them, until stopped. A connection that
    // fails or ends is made again, sooner after one that worked.
    void HearLoop(std::stop_token stop) {
        Common::SetCurrentThreadName("nx2:chat:hear");
        BlockBrokenPipeSignal();
        auto backoff = 0ms;
        while (!stop.stop_requested()) {
            if (backoff > 0ms && !Pause(stop, backoff)) {
                break;
            }
            auto client = MakeClient(15);
            {
                std::scoped_lock lk{m_client_mutex};
                if (stop.stop_requested()) {
                    break;
                }
                m_down_client = client.get();
            }
            int status = 0;
            bool streaming = false;
            std::string pending;
            const auto result = client->Get(
                "/v1/chat/voice", Headers(),
                [&](const httplib::Response& response) {
                    status = response.status;
                    return status == 200 && !stop.stop_requested();
                },
                [&](const char* data, size_t length) {
                    if (stop.stop_requested()) {
                        return false;
                    }
                    if (!streaming) {
                        streaming = true;
                        Heard();
                    }
                    pending.append(data, length);
                    return TakeMessages(pending);
                });
            {
                std::scoped_lock lk{m_client_mutex};
                m_down_client = nullptr;
            }
            Wipe(pending);
            if (stop.stop_requested()) {
                break;
            }
            if (status == 404) {
                SetState(State::NoVoice, "this server has no GameChat voice (an older server)");
                LOG_INFO(Service, "GameChat voice: the server has none");
                break;
            }
            SetState(State::Failed, Why(status, result));
            backoff = streaming ? std::chrono::milliseconds{1s}
                                : std::clamp<std::chrono::milliseconds>(backoff * 2, 1s, 15s);
        }
    }

    void Heard() {
        {
            std::scoped_lock lk{m_state_mutex};
            m_confirmed = true;
        }
        SetState(State::On, {});
        m_state_cv.notify_all();
    }

    // The messages in `pending` (a u16 size, then the message), leaving an unfinished one there.
    // False if they aren't messages.
    bool TakeMessages(std::string& pending) {
        size_t at = 0;
        while (pending.size() - at >= 2) {
            const size_t size = static_cast<u8>(pending[at]) | static_cast<u8>(pending[at + 1]) << 8;
            if (size == 0 || size > MaxMessage) {
                return false;
            }
            if (pending.size() - at - 2 < size) {
                break;
            }
            TakeMessage({reinterpret_cast<const u8*>(pending.data()) + at + 2, size});
            at += 2 + size;
        }
        // Moves the unfinished rest to the front, wiping what it leaves behind
        const size_t rest = pending.size() - at;
        std::copy(pending.begin() + at, pending.end(), pending.begin());
        std::fill(pending.begin() + rest, pending.end(), '\0');
        pending.resize(rest);
        return pending.size() <= MaxPending;
    }

    void TakeMessage(std::span<const u8> message) {
        if (message[0] == KindRoom) {
            std::scoped_lock lk{m_callback_mutex};
            if (m_room_changed) {
                m_room_changed();
            }
            return;
        }
        // A voice frame: the speaker's code, u16 seq, Opus
        if (message[0] != KindFrame || message.size() <= 15 || message.size() > 15 + MaxOpusBytes) {
            return;
        }
        const std::string code(reinterpret_cast<const char*>(message.data()) + 1, 12);
        if (!IsCode(code) || code == m_endpoint.code) {
            return;
        }
        const u16 seq = static_cast<u16>(message[13] | message[14] << 8);
        Decode(code, seq, message.subspan(15));
    }

    void Decode(const std::string& code, u16 seq, std::span<const u8> data) {
        std::array<s16, MaxDecoded> pcm;
        std::scoped_lock lk{m_audio_mutex};
        const auto now = Clock::now();
        auto it = m_talkers.find(code);
        if (it == m_talkers.end()) {
            if (m_talkers.size() >= MaxTalkers && !ForgetQuietest(now)) {
                return;
            }
            int error = 0;
            std::unique_ptr<OpusDecoder, DecoderDeleter> decoder{
                opus_decoder_create(SampleRate, 1, &error)};
            if (!decoder || error != OPUS_OK) {
                return;
            }
            it = m_talkers.emplace(code, Talker{}).first;
            it->second.decoder = std::move(decoder);
            it->second.created = now;
            if (TestVoiceHz() != 0) {
                LOG_INFO(Service, "GameChat voice test: hearing {}", code);
            }
            if (const auto volume = m_volumes.find(code); volume != m_volumes.end()) {
                it->second.volume = volume->second;
            }
        }
        Talker& talker = it->second;
        talker.heard = now;
        // A frame or two missing in a run of speech (the server dropped them for a slow
        // listener): the decoder fills the gap.
        const u16 gap = static_cast<u16>(seq - talker.next_seq);
        if (talker.have_seq && talker.primed && gap > 0 && gap <= 2) {
            for (u16 i = 0; i < gap; ++i) {
                const int lost = opus_decode(talker.decoder.get(), nullptr, 0, pcm.data(),
                                             static_cast<int>(FrameSamples), 0);
                if (lost > 0) {
                    Queue(talker, {pcm.data(), static_cast<size_t>(lost)});
                }
            }
        }
        talker.have_seq = true;
        talker.next_seq = static_cast<u16>(seq + 1);
        const int samples = opus_decode(talker.decoder.get(), data.data(),
                                        static_cast<opus_int32>(data.size()), pcm.data(),
                                        static_cast<int>(pcm.size()), 0);
        if (samples <= 0 || static_cast<size_t>(samples) > pcm.size()) {
            Wipe(pcm);
            return;
        }
        const std::span<const s16> sound{pcm.data(), static_cast<size_t>(samples)};
        Queue(talker, sound);
        talker.frames += TestVoiceHz() != 0 ? 1 : 0;
        if (LevelDb(sound) > VoicedDb) {
            talker.last_voice = now;
            if (TestVoiceHz() != 0) {
                for (const s16 sample : sound) {
                    talker.crossings += (sample >= 0) != (talker.last_sample >= 0);
                    talker.last_sample = sample;
                }
                talker.voiced_samples += sound.size();
            }
        }
        Wipe(pcm);
        if (TestVoiceHz() != 0) {
            ReportTest(false);
        }
    }

    // Adds sound to a member's queue; when it piles up (their connection caught up after a
    // stall) the oldest goes, so they're heard with little delay again.
    static void Queue(Talker& talker, std::span<const s16> sound) {
        if (sound.size() > QueueSamples) {
            sound = sound.last(QueueSamples);
        }
        if (talker.size + sound.size() > QueueSamples) {
            const size_t drop = std::min(talker.size, talker.size + sound.size() - TrimSamples);
            for (size_t i = 0; i < drop; ++i) {
                talker.queue[(talker.head + i) % QueueSamples] = 0;
            }
            talker.head = (talker.head + drop) % QueueSamples;
            talker.size -= drop;
        }
        for (const s16 sample : sound) {
            talker.queue[(talker.head + talker.size) % QueueSamples] = sample;
            ++talker.size;
        }
    }

    // m_audio_mutex held. Forgets a member's decoder and sound.
    std::map<std::string, Talker>::iterator Forget(std::map<std::string, Talker>::iterator it,
                                                   std::string_view why) {
        if (TestVoiceHz() != 0) {
            const double seconds = double(it->second.voiced_samples) / SampleRate;
            LOG_INFO(Service, "GameChat voice test: {} {} after {} frames, about {:.0f} Hz",
                     it->first, why, it->second.frames,
                     seconds > 0.0 ? double(it->second.crossings) / 2.0 / seconds : 0.0);
        }
        Wipe(it->second.queue);
        return m_talkers.erase(it);
    }

    // m_audio_mutex held. In a full room (members heard but not yet known to have left), the
    // slot of whoever has been quiet longest, if a while; false if everyone talked just now.
    bool ForgetQuietest(Clock::time_point now) {
        auto quietest = m_talkers.end();
        for (auto it = m_talkers.begin(); it != m_talkers.end(); ++it) {
            if (now - it->second.heard >= QuietBeforeReuse &&
                (quietest == m_talkers.end() || it->second.heard < quietest->second.heard)) {
                quietest = it;
            }
        }
        if (quietest == m_talkers.end()) {
            return false;
        }
        Forget(quietest, "quiet");
        return true;
    }

    // m_audio_mutex held. The test voice's statistics, every 5 s and when leaving: counts and
    // pitch only.
    void ReportTest(bool leaving) {
        const auto now = Clock::now();
        if (now - m_last_report < 5s && !leaving) {
            return;
        }
        m_last_report = now;
        for (const auto& [code, talker] : m_talkers) {
            const double seconds = double(talker.voiced_samples) / SampleRate;
            LOG_INFO(Service, "GameChat voice test: heard {} in {} frames, about {:.0f} Hz; played "
                              "peak {:.1f} dBFS; sent {} frames",
                     code, talker.frames,
                     seconds > 0.0 ? double(talker.crossings) / 2.0 / seconds : 0.0,
                     20.0 * std::log10(std::max(1.0f, m_played_peak.exchange(0.0f)) / 32768.0),
                     m_sent_frames.load());
        }
    }

    // This player's voice, as the capture thread queues it, until stopped.
    void TalkLoop(std::stop_token stop) {
        Common::SetCurrentThreadName("nx2:chat:talk");
        BlockBrokenPipeSignal();
        if (!WaitForVoice(stop)) {
            return;
        }
        auto backoff = 0ms;
        while (!stop.stop_requested()) {
            if (backoff > 0ms && !Pause(stop, backoff)) {
                break;
            }
            auto client = MakeClient(5);
            {
                std::scoped_lock lk{m_client_mutex};
                if (stop.stop_requested()) {
                    break;
                }
                m_up_client = client.get();
            }
            auto last_sent = Clock::now();
            bool first = true;
            bool sent = false;
            std::string out;
            const auto result = client->Post(
                "/v1/chat/voice", Headers(),
                [&](size_t, httplib::DataSink& sink) {
                    {
                        std::unique_lock lk{m_up_mutex};
                        if (first) {
                            // Sound from before this connection is too late to be heard
                            first = false;
                            WipeUpQueue();
                        }
                        m_up_cv.wait_for(lk, stop, 100ms, [this] { return !m_up_queue.empty(); });
                        if (stop.stop_requested()) {
                            return false;
                        }
                        for (auto& frame : m_up_queue) {
                            const size_t size = 3 + frame.data.size();
                            out.push_back(static_cast<char>(size & 0xFF));
                            out.push_back(static_cast<char>(size >> 8));
                            out.push_back(static_cast<char>(KindFrame));
                            out.push_back(static_cast<char>(frame.seq & 0xFF));
                            out.push_back(static_cast<char>(frame.seq >> 8));
                            out.append(reinterpret_cast<const char*>(frame.data.data()),
                                       frame.data.size());
                        }
                        WipeUpQueue();
                    }
                    if (out.empty() && Clock::now() - last_sent >= KeepAliveEvery) {
                        out.assign({char{1}, char{0}, static_cast<char>(KindKeepAlive)});
                    }
                    if (out.empty()) {
                        return true;
                    }
                    last_sent = Clock::now();
                    const bool written = sink.write(out.data(), out.size());
                    sent = sent || written;
                    Wipe(out);
                    return written;
                },
                "application/octet-stream");
            {
                std::scoped_lock lk{m_client_mutex};
                m_up_client = nullptr;
            }
            Wipe(out);
            if (stop.stop_requested()) {
                break;
            }
            const int status = result ? result->status : 0;
            LOG_DEBUG(Service, "GameChat voice: talking ended ({})", status);
            if (status == 404) {
                break;
            }
            backoff = sent ? std::chrono::milliseconds{1s}
                           : std::clamp<std::chrono::milliseconds>(backoff * 2, 1s, 15s);
        }
    }

    void QueueFrame(u16 seq, std::span<const u8> data) {
        {
            std::scoped_lock lk{m_up_mutex};
            if (m_up_queue.size() >= MaxUpQueue) {
                Wipe(m_up_queue.front().data);
                m_up_queue.pop_front();
            }
            m_up_queue.push_back({seq, {data.begin(), data.end()}});
        }
        m_sent_frames.fetch_add(1, std::memory_order_relaxed);
        m_up_cv.notify_all();
    }

    // The microphone (or the test voice), 20 ms at a time through the noise gate and the Opus
    // encoder to the talk thread, until stopped.
    void CaptureLoop(std::stop_token stop) {
        Common::SetCurrentThreadName("nx2:chat:mic");
        if (!WaitForVoice(stop)) {
            return;
        }
        int error = 0;
        std::unique_ptr<OpusEncoder, EncoderDeleter> encoder{
            opus_encoder_create(SampleRate, 1, OPUS_APPLICATION_VOIP, &error)};
        if (!encoder || error != OPUS_OK) {
            LOG_ERROR(Service, "GameChat voice: no Opus encoder ({})", error);
            return;
        }
        opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(Bitrate));
        opus_encoder_ctl(encoder.get(), OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        opus_encoder_ctl(encoder.get(), OPUS_SET_COMPLEXITY(5));
        const int test_hz = TestVoiceHz();
        AudioCore::Sink::Microphone microphone;
        if (test_hz == 0 &&
            !microphone.Open(Settings::values.audio_input_device_id.GetValue())) {
            SetMicrophone(false, true);
            return;
        }
        SetMicrophone(true, false);
        std::array<s16, 4800 + FrameSamples> buffer{};
        size_t filled = 0;
        std::array<u8, MaxOpusBytes> packet{};
        Gate gate;
        u16 seq = 0;
        u64 test_position = 0;
        const auto start = Clock::now();
        auto next = start;
        while (!stop.stop_requested()) {
            next += 10ms;
            if (const auto now = Clock::now(); next < now - 100ms) {
                next = now;
            }
            {
                std::unique_lock lk{m_capture_mutex};
                m_capture_cv.wait_until(lk, stop, next, [] { return false; });
            }
            if (stop.stop_requested()) {
                break;
            }
            const std::span<s16> room{buffer.data() + filled, buffer.size() - filled};
            size_t got;
            if (test_hz != 0) {
                const auto due = static_cast<u64>(
                    std::chrono::duration<double>(Clock::now() - start).count() * SampleRate);
                got = static_cast<size_t>(std::min<u64>(due - test_position, room.size()));
                TestTone(test_hz, test_position, room.first(got));
                test_position += got;
            } else {
                got = microphone.Read(room);
            }
            filled += got;
            size_t at = 0;
            while (filled - at >= FrameSamples) {
                const std::span<const s16> frame{buffer.data() + at, FrameSamples};
                const bool open = gate.Pass(frame);
                m_self_talking.store(open, std::memory_order_relaxed);
                if (open) {
                    const int bytes =
                        opus_encode(encoder.get(), frame.data(), static_cast<int>(FrameSamples),
                                    packet.data(), static_cast<opus_int32>(packet.size()));
                    if (bytes > 0) {
                        QueueFrame(seq, {packet.data(), static_cast<size_t>(bytes)});
                    }
                }
                ++seq;
                at += FrameSamples;
            }
            std::copy(buffer.begin() + at, buffer.begin() + filled, buffer.begin());
            std::fill(buffer.begin() + (filled - at), buffer.begin() + filled, s16{0});
            filled -= at;
        }
        microphone.Close();
        Wipe(buffer);
        Wipe(packet);
        m_self_talking.store(false, std::memory_order_relaxed);
        SetMicrophone(false, false);
    }

    // --- the output (SDL's audio thread) ------------------------------------------------------

    // A phone-like ring: two short chimes, then quiet, every 3 s.
    float RingSample() {
        const double t = double(m_ring_position++) / SampleRate;
        const double cycle = std::fmod(t, 3.0);
        double envelope = 0.0;
        for (const double start : {0.0, 0.5}) {
            const double x = cycle - start;
            if (x >= 0.0 && x < 0.4) {
                envelope = std::max(envelope, std::min(1.0, x / 0.005) * std::exp(-6.0 * x));
            }
        }
        if (envelope == 0.0) {
            return 0.0f;
        }
        const double tone = 0.6 * std::sin(2.0 * std::numbers::pi * 1046.5 * t) +
                            0.4 * std::sin(2.0 * std::numbers::pi * 1318.5 * t);
        return static_cast<float>(envelope * 0.18 * 32767.0 * tone);
    }

    void Mix(std::span<s16> out) {
        float volume = Settings::Volume(); // 0 while the sound is muted
        if (volume > 1.0f) {
            volume = 0.6f + 20.0f * std::log10(volume); // as the games' sound
        }
        for (size_t done = 0; done < out.size() / 2;) {
            const size_t count = std::min(out.size() / 2 - done, MixBlock);
            std::array<float, MixBlock> mix{};
            bool others_talking = false;
            {
                std::scoped_lock lk{m_audio_mutex};
                const auto now = Clock::now();
                for (auto& [code, talker] : m_talkers) {
                    others_talking = others_talking || now - talker.last_voice < TalkingFor;
                    if (!talker.primed) {
                        if (talker.size < PrimeSamples) {
                            continue;
                        }
                        talker.primed = true;
                    }
                    const size_t take = std::min(count, talker.size);
                    for (size_t i = 0; i < take; ++i) {
                        s16& sample = talker.queue[(talker.head + i) % QueueSamples];
                        mix[i] += static_cast<float>(sample) * talker.volume;
                        sample = 0;
                    }
                    talker.head = (talker.head + take) % QueueSamples;
                    talker.size -= take;
                    talker.primed = talker.size != 0;
                }
                if (m_ringing) {
                    for (size_t i = 0; i < count; ++i) {
                        mix[i] += RingSample();
                    }
                }
            }
            float peak = 0.0f;
            for (size_t i = 0; i < count; ++i) {
                const float value = std::clamp(mix[i] * volume, -32768.0f, 32767.0f);
                peak = std::max(peak, std::abs(value));
                out[(done + i) * 2] = static_cast<s16>(value);
                out[(done + i) * 2 + 1] = static_cast<s16>(value);
            }
            mix.fill(0.0f);
            if (peak > m_played_peak.load(std::memory_order_relaxed)) {
                m_played_peak.store(peak, std::memory_order_relaxed);
            }
            // The games' sound goes down quickly while others talk and comes back slowly
            const bool lower =
                others_talking && Settings::values.gamechat_lower_game_volume.GetValue();
            const float target = lower ? GameVolumeWhileTalking : 1.0f;
            const float seconds = static_cast<float>(count) / SampleRate;
            const float step = (1.0f - GameVolumeWhileTalking) * seconds /
                               (target < m_game_volume ? 0.15f : 0.6f);
            m_game_volume = target < m_game_volume ? std::max(target, m_game_volume - step)
                                                   : std::min(target, m_game_volume + step);
            AudioCore::Sink::SetGameVolumeScale(m_game_volume);
            done += count;
        }
    }

    std::mutex m_life_mutex; // Join, Leave, talking and the threads
    bool m_joined{};
    bool m_talking{};
    Endpoint m_endpoint; // set while no thread runs
    std::jthread m_down;
    std::jthread m_up;
    std::jthread m_capture;
    AudioCore::Sink::Speaker m_speaker;

    std::mutex m_state_mutex;
    std::condition_variable_any m_state_cv;
    Status m_status; // without the talking parts, which GetStatus adds
    bool m_confirmed{}; // the server's voice answered since Join

    std::mutex m_client_mutex; // the connections in progress, which Stop cuts short
    httplib::SSLClient* m_down_client{};
    httplib::SSLClient* m_up_client{};

    std::mutex m_up_mutex;
    std::condition_variable_any m_up_cv;
    std::deque<UpFrame> m_up_queue;

    std::mutex m_capture_mutex;
    std::condition_variable_any m_capture_cv;
    std::atomic<bool> m_self_talking{};
    std::atomic<u64> m_sent_frames{};

    std::mutex m_audio_mutex; // the members' sound and the ring
    std::map<std::string, Talker> m_talkers;
    std::map<std::string, float> m_volumes;
    bool m_ringing{};
    u64 m_ring_position{};
    float m_game_volume{1.0f}; // SDL's audio thread, and Close
    std::atomic<float> m_played_peak{};
    Clock::time_point m_last_report{};

    std::mutex m_callback_mutex;
    std::function<void()> m_room_changed;
    std::function<void()> m_status_changed;
};

} // namespace

void BlockBrokenPipeSignal() {
#ifndef _WIN32
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
#endif
}

bool Gate::Pass(std::span<const s16> frame) {
    const double db = LevelDb(frame);
    // The floor follows quiet at once and loud slowly (2.5 dB a second), so it settles on the
    // room's noise between words
    floor_db = db < floor_db ? db : std::min(floor_db + 0.05, db);
    floor_db = std::max(floor_db, -90.0);
    if (db > std::max(floor_db + 9.0, -55.0)) {
        hold = 20; // 400 ms
    } else if (hold > 0) {
        --hold;
    }
    return hold > 0;
}

void Join(const Endpoint& endpoint) {
    Engine::Get().Join(endpoint);
}

void Leave() {
    Engine::Get().Leave();
}

void SetTalking(bool talking) {
    Engine::Get().SetTalking(talking);
}

void SetRinging(bool ringing) {
    Engine::Get().SetRinging(ringing);
}

void SetMembers(const std::vector<std::string>& codes) {
    Engine::Get().SetMembers(codes);
}

void SetVolume(const std::string& code, int percent) {
    Engine::Get().SetVolume(code, percent);
}

void DevicesChanged() {
    Engine::Get().DevicesChanged();
}

Status GetStatus() {
    return Engine::Get().GetStatus();
}

void SetCallbacks(std::function<void()> room_changed, std::function<void()> status_changed) {
    Engine::Get().SetCallbacks(std::move(room_changed), std::move(status_changed));
}

} // namespace Service::Nx2Chat::Voice
