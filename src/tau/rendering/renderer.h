#pragma once

#include "tau/assets/material.h"
#include "tau/defines.h"
#include "tau/ecs_fwd.h"
#include "tau/rendering/renderer_types.h"
#include "tau/rendering/rendergraph.h"
#include "tau/window.h"

#include <span>
#include <vector>

namespace tau::renderer
{
    using graph_builder_func_t = void (*)(rendergraph_t&, ecs::registry_t&);

    TAU_ENGINE_API mesh_pass_e pass_for(const shader_t& shader);

    TAU_ENGINE_API VkPipeline binding_pipeline(const shader_t& shader);

    // capturing: a pass is handed the resources of its own view, not a lookup by name
    // two colour views own separately named gbuffers
    using pass_execute_func_t = tau::arena_function<void(rendergraph_t&, tau::material_t&)>;

    TAU_ENGINE_API void register_renderer_feature(u32_t name_hash, i32 order, graph_builder_func_t builder);

    // features whose builder is code in that module (see os::module_base_of)
    // a game library's go when it is unloaded and it registers them again from TAU_ON_LOAD
    TAU_ENGINE_API u32_t remove_features_of(void* module_base);
    TAU_ENGINE_API u32_t count_features_of(void* module_base);

    TAU_ENGINE_API rg_pass_t& add_mesh_pass(rendergraph_t& graph, u32_t name_hash, const char* debug_name,
                                            u32_t view_name_hash, mesh_pass_e pass, bool depth_only,
                                            const std::vector<rg_resource_id>& reads,
                                            const std::vector<rg_resource_id>& writes, rg_resource_id depth);

    // reads and writes are spans, copied into the pass at once, vectors cost a temporary per call per frame
    TAU_ENGINE_API rg_pass_t& add_fullscreen_pass(rendergraph_t& graph, u32_t name_hash, const char* debug_name,
                                                  tau::material_t* material, std::span<const rg_resource_id> reads,
                                                  std::span<const rg_resource_id> writes,
                                                  pass_execute_func_t on_execute = {}, u32_t view_name_hash = 0,
                                                  rg_resource_id depth_tex_res = RG_NULL_ID);

    TAU_ENGINE_API bool bind_view_descriptors(VkCommandBuffer cmd, VkPipelineLayout layout, u32_t view_name_hash);

    // every culled object in the view, one pipeline, fills object_buffer
    TAU_ENGINE_API void draw_view_objects(VkCommandBuffer cmd, u32_t view_name_hash, VkPipeline pipeline,
                                          VkPipelineLayout layout, push_constants_t pc);

    TAU_ENGINE_API rg_pass_t& add_compute_pass(rendergraph_t& graph, u32_t name_hash, const char* debug_name,
                                               tau::material_t* material, u32_t dispatch_x, u32_t dispatch_y,
                                               u32_t dispatch_z, const std::vector<rg_resource_id>& reads,
                                               const std::vector<rg_resource_id>& writes,
                                               pass_execute_func_t on_execute = {}, u32_t view_name_hash = 0);

    bool init(const context_config_t& config, SDL_Window* window);
    void reset_assets();
    void shutdown();

    TAU_ENGINE_API extern render_context_t ctx;

    // waits for the frame slot, acquires the swapchain image (vsync wait under FIFO)
    // called before input so the frame shows post-wait input
    void begin_frame();
    void render(ecs::registry_t& reg);

    void init_per_frame(per_frame_t& frame_data);
    void shutdown_per_frame(per_frame_t& frame_data);

    bool resize(const u32_t width, const u32_t height);
    void init_swapchain();
    VkResult acquire_next_image(u32_t* image);
    VkFormat find_depth_format();
    VkSurfaceFormatKHR select_surface_format(VkPhysicalDevice physical_device, VkSurfaceKHR surface,
                                             std::vector<VkFormat> const& preferred_formats = {
                                                 VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB,
                                                 VK_FORMAT_A8B8G8R8_SRGB_PACK32});

    void transition_image(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                          VkImageAspectFlags aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT);

    void create_buffer(VkDeviceSize size, VkBufferUsageFlags buffer_usage, VmaMemoryUsage mem_usage, VkBuffer& buffer,
                       VmaAllocation& allocation);
    TAU_ENGINE_API VkDeviceAddress get_buffer_address(VkBuffer buffer);
    void create_image(u32 width, u32 height, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage,
                      VkMemoryPropertyFlags props, VkImage& image, VkDeviceMemory& image_mem);

    TAU_ENGINE_API VkCommandBuffer begin_transfer_commands();
    TAU_ENGINE_API u64_t submit_transfer_commands(VkCommandBuffer cmd);

    VkSampler create_sampler(VkFilter filter, VkSamplerAddressMode address_mode);
    VkSampler create_shadow_sampler();

    TAU_ENGINE_API void submit_output_target(u32_t target_hash, VkExtent2D extent);

    TAU_ENGINE_API u32_t get_target_texture_id(u32_t name_hash);

    TAU_ENGINE_API void submit_color_view(u32_t name_hash, const mat4_t& view, const mat4_t& projection,
                                          const mat4_t& view_proj, const vec3_t& position, u32_t color_target_hash,
                                          u32_t depth_target_hash, VkExtent2D extent, f32 z_near = 0.0f,
                                          f32 z_far = 0.0f);

    TAU_ENGINE_API void set_view_debug_view(u32_t view_name_hash, debug_view_e debug_view);

    TAU_ENGINE_API void set_view_post_processing(u32_t view_name_hash, bool enabled);

    TAU_ENGINE_API void set_fallback_light(bool enabled, const vec3_t& direction = {0.0f, -1.0f, 0.0f},
                                           const vec3_t& color = {1.0f, 1.0f, 1.0f}, f32 intensity = 1.0f);

    TAU_ENGINE_API void submit_shadow_cascade_view(u32_t name_hash, const mat4_t& view, const mat4_t& projection,
                                                   u32_t atlas_target_hash, const image_desc_t& atlas_desc,
                                                   VkRect2D atlas_rect, f32 split_far, f32 normal_bias_texels = 1.5f,
                                                   f32 depth_bias_ndc = 0.0f);

    TAU_ENGINE_API void submit_punctual_shadow_view(u32_t name_hash, const mat4_t& view, const mat4_t& projection,
                                                    const image_desc_t& atlas_desc, VkRect2D atlas_rect,
                                                    u32_t shadow_slot, f32 range, bool needs_render = true,
                                                    f32 normal_bias_texels = 2.0f, f32 depth_bias_ndc = 0.0f);

    TAU_ENGINE_API void submit_shadow_cascade_light(ecs::entity_t light);

    TAU_ENGINE_API void submit_punctual_shadow_slot(ecs::entity_t light, u32_t shadow_slot);

    TAU_ENGINE_API bool get_primary_camera_position(ecs::registry_t& reg, vec3_t& out_pos);

    struct primary_view_info_t
    {
        vec3_t position;
        vec3_t forward;
        f32 fov_rad;
        f32 aspect;
        f32 z_near;
        f32 z_far;

        // inward facing planes
        vec4_t frustum_planes[6];
        bool has_frustum_planes = false;
    };

    TAU_ENGINE_API bool get_primary_view_info(ecs::registry_t& reg, primary_view_info_t& out_info);

    TAU_ENGINE_API bool get_primary_depth_bounds(f32& out_near_z, f32& out_far_z);

    // a missing asset is reported once, not once per frame
    // reloading anything clears that, so breaking the same file twice tells you twice
    TAU_ENGINE_API void forget_missing_asset_reports();

    // every pipeline creation reports here, so the cache knows there is something to save and the log can show the cost
    TAU_ENGINE_API void note_pipeline_created(f64 milliseconds);

    // the render timeline value a GPU resource freed now must outlive
    // submitted frames are at or below ctx.timeline_value and the one being recorded signals the next
    // so once it completes nothing in flight refers to it
    // the render timeline on purpose, res_system's is the transfer one and says nothing about frames binding the thing
    TAU_ENGINE_API u64_t deletion_timeline_value();
} // namespace tau::renderer