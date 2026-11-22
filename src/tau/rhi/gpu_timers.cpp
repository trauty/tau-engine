#include "gpu_timers.h"

#include "tau/log.h"
#include "tau/rhi/context.h"
#include "tau/rhi/renderer_constants.h"

#include <cstring>

namespace tau::renderer::gpu_timers
{
    namespace
    {
        // two queries per pass plus the frame's pair
        constexpr u32_t MAX_QUERIES = 1024;

        struct slot_t
        {
            VkQueryPool pool = VK_NULL_HANDLE;
            // name of each begin / end pair, pair 0 is the frame
            std::vector<const char*> names;
            u32_t used = 0;
            bool recorded = false;
        };

        struct total_t
        {
            std::string name;
            f64 sum_ms = 0.0;
            u32_t frames = 0;
            // this frame's share, a name can run more than once per frame
            f64 frame_ms = 0.0;
            bool seen = false;
        };

        slot_t slots[MAX_FRAMES_IN_FLIGHT];
        slot_t* cur = nullptr;
        std::vector<total_t> totals;
        std::vector<u64_t> stamps;
        bool is_enabled = false;
        bool is_accumulating = false;
        bool supported = false;

        total_t& total_of(const char* name)
        {
            for (total_t& t : totals)
            {
                if (t.name == name) { return t; }
            }
            return totals.emplace_back(total_t{.name = name});
        }

        void write(VkCommandBuffer cmd, VkPipelineStageFlags2 stage)
        { vkCmdWriteTimestamp2(cmd, stage, cur->pool, cur->used++); }
    } // namespace

    void enable(bool on) { is_enabled = on && supported; }
    bool enabled() { return is_enabled; }

    void accumulate(bool on)
    {
        if (on && !is_accumulating)
        {
            for (total_t& t : totals)
            {
                t.sum_ms = 0.0;
                t.frames = 0;
            }
        }
        is_accumulating = on;
    }

    std::vector<result_t> results()
    {
        std::vector<result_t> out;
        for (const total_t& t : totals)
        {
            if (t.frames > 0) { out.push_back({t.name, t.sum_ms / t.frames, t.frames}); }
        }
        return out;
    }

    void init()
    {
        u32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(ctx.physical_device, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(ctx.physical_device, &family_count, families.data());

        const u32_t graphics = ctx.queue_fam_indices.graphics_family.value();
        supported = families[graphics].timestampValidBits > 0 && ctx.properties.limits.timestampPeriod > 0.0f;
        if (!supported)
        {
            TAU_LOG_WARN("RENDERER", "The graphics queue has no timestamps, GPU pass timers are off");
            return;
        }

        const VkQueryPoolCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = MAX_QUERIES,
        };
        for (slot_t& slot : slots)
        {
            vkCreateQueryPool(ctx.device, &info, nullptr, &slot.pool);
            slot.names.reserve(MAX_QUERIES / 2);
        }
        stamps.resize(MAX_QUERIES);
        totals.reserve(64);
    }

    void shutdown()
    {
        for (slot_t& slot : slots)
        {
            if (slot.pool != VK_NULL_HANDLE) { vkDestroyQueryPool(ctx.device, slot.pool, nullptr); }
            slot = {};
        }
        totals.clear();
        cur = nullptr;
    }

    void begin_frame(VkCommandBuffer cmd, u32_t frame_slot)
    {
        cur = nullptr;
        if (!is_enabled) { return; }

        cur = &slots[frame_slot];
        cur->names.clear();
        cur->used = 0;
        cur->recorded = true;
        vkCmdResetQueryPool(cmd, cur->pool, 0, MAX_QUERIES);

        // the frame is pair 0, its end goes into query 1 at end_frame
        cur->names.push_back("frame");
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, cur->pool, 0);
        cur->used = 2;
    }

    void end_frame(VkCommandBuffer cmd)
    {
        if (!cur) { return; }
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, cur->pool, 1);
        cur = nullptr;
    }

    void begin_pass(VkCommandBuffer cmd, const char* name)
    {
        if (!cur || cur->used + 2 > MAX_QUERIES) { return; }

        cur->names.push_back(name ? name : "rg_pass");
        write(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    }

    void end_pass(VkCommandBuffer cmd)
    {
        // odd: its begin was written
        if (!cur || cur->used % 2 == 0) { return; }
        write(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    }

    void collect(u32_t frame_slot)
    {
        slot_t& slot = slots[frame_slot];
        if (!slot.recorded) { return; }
        slot.recorded = false;

        const u32_t count = slot.used;
        if (vkGetQueryPoolResults(ctx.device, slot.pool, 0, count, count * sizeof(u64_t), stamps.data(), sizeof(u64_t),
                                  VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        {
            return;
        }
        if (!is_accumulating) { return; }

        const f64 period_ms = static_cast<f64>(ctx.properties.limits.timestampPeriod) * 1e-6;
        for (std::size_t pair = 0; pair < slot.names.size() && pair * 2 + 1 < count; pair++)
        {
            total_t& t = total_of(slot.names[pair]);
            t.frame_ms += static_cast<f64>(stamps[pair * 2 + 1] - stamps[pair * 2]) * period_ms;
            t.seen = true;
        }
        for (total_t& t : totals)
        {
            if (!t.seen) { continue; }
            t.sum_ms += t.frame_ms;
            t.frames++;
            t.frame_ms = 0.0;
            t.seen = false;
        }
    }
} // namespace tau::renderer::gpu_timers
