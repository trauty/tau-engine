#pragma once

#include "tau/asset.h"
#include "tau/defines.h"
#include "tau/math.h"
#include "tau/rhi/vulkan.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tau
{
    struct TAU_ENGINE_API vertex_t
    {
        f32 position[3];
        f32 normal[3];
        f32 uv[2];
    };

    struct material_t;

    struct TAU_ENGINE_API submesh_t
    {
        u32_t first_index = 0;
        u32_t index_count = 0;
        vec3 local_center;
        f32 local_radius = 0.0f;

        asset_t<material_t> material;
        std::string material_name;
    };

    struct TAU_ENGINE_API mesh_t
    {
        static constexpr std::string_view type_key = "mesh";

        u32_t vertex_count = 0;
        u32_t index_count = 0;

        // all submeshes
        vec3 local_center;
        f32 local_radius;

        std::vector<submesh_t> submeshes;

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
        static std::optional<mesh_t> load(const std::string& path, std::span<const u8_t> bytes);
        static void unload(mesh_t& mesh);
        static void deps(const mesh_t& mesh, std::vector<asset_handle_t>& out);
    };
} // namespace tau