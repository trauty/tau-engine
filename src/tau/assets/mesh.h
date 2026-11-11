#pragma once

#include "tau/asset.h"
#include "tau/defines.h"
#include "tau/math.h"
#include "tau/rendering/vulkan.h"

#include <optional>
#include <string>
#include <string_view>

namespace tau
{
    struct TAU_ENGINE_API vertex_t
    {
        f32 position[3];
        f32 normal[3];
        f32 uv[2];
    };

    struct TAU_ENGINE_API mesh_t
    {
        static constexpr std::string_view type_key = "mesh";

        u32_t vertex_count = 0;
        u32_t index_count = 0;

        vec3_t local_center;
        f32 local_radius;

        VkBuffer vertex_buffer = VK_NULL_HANDLE;
        VmaAllocation vertex_allocation = VK_NULL_HANDLE;

        VkBuffer index_buffer = VK_NULL_HANDLE;
        VmaAllocation index_allocation = VK_NULL_HANDLE;

        VkDeviceAddress vertex_buffer_address = 0;
        VkDeviceAddress index_buffer_address = 0;
    };

    template <>
    struct TAU_ENGINE_API asset_loader_t<mesh_t>
    {
        static std::optional<mesh_t> load(const std::string& path);
        static void unload(mesh_t& mesh);
    };
} // namespace tau