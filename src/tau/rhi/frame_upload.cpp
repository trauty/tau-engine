#include "frame_upload.h"

#include "tau/rhi/renderer.h"
#include "tau/rhi/renderer_constants.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace tau::renderer
{
    namespace
    {
        constexpr u64_t MIN_BLOCK = 64 * 1024;

        struct block_t
        {
            VkBuffer buffer = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            u8_t* mapped = nullptr;
            VkDeviceAddress address = 0;
            u64_t capacity = 0;
            u64_t used = 0;
        };

        struct slot_t
        {
            std::vector<block_t> blocks;
            u64_t next_capacity = MIN_BLOCK;
        };

        slot_t slots[MAX_FRAMES_IN_FLIGHT];

        void release(block_t& block)
        {
            if (block.buffer != VK_NULL_HANDLE) { vmaDestroyBuffer(ctx.allocator, block.buffer, block.allocation); }
            block = {};
        }

        block_t create(u64_t capacity)
        {
            const VkBufferCreateInfo buf_info = {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = capacity,
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            };
            const VmaAllocationCreateInfo alloc_info = {
                .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                .usage = VMA_MEMORY_USAGE_AUTO,
            };

            block_t block;
            VmaAllocationInfo info;
            VK_CHECK(vmaCreateBuffer(ctx.allocator, &buf_info, &alloc_info, &block.buffer, &block.allocation, &info));
            block.mapped = static_cast<u8_t*>(info.pMappedData);
            block.address = get_buffer_address(block.buffer);
            block.capacity = capacity;
            return block;
        }
    } // namespace

    VkDeviceAddress upload_frame_data(const void* data, u64_t size)
    {
        if (size == 0) { return 0; }

        slot_t& slot = slots[ctx.cur_frame];

        block_t* block = slot.blocks.empty() ? nullptr : &slot.blocks.back();
        u64_t offset = block ? (block->used + 15) & ~u64_t(15) : 0;

        if (!block || offset + size > block->capacity)
        {
            slot.blocks.push_back(create(std::max(slot.next_capacity, size * 2)));
            block = &slot.blocks.back();
            offset = 0;
        }

        std::memcpy(block->mapped + offset, data, size);
        block->used = offset + size;
        return block->address + offset;
    }

    namespace frame_upload
    {
        void reset(u32_t frame_slot)
        {
            slot_t& slot = slots[frame_slot];

            // outgrew a block: next frame gets one block of the total
            if (slot.blocks.size() > 1)
            {
                u64_t total = 0;
                for (block_t& block : slot.blocks)
                {
                    total += block.capacity;
                    release(block);
                }
                slot.blocks.clear();
                slot.next_capacity = total;
                return;
            }

            for (block_t& block : slot.blocks) { block.used = 0; }
        }

        void shutdown()
        {
            for (slot_t& slot : slots)
            {
                for (block_t& block : slot.blocks) { release(block); }
                slot.blocks.clear();
                slot.next_capacity = MIN_BLOCK;
            }
        }
    } // namespace frame_upload
} // namespace tau::renderer
