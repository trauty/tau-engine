#pragma once

#include "tau/defines.h"
#include "tau/rhi/vulkan.h"

// uploads on the transfer queue, on their own timeline; the graphics queue finishes ownership transfers
namespace tau::renderer::transfer
{
    void init();
    void shutdown();

    // any thread; uploads are serialised from begin() to submit(), every begin() needs its submit()
    TAU_ENGINE_API VkCommandBuffer begin();
    // the timeline value the upload signals; the command buffer is freed once it passed
    TAU_ENGINE_API u64_t submit(VkCommandBuffer cmd);

    // queue family ownership transfers the next frame acquires
    TAU_ENGINE_API void acquire(const VkImageMemoryBarrier& barrier);
    TAU_ENGINE_API void acquire(const VkBufferMemoryBarrier& barrier);
    void flush_acquires(VkCommandBuffer cmd);

    VkSemaphore timeline();
    // the newest value submitted
    u64_t last_value();
    u64_t completed();
} // namespace tau::renderer::transfer
