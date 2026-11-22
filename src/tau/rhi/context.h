#pragma once

#include "tau/defines.h"
#include "tau/log.h"
#include "tau/memory/linear_allocator.h"
#include "tau/rhi/device.h"
#include "tau/rhi/renderer_constants.h"
#include "tau/rhi/vulkan.h"

#include <cstdlib>
#include <optional>
#include <string>
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

struct SDL_Window;

// the GPU context: device, queues, pipeline cache, the frames in flight and their timeline
namespace tau::renderer
{
    struct queue_family_indices_t
    {
        std::optional<u32_t> graphics_family;
        std::optional<u32_t> compute_family;
        std::optional<u32_t> transfer_family;
        std::optional<u32_t> present_family;
    };

    // one per frame in flight
    struct frame_slot_t
    {
        // the render timeline value its last submit signals
        u64_t target_timeline_value = 0;
        VkCommandPool command_pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore acquire_semaphore = VK_NULL_HANDLE;
        // reset when the slot is reused
        linear_allocator_t allocator;
    };

    struct context_t
    {
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physical_device = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        // every pipeline is created through it
        VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
        VmaAllocator allocator = VK_NULL_HANDLE;

        VkQueue graphics_queue = VK_NULL_HANDLE;
        VkQueue present_queue = VK_NULL_HANDLE;
        VkQueue transfer_queue = VK_NULL_HANDLE;

        queue_family_indices_t queue_fam_indices;
        VkPhysicalDeviceProperties properties;
        device::device_caps_t caps;

        VkFormat depth_format = VK_FORMAT_UNDEFINED;

        frame_slot_t frames[MAX_FRAMES_IN_FLIGHT];
        u32_t cur_frame = 0;

        VkSemaphore timeline_semaphore = VK_NULL_HANDLE;
        u64_t timeline_value = 0;
    };

    TAU_ENGINE_API extern context_t ctx;

    struct bindless_limits_t
    {
        u32_t max_samplers = MAX_SAMPLERS;
        u32_t max_sampled_textures = MAX_SAMPLED_TEXTURES;
        u32_t max_storage_images = MAX_STORAGE_TEXTURES;
        // grows from here
        u32_t material_heap_size = 1024u * 1024u;
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

    // device, bindless, material heap, transfer, swapchain, frame slots, timeline, pipelines
    bool init_context(const context_config_t& config, SDL_Window* window);
    void shutdown_context();

    // until the GPU finished the slot's previous frame; once per frame
    void wait_for_frame();
    // after the frame's submit
    void advance_frame();

    // saves once pipeline creation went quiet
    void save_pipeline_cache_when_settled();
    // every pipeline creation reports here, so the cache knows there is something to save and the log can show the cost
    TAU_ENGINE_API void note_pipeline_created(f64 milliseconds);

    void transition_image(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                          VkImageAspectFlags aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT);
    TAU_ENGINE_API VkDeviceAddress get_buffer_address(VkBuffer buffer);
} // namespace tau::renderer
