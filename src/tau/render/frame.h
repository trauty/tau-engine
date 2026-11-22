#pragma once

#include "tau/asset.h"
#include "tau/defines.h"
#include "tau/ecs_fwd.h"
#include "tau/math.h"
#include "tau/rhi/gpu_structs.h"
#include "tau/rhi/graph.h"
#include "tau/rhi/shader_shared.h"

namespace tau
{
    struct texture_t;
    struct mesh_t;
    struct material_t;
} // namespace tau

// the frame: views, the frame loop, the per view globals, then the graph v2 part (post and every pass producer)
namespace tau::renderer::frame
{
#define TAU_DEBUG_VIEW_ENUMERATOR(name, value, label) name = value,

    // the same list drives the shader's constants and the editor's dropdown
    enum class debug_view_e : u32_t
    {
        TAU_DEBUG_VIEW_LIST(TAU_DEBUG_VIEW_ENUMERATOR)
    };

#undef TAU_DEBUG_VIEW_ENUMERATOR

    struct render_view_t
    {
        u32_t name_hash = 0;
        mat4 view;
        mat4 projection;
        mat4 view_proj;
        vec3 position;
        u32_t color_target_hash = 0;
        u32_t depth_target_hash = 0;
        VkExtent2D extent{};

        f32 z_near = 0.0f;
        f32 z_far = 0.0f;
    };

    struct output_target_t
    {
        u32_t target_hash = 0;
        VkExtent2D extent{};
    };

    // after init_context
    void init();
    void reset_assets();
    void shutdown();

    // waits for the frame slot, acquires the swapchain image (vsync wait under FIFO)
    // called before input so the frame shows post-wait input
    void begin();
    void render(ecs::registry_t& reg);
    void on_window_resized();

    // the frame's output size; logical: as the window sees it
    TAU_ENGINE_API VkExtent2D render_extent();
    TAU_ENGINE_API VkExtent2D logical_extent();

    // views, submitted for the next render()
    TAU_ENGINE_API void submit_output_target(u32_t target_hash, VkExtent2D extent);
    // the output target's sampled id from the last frame
    TAU_ENGINE_API u32_t get_target_texture_id(u32_t name_hash);

    TAU_ENGINE_API void submit_color_view(u32_t name_hash, const mat4& view, const mat4& projection,
                                          const mat4& view_proj, const vec3& position, u32_t color_target_hash,
                                          u32_t depth_target_hash, VkExtent2D extent, f32 z_near = 0.0f,
                                          f32 z_far = 0.0f);
    TAU_ENGINE_API void set_view_debug_view(u32_t view_name_hash, debug_view_e debug_view);
    TAU_ENGINE_API void set_view_post_processing(u32_t view_name_hash, bool enabled);

    // the submitted primary view, else the active camera
    TAU_ENGINE_API bool get_primary_camera_position(ecs::registry_t& reg, vec3& out_pos);

    // per view globals (set 1, dynamic offset) of this frame's view; false: no such view
    TAU_ENGINE_API bool bind_view_descriptors(VkCommandBuffer cmd, VkPipelineLayout layout, u32_t view_name_hash);
    // 0: the primary view; a missing view: the frame's first
    TAU_ENGINE_API u32_t view_globals_offset(u32_t view_name_hash);
    TAU_ENGINE_API u32_t first_view_globals_offset();

    // what a slot that was never bound gets, and what a failed load gets instead of nothing
    struct fallbacks_t
    {
        asset_t<texture_t> default_tex;
        asset_t<texture_t> texture;
        asset_t<mesh_t> mesh;
        asset_t<material_t> material;
        asset_t<material_t> default_material;
    };
    const fallbacks_t& fallbacks();

    struct targets_t
    {
        graph::image_h scene_color;
        graph::image_h scene_depth;
        graph::image_h final_output;
        // invalid when headless
        graph::image_h swapchain;
        bool final_is_swapchain = false;
        u32_t primary_view_hash = 0;
    };

    using producer_fn_t = void (*)(graph::graph_t& g, const targets_t& t, ecs::registry_t& reg);

    // order: below FEATURE_ORDER_AFTER_POST runs before post, the rest after; same name replaces
    TAU_ENGINE_API void register_producer(u32_t name_hash, i32 order, producer_fn_t fn);
    // producers whose code is in that module (game library unload)
    TAU_ENGINE_API u32_t remove_producers_of(void* module_base);
    TAU_ENGINE_API u32_t count_producers_of(void* module_base);

} // namespace tau::renderer::frame
