#include "material_heap.h"

#include "tau/log.h"
#include "tau/rhi/deletion.h"
#include "tau/rhi/device.h"
#include "tau/rhi/renderer.h"
#include "tau/rhi/renderer_constants.h"

#include <bit>
#include <cstring>
#include <mutex>
#include <vector>

namespace tau::renderer::material_heap
{
    namespace
    {
        constexpr u32_t MIN_BLOCK = 256;
        constexpr u32_t CLASS_COUNT = 24;

        std::mutex g_mutex;
        VkBuffer g_buffer = VK_NULL_HANDLE;
        VmaAllocation g_allocation = VK_NULL_HANDLE;
        u8* g_mapped = nullptr;
        VkDeviceAddress g_address = 0;
        u32_t g_capacity = 0;
        u32_t g_top = 0;
        u32_t g_used = 0;
        u32_t g_grown = 0;
        // what the GPU buffer holds, so growing copies from cached memory, not write-combined
        std::vector<u8> g_shadow;
        // power of two classes from MIN_BLOCK
        std::vector<u32_t> g_free[CLASS_COUNT];

        u32_t class_of(u32_t size)
        {
            const u32_t block = std::bit_ceil(std::max(size, MIN_BLOCK));
            return static_cast<u32_t>(std::countr_zero(block) - std::countr_zero(MIN_BLOCK));
        }

        u32_t block_size(u32_t cls) { return MIN_BLOCK << cls; }

        bool create(u32_t capacity, VkBuffer& buffer, VmaAllocation& allocation, u8*& mapped, VkDeviceAddress& address)
        {
            const VkBufferCreateInfo info = {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = capacity,
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            };
            const VmaAllocationCreateInfo alloc = {
                .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                .usage = VMA_MEMORY_USAGE_AUTO,
            };
            VmaAllocationInfo out = {};
            if (vmaCreateBuffer(ctx.allocator, &info, &alloc, &buffer, &allocation, &out) != VK_SUCCESS)
            {
                return false;
            }
            mapped = static_cast<u8*>(out.pMappedData);
            const VkBufferDeviceAddressInfo addr = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                                                    .buffer = buffer};
            address = vkGetBufferDeviceAddress(ctx.device, &addr);
            return true;
        }

        // g_mutex held; frames recorded before still read the old buffer, it waits for them
        bool grow(u32_t at_least)
        {
            u32_t capacity = std::max(g_capacity, MIN_BLOCK);
            while (capacity < at_least) { capacity *= 2; }

            VkBuffer buffer = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            u8* mapped = nullptr;
            VkDeviceAddress address = 0;
            if (!create(capacity, buffer, allocation, mapped, address))
            {
                TAU_LOG_ERROR("MATERIAL", "Could not grow the material heap to {} bytes", capacity);
                return false;
            }
            device::name(buffer, VK_OBJECT_TYPE_BUFFER, "MaterialHeap");
            if (g_top > 0) { std::memcpy(mapped, g_shadow.data(), g_top); }
            g_shadow.resize(capacity);

            if (g_buffer != VK_NULL_HANDLE)
            {
                deletion::retire(deletion::buffer(g_buffer, g_allocation));
                g_grown++;
                TAU_LOG_INFO("MATERIAL", "Material heap grown from {} to {} bytes", g_capacity, capacity);
            }
            g_buffer = buffer;
            g_allocation = allocation;
            g_mapped = mapped;
            g_address = address;
            g_capacity = capacity;
            return true;
        }
    } // namespace

    void init(u32_t initial_size)
    {
        std::scoped_lock lock(g_mutex);
        grow(initial_size);
    }

    void shutdown()
    {
        std::scoped_lock lock(g_mutex);
        if (g_buffer != VK_NULL_HANDLE) { vmaDestroyBuffer(ctx.allocator, g_buffer, g_allocation); }
        g_buffer = VK_NULL_HANDLE;
        g_mapped = nullptr;
        g_capacity = g_top = g_used = 0;
        g_shadow.clear();
        for (std::vector<u32_t>& f : g_free) { f.clear(); }
    }

    u32_t allocate(u32_t size)
    {
        const u32_t cls = class_of(size);
        if (cls >= CLASS_COUNT)
        {
            TAU_LOG_ERROR("MATERIAL", "Material data of {} bytes is too large", size);
            return BINDLESS_NULL_HANDLE;
        }

        std::scoped_lock lock(g_mutex);
        g_used += block_size(cls);
        if (!g_free[cls].empty())
        {
            const u32_t offset = g_free[cls].back();
            g_free[cls].pop_back();
            return offset;
        }

        const u32_t offset = g_top;
        if (offset + block_size(cls) > g_capacity)
        {
            TAU_LOG_WARN("MATERIAL", "Material heap full mid frame, grown at once");
            if (!grow(offset + block_size(cls)))
            {
                g_used -= block_size(cls);
                return BINDLESS_NULL_HANDLE;
            }
        }
        g_top += block_size(cls);
        return offset;
    }

    void free(u32_t offset, u32_t size)
    {
        if (offset == BINDLESS_NULL_HANDLE) { return; }
        deletion::entry_t e{.kind = deletion::kind_e::MATERIAL_RANGE};
        e.handle.range = {offset, size};
        deletion::retire(e);
    }

    void release(u32_t offset, u32_t size)
    {
        const u32_t cls = class_of(size);
        std::scoped_lock lock(g_mutex);
        g_free[cls].push_back(offset);
        g_used -= block_size(cls);
    }

    void write(u32_t offset, const void* data, u32_t size)
    {
        if (offset == BINDLESS_NULL_HANDLE) { return; }
        std::scoped_lock lock(g_mutex);
        if (offset + size > g_capacity) { return; }
        std::memcpy(g_shadow.data() + offset, data, size);
        std::memcpy(g_mapped + offset, data, size);
    }

    VkDeviceAddress address()
    {
        std::scoped_lock lock(g_mutex);
        return g_address;
    }

    void begin_frame()
    {
        std::scoped_lock lock(g_mutex);
        // headroom so a frame's allocations fit without growing mid frame
        if (g_top > g_capacity / 4 * 3) { grow(g_capacity * 2); }
    }

    stats_t stats()
    {
        std::scoped_lock lock(g_mutex);
        return {g_capacity, g_used, g_grown};
    }
} // namespace tau::renderer::material_heap
