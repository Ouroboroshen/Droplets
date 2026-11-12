// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stop_token>
#include <thread>

#include <SDL3/SDL.h>

#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/hle/service/nx2_camera.h"

namespace Service::Nx2Camera {
namespace {

using Clock = std::chrono::steady_clock;

constexpr u32 PatternWidth = 1280;
constexpr u32 PatternHeight = 720;
constexpr auto PatternInterval = std::chrono::milliseconds{33};
// Frames are kept centre-cropped to 16:9, the shape games ask for, and at most this wide.
constexpr u32 MaxFrameWidth = 1920;

struct Config {
    bool enabled{};
    std::string device;
    bool mirror{};
};

Config ReadConfig() {
    return {Settings::values.camera_enabled.GetValue(), Settings::values.camera_device.GetValue(),
            Settings::values.camera_mirror.GetValue()};
}

bool InitSdlCamera() {
    static const bool ok = [] {
        if (SDL_WasInit(SDL_INIT_CAMERA) != 0 || SDL_InitSubSystem(SDL_INIT_CAMERA)) {
            return true;
        }
        LOG_ERROR(Service, "Camera: SDL's camera support failed to start: {}", SDL_GetError());
        return false;
    }();
    return ok;
}

// Bilinear scale of a sw x sh RGBA8 picture (rows of src_pitch bytes) to dw x dh, centre-cropped
// to the target's shape and mirrored left to right if asked.
void Scale(const u8* src, size_t src_pitch, u32 sw, u32 sh, u32 dw, u32 dh, bool mirror, u8* dst) {
    u64 cw = sw;
    u64 ch = sh;
    if (u64{sw} * dh > u64{sh} * dw) {
        cw = u64{sh} * dw / dh;
    } else {
        ch = u64{sw} * dh / dw;
    }
    // Positions in 1/65536 pixel.
    const s64 x0 = static_cast<s64>((sw - cw) << 15);
    const s64 y0 = static_cast<s64>((sh - ch) << 15);
    const s64 step_x = static_cast<s64>((cw << 16) / dw);
    const s64 step_y = static_cast<s64>((ch << 16) / dh);
    const s64 max_x = s64{sw - 1} << 16;
    const s64 max_y = s64{sh - 1} << 16;
    std::vector<u32> col_a(dw);
    std::vector<u32> col_b(dw);
    std::vector<u32> col_w(dw);
    for (u32 x = 0; x < dw; ++x) {
        const u32 tx = mirror ? dw - 1 - x : x;
        const s64 sx = std::clamp<s64>(x0 + tx * step_x + step_x / 2 - 32768, 0, max_x);
        col_a[x] = static_cast<u32>(sx >> 16) * 4;
        col_b[x] = std::min(static_cast<u32>(sx >> 16) + 1, sw - 1) * 4;
        col_w[x] = static_cast<u32>(sx & 0xFFFF) >> 8;
    }
    for (u32 y = 0; y < dh; ++y) {
        const s64 sy = std::clamp<s64>(y0 + y * step_y + step_y / 2 - 32768, 0, max_y);
        const u32 ya = static_cast<u32>(sy >> 16);
        const u32 yb = std::min(ya + 1, sh - 1);
        const u32 wy = static_cast<u32>(sy & 0xFFFF) >> 8;
        const u8* const top = src + ya * src_pitch;
        const u8* const bottom = src + yb * src_pitch;
        u8* const out = dst + size_t{y} * dw * 4;
        for (u32 x = 0; x < dw; ++x) {
            const u32 a = col_a[x];
            const u32 b = col_b[x];
            const u32 wx = col_w[x];
            for (u32 c = 0; c < 4; ++c) {
                const u32 t = top[a + c] * (256 - wx) + top[b + c] * wx;
                const u32 m = bottom[a + c] * (256 - wx) + bottom[b + c] * wx;
                out[x * 4 + c] = static_cast<u8>((t * (256 - wy) + m * wy + 32768) >> 16);
            }
        }
    }
}

// Colour bars with a face drifting across them, red and blue squares in the top corners (to show
// flips) and a grey ramp below.
void DrawPattern(u64 frame, std::vector<u8>& out) {
    static constexpr std::array<std::array<u8, 3>, 7> Bars{{{191, 191, 191},
                                                             {191, 191, 0},
                                                             {0, 191, 191},
                                                             {0, 191, 0},
                                                             {191, 0, 191},
                                                             {191, 0, 0},
                                                             {0, 0, 191}}};
    out.resize(size_t{PatternWidth} * PatternHeight * 4);
    const double t = static_cast<double>(frame) / 30.0;
    const int cx = static_cast<int>(PatternWidth / 2) + static_cast<int>(200.0 * std::sin(t * 1.5));
    const int cy = static_cast<int>(PatternHeight / 2) - 20;
    constexpr int Radius = 150;
    for (u32 y = 0; y < PatternHeight; ++y) {
        u8* row = out.data() + size_t{y} * PatternWidth * 4;
        for (u32 x = 0; x < PatternWidth; ++x) {
            std::array<u8, 3> rgb;
            if (y < PatternHeight * 2 / 3) {
                rgb = Bars[x * Bars.size() / PatternWidth];
            } else {
                const u8 grey = static_cast<u8>(x * 255 / (PatternWidth - 1));
                rgb = {grey, grey, grey};
            }
            if (y < 80 && x < 80) {
                rgb = {255, 0, 0};
            } else if (y < 80 && x >= PatternWidth - 80) {
                rgb = {0, 0, 255};
            }
            const int dx = static_cast<int>(x) - cx;
            const int dy = static_cast<int>(y) - cy;
            const int d2 = dx * dx + dy * dy;
            if (d2 < Radius * Radius) {
                rgb = {236, 188, 150};
                const int ex = std::abs(dx) - 55;
                const int ey = dy + 40;
                if (ex * ex + ey * ey < 18 * 18) {
                    rgb = {40, 30, 30};
                } else if (dy > 20 && d2 > 70 * 70 && d2 < 82 * 82) {
                    rgb = {150, 40, 40};
                }
            }
            row[x * 4 + 0] = rgb[0];
            row[x * 4 + 1] = rgb[1];
            row[x * 4 + 2] = rgb[2];
            row[x * 4 + 3] = 255;
        }
    }
}

// Zeroes and frees a buffer that held camera pictures.
void Wipe(std::vector<u8>& pixels) {
    std::fill(pixels.begin(), pixels.end(), u8{0});
    pixels.clear();
    pixels.shrink_to_fit();
}

class Capture {
public:
    static Capture& Get() {
        static Capture capture;
        return capture;
    }

    Capture() : m_config{ReadConfig()}, m_mirror{m_config.mirror} {}

    // Acquire and Release hold m_life_mutex throughout, so a new capture thread never starts
    // while the last one is still turning the camera off.
    void Acquire() {
        std::scoped_lock life{m_life_mutex};
        std::scoped_lock lk{m_mutex};
        // The settings come from Reconfigure (Core::System::ApplySettings) on the thread that
        // changes them, never read here while the frontend might be writing them.
        if (m_users++ == 0) {
            m_thread = std::jthread{[this](std::stop_token stop) { Run(stop); }};
        }
    }

    void Release() {
        std::scoped_lock life{m_life_mutex};
        std::jthread thread;
        {
            std::scoped_lock lk{m_mutex};
            if (--m_users != 0) {
                return;
            }
            thread = std::move(m_thread);
        }
        // The capture thread turns the camera off and wipes the frame before this join returns.
        thread.request_stop();
        thread.join();
    }

    void Reconfigure() {
        std::scoped_lock lk{m_mutex};
        m_config = ReadConfig();
        m_mirror = m_config.mirror;
        ++m_generation;
        m_cv.notify_all();
    }

    bool Enabled() const {
        std::scoped_lock lk{m_mutex};
        return m_config.enabled && !m_config.device.empty();
    }

    // Enabled, and the chosen camera is plugged in (looked up at most once a second) and has not
    // failed to open since the settings last changed.
    bool Available() {
        Config config;
        {
            std::scoped_lock lk{m_mutex};
            config = m_config;
        }
        if (!config.enabled || config.device.empty()) {
            return false;
        }
        if (config.device == TestPatternDevice) {
            return true;
        }
        std::scoped_lock lk{m_presence_mutex};
        const auto now = Clock::now();
        if (config.device != m_presence_device || now - m_presence_checked >= std::chrono::seconds{1}) {
            const auto devices = ListDevices();
            const bool present =
                std::find(devices.begin(), devices.end(), config.device) != devices.end();
            if (present && !m_present) {
                // Plugged in (again): worth another try.
                m_failed = false;
            }
            m_present = present;
            m_presence_device = config.device;
            m_presence_checked = now;
        }
        return m_present && !m_failed.load(std::memory_order_relaxed);
    }

    bool Active() const {
        return m_active.load(std::memory_order_relaxed);
    }

    bool Read(u32 width, u32 height, u64& seen, std::vector<u8>& out) const {
        if (width == 0 || height == 0) {
            return false;
        }
        std::scoped_lock lk{m_frame_mutex};
        if (m_frame.empty() || m_sequence == seen) {
            return false;
        }
        out.resize(size_t{width} * height * 4);
        Scale(m_frame.data(), size_t{m_width} * 4, m_width, m_height, width, height,
              m_mirror.load(std::memory_order_relaxed), out.data());
        seen = m_sequence;
        return true;
    }

private:
    void Run(std::stop_token stop) {
        Common::SetCurrentThreadName("nx2:camera");
        std::string open; // the device on now, empty for none
        // A device that failed to open, skipped until the settings change or it is plugged in
        // again.
        std::string refused;
        Config last_config;
        u64 pattern_frame = 0;
        while (!stop.stop_requested()) {
            Config config;
            {
                std::scoped_lock lk{m_mutex};
                config = m_config;
            }
            if (config.device != last_config.device || config.enabled != last_config.enabled) {
                m_failed = false;
                last_config = config;
            }
            if (!m_failed) {
                refused.clear();
            }
            const bool wanted = config.enabled && !config.device.empty();
            if (!open.empty() && (!wanted || config.device != open)) {
                Close(open);
            }
            if (wanted && open.empty() && config.device != refused) {
                if (config.device == TestPatternDevice || Open(config.device)) {
                    open = config.device;
                    m_active = true;
                    LOG_INFO(Service, "Camera on for the game ({})",
                             open == TestPatternDevice ? "test pattern" : "camera");
                } else {
                    refused = config.device;
                    m_failed = true;
                }
            }
            if (open.empty()) {
                Wait(stop, std::chrono::milliseconds{250});
                continue;
            }
            if (open == TestPatternDevice) {
                DrawPattern(pattern_frame++, m_buffer);
                Publish(PatternWidth, PatternHeight);
                Wait(stop, PatternInterval);
                continue;
            }
            if (!Poll()) {
                if (m_camera == nullptr) {
                    // Access was denied or the camera went away.
                    Close(open);
                    refused = config.device;
                    m_failed = true;
                    continue;
                }
                Wait(stop, std::chrono::milliseconds{4});
            }
        }
        if (!open.empty()) {
            Close(open);
        }
    }

    // Sleeps until the time passes, the settings change or the thread is asked to stop.
    void Wait(const std::stop_token& stop, std::chrono::milliseconds time) {
        std::unique_lock lk{m_mutex};
        const u64 generation = m_generation;
        m_cv.wait_for(lk, stop, time, [&] { return m_generation != generation; });
    }

    bool Open(const std::string& name) {
        if (!InitSdlCamera()) {
            return false;
        }
        int count = 0;
        SDL_CameraID* const ids = SDL_GetCameras(&count);
        SDL_CameraID id = 0;
        for (int i = 0; ids != nullptr && i < count; ++i) {
            const char* const device_name = SDL_GetCameraName(ids[i]);
            if (device_name != nullptr && name == device_name) {
                id = ids[i];
                break;
            }
        }
        SDL_free(ids);
        if (id == 0) {
            // Never fall back to another camera than the one the user chose.
            LOG_WARNING(Service, "Camera: the chosen camera is not connected");
            return false;
        }
        // The camera's own format closest to 1280x720, at 15 fps or more where it has one; SDL
        // only converts its pixels to RGBA.
        SDL_CameraSpec spec{};
        s64 best = std::numeric_limits<s64>::max();
        int format_count = 0;
        SDL_CameraSpec** const specs = SDL_GetCameraSupportedFormats(id, &format_count);
        for (int i = 0; specs != nullptr && i < format_count; ++i) {
            const SDL_CameraSpec& candidate = *specs[i];
            const bool slow = candidate.framerate_denominator <= 0 ||
                              candidate.framerate_numerator < 15 * candidate.framerate_denominator;
            const s64 score = std::abs(s64{candidate.width} * candidate.height - 1280 * 720) +
                              (slow ? (s64{1} << 40) : 0);
            if (candidate.width > 0 && candidate.height > 0 && score < best) {
                best = score;
                spec = candidate;
            }
        }
        SDL_free(specs);
        spec.format = SDL_PIXELFORMAT_RGBA32;
        spec.colorspace = SDL_COLORSPACE_SRGB;
        m_camera = SDL_OpenCamera(id, spec.width > 0 ? &spec : nullptr);
        if (m_camera == nullptr) {
            LOG_WARNING(Service, "Camera: the chosen camera did not open: {}", SDL_GetError());
            return false;
        }
        return true;
    }

    // Takes the camera's next frame if it has one; false when it has none (or went away, which
    // leaves m_camera null).
    bool Poll() {
        const int permission = SDL_GetCameraPermissionState(m_camera);
        if (permission < 0) {
            LOG_WARNING(Service, "Camera: access to the camera was denied");
            SDL_CloseCamera(m_camera);
            m_camera = nullptr;
            return false;
        }
        if (permission == 0) {
            return false;
        }
        Uint64 timestamp = 0;
        SDL_Surface* const frame = SDL_AcquireCameraFrame(m_camera, &timestamp);
        if (frame == nullptr) {
            return false;
        }
        SDL_Surface* const rgba = frame->format == SDL_PIXELFORMAT_RGBA32
                                      ? frame
                                      : SDL_ConvertSurface(frame, SDL_PIXELFORMAT_RGBA32);
        if (rgba != nullptr && rgba->w >= 2 && rgba->h >= 2 && rgba->pixels != nullptr) {
            u32 width = static_cast<u32>(rgba->w);
            u32 height = static_cast<u32>(rgba->h);
            // Keep a 16:9 crop, scaled down if the camera is wider than needed.
            u32 crop_w = width;
            u32 crop_h = height;
            if (u64{width} * 9 > u64{height} * 16) {
                crop_w = height * 16 / 9;
            } else {
                crop_h = width * 9 / 16;
            }
            const u32 out_w = std::min(crop_w, MaxFrameWidth) & ~1U;
            const u32 out_h = std::max<u32>(static_cast<u32>(u64{out_w} * crop_h / crop_w), 2);
            m_buffer.resize(size_t{out_w} * out_h * 4);
            Scale(static_cast<const u8*>(rgba->pixels), static_cast<size_t>(rgba->pitch), width,
                  height, out_w, out_h, false, m_buffer.data());
            Publish(out_w, out_h);
        }
        if (rgba != nullptr && rgba != frame) {
            SDL_DestroySurface(rgba);
        }
        SDL_ReleaseCameraFrame(m_camera, frame);
        return true;
    }

    // Makes m_buffer the newest frame; m_buffer gets the frame before it, to be overwritten.
    void Publish(u32 width, u32 height) {
        std::scoped_lock lk{m_frame_mutex};
        m_frame.swap(m_buffer);
        m_width = width;
        m_height = height;
        ++m_sequence;
    }

    void Close(std::string& open) {
        if (m_camera != nullptr) {
            SDL_CloseCamera(m_camera);
            m_camera = nullptr;
        }
        {
            std::scoped_lock lk{m_frame_mutex};
            Wipe(m_frame);
        }
        Wipe(m_buffer);
        if (!open.empty()) {
            LOG_INFO(Service, "Camera off");
        }
        open.clear();
        m_active = false;
    }

    std::mutex m_life_mutex;
    mutable std::mutex m_mutex;
    std::condition_variable_any m_cv;
    Config m_config;
    u64 m_generation{};
    std::atomic<bool> m_mirror{};
    u32 m_users{};
    std::jthread m_thread;
    std::atomic<bool> m_active{};
    std::atomic<bool> m_failed{}; // the chosen camera failed to open or was denied
    SDL_Camera* m_camera{};       // capture thread only
    std::vector<u8> m_buffer;     // capture thread only: the frame being made

    std::mutex m_presence_mutex;
    std::string m_presence_device;
    Clock::time_point m_presence_checked{};
    bool m_present{};

    mutable std::mutex m_frame_mutex;
    std::vector<u8> m_frame;
    u32 m_width{};
    u32 m_height{};
    u64 m_sequence{};
};

} // namespace

std::vector<std::string> ListDevices() {
    std::vector<std::string> names;
    if (!InitSdlCamera()) {
        return names;
    }
    int count = 0;
    SDL_CameraID* const ids = SDL_GetCameras(&count);
    for (int i = 0; ids != nullptr && i < count; ++i) {
        if (const char* const name = SDL_GetCameraName(ids[i]); name != nullptr && *name != '\0') {
            names.emplace_back(name);
        }
    }
    SDL_free(ids);
    return names;
}

bool Enabled() {
    return Capture::Get().Enabled();
}

bool Available() {
    return Capture::Get().Available();
}

bool IsActive() {
    return Capture::Get().Active();
}

void ApplySettings() {
    Capture::Get().Reconfigure();
}

Stream::Stream() {
    Capture::Get().Acquire();
}

Stream::~Stream() {
    Capture::Get().Release();
}

bool Stream::Read(u32 width, u32 height, u64& seen, std::vector<u8>& out) const {
    return Capture::Get().Read(width, height, seen, out);
}

} // namespace Service::Nx2Camera
