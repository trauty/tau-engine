#pragma once

#include "tau/defines.h"
#include "tau/rhi/vulkan.h"

// material data in one buffer read through its device address + an offset per material
// no cap: it doubles at a frame boundary (headroom), or at once when an allocation does not fit
namespace tau::renderer::material_heap
{
    void init(u32_t initial_size);
    void shutdown();

    // any thread; BINDLESS_NULL_HANDLE never
    TAU_ENGINE_API u32_t allocate(u32_t size);
    // reused once the GPU finished the frame being recorded
    TAU_ENGINE_API void free(u32_t offset, u32_t size);
    TAU_ENGINE_API void write(u32_t offset, const void* data, u32_t size);

    // changes when the heap grows: read it per frame / per draw, never keep it
    TAU_ENGINE_API VkDeviceAddress address();

    // frame boundary, before recording
    void begin_frame();

    struct stats_t
    {
        u32_t capacity = 0;
        u32_t used = 0;
        u32_t grown = 0;
    };
    TAU_ENGINE_API stats_t stats();

    // from the deletion queue
    void release(u32_t offset, u32_t size);
} // namespace tau::renderer::material_heap
