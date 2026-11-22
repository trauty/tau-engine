#pragma once

#include "tau/defines.h"
#include "tau/math.h"
#include "tau/rhi/vulkan.h"

#include <vector>

// the window's swapchain; headless keeps only the format
namespace tau::renderer::swapchain
{
    struct swapchain_t
    {
        VkSwapchainKHR handle = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        // physical, rotated with the surface
        VkExtent2D extent = {0, 0};
        // as the window sees it
        VkExtent2D logical_extent = {0, 0};
        VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;

        std::vector<VkImage> images;
        std::vector<VkImageView> views;
        std::vector<VkSemaphore> release_semaphores;
    };

    // vsync off: IMMEDIATE, else MAILBOX, for timing runs; false: nothing created (no surface, minimised)
    bool init(bool vsync);
    bool vsync();
    // takes effect at the next resize
    void set_vsync(bool vsync);
    void shutdown();
    // false: nothing created
    bool resize();
    // after VK_ERROR_SURFACE_LOST_KHR; the next acquire reports out of date
    void recreate_surface();

    // OUT_OF_DATE without a swapchain
    VkResult acquire(VkSemaphore signal, u32_t* index);
    // waits on the image's release semaphore
    VkResult present(u32_t index);

    TAU_ENGINE_API const swapchain_t& state();
    // colour views are pre multiplied by it
    mat4 pre_rotation();
} // namespace tau::renderer::swapchain
