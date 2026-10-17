// SPDX-License-Identifier: GPL-2.0-or-later
// Ratchet & Clank (2016), 1.00: what the translator knows about its frame. No hooks in its code;
// the shaders and images below were read from frame captures (BB_CAPTURE_TRIGGER) of the EU
// release, whose targets sit at the same addresses every session.
#include "game_profile.h"

namespace Game {
namespace {
constexpr const char* Serials[] = {"CUSA01073"};

/// The TAA resolve (compute 4595ddd5, 240x135 groups of 8x8) reads the scene color, the motion
/// vectors at the nearest-depth pixel, linear depth and its history, and writes new history;
/// compute 17f7e64a then copies that into the scene color before bloom and tonemapping.
/// Its constants: [24..27] render size and its reciprocal (1920x1080), [38..39] this frame's
/// jitter in pixels (Halton 2/3 minus 0.5). Its history lookup, (pixel + 0.5) - motion, shows the
/// motion vectors are pixels, current minus previous; on still geometry they are about 0.01 px,
/// so they carry no jitter. The linear depth is 1/w from the motion vector pass (sky: 1000).
constexpr SceneAa RatchetTaa = {
    .taa_shader = 0x4595ddd5,
    .copy_shader = 0x17f7e64a,
    .color = 0x1084e88000,
    .motion = 0x1086e60000,
    .linear_depth = 0x1084690000,
    .far = 1000.0f,
    .jitter_dword = 38,
};
} // namespace

extern const Profile RatchetProfile = {
    .name = "Ratchet & Clank",
    .serials = Serials,
    .scene_aa = &RatchetTaa,
};

} // namespace Game
