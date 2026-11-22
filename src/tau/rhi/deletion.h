#pragma once

#include "tau/defines.h"
#include "tau/rhi/vulkan.h"

// GPU objects destroyed once the timeline they were last used on has passed
// one FIFO per clock, each in timeline order: a frame pops only what finished
namespace tau::renderer::deletion
{
    enum class clock_e : u8_t
    {
        // the render timeline
        RENDER,
        // upload submits
        TRANSFER,
    };

    enum class kind_e : u8_t
    {
        IMAGE,
        BUFFER,
        IMAGE_VIEW,
        PIPELINE,
        SWAPCHAIN,
        SEMAPHORE,
        DESCRIPTOR_SET_LAYOUT,
        DESCRIPTOR_SET,
        COMMAND_BUFFER,
        BINDLESS,
        MATERIAL_RANGE,
    };

    struct entry_t
    {
        kind_e kind = kind_e::IMAGE;
        union
        {
            struct
            {
                VkImage image;
                VmaAllocation allocation;
                VkImageView view;
            } image;

            struct
            {
                VkBuffer buffer;
                VmaAllocation allocation;
            } buffer;

            struct
            {
                VkPipeline pipeline;
                VkPipelineLayout layout;
            } pipeline;

            struct
            {
                VkDescriptorPool pool;
                VkDescriptorSet set;
            } set;

            struct
            {
                VkCommandPool pool;
                VkCommandBuffer cmd;
            } cmd;

            struct
            {
                u32_t kind;
                u32_t index;
            } bindless;

            struct
            {
                u32_t offset;
                u32_t size;
            } range;

            VkImageView view;
            VkSwapchainKHR swapchain;
            VkSemaphore semaphore;
            VkDescriptorSetLayout set_layout;
        } handle = {};
        u64_t value = 0;
    };

    // value 0: after the frame being recorded (RENDER), required for TRANSFER
    TAU_ENGINE_API void retire(const entry_t& e, clock_e clock = clock_e::RENDER, u64_t value = 0);

    inline entry_t image(VkImage image, VmaAllocation allocation, VkImageView view = VK_NULL_HANDLE)
    {
        entry_t e{.kind = kind_e::IMAGE};
        e.handle.image = {image, allocation, view};
        return e;
    }

    inline entry_t buffer(VkBuffer buffer, VmaAllocation allocation)
    {
        entry_t e{.kind = kind_e::BUFFER};
        e.handle.buffer = {buffer, allocation};
        return e;
    }

    inline entry_t view(VkImageView view)
    {
        entry_t e{.kind = kind_e::IMAGE_VIEW};
        e.handle.view = view;
        return e;
    }

    inline entry_t pipeline(VkPipeline pipeline, VkPipelineLayout layout)
    {
        entry_t e{.kind = kind_e::PIPELINE};
        e.handle.pipeline = {pipeline, layout};
        return e;
    }

    inline entry_t set_layout(VkDescriptorSetLayout layout)
    {
        entry_t e{.kind = kind_e::DESCRIPTOR_SET_LAYOUT};
        e.handle.set_layout = layout;
        return e;
    }

    inline entry_t set(VkDescriptorPool pool, VkDescriptorSet set)
    {
        entry_t e{.kind = kind_e::DESCRIPTOR_SET};
        e.handle.set = {pool, set};
        return e;
    }

    // the value a retire made now waits for
    TAU_ENGINE_API u64_t render_value();

    // completed values of both timelines
    void process(u64_t render_done, u64_t transfer_done);
    // device idle: everything
    void flush();
    TAU_ENGINE_API u32_t pending();
} // namespace tau::renderer::deletion
