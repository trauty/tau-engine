#pragma once

#include "tau/defines.h"
#include "tau/rhi/vulkan.h"

namespace tau::renderer
{
    // host data the gpu reads this frame, by device address
    TAU_ENGINE_API VkDeviceAddress upload_frame_data(const void* data, u64_t size);

    namespace frame_upload
    {
        void reset(u32_t frame_slot);
        void shutdown();
    } // namespace frame_upload
} // namespace tau::renderer
