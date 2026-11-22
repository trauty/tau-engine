#pragma once

#include "tau/defines.h"
#include "tau/rhi/renderer_types.h"
#include "vulkan/vulkan_core.h"

#include <deque>
#include <mutex>
#include <vector>

namespace tau::renderer
{
    enum class resource_type_e
    {
        TEXTURE,
        BUFFER,
        STORAGE_IMAGE,
        COMMAND_BUFFER,
        PIPELINE,
        SWAPCHAIN,
        IMAGE_VIEW,
        SEMAPHORE,
        DESCRIPTOR_SET_LAYOUT,
        DESCRIPTOR_SET
    };

    constexpr u32_t SAMPLERS_BINDING_POINT = 0;
    constexpr u32_t TEXTURES_BINDING_POINT = 1;
    constexpr u32_t STORAGE_IMAGES_BINDING_POINT = 2;

    struct pending_resource_t
    {
        resource_type_e type;

        union
        {
            VkImage image;
            VkBuffer buffer;
        } handle;

        union
        {
            VkImageMemoryBarrier image_barrier;
            VkBufferMemoryBarrier buffer_barrier;
        } barrier;
    };

    // the two timelines a deletion can wait on
    // uploads signal their own transfer semaphore, so their command buffer and staging buffer are free once it passes
    // everything else is tagged with deletion_timeline_value() and waits on the render timeline
    enum class deletion_clock_e : u8_t
    {
        RENDER,
        TRANSFER,
    };

    struct deferred_delete_t
    {
        resource_type_e type;

        union
        {
            struct
            {
                VkImage image;
                VmaAllocation allocation;
                VkImageView view;
            } texture;

            struct
            {
                VkBuffer buffer;
                VmaAllocation allocation;
            } buffer;

            struct
            {
                VkCommandBuffer cmd;
                VkCommandPool pool;
            } cmd_buffer;

            struct
            {
                VkPipeline pipeline;
                VkPipelineLayout layout;
            } pipeline;

            VkSwapchainKHR swapchain;
            VkImageView image_view;
            VkSemaphore semaphore;
            VkDescriptorSetLayout descriptor_set_layout;

            struct
            {
                VkDescriptorPool pool;
                VkDescriptorSet set;
            } descriptor_set;
        } handle;

        u32_t bindless_id;
        u64_t gpu_timeline_value;
        deletion_clock_e clock = deletion_clock_e::RENDER;
    };

    struct descriptor_heap_t
    {
        std::vector<u32_t> free_indices;
        u32_t capacity;
        u32_t next_unused;

        void init(u32_t max_indices);
        TAU_ENGINE_API u32_t acquire();
        TAU_ENGINE_API void release(u32_t index);
    };

    constexpr u32_t MATERIAL_HEAP_SIZE = 24 * 1024 * 1024;

    struct free_block_t
    {
        u32_t offset;
        u32_t size;
    };

    struct material_heap_t
    {
        u32_t capacity;
        u32_t allocated_size = 0;

        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        void* mapped_mem = nullptr;

        VkDeviceAddress device_address = 0;

        std::vector<free_block_t> free_blocks;

        void init(u32_t max_size);
        void shutdown();

        u32_t allocate(u32_t size);
        void free(u32_t offset, u32_t size);

        void update(u32_t offset, const void* data, u32_t size);
    };

    struct resource_system_t
    {
        VkDescriptorSetLayout global_layout;
        VkDescriptorSetLayout frame_layout;

        VkDescriptorPool global_pool;
        VkDescriptorSet global_set;
        VkDescriptorSet frame_set;

        VkDescriptorPool instance_pool;

        descriptor_heap_t texture_heap;
        descriptor_heap_t storage_image_heap;

        material_heap_t material_heap;

        u32_t max_samplers = MAX_SAMPLERS;
        u32_t max_sampled_textures = MAX_SAMPLED_TEXTURES;
        u32_t max_storage_images = MAX_STORAGE_TEXTURES;
        u32_t material_heap_size = MATERIAL_HEAP_SIZE;

        VkSemaphore timeline_semaphore;
        u64_t timeline_value;

        std::vector<pending_resource_t> pending_acquires;
        std::mutex pending_mutex;

        std::deque<deferred_delete_t> deletion_queue;
        std::mutex deletion_mutex;

        void process_deletions(u64_t render_completed, u64_t transfer_completed);
    };

    TAU_ENGINE_API extern resource_system_t res_system;

    void init_resources();
    void shutdown_resources();
} // namespace tau::renderer