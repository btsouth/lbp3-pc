// SPDX-License-Identifier: GPL-2.0-or-later
// AmdGpu::Scissor sizes: an inverted rectangle is empty instead of wrapping to ~4 billion.
#include <cassert>
#include <cstdio>
#include "video_core/amdgpu/regs_primitive.h"

int main() {
    using AmdGpu::Scissor;
    Scissor normal{{10, 20}, {110, 70}};
    assert(normal.GetWidth() == 100 && normal.GetHeight() == 50);
    // Seen in Ratchet & Clank: top left 1604 past bottom right 1598.
    Scissor inverted{{1604, 16899}, {1598, 1795}};
    assert(inverted.GetWidth() == 0 && inverted.GetHeight() == 0);
    // Negative corners clamp to 0 first.
    Scissor negative{{-8, -4}, {16, 8}};
    assert(negative.GetWidth() == 16 && negative.GetHeight() == 8);
    Scissor all_negative{{-8, -8}, {-2, -1}};
    assert(all_negative.GetWidth() == 0 && all_negative.GetHeight() == 0);
    std::printf("scissor-test: ok\n");
    return 0;
}
