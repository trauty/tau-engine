#include "transfer.h"

#include "tau/rhi/renderer.h"

#include <atomic>
#include <mutex>
#include <vector>

namespace tau::renderer::transfer
{
    namespace
    {
        // the pool and its command buffers are externally synchronised: held from begin() to submit()
        std::mutex g_mutex;
        VkCommandPool g_pool = VK_NULL_HANDLE;
        struct in_flight_t
        {
            VkCommandBuffer cmd;
            u64_t value;
        };
        std::vector<in_flight_t> g_in_flight;
        VkSemaphore g_timeline = VK_NULL_HANDLE;
        std::atomic<u64_t> g_value = 0;

        std::mutex g_acquire_mutex;
        std::vector<VkImageMemoryBarrier> g_image_acquires;
        std::vector<VkBufferMemoryBarrier> g_buffer_acquires;
    } // namespace

    void init()
    {
        const VkCommandPoolCreateInfo pool_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = ctx.queue_fam_indices.transfer_family.value(),
        };
        VK_CHECK(vkCreateCommandPool(ctx.device, &pool_info, nullptr, &g_pool));

        const VkSemaphoreTypeCreateInfo type_info = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        };
        const VkSemaphoreCreateInfo sem_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type_info};
        VK_CHECK(vkCreateSemaphore(ctx.device, &sem_info, nullptr, &g_timeline));
        g_value = 0;
    }

    void shutdown()
    {
        g_in_flight.clear();
        vkDestroyCommandPool(ctx.device, g_pool, nullptr);
        vkDestroySemaphore(ctx.device, g_timeline, nullptr);
        g_pool = VK_NULL_HANDLE;
        g_timeline = VK_NULL_HANDLE;
    }

    VkCommandBuffer begin()
    {
        const VkCommandBufferAllocateInfo alloc = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = g_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        g_mutex.lock();

        // finished uploads give their command buffers back
        const u64_t done = completed();
        for (size_t i = 0; i < g_in_flight.size();)
        {
            if (g_in_flight[i].value > done)
            {
                i++;
                continue;
            }
            vkFreeCommandBuffers(ctx.device, g_pool, 1, &g_in_flight[i].cmd);
            g_in_flight[i] = g_in_flight.back();
            g_in_flight.pop_back();
        }

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateCommandBuffers(ctx.device, &alloc, &cmd));
        const VkCommandBufferBeginInfo begin_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        VK_CHECK(vkBeginCommandBuffer(cmd, &begin_info));
        return cmd;
    }

    u64_t submit(VkCommandBuffer cmd)
    {
        VK_CHECK(vkEndCommandBuffer(cmd));

        // signal values rise in submission order: value and submit under the lock begin() took
        u64_t value = 0;
        {
            std::scoped_lock lock(std::adopt_lock, g_mutex);
            value = g_value.load() + 1;
            const VkTimelineSemaphoreSubmitInfo timeline_info = {
                .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                .signalSemaphoreValueCount = 1,
                .pSignalSemaphoreValues = &value,
            };
            const VkSubmitInfo submit_info = {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .pNext = &timeline_info,
                .commandBufferCount = 1,
                .pCommandBuffers = &cmd,
                .signalSemaphoreCount = 1,
                .pSignalSemaphores = &g_timeline,
            };
            VK_CHECK(vkQueueSubmit(ctx.transfer_queue, 1, &submit_info, VK_NULL_HANDLE));
            g_value.store(value);
            g_in_flight.push_back({cmd, value});
        }
        return value;
    }

    void acquire(const VkImageMemoryBarrier& barrier)
    {
        std::scoped_lock lock(g_acquire_mutex);
        g_image_acquires.push_back(barrier);
    }

    void acquire(const VkBufferMemoryBarrier& barrier)
    {
        std::scoped_lock lock(g_acquire_mutex);
        g_buffer_acquires.push_back(barrier);
    }

    void flush_acquires(VkCommandBuffer cmd)
    {
        std::scoped_lock lock(g_acquire_mutex);
        if (g_image_acquires.empty() && g_buffer_acquires.empty()) { return; }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, nullptr,
                             static_cast<u32_t>(g_buffer_acquires.size()), g_buffer_acquires.data(),
                             static_cast<u32_t>(g_image_acquires.size()), g_image_acquires.data());
        g_image_acquires.clear();
        g_buffer_acquires.clear();
    }

    VkSemaphore timeline() { return g_timeline; }
    u64_t last_value() { return g_value.load(); }

    u64_t completed()
    {
        u64_t v = 0;
        vkGetSemaphoreCounterValue(ctx.device, g_timeline, &v);
        return v;
    }
} // namespace tau::renderer::transfer
