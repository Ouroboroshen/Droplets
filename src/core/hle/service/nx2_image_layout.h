// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <span>

#include "common/alignment.h"
#include "common/common_types.h"

namespace Service {

// Block-linear layout of an InterModuleUserImagePreference (16 words): word 2 holds
// (width - 1) | (height - 1) << 16, word 4 the block height (log2 of GOB rows), word 5 the row
// pitch in 64-byte GOBs. Formula from changes.patch (native_image_layout.h); preferences outside
// it fall back to 4 bytes per pixel and 128-row blocks.
struct Nx2ImageLayout {
    u32 width;
    u32 height;
    u64 pitch;
    u32 block_height_log2;
    u64 size;
    u64 alignment;
};

inline Nx2ImageLayout Nx2CalculateImageLayout(const u32* preference) {
    Nx2ImageLayout layout{};
    layout.width = (preference[2] & 0xFFFF) + 1;
    layout.height = (preference[2] >> 16) + 1;
    const u64 pitch_gobs = preference[5];
    const u32 block_height_log2 = preference[4];
    if (block_height_log2 <= 8 && pitch_gobs >= (u64{layout.width} * 4 + 63) / 64 &&
        pitch_gobs <= 4096) {
        layout.pitch = pitch_gobs * 64;
        layout.block_height_log2 = block_height_log2;
    } else {
        layout.pitch = Common::AlignUp(u64{layout.width} * 4, 64);
        layout.block_height_log2 = 7;
    }
    const u64 block_rows = u64{1} << layout.block_height_log2;
    layout.size = layout.pitch * Common::AlignUp(u64{layout.height}, block_rows);
    layout.alignment = std::max<u64>(64 * block_rows, 0x1000);
    return layout;
}

// gsv 1510 (CalculateRequirements) for one plain block-linear colour image: the same size, but
// page alignment. Star Fox places its 3840x2160 window images 0x1000 into their memory and checks
// that offset against the alignment; with a block-sized (0x2000) one it got no window and called
// through a null window at its first AcquireNativeWindowBuffer. From Lichii (zortos293/Eglantine
// f6a8f51324); other preferences keep the generic layout.
inline Nx2ImageLayout Nx2CalculateNativeImageRequirements(std::span<const u32, 16> preference,
                                                         u32 count) {
    auto layout = Nx2CalculateImageLayout(preference.data());
    if (count != 1 || preference[0] != 0 || preference[1] != 2 || preference[3] != 0 ||
        preference[4] < 3 || preference[4] > 8 || preference[6] != 1 || preference[7] != 1 ||
        layout.pitch != u64{preference[5]} * 64 ||
        std::any_of(preference.begin() + 8, preference.end(), [](u32 value) { return value != 0; })) {
        return layout;
    }
    layout.alignment = 0x1000;
    return layout;
}

} // namespace Service
