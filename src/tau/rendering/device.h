#pragma once

#include "tau/defines.h"
#include "tau/rendering/vulkan.h"

#include <string>
#include <vector>

struct SDL_Window;

// instance, physical device, logical device, queues and allocator: the one creation path, written into renderer::ctx
namespace tau::renderer::device
{
    struct device_config_t
    {
        std::string app_name = "tau";
        // validation layers with synchronization validation chained
        bool validation = false;
        // no hardware ray tracing and no 16-bit integers even where the GPU has them: the minimum spec
        bool force_compat = false;
        // no surface and no swapchain: offscreen only, no display needed
        bool headless = false;
        // VK_KHR_pipeline_executable_properties: driver statistics per pipeline (registers, spills)
        bool pipeline_properties = false;
        std::vector<const char*> instance_exts;
    };

    // what the created device can do; read through renderer::ctx.caps
    struct device_caps_t
    {
        bool ray_query = false;
        bool position_fetch = false;
        bool int16 = false;
        bool pipeline_properties = false;
        bool headless = false;
        bool validation = false;
        // a compute-only queue family exists (async compute, Part 14); found, not created
        bool has_compute_family = false;
        u32_t compute_family = 0;
    };

    // window may be null when headless
    bool create(const device_config_t& config, SDL_Window* window);
    void destroy();

    // validation errors reported since startup
    TAU_ENGINE_API u32_t validation_errors();
} // namespace tau::renderer::device
