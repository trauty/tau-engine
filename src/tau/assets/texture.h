#pragma once
#include "tau/asset.h"
#include "tau/defines.h"
#include "tau/rendering/renderer_constants.h"
#include "tau/rendering/vulkan.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace tau
{
    struct TAU_ENGINE_API texture_t
    {
        static constexpr std::string_view type_key = "texture";

        i32 width = 0;
        i32 height = 0;

        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        u32_t bindless_id = renderer::BINDLESS_NULL_HANDLE;
    };

    template <>
    struct TAU_ENGINE_API asset_loader_t<texture_t>
    {
        static std::optional<texture_t> load(const std::string& path);
        static void unload(texture_t& tex);
    };
} // namespace tau