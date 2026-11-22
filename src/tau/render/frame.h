#pragma once

#include "tau/asset.h"
#include "tau/defines.h"
#include "tau/ecs_fwd.h"
#include "tau/math.h"
#include "tau/render/legacy_graph.h"
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

    enum class view_kind_e : u32_t
    {
        COLOR,
        DEPTH_ONLY
    };

    enum class shadow_tile_e : u32_t
    {
        CASCADE,
        PUNCTUAL
    };

    struct render_view_t
    {
        u32_t name_hash = 0;
        view_kind_e kind = view_kind_e::COLOR;
        mat4 view;
        mat4 projection;
        mat4 view_proj;
        vec3 position;
        u32_t color_target_hash = 0;
        u32_t depth_target_hash = 0;
        bool has_depth_desc = false;
        image_desc_t depth_desc{}; // used to create the shadowmap image
        VkExtent2D extent{};

        shadow_tile_e shadow_tile = shadow_tile_e::CASCADE;
        bool needs_render = true;
        u32_t shadow_slot = 0;         // index into the punctual tile buffer, punctual views only
        VkRect2D shadow_atlas_rect{};  // tile this view owns inside its atlas
        f32 shadow_split_far = 0.0f;   // view space distance where this cascade stops
        f32 shadow_normal_bias = 1.5f; // in shadow texels
        f32 shadow_depth_bias = 0.0f;  // in light space ndc depth

        f32 z_near = 0.0f;
        f32 z_far = 0.0f;
    };

    struct output_target_t
    {
        u32_t target_hash = 0;
        VkExtent2D extent{};
    };

    struct primary_view_info_t
    {
        vec3 position;
        vec3 forward;
        f32 fov_rad;
        f32 aspect;
        f32 z_near;
        f32 z_far;

        // inward facing planes
        vec4 frustum_planes[6];
        bool has_frustum_planes = false;
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

    TAU_ENGINE_API void submit_shadow_cascade_view(u32_t name_hash, const mat4& view, const mat4& projection,
                                                   u32_t atlas_target_hash, const image_desc_t& atlas_desc,
                                                   VkRect2D atlas_rect, f32 split_far, f32 normal_bias_texels = 1.5f,
                                                   f32 depth_bias_ndc = 0.0f);
    TAU_ENGINE_API void submit_punctual_shadow_view(u32_t name_hash, const mat4& view, const mat4& projection,
                                                    const image_desc_t& atlas_desc, VkRect2D atlas_rect,
                                                    u32_t shadow_slot, f32 range, bool needs_render = true,
                                                    f32 normal_bias_texels = 2.0f, f32 depth_bias_ndc = 0.0f);
    TAU_ENGINE_API void submit_shadow_cascade_light(ecs::entity_t light);
    TAU_ENGINE_API void submit_punctual_shadow_slot(ecs::entity_t light, u32_t shadow_slot);

    // the submitted primary view, else the active camera
    TAU_ENGINE_API bool get_primary_camera_position(ecs::registry_t& reg, vec3& out_pos);
    TAU_ENGINE_API bool get_primary_view_info(ecs::registry_t& reg, primary_view_info_t& out_info);

    // per view globals (set 1, dynamic offset) of this frame's view; false: no such view
    TAU_ENGINE_API bool bind_view_descriptors(VkCommandBuffer cmd, VkPipelineLayout layout, u32_t view_name_hash);
    // 0: the primary view; a missing view: the frame's first
    TAU_ENGINE_API u32_t view_globals_offset(u32_t view_name_hash);
    TAU_ENGINE_API u32_t first_view_globals_offset();

    // legacy block: globals of the view at index, the camera part filled from the view
    gpu::GlobalData* view_globals(u32_t view_index);
    u32_t view_globals_offset_at(u32_t view_index);
    void fill_view_camera(gpu::GlobalData& globals, const render_view_t& view);

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

    // images the legacy block owns, imported by name
    struct legacy_image_t
    {
        const char* name = nullptr;
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent = {};
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        u32_t sampled_id = graph::NONE;
    };

    struct inputs_t
    {
        static constexpr u32_t MAX_LEGACY = 8;
        legacy_image_t legacy[MAX_LEGACY];
        u32_t legacy_count = 0;

        // swapchain image, or the output target named by hash
        VkImage swapchain = VK_NULL_HANDLE;
        VkImageView swapchain_view = VK_NULL_HANDLE;
        bool to_swapchain = false;
        u32_t output_hash = 0;
        VkExtent2D output_extent = {};

        u32_t post_view_hash = 0;
        vec3 post_view_pos = {};
        bool post_enabled = true;
    };
} // namespace tau::renderer::frame
