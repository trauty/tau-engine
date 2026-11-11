#pragma once

#include "tau/defines.h"
#include "tau/rendering/shader_shared.h"

namespace tau::renderer
{
    constexpr u32_t MAX_SAMPLED_TEXTURES = 100000;
    constexpr u32_t MAX_STORAGE_TEXTURES = 4096;
    constexpr u32_t MAX_SAMPLERS = 32;

    constexpr u32_t MAX_MATERIAL_COUNT = 4096;
    constexpr u32_t MAX_MATERIAL_BUFFER_SIZE = MAX_MATERIAL_COUNT * 512;

    constexpr u32_t BINDLESS_NULL_HANDLE = 0xffffffff;

    constexpr u32_t MAX_FRAMES_IN_FLIGHT = 2;

    // color views
    constexpr u32_t MAX_VIEWS_PER_FRAME = 32;

    constexpr i32 FEATURE_ORDER_AFTER_POST = 1000;
    constexpr i32 FEATURE_ORDER_OVERLAY = FEATURE_ORDER_AFTER_POST + 10;
    constexpr i32 FEATURE_ORDER_UI = FEATURE_ORDER_AFTER_POST + 100;

    // what a mesh, material or texture that failed to load is drawn with instead
    inline constexpr const char* FALLBACK_TEXTURE_PATH = "engine://assets/textures/fallback_tex.png";
    inline constexpr const char* FALLBACK_MESH_PATH = "engine://assets/models/fallback_mesh.glb";
    inline constexpr const char* FALLBACK_MATERIAL_PATH = "engine://assets/materials/fallback.mat";

    // shadows
    constexpr u32_t SHADOW_CASCADE_COUNT = 4;
    constexpr u32_t SHADOW_CASCADE_TILE_DIM = 2048;
    // cascades share one atlas as a 2x2 grid of tiles
    constexpr u32_t SHADOW_ATLAS_DIM = SHADOW_CASCADE_TILE_DIM * 2;

    constexpr u32_t PUNCTUAL_SHADOW_ATLAS_DIM = 2048;
    constexpr u32_t PUNCTUAL_SHADOW_TILE_DIM = 512;
    constexpr u32_t PUNCTUAL_SHADOW_TILES_PER_ROW = PUNCTUAL_SHADOW_ATLAS_DIM / PUNCTUAL_SHADOW_TILE_DIM;
    constexpr u32_t MAX_PUNCTUAL_SHADOW_TILES = PUNCTUAL_SHADOW_TILES_PER_ROW * PUNCTUAL_SHADOW_TILES_PER_ROW;
    constexpr u32_t POINT_SHADOW_FACE_COUNT = 6;

    constexpr u32_t MAX_LIGHTS = 1024;
    constexpr u32_t MAX_DIR_LIGHTS = 32;

    // clustering
    constexpr u32_t CLUSTER_TILE_SIZE_PX = 64;
    constexpr u32_t MAX_CLUSTER_TILES_X = 32;
    constexpr u32_t MAX_CLUSTER_TILES_Y = 32;
    constexpr u32_t CLUSTER_SLICES_Z = TAU_CLUSTER_SLICES_Z;

    constexpr u32_t cluster_tiles_for(u32_t extent_px, u32_t max_tiles)
    {
        const u32_t tiles = (extent_px + CLUSTER_TILE_SIZE_PX - 1) / CLUSTER_TILE_SIZE_PX;
        return (tiles < 1) ? 1 : ((tiles > max_tiles) ? max_tiles : tiles);
    }

    constexpr u32_t MAX_LIGHTS_PER_CLUSTER = TAU_MAX_LIGHTS_PER_CLUSTER;
} // namespace tau::renderer
