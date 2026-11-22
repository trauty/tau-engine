#include "mesh.h"

#include "tau/asset.h"
#include "tau/assets/material.h"
#include "tau/assets/mesh_format.h"
#include "tau/log.h"
#include "tau/rhi/deletion.h"
#include "tau/rhi/device.h"
#include "tau/rhi/renderer.h"
#include "tau/rhi/renderer_types.h"
#include "tau/rhi/transfer.h"
#include "tau/rhi/vulkan.h"
#include "tau/vfs.h"
#include "vulkan/vulkan_core.h"

#include <SDL3/SDL_iostream.h>
#include <mutex>
#include <optional>
#include <vector>

namespace tau
{
    std::optional<mesh_t> asset_loader_t<mesh_t>::load(const std::string& path)
    {
        std::string cooked_path = tau::assets::cooked_path(tau::get_type_id<mesh_t>(), path);

        SDL_IOStream* stream = tau::vfs::open_read(cooked_path);
        if (!stream)
        {
            TAU_LOG_ERROR("MESH", "Mesh not found: {}", cooked_path);
            return std::nullopt;
        }

        mesh_header_t header;
        if (SDL_ReadIO(stream, &header, sizeof(mesh_header_t)) != sizeof(mesh_header_t) ||
            header.magic != TAU_MESH_MAGIC)
        {
            TAU_LOG_ERROR("MESH", "Invalid .taumesh file: {}", cooked_path);
            SDL_CloseIO(stream);
            return std::nullopt;
        }

        if (header.version != TAU_MESH_VERSION)
        {
            TAU_LOG_ERROR("MESH", "Unsupported .taumesh version {} (engine expects {}), recook: {}", header.version,
                          TAU_MESH_VERSION, cooked_path);
            SDL_CloseIO(stream);
            return std::nullopt;
        }

        mesh_t asset = {
            .vertex_count = header.vertex_count,
            .index_count = header.index_count,
            .local_center = header.local_center,
            .local_radius = header.local_radius,
        };

        auto read_string = [stream](u32_t length)
        {
            std::string text(length, '\0');
            if (length > 0 && SDL_ReadIO(stream, text.data(), length) != length) { text.clear(); }
            return text;
        };

        asset.submeshes.reserve(header.submesh_count);
        for (u32_t i = 0; i < header.submesh_count; i++)
        {
            mesh_submesh_record_t record;
            if (SDL_ReadIO(stream, &record, sizeof(record)) != sizeof(record) ||
                record.first_index + record.index_count > header.index_count)
            {
                TAU_LOG_ERROR("MESH", "Truncated submesh table in: {}", cooked_path);
                SDL_CloseIO(stream);
                return std::nullopt;
            }

            submesh_t& submesh = asset.submeshes.emplace_back();
            submesh.first_index = record.first_index;
            submesh.index_count = record.index_count;
            submesh.local_center = record.local_center;
            submesh.local_radius = record.local_radius;

            const std::string material_path = read_string(record.material_path_length);
            submesh.material_name = read_string(record.material_name_length);

            const std::string resolved = assets::resolve(record.material_guid, material_path);
            if (!resolved.empty()) { submesh.material = assets::load<material_t>(resolved); }
        }

        VkDeviceSize vertex_size = asset.vertex_count * sizeof(vertex_t);
        VkDeviceSize index_size = asset.index_count * sizeof(u32);
        VkDeviceSize total_staging_size = vertex_size + index_size;

        VkBuffer staging_buf;
        VmaAllocation staging_alloc;

        VkBufferCreateInfo staging_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = total_staging_size,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        };
        VmaAllocationCreateInfo alloc_info = {
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };
        VmaAllocationInfo staging_alloc_info;

        VK_CHECK(vmaCreateBuffer(renderer::ctx.allocator, &staging_info, &alloc_info, &staging_buf, &staging_alloc,
                                 &staging_alloc_info));

        // extra streams after the indices wait for the renderer that reads them
        const bool complete = SDL_ReadIO(stream, staging_alloc_info.pMappedData, vertex_size) == vertex_size &&
                              SDL_ReadIO(stream, static_cast<u8*>(staging_alloc_info.pMappedData) + vertex_size,
                                         index_size) == index_size;
        SDL_CloseIO(stream);

        if (!complete)
        {
            TAU_LOG_ERROR("MESH", "Truncated vertex or index data in: {}", cooked_path);
            vmaDestroyBuffer(renderer::ctx.allocator, staging_buf, staging_alloc);
            return std::nullopt;
        }

        VkBufferCreateInfo mesh_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = vertex_size,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        };
        VmaAllocationCreateInfo mesh_alloc_info = {
            .usage = VMA_MEMORY_USAGE_AUTO,
        };

        VK_CHECK(vmaCreateBuffer(renderer::ctx.allocator, &mesh_info, &mesh_alloc_info, &asset.vertex_buffer,
                                 &asset.vertex_allocation, nullptr));

        if (index_size > 0)
        {
            mesh_info.size = index_size;
            mesh_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            VK_CHECK(vmaCreateBuffer(renderer::ctx.allocator, &mesh_info, &mesh_alloc_info, &asset.index_buffer,
                                     &asset.index_allocation, nullptr));
        }
        renderer::device::name(asset.vertex_buffer, VK_OBJECT_TYPE_BUFFER, path.c_str());
        renderer::device::name(asset.index_buffer, VK_OBJECT_TYPE_BUFFER, path.c_str());

        VkCommandBuffer cmd_buf = renderer::transfer::begin();

        VkBufferCopy copy_region = {
            .srcOffset = 0,
            .dstOffset = 0,
            .size = vertex_size,
        };
        vkCmdCopyBuffer(cmd_buf, staging_buf, asset.vertex_buffer, 1, &copy_region);

        if (index_size > 0)
        {
            copy_region.srcOffset = vertex_size;
            copy_region.size = index_size;
            vkCmdCopyBuffer(cmd_buf, staging_buf, asset.index_buffer, 1, &copy_region);
        }

        bool is_same_queue_fam = renderer::ctx.queue_fam_indices.transfer_family.value() ==
                                 renderer::ctx.queue_fam_indices.graphics_family.value();

        std::vector<VkBufferMemoryBarrier> barriers;
        VkBufferMemoryBarrier base_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = is_same_queue_fam
                                 ? (VkAccessFlags)(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT)
                                 : (VkAccessFlags)0,
            .srcQueueFamilyIndex = renderer::ctx.queue_fam_indices.transfer_family.value(),
            .dstQueueFamilyIndex = renderer::ctx.queue_fam_indices.graphics_family.value(),
        };

        base_barrier.buffer = asset.vertex_buffer;
        base_barrier.size = vertex_size;
        barriers.push_back(base_barrier);

        if (index_size > 0)
        {
            base_barrier.buffer = asset.index_buffer;
            base_barrier.size = index_size;
            barriers.push_back(base_barrier);
        }

        VkPipelineStageFlags dst_stage =
            is_same_queue_fam ? (VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
                              : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

        vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_TRANSFER_BIT, dst_stage, 0, 0, nullptr,
                             static_cast<u32_t>(barriers.size()), barriers.data(), 0, nullptr);

        u64_t signal_value = renderer::transfer::submit(cmd_buf);

        if (!is_same_queue_fam)
        {
            for (VkBufferMemoryBarrier& barrier : barriers)
            {
                barrier.srcAccessMask = 0;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
                renderer::transfer::acquire(barrier);
            }
        }

        asset.vertex_buffer_address = renderer::get_buffer_address(asset.vertex_buffer);

        if (index_size > 0) { asset.index_buffer_address = renderer::get_buffer_address(asset.index_buffer); }

        renderer::deletion::retire(renderer::deletion::buffer(staging_buf, staging_alloc),
                                   renderer::deletion::clock_e::TRANSFER, signal_value);

        return asset;
    }

    void asset_loader_t<mesh_t>::deps(const mesh_t& mesh, std::vector<asset_handle_t>& out)
    {
        for (const submesh_t& submesh : mesh.submeshes)
        {
            if (submesh.material.is_valid()) { out.push_back(submesh.material); }
        }
    }

    void asset_loader_t<mesh_t>::unload(mesh_t& mesh)
    {
        if (mesh.vertex_buffer != VK_NULL_HANDLE)
        {
            renderer::deletion::retire(renderer::deletion::buffer(mesh.vertex_buffer, mesh.vertex_allocation));
        }
        if (mesh.index_buffer != VK_NULL_HANDLE)
        {
            renderer::deletion::retire(renderer::deletion::buffer(mesh.index_buffer, mesh.index_allocation));
        }
    }
} // namespace tau
