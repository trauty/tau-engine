#pragma once

#include "tau/asset.h"
#include "tau/assets/material.h"
#include "tau/assets/mesh.h"
#include "tau/assets/shader.h"
#include "tau/assets/texture.h"
#include "tau/containers/flat_map.h"
#include "tau/defines.h"
#include "tau/log.h"
#include "tau/math.h"
#include "tau/memory/linear_allocator.h"
#include "tau/rendering/device.h"
#include "tau/rendering/gpu_structs.h"
#include "tau/rendering/renderer_constants.h"
#include "tau/rendering/shader_instance.h"
#include "tau/rendering/shader_shared.h"
#include "tau/rendering/vulkan.h"
#include "vulkan/vulkan_core.h"

#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#define VK_CHECK(x)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        VkResult err = x;                                                                                              \
        if (err)                                                                                                       \
        {                                                                                                              \
            TAU_LOG_FATAL("VULKAN", "Error: {}", (int)err);                                                            \
            abort();                                                                                                   \
        }                                                                                                              \
    } while (0)

namespace tau::renderer
{
    enum class mesh_pass_e : u32_t
    {
        GBUFFER,        // opaque, implements the surface interface, deferred
        FORWARD_OPAQUE, // opaque, writes its own color
        BLENDED,        // anything that blends, always forward
        COUNT,
    };

    constexpr u32_t MESH_PASS_COUNT = static_cast<u32_t>(mesh_pass_e::COUNT);

    struct active_pipeline_t
    {
        VkPipeline pipeline;
        VkPipeline shadow_pipeline;
        VkPipeline gbuffer_pipeline;
        VkPipelineLayout layout;
        u32_t pipeline_index;
        mesh_pass_e pass;
        u32_t first_object = 0;
        u32_t object_count = 0;
    };

    struct pipeline_range_t
    {
        u32_t begin = 0;
        u32_t end = 0;
    };

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

    struct queue_family_indices_t
    {
        std::optional<u32_t> graphics_family;
        std::optional<u32_t> compute_family;
        std::optional<u32_t> transfer_family;
        std::optional<u32_t> present_family;
    };

    struct swapchain_t
    {
        VkSwapchainKHR handle = VK_NULL_HANDLE;
        VkFormat format;
        VkExtent2D extent;

        std::vector<VkImage> images;
        std::vector<VkImageView> views;
        std::vector<VkSemaphore> release_semaphores;

        VkImage depth_image = VK_NULL_HANDLE;
        VkImageView depth_view = VK_NULL_HANDLE;
        VmaAllocation depth_allocation = VK_NULL_HANDLE;
        VkFormat depth_format;
    };

    struct image_desc_t
    {
        u32_t width;
        u32_t height;
        VkFormat format;
        VkImageUsageFlags usage;
        VkImageAspectFlags aspect;
        f32 depth_clear = 0.0f;
        VkClearColorValue color_clear = {
            {0.01f, 0.01f, 0.01f, 1.0f}
        };

        bool operator==(const image_desc_t& other) const
        {
            return width == other.width && height == other.height && format == other.format && usage == other.usage &&
                   aspect == other.aspect;
        }
    };

    struct transient_image_t
    {
        image_desc_t desc;
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        u32_t bindless_id = BINDLESS_NULL_HANDLE;
        bool in_use = false;
        u32_t frames_unused = 0;
        const char* debug_name = nullptr;
    };

    struct persistent_image_t
    {
        image_desc_t desc{};
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        u32_t bindless_id = BINDLESS_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        bool needs_full_clear = true;
    };

    struct transient_pool_t
    {
        std::deque<transient_image_t> images;

        transient_image_t* acquire(const image_desc_t& desc);
        void reset();
        void shutdown();
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

    struct view_gpu_resources_t
    {
        VkBuffer indirect_buffer = VK_NULL_HANDLE;
        VmaAllocation indirect_alloc = VK_NULL_HANDLE;
        VkDeviceSize indirect_size = 0;

        VkBuffer draw_counts_buffer = VK_NULL_HANDLE;
        VmaAllocation draw_counts_alloc = VK_NULL_HANDLE;
        VkDeviceSize draw_counts_size = 0;

        u32_t cluster_count = 0;

        VkBuffer cluster_counts_buffer = VK_NULL_HANDLE;
        VmaAllocation cluster_counts_alloc = VK_NULL_HANDLE;
        VkDeviceAddress cluster_counts_address = 0;

        VkBuffer cluster_indices_buffer = VK_NULL_HANDLE;
        VmaAllocation cluster_indices_alloc = VK_NULL_HANDLE;
        VkDeviceAddress cluster_indices_address = 0;

        shader_instance_t culling_instance;
        shader_instance_t cluster_instance;

        u32_t global_data_offset = 0;
        view_kind_e kind = view_kind_e::COLOR;
    };

    struct per_frame_t
    {
        per_frame_t() = default;
        per_frame_t(const per_frame_t&) = delete;
        per_frame_t& operator=(const per_frame_t&) = delete;
        per_frame_t(per_frame_t&&) = default;
        per_frame_t& operator=(per_frame_t&&) = default;

        u64_t target_timeline_value = 0;
        VkCommandPool main_command_pool = VK_NULL_HANDLE;
        VkCommandBuffer main_command_buffer = VK_NULL_HANDLE;
        VkSemaphore swapchain_acquire_semaphore = VK_NULL_HANDLE;

        gpu::GlobalData* mapped_global_data = nullptr;
        u32_t global_data_offset = 0;

        VkBuffer object_buffer = VK_NULL_HANDLE;
        VmaAllocation object_allocation = VK_NULL_HANDLE;
        gpu::ObjectData* mapped_object_data = nullptr;
        VkDeviceAddress object_buffer_address = 0;

        VkBuffer cull_buffer = VK_NULL_HANDLE;
        VmaAllocation cull_allocation = VK_NULL_HANDLE;
        gpu::CullData* mapped_cull_data = nullptr;

        u32_t object_counter = 0;

        std::vector<active_pipeline_t> active_pipelines;
        pipeline_range_t pass_ranges[MESH_PASS_COUNT];
        flat_map_t<std::unique_ptr<view_gpu_resources_t>> views;

        VkBuffer material_buffer = VK_NULL_HANDLE;
        VmaAllocation material_allocation = VK_NULL_HANDLE;
        u8* mapped_material_data = nullptr;

        transient_pool_t transient_pool;

        VkBuffer dir_light_buffer = VK_NULL_HANDLE;
        VmaAllocation dir_light_allocation = VK_NULL_HANDLE;
        gpu::DirectionalLight* mapped_dir_lights = nullptr;
        VkDeviceAddress dir_light_buffer_address = 0;

        VkBuffer point_light_buffer = VK_NULL_HANDLE;
        VmaAllocation point_light_allocation = VK_NULL_HANDLE;
        gpu::PointLight* mapped_point_lights = nullptr;
        VkDeviceAddress point_light_buffer_address = 0;

        VkBuffer spot_light_buffer = VK_NULL_HANDLE;
        VmaAllocation spot_light_allocation = VK_NULL_HANDLE;
        gpu::SpotLight* mapped_spot_lights = nullptr;
        VkDeviceAddress spot_light_buffer_address = 0;

        VkBuffer shadow_cascade_buffer = VK_NULL_HANDLE;
        VmaAllocation shadow_cascade_allocation = VK_NULL_HANDLE;
        gpu::ShadowTile* mapped_shadow_cascades = nullptr;
        VkDeviceAddress shadow_cascade_buffer_address = 0;

        VkBuffer punctual_shadow_buffer = VK_NULL_HANDLE;
        VmaAllocation punctual_shadow_allocation = VK_NULL_HANDLE;
        gpu::ShadowTile* mapped_punctual_shadows = nullptr;
        VkDeviceAddress punctual_shadow_buffer_address = 0;

        VkBuffer depth_bounds_buffer = VK_NULL_HANDLE;
        VmaAllocation depth_bounds_allocation = VK_NULL_HANDLE;
        u32_t* mapped_depth_bounds = nullptr;
        shader_instance_t depth_reduce_instance;

        f32 depth_bounds_z_near = 0.0f;
        bool depth_bounds_pending = false;

        tau::linear_allocator_t frame_allocator;
    };

    struct view_depth_bounds_t
    {
        f32 near_z = 0.0f;
        f32 far_z = 0.0f;
        bool valid = false;
    };

    struct render_context_t
    {
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physical_device = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        VkPipelineCache pipeline_cache = VK_NULL_HANDLE; // every pipeline is created through it
        VkSurfaceKHR surface = VK_NULL_HANDLE;

        swapchain_t swapchain;

        VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;

        VmaAllocator allocator = VK_NULL_HANDLE;

        VkQueue graphics_queue = VK_NULL_HANDLE;
        VkQueue present_queue = VK_NULL_HANDLE;
        VkQueue transfer_queue = VK_NULL_HANDLE;

        queue_family_indices_t queue_fam_indices;
        VkPhysicalDeviceProperties properties;
        device::device_caps_t caps;

        VkCommandPool transfer_command_pool = VK_NULL_HANDLE;
        std::mutex transfer_mutex;

        std::mutex descriptor_mutex;

        std::vector<per_frame_t> per_frame_objects;
        u32_t cur_frame = 0;

        VkSemaphore timeline_semaphore = VK_NULL_HANDLE;
        u64_t timeline_value = 0;

        std::vector<VkSampler> samplers;

        asset_t<shader_t> culling_shader;

        asset_t<shader_t> cluster_shader;

        asset_t<shader_t> depth_reduce_shader;

        persistent_image_t punctual_shadow_atlas;
        view_depth_bounds_t primary_depth_bounds;

        asset_t<material_t> uberpost_material;
        asset_t<material_t> deferred_lighting_material;

        // what a slot that was never bound gets
        asset_t<texture_t> default_tex;

        // and what a mesh, material or texture that failed to load gets instead of nothing
        asset_t<texture_t> fallback_tex;
        asset_t<mesh_t> fallback_mesh;
        asset_t<material_t> fallback_material;
        asset_t<material_t> default_material;

        u32_t global_data_aligned_size = 0;
        VkBuffer global_data_buffer = VK_NULL_HANDLE;
        VmaAllocation global_data_alloc = VK_NULL_HANDLE;
        u8* global_data_mapped = nullptr;

        VkExtent2D render_extent = {0, 0};
        VkExtent2D logical_extent = {0, 0};
        VkSurfaceTransformFlagBitsKHR cur_surface_transform;

        flat_map_t<u32_t> output_texture_ids;

        std::unordered_map<u64_t, std::unique_ptr<tau::material_t>> resolved_post_effects;
    };

    struct bindless_limits_t
    {
        u32_t max_samplers = MAX_SAMPLERS;
        u32_t max_sampled_textures = MAX_SAMPLED_TEXTURES;
        u32_t max_storage_images = MAX_STORAGE_TEXTURES;
        u32_t material_heap_size = 24u * 1024u * 1024u;
    };

    struct context_config_t
    {
        std::string app_name = "tau-engine";
        // + sync validation
        bool validation = false;
        // no RT, no int16
        bool force_compat = false;
        // no surface / swapchain
        bool headless = false;
        bool pipeline_properties = false;
        std::vector<const char*> required_instance_exts;
        bindless_limits_t bindless_limits = {};
        bool vsync = true;
    };
} // namespace tau::renderer