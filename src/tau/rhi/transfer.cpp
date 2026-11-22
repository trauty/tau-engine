#include "transfer.h"

#include "tau/log.h"
#include "tau/profiling.h"
#include "tau/rhi/context.h"
#include "tau/rhi/deletion.h"
#include "tau/rhi/device.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>
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

        constexpr VkDeviceSize RING_SIZE = 256ull << 20;
        constexpr VkDeviceSize RING_ALIGN = 16;
        constexpr u64_t PENDING = ~0ull;

        // regions in allocation order, freed from the front once their upload passed
        struct region_t
        {
            VkDeviceSize begin = 0;
            VkDeviceSize end = 0;
            u64_t id = 0;
            // the upload's timeline value, PENDING until submitted, 0 when discarded
            u64_t value = PENDING;
        };

        struct ring_t
        {
            std::mutex mutex;
            VkBuffer buffer = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            u8_t* data = nullptr;
            std::deque<region_t> regions;
            u64_t next_id = 1;
            // a region was submitted or discarded
            std::condition_variable changed;
        };
        ring_t g_ring;

        // set from begin() to submit(): staging inside it would wait while holding the pool
        thread_local bool t_recording = false;

        // locked
        void reclaim(u64_t done)
        {
            while (!g_ring.regions.empty())
            {
                const region_t& front = g_ring.regions.front();
                if (front.value == PENDING || front.value > done) { break; }
                g_ring.regions.pop_front();
            }
        }

        // locked; the start of a free range of `size`, or RING_SIZE
        VkDeviceSize fit(VkDeviceSize size)
        {
            if (g_ring.regions.empty()) { return 0; }

            const region_t& front = g_ring.regions.front();
            const region_t& back = g_ring.regions.back();
            const VkDeviceSize head = (back.end + RING_ALIGN - 1) & ~(RING_ALIGN - 1);
            const bool wrapped = back.begin < front.begin;

            if (wrapped) { return head + size <= front.begin ? head : RING_SIZE; }
            if (head + size <= RING_SIZE) { return head; }
            return size <= front.begin ? 0 : RING_SIZE;
        }

        region_t* find_region(u64_t id)
        {
            if (g_ring.regions.empty() || id < g_ring.regions.front().id) { return nullptr; }
            const u64_t index = id - g_ring.regions.front().id;
            return index < g_ring.regions.size() ? &g_ring.regions[index] : nullptr;
        }

        void wait_value(u64_t value)
        {
            const VkSemaphoreWaitInfo wait = {
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                .semaphoreCount = 1,
                .pSemaphores = &g_timeline,
                .pValues = &value,
            };
            vkWaitSemaphores(ctx.device, &wait, 1'000'000);
        }
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

        const VkBufferCreateInfo ring_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = RING_SIZE,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        };
        const VmaAllocationCreateInfo ring_alloc = {
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        };
        VmaAllocationInfo mapped;
        VK_CHECK(vmaCreateBuffer(ctx.allocator, &ring_info, &ring_alloc, &g_ring.buffer, &g_ring.allocation, &mapped));
        g_ring.data = static_cast<u8_t*>(mapped.pMappedData);
        g_ring.regions.clear();
        device::name(g_ring.buffer, VK_OBJECT_TYPE_BUFFER, "Staging ring");
    }

    void shutdown()
    {
        vmaDestroyBuffer(ctx.allocator, g_ring.buffer, g_ring.allocation);
        g_ring.buffer = VK_NULL_HANDLE;
        g_ring.allocation = VK_NULL_HANDLE;
        g_ring.data = nullptr;
        g_ring.regions.clear();
        g_in_flight.clear();
        vkDestroyCommandPool(ctx.device, g_pool, nullptr);
        vkDestroySemaphore(ctx.device, g_timeline, nullptr);
        g_pool = VK_NULL_HANDLE;
        g_timeline = VK_NULL_HANDLE;
    }

    staging_t stage(VkDeviceSize size)
    {
        ZoneScoped;

        if (t_recording)
        {
            TAU_LOG_ERROR("TRANSFER", "stage() between begin() and submit() can wait forever, stage first");
        }

        // larger than the ring: its own buffer, retired at submit
        if (size > RING_SIZE)
        {
            const VkBufferCreateInfo info = {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = size,
                .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            };
            const VmaAllocationCreateInfo alloc = {
                .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            };
            staging_t own;
            VmaAllocation allocation = VK_NULL_HANDLE;
            VmaAllocationInfo mapped;
            if (vmaCreateBuffer(ctx.allocator, &info, &alloc, &own.buffer, &allocation, &mapped) != VK_SUCCESS)
            {
                return {};
            }
            own.data = static_cast<u8_t*>(mapped.pMappedData);
            own.size = size;
            own.own_allocation = allocation;
            return own;
        }

        for (;;)
        {
            u64_t wait_for = 0;
            {
                std::unique_lock lock(g_ring.mutex);
                reclaim(completed());

                const VkDeviceSize at = fit(size);
                if (at != RING_SIZE)
                {
                    const u64_t id = g_ring.next_id++;
                    g_ring.regions.push_back({at, at + size, id, PENDING});
                    return {g_ring.buffer, at, g_ring.data + at, size, id};
                }

                // the oldest region holds the space: its submit, or its upload
                wait_for = g_ring.regions.front().value;
                if (wait_for == PENDING)
                {
                    g_ring.changed.wait_for(lock, std::chrono::milliseconds(1));
                    continue;
                }
            }
            wait_value(wait_for);
        }
    }

    void discard(const staging_t& staging)
    {
        if (staging.buffer == VK_NULL_HANDLE) { return; }
        if (staging.buffer != g_ring.buffer)
        {
            vmaDestroyBuffer(ctx.allocator, staging.buffer, staging.own_allocation);
            return;
        }

        {
            std::scoped_lock lock(g_ring.mutex);
            if (region_t* region = find_region(staging.id)) { region->value = 0; }
        }
        g_ring.changed.notify_all();
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
        t_recording = true;
        return cmd;
    }

    u64_t submit(VkCommandBuffer cmd, const staging_t* staging)
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
        t_recording = false;

        if (staging && staging->buffer != VK_NULL_HANDLE)
        {
            if (staging->buffer != g_ring.buffer)
            {
                deletion::retire(deletion::buffer(staging->buffer, staging->own_allocation),
                                 deletion::clock_e::TRANSFER, value);
            }
            else
            {
                {
                    std::scoped_lock lock(g_ring.mutex);
                    if (region_t* region = find_region(staging->id)) { region->value = value; }
                }
                g_ring.changed.notify_all();
            }
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
