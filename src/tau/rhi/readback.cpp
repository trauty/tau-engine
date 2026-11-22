#include "readback.h"

#include "tau/log.h"
#include "tau/rhi/context.h"
#include "tau/rhi/renderer_constants.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace tau::renderer
{
    namespace
    {
        constexpr u64_t MIN_CAPACITY = 64 * 1024;

        struct pending_copy_t
        {
            readback_ticket_t ticket;
            u64_t offset;
            u64_t size;
        };

        struct slot_t
        {
            VkBuffer buffer = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            u8_t* mapped = nullptr;
            u64_t capacity = 0;
            u64_t used = 0;
            std::vector<pending_copy_t> pending;
        };

        slot_t slots[MAX_FRAMES_IN_FLIGHT];
        std::unordered_map<readback_ticket_t, std::vector<u8_t>> finished;
        readback_ticket_t next_ticket = 1;

        void destroy(slot_t& slot)
        {
            if (slot.buffer != VK_NULL_HANDLE) { vmaDestroyBuffer(ctx.allocator, slot.buffer, slot.allocation); }
            slot.buffer = VK_NULL_HANDLE;
            slot.allocation = VK_NULL_HANDLE;
            slot.mapped = nullptr;
            slot.capacity = 0;
        }

        void create(slot_t& slot, u64_t capacity)
        {
            const VkBufferCreateInfo buf_info = {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = capacity,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            };
            const VmaAllocationCreateInfo alloc_info = {
                .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                .usage = VMA_MEMORY_USAGE_AUTO,
                .requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            };

            VmaAllocationInfo info;
            VK_CHECK(vmaCreateBuffer(ctx.allocator, &buf_info, &alloc_info, &slot.buffer, &slot.allocation, &info));
            slot.mapped = static_cast<u8_t*>(info.pMappedData);
            slot.capacity = capacity;
        }
    } // namespace

    readback_ticket_t record_readback(VkCommandBuffer cmd, VkImage image, VkImageLayout layout,
                                      VkImageAspectFlags aspect, u32_t texel_size, VkRect2D rect)
    {
        slot_t& slot = slots[ctx.cur_frame];

        const u64_t size = u64_t(rect.extent.width) * rect.extent.height * texel_size;
        const u64_t offset = (slot.used + 15) & ~u64_t(15);
        if (size == 0) { return READBACK_NONE; }

        if (offset + size > slot.capacity)
        {
            // grow before the frame's first copy only
            if (slot.used != 0)
            {
                TAU_LOG_WARN("RENDERER", "Readback of {} bytes does not fit this frame", size);
                return READBACK_NONE;
            }
            destroy(slot);
            create(slot, std::max(MIN_CAPACITY, size * 2));
        }

        // graph v2 passes hand it over as a transfer source already
        const bool transition = layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        if (transition) { transition_image(cmd, image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, aspect); }

        const VkBufferImageCopy region = {
            .bufferOffset = offset,
            .imageSubresource = {.aspectMask = aspect, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
            .imageOffset = {rect.offset.x, rect.offset.y, 0},
            .imageExtent = {rect.extent.width, rect.extent.height, 1},
        };
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.buffer, 1, &region);

        if (transition) { transition_image(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout, aspect); }

        // semaphore wait alone does not make transfer writes host visible
        const VkBufferMemoryBarrier2 to_host = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
            .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
            .buffer = slot.buffer,
            .offset = offset,
            .size = size,
        };
        const VkDependencyInfo dep = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &to_host,
        };
        vkCmdPipelineBarrier2(cmd, &dep);

        const readback_ticket_t ticket = next_ticket++;
        slot.pending.push_back({ticket, offset, size});
        slot.used = offset + size;
        return ticket;
    }

    bool fetch_readback(readback_ticket_t ticket, std::vector<u8_t>& out)
    {
        const auto it = finished.find(ticket);
        if (it == finished.end()) { return false; }

        out = std::move(it->second);
        finished.erase(it);
        return true;
    }

    namespace readback
    {
        void collect(u32_t frame_slot)
        {
            slot_t& slot = slots[frame_slot];
            for (const pending_copy_t& copy : slot.pending)
            {
                std::vector<u8_t>& bytes = finished[copy.ticket];
                bytes.resize(copy.size);
                std::memcpy(bytes.data(), slot.mapped + copy.offset, copy.size);
            }
            slot.pending.clear();
            slot.used = 0;
        }

        void shutdown()
        {
            for (slot_t& slot : slots)
            {
                destroy(slot);
                slot.pending.clear();
                slot.used = 0;
            }
            finished.clear();
        }
    } // namespace readback
} // namespace tau::renderer
