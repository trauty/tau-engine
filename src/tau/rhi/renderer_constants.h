#pragma once

#include "tau/defines.h"
#include "tau/rhi/shader_shared.h"

namespace tau::renderer
{
    constexpr u32_t MAX_SAMPLED_TEXTURES = 100000;
    constexpr u32_t MAX_STORAGE_TEXTURES = 4096;
    constexpr u32_t MAX_SAMPLERS = 32;

    constexpr u32_t BINDLESS_NULL_HANDLE = 0xffffffff;

    constexpr u32_t MAX_FRAMES_IN_FLIGHT = 2;

    // color views
    constexpr u32_t MAX_VIEWS_PER_FRAME = 32;

    constexpr i32 FEATURE_ORDER_AFTER_POST = 1000;
    constexpr i32 FEATURE_ORDER_OVERLAY = FEATURE_ORDER_AFTER_POST + 10;
    constexpr i32 FEATURE_ORDER_UI = FEATURE_ORDER_AFTER_POST + 100;

    // what a mesh, material or texture that failed to load is drawn with instead
    inline constexpr const char* FALLBACK_TEXTURE_PATH = "engine://assets/textures/fallback_tex.png";
    inline constexpr const char* FALLBACK_MESH_PATH = "engine://assets/models/fallback_mesh.glb/Cube.002.mesh";
    inline constexpr const char* DEFAULT_MATERIAL_PATH = "engine://assets/materials/default.mat";
    inline constexpr const char* FALLBACK_MATERIAL_PATH = "engine://assets/materials/fallback.mat";

} // namespace tau::renderer
