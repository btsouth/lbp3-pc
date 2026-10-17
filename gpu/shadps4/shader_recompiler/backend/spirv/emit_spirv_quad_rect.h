// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>
#include "common/types.h"

namespace Shader {
struct HwFragmentRuntimeInfo;
}

namespace Shader::Backend::SPIRV {

enum class AuxShaderType : u32 {
    RectListTCS,
    QuadListTCS,
    PassthroughTES,
};

/// `vs_outputs`: bit N set when the vertex shader writes the parameter at location N. The TCS reads
/// only those; a fragment input the vertex shader does not write is passed on as zero (bbport:
/// reading it was an interface mismatch, VUID-RuntimeSpirv-OpEntryPoint-08743).
[[nodiscard]] std::vector<u32> EmitAuxilaryTessShader(AuxShaderType type,
                                                      const HwFragmentRuntimeInfo& fs_info,
                                                      u64 vs_outputs = ~0ull);

} // namespace Shader::Backend::SPIRV
