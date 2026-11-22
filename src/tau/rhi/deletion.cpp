#include "deletion.h"

#include "tau/rhi/bindless.h"
#include "tau/rhi/material_heap.h"
#include "tau/rhi/renderer.h"

#include <deque>
#include <mutex>

namespace tau::renderer::deletion
{
    namespace
    {
        std::mutex g_mutex;
        std::deque<entry_t> g_queues[2];

        void destroy(const entry_t& e)
        {
            const auto& h = e.handle;
            switch (e.kind)
            {
            case kind_e::IMAGE:
                if (h.image.view != VK_NULL_HANDLE) { vkDestroyImageView(ctx.device, h.image.view, nullptr); }
                if (h.image.image != VK_NULL_HANDLE)
                {
                    vmaDestroyImage(ctx.allocator, h.image.image, h.image.allocation);
                }
                break;
            case kind_e::BUFFER: vmaDestroyBuffer(ctx.allocator, h.buffer.buffer, h.buffer.allocation); break;
            case kind_e::IMAGE_VIEW: vkDestroyImageView(ctx.device, h.view, nullptr); break;
            case kind_e::PIPELINE:
                if (h.pipeline.pipeline != VK_NULL_HANDLE)
                {
                    vkDestroyPipeline(ctx.device, h.pipeline.pipeline, nullptr);
                }
                if (h.pipeline.layout != VK_NULL_HANDLE)
                {
                    vkDestroyPipelineLayout(ctx.device, h.pipeline.layout, nullptr);
                }
                break;
            case kind_e::SWAPCHAIN: vkDestroySwapchainKHR(ctx.device, h.swapchain, nullptr); break;
            case kind_e::SEMAPHORE: vkDestroySemaphore(ctx.device, h.semaphore, nullptr); break;
            case kind_e::DESCRIPTOR_SET_LAYOUT: vkDestroyDescriptorSetLayout(ctx.device, h.set_layout, nullptr); break;
            case kind_e::DESCRIPTOR_SET: vkFreeDescriptorSets(ctx.device, h.set.pool, 1, &h.set.set); break;
            case kind_e::COMMAND_BUFFER: vkFreeCommandBuffers(ctx.device, h.cmd.pool, 1, &h.cmd.cmd); break;
            case kind_e::BINDLESS:
                bindless::release(static_cast<bindless::kind_e>(h.bindless.kind), h.bindless.index);
                break;
            case kind_e::MATERIAL_RANGE: material_heap::release(h.range.offset, h.range.size); break;
            }
        }
    } // namespace

    // one frame of slack: other threads read the value while a frame is being submitted
    u64_t render_value() { return ctx.timeline_value + 2; }

    void retire(const entry_t& e, clock_e clock, u64_t value)
    {
        entry_t entry = e;
        entry.value = value != 0 ? value : render_value();
        std::scoped_lock lock(g_mutex);
        g_queues[static_cast<u32_t>(clock)].push_back(entry);
    }

    void process(u64_t render_done, u64_t transfer_done)
    {
        const u64_t done[2] = {render_done, transfer_done};
        for (u32_t c = 0; c < 2; c++)
        {
            // destroyed outside the lock: bindless / heap releases take their own
            while (true)
            {
                entry_t e;
                {
                    std::scoped_lock lock(g_mutex);
                    std::deque<entry_t>& q = g_queues[c];
                    if (q.empty() || q.front().value > done[c]) { break; }
                    e = q.front();
                    q.pop_front();
                }
                destroy(e);
            }
        }
    }

    void flush() { process(~0ull, ~0ull); }

    u32_t pending()
    {
        std::scoped_lock lock(g_mutex);
        return static_cast<u32_t>(g_queues[0].size() + g_queues[1].size());
    }
} // namespace tau::renderer::deletion
