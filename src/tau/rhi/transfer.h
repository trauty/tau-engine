#pragma once

#include "tau/defines.h"
#include "tau/rhi/vulkan.h"

// uploads on the transfer queue, on their own timeline; the graphics queue finishes ownership transfers
namespace tau::renderer::transfer
{
    void init();
    void shutdown();

    // a region of the persistent staging ring (or its own buffer when larger than the ring)
    struct staging_t
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        u8_t* data = nullptr;
        VkDeviceSize size = 0;
        u64_t id = 0;
        // only for an upload larger than the ring
        VmaAllocation own_allocation = VK_NULL_HANDLE;
    };

    // any thread, before begin(); waits while the ring is full
    TAU_ENGINE_API staging_t stage(VkDeviceSize size);
    // gives a region back unused (a failed upload)
    TAU_ENGINE_API void discard(const staging_t& staging);

    // any thread; uploads are serialised from begin() to submit(), every begin() needs its submit()
    TAU_ENGINE_API VkCommandBuffer begin();
    // the timeline value the upload signals; the command buffer and the staging region are freed once it passed
    TAU_ENGINE_API u64_t submit(VkCommandBuffer cmd, const staging_t* staging = nullptr);

    // queue family ownership transfers the next frame acquires
    TAU_ENGINE_API void acquire(const VkImageMemoryBarrier& barrier);
    TAU_ENGINE_API void acquire(const VkBufferMemoryBarrier& barrier);
    void flush_acquires(VkCommandBuffer cmd);

    VkSemaphore timeline();
    // the newest value submitted
    u64_t last_value();
    u64_t completed();
} // namespace tau::renderer::transfer
