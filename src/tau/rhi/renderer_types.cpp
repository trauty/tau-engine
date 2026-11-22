#include "renderer_types.h"

#include "tau/rhi/renderer.h"
#include "tau/rhi/renderer_resources.h"
#include "tau/rhi/vulkan.h"

namespace tau::renderer
{
    transient_image_t* transient_pool_t::acquire(const image_desc_t& desc)
    {
        for (transient_image_t& img : images)
        {
            if (!img.in_use && img.desc == desc)
            {
                img.in_use = true;
                img.frames_unused = 0;
                return &img;
            }
        }

        transient_image_t new_img;
        new_img.desc = desc;
        new_img.in_use = true;
        new_img.frames_unused = 0;

        VkImageCreateInfo image_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = desc.format,
            .extent = {desc.width, desc.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = desc.usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        VmaAllocationCreateInfo alloc_info = {
            .usage = VMA_MEMORY_USAGE_AUTO,
            .priority = 1.0f,
        };

        VK_CHECK(vmaCreateImage(ctx.allocator, &image_info, &alloc_info, &new_img.image, &new_img.allocation, nullptr));

        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = new_img.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = desc.format,
            .subresourceRange = {
                                 .aspectMask = desc.aspect,
                                 .baseMipLevel = 0,
                                 .levelCount = 1,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1,
                                 }
        };

        VK_CHECK(vkCreateImageView(ctx.device, &view_info, nullptr, &new_img.view));

        if (desc.usage & VK_IMAGE_USAGE_SAMPLED_BIT)
        {
            new_img.bindless_id = res_system.texture_heap.acquire();
            VkDescriptorImageInfo bindless_info = {
                .sampler = VK_NULL_HANDLE,
                .imageView = new_img.view,
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            };

            VkWriteDescriptorSet write_desc = {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = res_system.global_set,
                .dstBinding = TEXTURES_BINDING_POINT,
                .dstArrayElement = new_img.bindless_id,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                .pImageInfo = &bindless_info,
            };

            vkUpdateDescriptorSets(ctx.device, 1, &write_desc, 0, nullptr);
        }

        images.push_back(new_img);
        return &images.back();
    }

    void transient_pool_t::reset()
    {
        constexpr u32_t MAX_FRAMES_UNUSED = 8;

        for (auto it = images.begin(); it != images.end();)
        {
            if (!it->in_use)
            {
                it->frames_unused++;
                if (it->frames_unused > MAX_FRAMES_UNUSED)
                {
                    if (it->view) { vkDestroyImageView(ctx.device, it->view, nullptr); }
                    if (it->image) { vmaDestroyImage(ctx.allocator, it->image, it->allocation); }
                    if (it->bindless_id != BINDLESS_NULL_HANDLE) { res_system.texture_heap.release(it->bindless_id); }

                    it = images.erase(it);
                    continue;
                }
            }
            else
            {
                it->in_use = false;
                it->frames_unused = 0;
            }

            it++;
        }
    }

    void transient_pool_t::shutdown()
    {
        for (transient_image_t& img : images)
        {
            if (img.view) { vkDestroyImageView(ctx.device, img.view, nullptr); }
            if (img.image) { vmaDestroyImage(ctx.allocator, img.image, img.allocation); }
            if (img.bindless_id != BINDLESS_NULL_HANDLE) { res_system.texture_heap.release(img.bindless_id); }
        }

        images.clear();
    }
} // namespace tau::renderer