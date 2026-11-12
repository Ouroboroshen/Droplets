// SPDX-FileCopyrightText: Copyright 2026 Droplet
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/logging.h"
#include "common/settings.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/svc_types.h"
#include "core/hle/service/sockets/online_friends.h"
#include "core/hle/service/sockets/online_redirect.h"
#include "core/hle/service/sockets/online_rotation.h"
#include "core/memory.h"

namespace Service::Sockets::OnlineRedirect {

namespace {

// Mario Kart World 1.8.0's main module
constexpr std::array<u8, 16> MainBuild{0xd0, 0x0e, 0x4f, 0x39, 0x9e, 0x41, 0x1d, 0x06,
                                       0x13, 0xaa, 0x8d, 0x26, 0xb1, 0xc0, 0x2f, 0xef};

// Both places send the host's four choices to the lobby (event 0x92a5f285), each a {u32 kind (0: a
// course), u32 id, u32 id}: the vote set-up step's and the vote step's own. A race's choices are
// routes from the lobby's stage to another one and, as "Random", a race on one course; a battle's
// are arenas under its rule. All go by the ids mkw-courses.json gives them (MurmurHash3 of the
// game's names for them).
struct Hook {
    u64 offset;   // in the module
    u32 original; // the instruction the SVC replaces
};
constexpr Hook SetupHook{0x1ce5c64, 0xaa0103f9}; // mov x25, x1
constexpr Hook VoteHook{0x1d07378, 0xf94003ea};  // ldr x10, [sp]
// A Knockout Tour's host draws its rally from a list, in the step that syncs it, and sends it as
// event 0xf7a80a9d: this loads the one drawn
constexpr Hook RallyHook{0x1cfbd50, 0xb9400008}; // ldr w8, [x0]
constexpr std::array<Hook, 3> Hooks{SetupHook, VoteHook, RallyHook};
constexpr u32 SvcInstruction = 0xd4000001 | (CourseRotationSvc << 5);

std::atomic<u64> module_base{0};

enum Mode : size_t { Race, Knockout, Battle };
constexpr std::array<const char*, 3> ModeNames{"race", "knockout", "battle"};

struct ModeRotation {
    std::string kind = "game";           // "game", "pool" or "order"
    std::vector<std::vector<u32>> pick;  // the rotation's courses, each as the ids a vote shows for it
    std::vector<std::vector<u32>> known; // every course of the mode, the same way
    size_t next = 0;                     // order: where this emulator's votes are in `pick`
};

struct Rotation {
    std::array<ModeRotation, 3> modes;
    std::unordered_map<u32, std::pair<u32, u32>> routes; // a route's course id -> (from, to) stage
    std::map<std::pair<u32, u32>, u32> route_between;    // (from, to) stage -> the route's id
    u64 version = 0;
};

// Shared with the fetch threads, which may still run as the emulator exits: never destroyed
struct Shared {
    std::mutex mutex;
    std::optional<Rotation> rotation;
    std::chrono::steady_clock::time_point fetched;
    bool fetching = false;
};
Shared& State() {
    static Shared* const shared = new Shared;
    return *shared;
}

std::optional<Rotation> ParseRotation(const std::string& body, std::string& error) {
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        error = "the server's answer wasn't readable";
        return std::nullopt;
    }
    // A field of another type throws: such an answer is skipped, the emulator keeps running
    try {
        Rotation out;
        out.version = json.value("version", u64{0});
        for (size_t i = 0; i < ModeNames.size(); ++i) {
            const auto& mode = json.at(ModeNames[i]);
            out.modes[i].kind = mode.value("mode", std::string{"game"});
            out.modes[i].pick = mode.value("pick", std::vector<std::vector<u32>>{});
            out.modes[i].known = mode.value("known", std::vector<std::vector<u32>>{});
        }
        const nlohmann::json routes = json.at("race").value("routes", nlohmann::json::object());
        for (const auto& [id, stages] : routes.items()) {
            const auto ends = stages.get<std::array<u32, 2>>();
            out.routes[static_cast<u32>(std::stoul(id))] = {ends[0], ends[1]};
            out.route_between[{ends[0], ends[1]}] = static_cast<u32>(std::stoul(id));
        }
        return out;
    } catch (const std::exception&) {
        error = "the server's course rotations weren't readable";
        return std::nullopt;
    }
}

void Fetch(std::string server) {
    std::string error;
    const std::string body = FetchPublic(server, "/v1/rotation", error);
    std::optional<Rotation> fresh = body.empty() ? std::nullopt : ParseRotation(body, error);
    Shared& state = State();
    std::scoped_lock lock{state.mutex};
    state.fetching = false;
    state.fetched = std::chrono::steady_clock::now();
    if (!fresh) {
        LOG_WARNING(Network, "Course rotations: {}", error);
        return;
    }
    if (state.rotation && state.rotation->version == fresh->version) {
        return; // unchanged: the orders keep their places
    }
    LOG_INFO(Network, "Course rotations from the server (version {}): race {}, Knockout Tour {}, battle {}",
             fresh->version, fresh->modes[Race].kind, fresh->modes[Knockout].kind,
             fresh->modes[Battle].kind);
    state.rotation = std::move(fresh);
}

// Fetches the rotation again, on a thread of its own, when the copy is half a minute old
void Refresh() {
    const std::string server = ServerAddress();
    if (server.empty()) {
        return;
    }
    Shared& state = State();
    std::scoped_lock lock{state.mutex};
    if (state.fetching || (state.fetched != std::chrono::steady_clock::time_point{} &&
                           std::chrono::steady_clock::now() - state.fetched < std::chrono::seconds{30})) {
        return;
    }
    state.fetching = true;
    std::thread{Fetch, server}.detach();
}

std::string Ids(const std::array<u32, 4>& ids) {
    return fmt::format("{}, {}, {}, {}", ids[0], ids[1], ids[2], ids[3]);
}

// A race vote's choices from the rotation's courses (each {its own race, its stage}): a course
// by its route from the lobby's stage when there is one, else by its own race, as the game's
// "Random" choice is
std::optional<std::array<u32, 4>> ChooseRace(const Rotation& r, ModeRotation& m,
                                             std::optional<u32> stage, std::string& why) {
    std::vector<const std::vector<u32>*> list;
    for (const auto& ids : m.pick) {
        if (ids.size() == 2 && ids[0] != 0) {
            list.push_back(&ids);
        }
    }
    if (m.kind == "game" || list.empty()) {
        why = "the server's rotation is the game's own choices";
        return std::nullopt;
    }
    const auto offer = [&](const std::vector<u32>& course) {
        if (stage) {
            if (const auto it = r.route_between.find({*stage, course[1]}); it != r.route_between.end()) {
                return it->second;
            }
        }
        return course[0];
    };
    std::array<u32, 4> out{};
    if (m.kind == "order") {
        // The course after the lobby's stage in the list, else after this emulator's last vote
        size_t at = m.next % list.size();
        for (size_t i = 0; stage && i < list.size(); ++i) {
            if ((*list[(m.next + i) % list.size()])[1] == *stage) {
                at = (m.next + i + 1) % list.size();
                break;
            }
        }
        out.fill(offer(*list[at]));
        m.next = at + 1;
        return out;
    }
    std::vector<const std::vector<u32>*> pool;
    for (const auto* course : list) {
        if (std::none_of(pool.begin(), pool.end(), [&](const auto* c) { return (*c)[1] == (*course)[1]; })) {
            pool.push_back(course);
        }
    }
    static std::mt19937 random{std::random_device{}()};
    std::shuffle(pool.begin(), pool.end(), random);
    for (size_t i = 0; i < 3; ++i) {
        out[i] = offer(*pool[i % pool.size()]);
    }
    out[3] = offer(*pool[std::uniform_int_distribution<size_t>{0, pool.size() - 1}(random)]); // "Random"
    return out;
}

// The choices the rotation gives a vote the game made `offered`, or none to leave the game's
// (`why`); `mode` says whose rotation it was
std::optional<std::array<u32, 4>> Choose(Rotation& r, const std::array<u32, 4>& offered,
                                         size_t& mode, std::string& why) {
    // A race vote: routes from the lobby's stage, whose start tells where the lobby is
    std::optional<u32> stage;
    bool race = false;
    for (const u32 id : offered) {
        if (const auto it = r.routes.find(id); it != r.routes.end()) {
            stage = it->second.first;
            race = true;
            break;
        }
        race = race || std::any_of(r.modes[Race].known.begin(), r.modes[Race].known.end(),
                                   [id](const auto& ids) { return !ids.empty() && ids[0] == id; });
    }
    if (race) {
        mode = Race;
        return ChooseRace(r, r.modes[Race], stage, why);
    }
    for (mode = Knockout; mode < r.modes.size(); ++mode) {
        ModeRotation& m = r.modes[mode];
        // The mode whose courses the game offered, and which of each course's ids it shows (an
        // arena has one per battle rule)
        std::optional<size_t> column;
        for (const u32 id : offered) {
            for (const auto& ids : m.known) {
                if (const auto it = std::find(ids.begin(), ids.end(), id); id != 0 && it != ids.end()) {
                    column = static_cast<size_t>(it - ids.begin());
                    break;
                }
            }
            if (column) {
                break;
            }
        }
        if (!column) {
            continue;
        }
        std::vector<u32> list;
        for (const auto& ids : m.pick) {
            if (*column < ids.size() && ids[*column] != 0) {
                list.push_back(ids[*column]);
            }
        }
        if (m.kind == "game" || list.empty()) {
            why = "the server's rotation is the game's own choices";
            return std::nullopt;
        }
        std::array<u32, 4> out{};
        if (m.kind == "order") {
            const size_t at = m.next % list.size();
            out.fill(list[at]);
            m.next = at + 1;
            return out;
        }
        std::vector<u32> pool;
        for (const u32 id : list) {
            if (std::find(pool.begin(), pool.end(), id) == pool.end()) {
                pool.push_back(id);
            }
        }
        static std::mt19937 random{std::random_device{}()};
        std::shuffle(pool.begin(), pool.end(), random);
        for (size_t i = 0; i < 3; ++i) {
            out[i] = pool[i % pool.size()];
        }
        out[3] = pool[std::uniform_int_distribution<size_t>{0, pool.size() - 1}(random)]; // "Random"
        return out;
    }
    why = "not a course list the server knows";
    return std::nullopt;
}

// The rally a Knockout Tour's host plays instead of the one it drew
u32 RallyFor(u32 drawn) {
    Refresh();
    Shared& state = State();
    std::scoped_lock lock{state.mutex};
    if (!state.rotation) {
        LOG_INFO(Network, "Knockout Tour: the game draws {}; no rotations from the server yet", drawn);
        return drawn;
    }
    size_t mode = 0;
    std::string why;
    const auto chosen = Choose(*state.rotation, {drawn, drawn, drawn, drawn}, mode, why);
    if (!chosen || mode != Knockout) {
        LOG_INFO(Network, "Knockout Tour: the game draws {}; {}", drawn,
                 chosen ? "not a rally the server knows" : why);
        return drawn;
    }
    LOG_INFO(Network, "Knockout Tour: the game draws {}; the server's {} makes it {}", drawn,
             state.rotation->modes[mode].kind, (*chosen)[0]);
    return (*chosen)[0];
}

} // namespace

void InstallCourseRotation(std::span<u8> image, const std::array<u8, 0x20>& build_id, VAddr base) {
    if (!std::equal(MainBuild.begin(), MainBuild.end(), build_id.begin()) ||
        std::any_of(build_id.begin() + MainBuild.size(), build_id.end(), [](u8 b) { return b != 0; })) {
        return;
    }
    if (Settings::IsNceEnabled()) {
        LOG_INFO(Loader, "Mario Kart World: no course rotations with native code execution (NCE)");
        return;
    }
    for (const Hook& hook : Hooks) {
        u32 word = 0;
        if (hook.offset + sizeof(word) <= image.size()) {
            std::memcpy(&word, image.data() + hook.offset, sizeof(word));
        }
        if (word != hook.original) {
            LOG_WARNING(Loader, "Mario Kart World: unexpected code at +{:#x}; no course rotations",
                        hook.offset);
            return;
        }
    }
    for (const Hook& hook : Hooks) {
        std::memcpy(image.data() + hook.offset, &SvcInstruction, sizeof(SvcInstruction));
    }
    module_base = base;
    LOG_INFO(Loader, "Mario Kart World: course rotation hooks at {:#x}, {:#x} and {:#x}",
             base + SetupHook.offset, base + VoteHook.offset, base + RallyHook.offset);
    Refresh();
}

void RunCourseRotationHook(Core::System& system, Kernel::KProcess& process) {
    auto* cpu = process.GetArmInterface(system.Kernel().CurrentPhysicalCoreIndex());
    Kernel::Svc::ThreadContext ctx{};
    cpu->GetContext(ctx);
    auto& memory = process.GetMemory();
    const u64 at = ctx.pc - 4 - module_base.load();
    std::array<u64, 4> choices{};
    if (at == SetupHook.offset) {
        ctx.r[25] = ctx.r[1];
        choices = {ctx.r[2], ctx.r[4], ctx.r[6], ctx.r[28]};
    } else if (at == VoteHook.offset) {
        ctx.r[10] = memory.Read64(ctx.sp);
        choices = {ctx.r[2], ctx.r[4], ctx.r[6], ctx.r[10]};
    } else if (at == RallyHook.offset) {
        ctx.r[8] = RallyFor(memory.Read32(ctx.r[0]));
        cpu->SetContext(ctx);
        return;
    } else {
        LOG_CRITICAL(Network, "Course rotations: SVC {:#x} at {:#x} is no hook of theirs",
                     CourseRotationSvc, ctx.pc - 4);
        return;
    }
    cpu->SetContext(ctx);

    std::array<u32, 4> offered{};
    for (size_t i = 0; i < choices.size(); ++i) {
        if (!memory.IsValidVirtualAddress(choices[i] + 8)) {
            return;
        }
        offered[i] = memory.Read32(choices[i] + 4);
    }
    Refresh();
    Shared& state = State();
    std::scoped_lock lock{state.mutex};
    if (!state.rotation) {
        LOG_INFO(Network, "Course vote: the game offers {}; no rotations from the server yet", Ids(offered));
        return;
    }
    size_t mode = 0;
    std::string why;
    const auto chosen = Choose(*state.rotation, offered, mode, why);
    if (!chosen) {
        LOG_INFO(Network, "Course vote: the game offers {}; {}", Ids(offered), why);
        return;
    }
    for (size_t i = 0; i < choices.size(); ++i) {
        memory.Write32(choices[i], 0);
        memory.Write32(choices[i] + 4, (*chosen)[i]);
        memory.Write32(choices[i] + 8, (*chosen)[i]);
    }
    LOG_INFO(Network, "Course vote ({}): the game offers {}; the server's {} makes it {}",
             ModeNames[mode], Ids(offered), state.rotation->modes[mode].kind, Ids(*chosen));
}

} // namespace Service::Sockets::OnlineRedirect
