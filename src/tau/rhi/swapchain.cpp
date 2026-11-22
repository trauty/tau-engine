#include "swapchain.h"

#include "tau/rhi/context.h"
#include "tau/rhi/deletion.h"
#include "tau/window.h"

#include <SDL3/SDL_vulkan.h>
#include <algorithm>

namespace tau::renderer::swapchain
{
    namespace
    {
        swapchain_t g_sc;
        bool g_vsync = true;

        VkPresentModeKHR select_present_mode(VkPhysicalDevice physical_device, VkSurfaceKHR surface, bool want_vsync)
        {
            if (want_vsync) { return VK_PRESENT_MODE_FIFO_KHR; }

            u32_t count = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &count, nullptr);
            std::vector<VkPresentModeKHR> modes(count);
            vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &count, modes.data());

            for (const VkPresentModeKHR wanted : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR})
            {
                if (std::ranges::find(modes, wanted) != modes.end())
                {
                    TAU_LOG_INFO("VULKAN", "Uncapped present mode: {}",
                                 wanted == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : "MAILBOX");
                    return wanted;
                }
            }
            TAU_LOG_WARN("VULKAN", "No uncapped present mode, staying with vsync");
            return VK_PRESENT_MODE_FIFO_KHR;
        }

        VkSurfaceFormatKHR select_surface_format(VkPhysicalDevice physical_device, VkSurfaceKHR surface,
                                                 std::vector<VkFormat> const& preferred_formats = {
                                                     VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB,
                                                     VK_FORMAT_A8B8G8R8_SRGB_PACK32})
        {
            u32_t surface_format_count = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &surface_format_count, nullptr);
            std::vector<VkSurfaceFormatKHR> supported_surface_formats(surface_format_count);
            vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &surface_format_count,
                                                 supported_surface_formats.data());

            auto it = std::ranges::find_if(supported_surface_formats,
                                           [&preferred_formats](VkSurfaceFormatKHR surface_format)
                                           {
                                               return std::ranges::any_of(preferred_formats,
                                                                          [&surface_format](VkFormat format)
                                                                          { return format == surface_format.format; });
                                           });

            return it != supported_surface_formats.end() ? *it : supported_surface_formats[0];
        }

        bool init_swapchain()
        {
            if (ctx.surface == VK_NULL_HANDLE) { return false; }

            VkSurfaceCapabilitiesKHR surface_caps;
            vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.physical_device, ctx.surface, &surface_caps);

            if (surface_caps.currentExtent.width == 0 || surface_caps.currentExtent.height == 0) { return false; }

            VkExtent2D logical_extent;
            if (surface_caps.currentExtent.width != UINT32_MAX) { logical_extent = surface_caps.currentExtent; }
            else
            {
                i32 w, h;
                tau::window::get_window_size(&w, &h);

                logical_extent.width = std::clamp(static_cast<u32_t>(w), surface_caps.minImageExtent.width,
                                                  surface_caps.maxImageExtent.width);
                logical_extent.height = std::clamp(static_cast<u32_t>(h), surface_caps.minImageExtent.height,
                                                   surface_caps.maxImageExtent.height);
            }

            VkSurfaceTransformFlagBitsKHR pre_transform = surface_caps.currentTransform;
            VkExtent2D physical_extent = logical_extent;

            if (pre_transform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR))
            {
                physical_extent.width = logical_extent.height;
                physical_extent.height = logical_extent.width;
            }

            g_sc.transform = pre_transform;
            g_sc.extent = physical_extent;

            g_sc.logical_extent = logical_extent;

            VkSurfaceFormatKHR surface_format = select_surface_format(ctx.physical_device, ctx.surface);
            const VkPresentModeKHR present_mode = select_present_mode(ctx.physical_device, ctx.surface, g_vsync);

            u32_t desired_swapchain_images = surface_caps.minImageCount + 1;
            if ((surface_caps.maxImageCount > 0) && (desired_swapchain_images > surface_caps.maxImageCount))
            {
                desired_swapchain_images = surface_caps.maxImageCount;
            }

            VkSwapchainKHR old_swapchain = g_sc.handle;

            VkCompositeAlphaFlagBitsKHR composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            if (surface_caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
            {
                composite = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
            }
            else if (surface_caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
            {
                composite = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
            }
            else if (surface_caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR)
            {
                composite = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
            }

            VkSwapchainCreateInfoKHR swapchain_info = {
                .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                .surface = ctx.surface,
                .minImageCount = desired_swapchain_images,
                .imageFormat = surface_format.format,
                .imageColorSpace = surface_format.colorSpace,
                .imageExtent = physical_extent,
                .imageArrayLayers = 1,
                .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .preTransform = pre_transform,
                .compositeAlpha = composite,
                .presentMode = present_mode,
                .clipped = true,
                .oldSwapchain = old_swapchain,
            };

            VK_CHECK(vkCreateSwapchainKHR(ctx.device, &swapchain_info, nullptr, &g_sc.handle));

            if (old_swapchain != VK_NULL_HANDLE)
            {
                for (VkImageView view : g_sc.views) { deletion::retire(deletion::view(view)); }
                g_sc.views.clear();
                deletion::entry_t old = {.kind = deletion::kind_e::SWAPCHAIN};
                old.handle.swapchain = old_swapchain;
                deletion::retire(old);
            }

            g_sc.extent = {physical_extent.width, physical_extent.height};
            g_sc.format = surface_format.format;

            u32_t image_count;
            VK_CHECK(vkGetSwapchainImagesKHR(ctx.device, g_sc.handle, &image_count, nullptr));
            std::vector<VkImage> swapchain_images(image_count);
            VK_CHECK(vkGetSwapchainImagesKHR(ctx.device, g_sc.handle, &image_count, swapchain_images.data()));
            g_sc.images = swapchain_images;
            for (VkImage image : g_sc.images) { device::name(image, VK_OBJECT_TYPE_IMAGE, "Swapchain"); }

            if (!g_sc.release_semaphores.empty())
            {
                for (VkSemaphore sem : g_sc.release_semaphores)
                {
                    deletion::entry_t e = {.kind = deletion::kind_e::SEMAPHORE};
                    e.handle.semaphore = sem;
                    deletion::retire(e);
                }

                g_sc.release_semaphores.clear();
            }

            g_sc.release_semaphores.clear();
            g_sc.release_semaphores.resize(image_count);

            for (size_t i = 0; i < image_count; i++)
            {
                VkSemaphoreCreateInfo sem_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
                VK_CHECK(vkCreateSemaphore(ctx.device, &sem_info, nullptr, &g_sc.release_semaphores[i]));
            }

            for (size_t i = 0; i < image_count; i++)
            {
                VkImageViewCreateInfo view_info = {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                    .image = swapchain_images[i],
                    .viewType = VK_IMAGE_VIEW_TYPE_2D,
                    .format = g_sc.format,
                    .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                         .baseMipLevel = 0,
                                         .levelCount = 1,
                                         .baseArrayLayer = 0,
                                         .layerCount = 1},
                };

                VkImageView image_view;
                VK_CHECK(vkCreateImageView(ctx.device, &view_info, nullptr, &image_view));

                g_sc.views.push_back(image_view);
            }
            return true;
        }
    } // namespace

    bool init(bool vsync)
    {
        g_vsync = vsync;
        if (ctx.caps.headless)
        {
            g_sc.format = VK_FORMAT_B8G8R8A8_SRGB;
            g_sc.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
            return false;
        }
        return init_swapchain();
    }

    void shutdown()
    {
        for (VkImageView image_view : g_sc.views) { vkDestroyImageView(ctx.device, image_view, nullptr); }
        for (VkSemaphore sem : g_sc.release_semaphores) { vkDestroySemaphore(ctx.device, sem, nullptr); }
        if (g_sc.handle != VK_NULL_HANDLE) { vkDestroySwapchainKHR(ctx.device, g_sc.handle, nullptr); }
        g_sc = {};
    }

    bool resize()
    {
        if (ctx.surface == VK_NULL_HANDLE) { return false; }

        VkSurfaceCapabilitiesKHR surface_caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.physical_device, ctx.surface, &surface_caps);

        if (surface_caps.currentExtent.width == 0 && surface_caps.currentExtent.height == 0) { return false; }

        return init_swapchain();
    }

    void recreate_surface()
    {
        vkDeviceWaitIdle(ctx.device);

        if (g_sc.handle != VK_NULL_HANDLE)
        {
            for (VkImageView view : g_sc.views) { deletion::retire(deletion::view(view)); }
            g_sc.views.clear();

            vkDestroySwapchainKHR(ctx.device, g_sc.handle, nullptr);
            g_sc.handle = VK_NULL_HANDLE;
        }

        if (ctx.surface != VK_NULL_HANDLE)
        {
            vkDestroySurfaceKHR(ctx.instance, ctx.surface, nullptr);
            ctx.surface = VK_NULL_HANDLE;
        }

        if (!SDL_Vulkan_CreateSurface(tau::window::get_window(), ctx.instance, nullptr, &ctx.surface))
        {
            TAU_LOG_WARN("VULKAN", "Failed to recreate Vulkan surface on resume");
        }
    }

    VkResult acquire(VkSemaphore signal, u32_t* index)
    {
        if (g_sc.handle == VK_NULL_HANDLE) { return VK_ERROR_OUT_OF_DATE_KHR; }
        return vkAcquireNextImageKHR(ctx.device, g_sc.handle, UINT64_MAX, signal, VK_NULL_HANDLE, index);
    }

    VkResult present(u32_t index)
    {
        VkPresentInfoKHR present_info = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &g_sc.release_semaphores[index],
            .swapchainCount = 1,
            .pSwapchains = &g_sc.handle,
            .pImageIndices = &index,
        };
        return vkQueuePresentKHR(ctx.present_queue, &present_info);
    }

    const swapchain_t& state() { return g_sc; }

    mat4 pre_rotation()
    {
        if (g_sc.transform & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) { return rotate_z({}, radians(90.0f)); }
        if (g_sc.transform & VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) { return rotate_z({}, radians(180.0f)); }
        if (g_sc.transform & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) { return rotate_z({}, radians(270.0f)); }
        return {};
    }
} // namespace tau::renderer::swapchain
