#pragma once

#include "tau/asset.h"
#include "tau/assets/shader.h"
#include "tau/containers/flat_map.h"
#include "tau/defines.h"
#include "tau/rhi/renderer_constants.h"
#include "tau/rhi/samplers.h"

namespace tau
{
    struct TAU_ENGINE_API bound_resource_t
    {
        VkDescriptorType type;

        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        VkDeviceSize size = VK_WHOLE_SIZE;

        VkImageView view = VK_NULL_HANDLE;
        sampler_type_e sampler;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    struct TAU_ENGINE_API shader_instance_t
    {
        asset_t<shader_t> shader;

        flat_map_t<bound_resource_t> bound_resources;
        flat_map_t<VkDescriptorSet> sets[renderer::MAX_FRAMES_IN_FLIGHT];

        u32_t dirty_frames = renderer::MAX_FRAMES_IN_FLIGHT;
        u32_t last_synced_frame = renderer::BINDLESS_NULL_HANDLE;
        bool is_initialized = false;

        shader_instance_t() = default;
        explicit shader_instance_t(asset_t<shader_t> target_shader);
        ~shader_instance_t();

        void init(asset_t<shader_t> target_shader);
        void shutdown();
        void sync();

        void set_buffer(u32_t name_hash, VkBuffer buffer, VkDeviceSize = VK_WHOLE_SIZE, VkDeviceSize offset = 0);
        void set_image(u32_t name_hash, VkImageView view, sampler_type_e sampler, VkImageLayout layout);

        void bind(VkCommandBuffer cmd);
    };
} // namespace tau