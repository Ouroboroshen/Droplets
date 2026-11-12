// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include <opus.h>
#include <opus_multistream.h>

#include "common/logging.h"
#include "common/swap.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/service/audio/errors.h"
#include "core/hle/service/audio/nx2_hardware_audio_codec.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/memory.h"

namespace Service::Audio {

namespace {

using LibOpusDecoder = ::OpusDecoder;
using LibOpusMSDecoder = ::OpusMSDecoder;

// The work buffer sizes reported to the game. It sets that much aside for the DSP's own state,
// which Droplet keeps on the host, so any size works.
constexpr u32 ManagerWorkBufferSize = 0x1000;
constexpr u32 InstanceWorkBufferSize = 0x1000;

constexpr u32 MaxInstances = 256;
constexpr u32 MaxIoBuffers = 1024;
constexpr u32 MaxIoBufferSize = 0x100000;
constexpr size_t MaxPendingTasks = 4 * MaxIoBuffers;
constexpr u32 MaxLoggedProblems = 8;
// When the DSP answers a batch: hardware takes a fraction of a millisecond.
constexpr std::chrono::nanoseconds ProcessDelay{std::chrono::microseconds{500}};

enum class Codec : u64 {
    Stub = 0, // copies its input
    Opus = 1,
    OpusMultiStream = 2,
};

// A decoder's parameters (InstanceParameter).
struct InstanceParameter {
    Codec codec;
    s32 sample_rate;
    s32 channel_count;
    s64 frame_duration_ns; // nn::TimeSpan
    s32 total_stream_count; // multistream only
    s32 stereo_stream_count;
    std::array<u8, 0x100> channel_mapping;
};
static_assert(sizeof(InstanceParameter) == 0x120);

// Where the game put things in "region B", the memory it shares with the DSP: the I/O buffers,
// each an input half and then an output half, then the control block with the result ring.
struct SharedMemoryParameter {
    u32 version;
    std::array<u32, 7> dsp_memory_layout; // region A, the DSP's own memory (unused here)
    u32 io_area_size;
    u32 io_area_offset;
    u32 control_area_size;
    u32 control_area_offset;
    u32 unknown_30;
    u32 instance_size;
    u32 instance_count;
    u32 input_buffer_size;
    u32 output_buffer_size;
    u32 io_buffer_count;
    std::array<u32, 6> reserved;
};
static_assert(sizeof(SharedMemoryParameter) == 0x60);

// The client session's command 10 raw arguments; each scalar takes an 8-byte slot.
struct OpenManagerSessionParameters {
    SharedMemoryParameter shared_memory;
    u64 region_a_size; // sent as transfer memory, unused here
    u64 region_b_address;
    u64 region_b_size;
    u64 properties;
};
static_assert(sizeof(OpenManagerSessionParameters) == 0x80);

// A queued task: decode the packet in an I/O buffer's input half into its output half.
struct Task {
    u16 client_id;
    u16 instance;
    u16 io_buffer;
    u16 serial;
    u32 input_size;
    u8 reset; // reset the decoder first
    u8 last;
    u16 padding;
};
static_assert(sizeof(Task) == 0x10);

// What the DSP writes back for a task, in the result ring.
struct TaskResult {
    u16 client_id;
    u16 instance;
    u16 io_buffer;
    u16 serial;
    s64 processing_time_ns;
    u32 output_size;
    u32 result;
    std::array<u8, 0x28> reserved;
};
static_assert(sizeof(TaskResult) == 0x40);

// The control block: the DSP's write index into the result ring, the game's read index, the ring.
constexpr u64 ResultWriteIndexOffset = 0x4;
constexpr u64 ResultReadIndexOffset = 0x44;
constexpr u64 ResultRingOffset = 0x80;

// Each packet starts with a big-endian header: payload size, then the encoder's final range.
constexpr u32 OpusPacketHeaderSize = 8;

bool IsOpusSampleRate(s32 rate) {
    return rate == 8000 || rate == 12000 || rate == 16000 || rate == 24000 || rate == 48000;
}

bool IsUsableLayout(const OpenManagerSessionParameters& p, Core::Memory::Memory& memory) {
    const SharedMemoryParameter& l = p.shared_memory;
    if (l.instance_count == 0 || l.instance_count > MaxInstances || l.io_buffer_count == 0 ||
        l.io_buffer_count > MaxIoBuffers || l.input_buffer_size > MaxIoBufferSize ||
        l.output_buffer_size > MaxIoBufferSize || p.region_b_size == 0) {
        return false;
    }
    const u64 io_end = u64{l.io_area_offset} +
                       u64{l.io_buffer_count} * (u64{l.input_buffer_size} + l.output_buffer_size);
    const u64 control_end = u64{l.control_area_offset} + ResultRingOffset +
                            (u64{l.io_buffer_count} + 1) * sizeof(TaskResult);
    return io_end <= p.region_b_size && control_end <= p.region_b_size &&
           memory.IsValidVirtualAddressRange(p.region_b_address, p.region_b_size);
}

class Decoder {
public:
    // nullptr for parameters no decoder takes.
    static std::unique_ptr<Decoder> Create(const InstanceParameter& p) {
        if (p.sample_rate <= 0 || p.channel_count <= 0 || p.frame_duration_ns <= 0 ||
            p.frame_duration_ns > 120'000'000) {
            return nullptr;
        }
        auto decoder = std::unique_ptr<Decoder>(new Decoder);
        decoder->codec = p.codec;
        decoder->channels = static_cast<u32>(p.channel_count);
        const u64 frame_samples = (static_cast<u64>(p.frame_duration_ns) *
                                       static_cast<u64>(p.sample_rate) +
                                   999'999'999) /
                                  1'000'000'000;
        decoder->frame_bytes = frame_samples * decoder->channels * sizeof(s16);
        int error = OPUS_OK;
        switch (p.codec) {
        case Codec::Stub:
            break;
        case Codec::Opus:
            if (!IsOpusSampleRate(p.sample_rate) || p.channel_count > 2) {
                return nullptr;
            }
            decoder->single = opus_decoder_create(p.sample_rate, p.channel_count, &error);
            break;
        case Codec::OpusMultiStream:
            if (!IsOpusSampleRate(p.sample_rate) || p.channel_count > 255 ||
                p.total_stream_count <= 0 || p.stereo_stream_count < 0 ||
                p.stereo_stream_count > p.total_stream_count ||
                p.total_stream_count + p.stereo_stream_count > 255) {
                return nullptr;
            }
            decoder->multi = opus_multistream_decoder_create(
                p.sample_rate, p.channel_count, p.total_stream_count, p.stereo_stream_count,
                p.channel_mapping.data(), &error);
            break;
        default:
            return nullptr;
        }
        if (error != OPUS_OK) {
            return nullptr;
        }
        return decoder;
    }

    ~Decoder() {
        if (single != nullptr) {
            opus_decoder_destroy(single);
        }
        if (multi != nullptr) {
            opus_multistream_decoder_destroy(multi);
        }
    }

    void Reset() {
        if (single != nullptr) {
            opus_decoder_ctl(single, OPUS_RESET_STATE);
        }
        if (multi != nullptr) {
            opus_multistream_decoder_ctl(multi, OPUS_RESET_STATE);
        }
    }

    // Decodes one framed packet into `output`: the bytes written, or nothing for a packet
    // libopus refuses.
    std::optional<u32> Decode(std::span<const u8> input, std::span<u8> output) {
        if (codec == Codec::Stub) {
            const size_t size = std::min(input.size(), output.size());
            std::memcpy(output.data(), input.data(), size);
            return static_cast<u32>(size);
        }
        if (input.size() < OpusPacketHeaderSize) {
            return std::nullopt;
        }
        u32 payload_size;
        std::memcpy(&payload_size, input.data(), sizeof(payload_size));
        payload_size = Common::swap32(payload_size);
        if (payload_size > input.size() - OpusPacketHeaderSize) {
            return std::nullopt;
        }
        const u8* const payload = input.data() + OpusPacketHeaderSize;
        const int max_samples = static_cast<int>(output.size() / (channels * sizeof(s16)));
        pcm.resize(static_cast<size_t>(max_samples) * channels);
        const auto size = static_cast<opus_int32>(payload_size);
        const int samples =
            single != nullptr
                ? opus_decode(single, payload, size, pcm.data(), max_samples, 0)
                : opus_multistream_decode(multi, payload, size, pcm.data(), max_samples, 0);
        if (samples < 0) {
            return std::nullopt;
        }
        const u32 bytes = static_cast<u32>(samples) * channels * sizeof(s16);
        std::memcpy(output.data(), pcm.data(), bytes);
        return bytes;
    }

    u64 FrameBytes() const {
        return frame_bytes;
    }

private:
    Decoder() = default;

    Codec codec{};
    u32 channels{};
    u64 frame_bytes{};
    LibOpusDecoder* single{};
    LibOpusMSDecoder* multi{};
    std::vector<opus_int16> pcm;
};

} // namespace

// One decoder manager: its decoders, and the tasks the game queued for the DSP.
class Nx2HardwareAudioCodecManager {
public:
    explicit Nx2HardwareAudioCodecManager(Core::System& system_,
                                          const OpenManagerSessionParameters& parameters,
                                          u16 client_id_)
        : system{system_}, service_context{system_, "nx2:hwacod"},
          layout{parameters.shared_memory}, region_b{parameters.region_b_address},
          client_id{client_id_}, event{service_context.CreateEvent("nx2:hwacod:ProcessEvent")},
          decoders(parameters.shared_memory.instance_count) {}

    ~Nx2HardwareAudioCodecManager() {
        service_context.CloseEvent(event);
    }

    Kernel::KReadableEvent& GetEvent() {
        return event->GetReadableEvent();
    }

    Result InitializeInstance(u32 index, const InstanceParameter& parameter) {
        std::scoped_lock lk{mutex};
        if (index >= decoders.size()) {
            return ResultLibOpusBadArg;
        }
        auto decoder = Decoder::Create(parameter);
        if (!decoder) {
            return ResultLibOpusBadArg;
        }
        decoders[index] = std::move(decoder);
        R_SUCCEED();
    }

    void FinalizeInstance(u32 index) {
        std::scoped_lock lk{mutex};
        if (index < decoders.size()) {
            decoders[index].reset();
        }
    }

    void Submit(std::span<const Task> tasks) {
        std::scoped_lock lk{mutex};
        for (const Task& task : tasks) {
            if (pending.size() >= MaxPendingTasks) {
                LogProblem("more tasks queued than the game has I/O buffers; dropping one");
                continue;
            }
            pending.push_back(task);
        }
    }

    // Runs the queued tasks, writes their results into the ring and signals the game.
    void Process() {
        {
            std::scoped_lock lk{mutex};
            if (pending.empty()) {
                return;
            }
            auto& memory = system.ApplicationMemory();
            const u64 control = region_b + layout.control_area_offset;
            const u32 ring_size = layout.io_buffer_count + 1;
            while (!pending.empty()) {
                const Task task = pending.front();
                pending.pop_front();
                const u32 next = (write_index + 1) % ring_size;
                if (next == memory.Read32(control + ResultReadIndexOffset) % ring_size) {
                    LogProblem("the result ring is full; dropping a result");
                    continue;
                }
                const TaskResult result = Run(task, memory);
                memory.WriteBlock(control + ResultRingOffset + u64{write_index} * sizeof(TaskResult),
                                  &result, sizeof(result));
                write_index = next;
                memory.Write32(control + ResultWriteIndexOffset, write_index);
            }
        }
        event->Signal(system.Kernel());
    }

private:
    TaskResult Run(const Task& task, Core::Memory::Memory& memory) {
        TaskResult result{
            .client_id = task.client_id,
            .instance = task.instance,
            .io_buffer = task.io_buffer,
            .serial = task.serial,
        };
        Decoder* const decoder =
            task.instance < decoders.size() ? decoders[task.instance].get() : nullptr;
        if (decoder == nullptr || task.io_buffer >= layout.io_buffer_count ||
            task.input_size > layout.input_buffer_size) {
            LogProblem("a task for an unknown decoder or I/O buffer; it returns nothing");
            return result;
        }
        const auto start = std::chrono::steady_clock::now();
        const u64 input_address =
            region_b + layout.io_area_offset +
            u64{task.io_buffer} * (u64{layout.input_buffer_size} + layout.output_buffer_size);
        const u64 output_address = input_address + layout.input_buffer_size;
        input.resize(task.input_size);
        memory.ReadBlock(input_address, input.data(), input.size());
        output.resize(layout.output_buffer_size);
        if (task.reset != 0) {
            decoder->Reset();
        }
        std::optional<u32> bytes = decoder->Decode(input, output);
        if (!bytes) {
            // Silence rather than an error result: the SDK aborts on results it doesn't expect.
            LogProblem("a packet libopus refuses; it plays as silence");
            bytes = static_cast<u32>(std::min<u64>(decoder->FrameBytes(), output.size()));
            std::fill_n(output.begin(), *bytes, u8{0});
        }
        memory.WriteBlock(output_address, output.data(), *bytes);
        result.output_size = *bytes;
        result.processing_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - start)
                                        .count();
        return result;
    }

    void LogProblem(const char* what) {
        if (logged_problems < MaxLoggedProblems) {
            ++logged_problems;
            LOG_WARNING(Service_Audio, "nx2:hwacod client {}: {}", client_id, what);
        }
    }

    Core::System& system;
    KernelHelpers::ServiceContext service_context;
    const SharedMemoryParameter layout;
    const u64 region_b;
    const u16 client_id;
    Kernel::KEvent* event;

    std::mutex mutex;
    std::vector<std::unique_ptr<Decoder>> decoders;
    std::deque<Task> pending;
    u32 write_index{};
    u32 logged_problems{};
    std::vector<u8> input;
    std::vector<u8> output;
};

INx2HardwareAudioCodecManagerSession::INx2HardwareAudioCodecManagerSession(
    Core::System& system_, std::shared_ptr<Nx2HardwareAudioCodecManager> manager_)
    : ServiceFramework{system_, "nx2:hwacod:IHardwareAudioCodecManagerSession"},
      manager{std::move(manager_)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &INx2HardwareAudioCodecManagerSession::InitializeInstance, "InitializeInstance"},
        {1, &INx2HardwareAudioCodecManagerSession::FinalizeInstance, "FinalizeInstance"},
        {10, &INx2HardwareAudioCodecManagerSession::Execute, "Execute"},
    };
    // clang-format on
    RegisterHandlers(functions);
    process_event = Core::Timing::CreateEvent(
        "nx2:hwacod process",
        [weak = std::weak_ptr{manager}](s64, std::chrono::nanoseconds)
            -> std::optional<std::chrono::nanoseconds> {
            if (const auto locked = weak.lock()) {
                locked->Process();
            }
            return std::nullopt;
        });
}

INx2HardwareAudioCodecManagerSession::~INx2HardwareAudioCodecManagerSession() {
    system.CoreTiming().UnscheduleEvent(process_event);
}

void INx2HardwareAudioCodecManagerSession::InitializeInstance(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u32 index = rp.Pop<u32>();
    const auto buffer = ctx.ReadBuffer();
    InstanceParameter parameter{};
    std::memcpy(&parameter, buffer.data(), std::min(buffer.size(), sizeof(parameter)));
    const Result result = buffer.size() < sizeof(parameter)
                              ? ResultLibOpusBadArg
                              : manager->InitializeInstance(index, parameter);
    LOG_INFO(Service_Audio,
             "nx2:hwacod decoder {}: codec {}, {} Hz, {} channels, {} us frames, {} streams{}",
             index, static_cast<u64>(parameter.codec), parameter.sample_rate,
             parameter.channel_count, parameter.frame_duration_ns / 1000,
             parameter.total_stream_count, result.IsSuccess() ? "" : " (refused)");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

void INx2HardwareAudioCodecManagerSession::FinalizeInstance(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    manager->FinalizeInstance(rp.Pop<u32>());
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void INx2HardwareAudioCodecManagerSession::Execute(HLERequestContext& ctx) {
    const auto buffer = ctx.ReadBuffer();
    std::vector<Task> tasks(buffer.size() / sizeof(Task));
    std::memcpy(tasks.data(), buffer.data(), tasks.size() * sizeof(Task));
    manager->Submit(tasks);
    system.CoreTiming().ScheduleEvent(ProcessDelay, process_event);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

INx2HardwareAudioCodecClientSession::INx2HardwareAudioCodecClientSession(Core::System& system_)
    : ServiceFramework{system_, "nx2:hwacod:IClientSession"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &INx2HardwareAudioCodecClientSession::GetInstanceWorkBufferSize, "GetInstanceWorkBufferSize"},
        {1, &INx2HardwareAudioCodecClientSession::GetManagerWorkBufferSize, "GetManagerWorkBufferSize"},
        {10, &INx2HardwareAudioCodecClientSession::OpenManagerSession, "OpenManagerSession"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

INx2HardwareAudioCodecClientSession::~INx2HardwareAudioCodecClientSession() = default;

void INx2HardwareAudioCodecClientSession::GetInstanceWorkBufferSize(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(InstanceWorkBufferSize);
}

void INx2HardwareAudioCodecClientSession::GetManagerWorkBufferSize(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(ManagerWorkBufferSize);
}

void INx2HardwareAudioCodecClientSession::OpenManagerSession(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto parameters = rp.PopRaw<OpenManagerSessionParameters>();
    const SharedMemoryParameter& l = parameters.shared_memory;
    LOG_INFO(Service_Audio,
             "nx2:hwacod manager: {} decoders, {} I/O buffers of {:#x}+{:#x} bytes, shared memory "
             "{:#x}+{:#x} (I/O at {:#x}, control at {:#x}), layout version {}",
             l.instance_count, l.io_buffer_count, l.input_buffer_size, l.output_buffer_size,
             parameters.region_b_address, parameters.region_b_size, l.io_area_offset,
             l.control_area_offset, l.version);
    if (!IsUsableLayout(parameters, system.ApplicationMemory())) {
        LOG_ERROR(Service_Audio, "nx2:hwacod: that layout doesn't fit its shared memory");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultLibOpusBadArg);
        return;
    }
    static std::atomic<u16> next_client_id{1};
    const u16 client_id = next_client_id++;
    auto manager = std::make_shared<Nx2HardwareAudioCodecManager>(system, parameters, client_id);
    IPC::ResponseBuilder rb{ctx, 3, 1, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<INx2HardwareAudioCodecManagerSession>(
        ctx, std::make_shared<INx2HardwareAudioCodecManagerSession>(system, manager));
    rb.Push<u32>(client_id);
    rb.PushCopyObjects(ctx, manager->GetEvent());
}

} // namespace Service::Audio
