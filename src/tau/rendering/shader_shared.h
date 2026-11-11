#pragma once

// definitions the renderer and its shaders both need, stated once
// included from C++ and Slang under the same path, the cooker passes the engine's header root as an include dir
// preprocessor defines only, the subset both languages agree on, each side wraps them in its own idiom
// Slang expands macros only where used, so the X macro lists may carry C++ only text like VkFormat names

// clustering
#define TAU_CLUSTER_SLICES_Z 24
#define TAU_MAX_LIGHTS_PER_CLUSTER 64

// bindless sampler slots: indices into the global sampler array
// C++ builds the samplers in this order, shaders ask for them by the same number
#define TAU_SAMPLER_LINEAR_REPEAT 0
#define TAU_SAMPLER_LINEAR_CLAMP 1
#define TAU_SAMPLER_NEAREST_REPEAT 2
#define TAU_SAMPLER_NEAREST_CLAMP 3
#define TAU_SAMPLER_SHADOW_CMP 4
#define TAU_SAMPLER_COUNT 5

// debug views: one list, three readers (the C++ enum, the deferred lighting constants, the editor's dropdown)
// the label is only ever expanded by C++
#define TAU_DEBUG_VIEW_LIST(X)                                                                                         \
    X(OFF, 0, "Shaded")                                                                                                \
    X(ALBEDO, 1, "Albedo")                                                                                             \
    X(NORMAL, 2, "Normal")                                                                                             \
    X(ROUGHNESS, 3, "Roughness")                                                                                       \
    X(METALLIC, 4, "Metallic")                                                                                         \
    X(DEPTH, 5, "Depth")                                                                                               \
    X(CLUSTERS, 6, "Clusters")                                                                                         \
    X(SHADOW_CASCADES, 7, "Cascades")                                                                                  \
    X(SHADOW_TERM, 8, "ShadowTerm")                                                                                    \
    X(WORLD_POS, 9, "WorldPos")

// render target format aliases accepted by [RenderTarget("...")], the cooker expands this into its alias lookup
#define TAU_RENDER_TARGET_FORMATS(X)                                                                                   \
    X(Swapchain, VK_FORMAT_UNDEFINED)                                                                                  \
    X(RGBA8_SRGB, VK_FORMAT_R8G8B8A8_SRGB)                                                                             \
    X(RGBA8_UNORM, VK_FORMAT_R8G8B8A8_UNORM)                                                                           \
    X(RGBA16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT)                                                                     \
    X(RG16_FLOAT, VK_FORMAT_R16G16_SFLOAT)                                                                             \
    X(R8_UNORM, VK_FORMAT_R8_UNORM)                                                                                    \
    X(R32_UINT, VK_FORMAT_R32_UINT)
