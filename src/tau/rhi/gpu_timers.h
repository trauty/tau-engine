#pragma once

#include "tau/defines.h"
#include "tau/rhi/vulkan.h"

#include <string>
#include <vector>

// GPU time per render graph pass from timestamp queries, read back when the frame slot comes round again
namespace tau::renderer::gpu_timers
{
    struct result_t
    {
        std::string name;
        f64 mean_ms = 0.0;
        // frames that ran the pass
        u32_t frames = 0;
    };

    // queries are written from the next frame on
    TAU_ENGINE_API void enable(bool on);
    TAU_ENGINE_API bool enabled();

    // sums only while on (a warm-up runs before), turning it on clears them
    TAU_ENGINE_API void accumulate(bool on);

    // "frame" first, then the passes in first-seen order; a name used twice in a frame is summed
    TAU_ENGINE_API std::vector<result_t> results();

    void init();
    void shutdown();

    void begin_frame(VkCommandBuffer cmd, u32_t frame_slot);
    void end_frame(VkCommandBuffer cmd);
    void begin_pass(VkCommandBuffer cmd, const char* name);
    void end_pass(VkCommandBuffer cmd);

    // after the slot's frame finished
    void collect(u32_t frame_slot);
} // namespace tau::renderer::gpu_timers
