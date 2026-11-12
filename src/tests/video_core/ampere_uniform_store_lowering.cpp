// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

TEST_CASE("Ampere STS with a uniform base lowers", "[video_core][ampere]") {
    // @!P1 STS [UR9+0x130], R32 from Pokemon Pokopia's photo compute shaders.
    const std::array<Instruction, 2> code{{{{0xff009988, 0x00013020, 0x08000809, 0x000fe800}}, Exit}};
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    // @!P1 IADD R250, RZ, R225 (UR9's register), then @!P1 STS [R250+0x130], R32.
    const auto& out = lowered.instructions;
    REQUIRE(std::find(out.begin(), out.end(), 0x5c1000000e19fffaULL) != out.end());
    REQUIRE(std::find(out.begin(), out.end(), 0xef5c00001309fa20ULL) != out.end());
    // Without bit 91, bits 64-71 aren't a uniform base: still rejected.
    const Instruction plain{{0xff009988, 0x00013020, 0x00000809, 0x000fe800}};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{plain, Exit})));
}

TEST_CASE("Ampere UMOV immediate writes uniform registers past UR15", "[video_core][ampere]") {
    // UMOV UR17, 0x20 writes UR17's register, R233.
    const std::array<Instruction, 2> code{{{{0x00117882, 0x20, 0, 0x000fe200}}, Exit}};
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.instructions.front() == 0x010000000207f0e9ULL);
}
} // namespace
