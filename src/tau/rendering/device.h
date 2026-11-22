#pragma once

#include "tau/defines.h"
#include "tau/rendering/vulkan.h"

#include <string>
#include <vector>

struct SDL_Window;

namespace tau::renderer::device
{
    struct device_config_t
    {
        std::string app_name = "tau";
        // + sync validation
        bool validation = false;
        // no RT, no int16
        bool force_compat = false;
        // no surface / swapchain
        bool headless = false;
        // VK_KHR_pipeline_executable_properties
        bool pipeline_properties = false;
        std::vector<const char*> instance_exts;
    };

    struct device_caps_t
    {
        bool ray_query = false;
        bool position_fetch = false;
        bool int16 = false;
        bool pipeline_properties = false;
        bool headless = false;
        bool validation = false;
        // object names, command labels
        bool debug_utils = false;
        // found, not created
        bool has_compute_family = false;
        u32_t compute_family = 0;
    };

    // which GPU this is, for tools outside the renderer (telemetry)
    struct gpu_identity_t
    {
        u32_t vendor_id = 0;
        u32_t device_id = 0;
        u8_t uuid[16] = {};
        bool has_pci = false;
        u32_t pci_domain = 0, pci_bus = 0, pci_device = 0, pci_function = 0;
    };

    // window null when headless
    bool create(const device_config_t& config, SDL_Window* window);
    void destroy();

    TAU_ENGINE_API u32_t validation_errors();
    TAU_ENGINE_API const gpu_identity_t& identity();

    // no-ops without VK_EXT_debug_utils
    TAU_ENGINE_API void set_name(VkObjectType type, u64_t handle, const char* name);
    TAU_ENGINE_API void begin_label(VkCommandBuffer cmd, const char* name);
    TAU_ENGINE_API void end_label(VkCommandBuffer cmd);

    template <typename T>
    void name(T handle, VkObjectType type, const char* n)
    { set_name(type, (u64_t)handle, n); }
} // namespace tau::renderer::device
