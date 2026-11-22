#pragma once

#include "tau/defines.h"
#include "tau/ecs_fwd.h"
#include "tau/math.h"
#include "tau/rhi/graph.h"

// the frame's graph v2 part: what the legacy block hands over, then post and every pass producer
namespace tau::renderer::frame
{
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

    void init(void* tracy_vk_ctx);
    void shutdown();

    // after the legacy block executed; fills ctx.output_texture_ids
    void record(VkCommandBuffer cmd, ecs::registry_t& reg, const inputs_t& in);
} // namespace tau::renderer::frame
