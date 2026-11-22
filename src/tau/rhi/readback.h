#pragma once

#include "tau/defines.h"
#include "tau/rhi/vulkan.h"

#include <vector>

namespace tau::renderer
{
    using readback_ticket_t = u64_t;
    constexpr readback_ticket_t READBACK_NONE = 0;

    // call inside a pass callback, the image is in `layout` and goes back to it
    TAU_ENGINE_API readback_ticket_t record_readback(VkCommandBuffer cmd, VkImage image, VkImageLayout layout,
                                                     VkImageAspectFlags aspect, u32_t texel_size, VkRect2D rect);

    // true once done, `out` = texels row by row
    TAU_ENGINE_API bool fetch_readback(readback_ticket_t ticket, std::vector<u8_t>& out);

    namespace readback
    {
        void collect(u32_t frame_slot);
        void shutdown();
    } // namespace readback
} // namespace tau::renderer
