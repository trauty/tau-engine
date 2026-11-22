#include "texture.h"

#include "tau/asset.h"
#include "tau/defines.h"
#include "tau/log.h"
#include "tau/rhi/bindless.h"
#include "tau/rhi/context.h"
#include "tau/rhi/deletion.h"
#include "tau/rhi/device.h"
#include "tau/rhi/transfer.h"
#include "tau/rhi/vulkan.h"
#include "vulkan/vulkan_core.h"

#include <cstddef>
#include <cstring>
#include <ktx.h>
#include <mutex>
#include <optional>
#include <stb/stb_image.h>
#include <vector>

namespace tau
{
    std::optional<texture_t> asset_loader_t<texture_t>::load(const std::string& path, std::span<const u8_t> bytes)
    {
        std::string cooked_path = tau::assets::cooked_path(tau::get_type_id<texture_t>(), path);

        // libktx decodes into its own memory, then one copy into the staging ring: zstd reads back what it wrote,
        // the ring is write-combined
        ktxTexture2* k_tex;
        if (ktxTexture_CreateFromMemory(bytes.data(), bytes.size(), KTX_TEXTURE_CREATE_NO_FLAGS,
                                        (ktxTexture**)&k_tex) != KTX_SUCCESS)
        {
            TAU_LOG_ERROR("TEXTURE", "Invalid texture: {}", cooked_path);
            return std::nullopt;
        }

        if (ktxTexture_LoadImageData(ktxTexture(k_tex), nullptr, 0) != KTX_SUCCESS)
        {
            TAU_LOG_ERROR("TEXTURE", "Failed to read the image data of: {}", cooked_path);
            ktxTexture_Destroy(ktxTexture(k_tex));
            return std::nullopt;
        }

        if (ktxTexture2_NeedsTranscoding(k_tex))
        {
            char* tau_type = nullptr;
            u32_t tau_type_len = 0;
            ktxHashList_FindValue(&k_tex->kvDataHead, "tau_tex_type", &tau_type_len, (void**)&tau_type);
            std::string type_str = tau_type ? std::string(tau_type) : "color";

            char* channels = nullptr;
            ktx_uint32_t chan_len = 0;
            ktxHashList_FindValue(&k_tex->kvDataHead, "tau_tex_channels", &chan_len, (void**)&channels);
            i32 num_channels = channels ? std::stoi(channels) : 4;

            ktx_transcode_fmt_e target_format = KTX_TTF_BC7_RGBA;

            if (type_str == "normal") { target_format = KTX_TTF_BC5_RG; }
            else if (type_str == "data")
            {
                if (num_channels == 1) { target_format = KTX_TTF_BC4_R; }
            }

            if (ktxTexture2_TranscodeBasis(k_tex, target_format, 0) != KTX_SUCCESS)
            {
                TAU_LOG_ERROR("TEXTURE", "Failed to transcode ktx texture: {}", cooked_path);
                ktxTexture_Destroy(ktxTexture(k_tex));
                return std::nullopt;
            }
        }

        const VkDeviceSize data_size = ktxTexture_GetDataSize(ktxTexture(k_tex));
        const renderer::transfer::staging_t staging = renderer::transfer::stage(data_size);
        if (staging.data) { std::memcpy(staging.data, ktxTexture_GetData(ktxTexture(k_tex)), data_size); }

        if (!staging.data)
        {
            TAU_LOG_ERROR("TEXTURE", "Failed to allocate staging memory for texture: {}", path);
            ktxTexture_Destroy(ktxTexture(k_tex));
            return std::nullopt;
        }

        texture_t tex;
        tex.width = k_tex->baseWidth;
        tex.height = k_tex->baseHeight;

        VkFormat format = (VkFormat)k_tex->vkFormat;

        VkImageCreateInfo image_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = {static_cast<u32_t>(tex.width), static_cast<u32_t>(tex.height), 1},
            .mipLevels = k_tex->numLevels,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        VmaAllocationCreateInfo img_alloc_info = {.usage = VMA_MEMORY_USAGE_AUTO};
        if (vmaCreateImage(renderer::ctx.allocator, &image_info, &img_alloc_info, &tex.image, &tex.allocation,
                           nullptr) != VK_SUCCESS)
        {
            TAU_LOG_ERROR("TEXTURE", "Failed to allocate GPU memory for texture: {}", path);
            renderer::transfer::discard(staging);
            ktxTexture_Destroy(ktxTexture(k_tex));
            return std::nullopt;
        }

        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = tex.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .baseMipLevel = 0,
                                 .levelCount = k_tex->numLevels,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1},
        };

        if (vkCreateImageView(renderer::ctx.device, &view_info, nullptr, &tex.view) != VK_SUCCESS)
        {
            TAU_LOG_ERROR("TEXTURE", "Failed to create image view for texture: {}", path);
            vmaDestroyImage(renderer::ctx.allocator, tex.image, tex.allocation);
            renderer::transfer::discard(staging);
            ktxTexture_Destroy(ktxTexture(k_tex));
            return std::nullopt;
        }
        renderer::device::name(tex.image, VK_OBJECT_TYPE_IMAGE, path.c_str());
        renderer::device::name(tex.view, VK_OBJECT_TYPE_IMAGE_VIEW, path.c_str());

        VkCommandBuffer cmd_buf = renderer::transfer::begin();

        VkImageMemoryBarrier barrier_to_dst = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = tex.image,
            .subresourceRange = view_info.subresourceRange,
        };

        vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &barrier_to_dst);

        std::vector<VkBufferImageCopy> copy_regions;
        for (u32_t mip = 0; mip < k_tex->numLevels; mip++)
        {
            ktx_size_t offset;
            if (ktxTexture_GetImageOffset(ktxTexture(k_tex), mip, 0, 0, &offset) != KTX_SUCCESS) { continue; }

            VkBufferImageCopy region = {
                .bufferOffset = staging.offset + offset,
                .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                     .mipLevel = mip,
                                     .baseArrayLayer = 0,
                                     .layerCount = 1},
                .imageExtent = {.width = std::max(1u, k_tex->baseWidth >> mip),
                                     .height = std::max(1u, k_tex->baseHeight >> mip),
                                     .depth = 1}
            };

            copy_regions.push_back(region);
        }

        vkCmdCopyBufferToImage(cmd_buf, staging.buffer, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               (u32_t)copy_regions.size(), copy_regions.data());

        bool is_same_queue = renderer::ctx.queue_fam_indices.transfer_family.value() ==
                             renderer::ctx.queue_fam_indices.graphics_family.value();

        VkImageMemoryBarrier release_barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = is_same_queue ? (VkAccessFlags)VK_ACCESS_SHADER_READ_BIT : (VkAccessFlags)0,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex =
                is_same_queue ? VK_QUEUE_FAMILY_IGNORED : renderer::ctx.queue_fam_indices.transfer_family.value(),
            .dstQueueFamilyIndex =
                is_same_queue ? VK_QUEUE_FAMILY_IGNORED : renderer::ctx.queue_fam_indices.graphics_family.value(),
            .image = tex.image,
            .subresourceRange = view_info.subresourceRange,
        };

        VkPipelineStageFlags dst_stage =
            is_same_queue ? (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
                          : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

        vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_TRANSFER_BIT, dst_stage, 0, 0, nullptr, 0, nullptr, 1,
                             &release_barrier);

        renderer::transfer::submit(cmd_buf, &staging);

        if (!is_same_queue)
        {
            VkImageMemoryBarrier acquire_barrier = release_barrier;
            acquire_barrier.srcAccessMask = 0;
            acquire_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            renderer::transfer::acquire(acquire_barrier);
        }

        tex.bindless = renderer::bindless::add_sampled(tex.view);
        tex.bindless_id = tex.bindless.index;

        ktxTexture_Destroy(ktxTexture(k_tex));

        return tex;
    }

    void asset_loader_t<texture_t>::unload(texture_t& tex)
    {
        if (tex.image == VK_NULL_HANDLE) { return; }

        renderer::deletion::retire(renderer::deletion::image(tex.image, tex.allocation, tex.view));
        renderer::bindless::remove(tex.bindless);
    }
} // namespace tau
