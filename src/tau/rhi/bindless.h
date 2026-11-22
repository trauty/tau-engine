#pragma once

#include "tau/defines.h"
#include "tau/rhi/renderer_constants.h"
#include "tau/rhi/vulkan.h"

namespace tau::renderer
{
    // shaders read the index; the generation catches a handle kept past its remove()
    struct bindless_h
    {
        u32_t index = BINDLESS_NULL_HANDLE;
        u32_t generation = 0;
        bool valid() const { return index != BINDLESS_NULL_HANDLE; }
    };
} // namespace tau::renderer

// the global descriptor set: samplers (fixed slots), sampled images, storage images; every id goes through here
namespace tau::renderer::bindless
{
    enum class kind_e : u8_t
    {
        SAMPLED,
        STORAGE,
        COUNT,
    };

    struct limits_t
    {
        u32_t samplers = MAX_SAMPLERS;
        u32_t sampled = MAX_SAMPLED_TEXTURES;
        u32_t storage = MAX_STORAGE_TEXTURES;
    };

    // clamped to the device's update-after-bind limits
    bool init(const limits_t& wanted);
    void shutdown();

    // any thread
    TAU_ENGINE_API bindless_h add_sampled(VkImageView view);
    TAU_ENGINE_API bindless_h add_storage(VkImageView view);
    // the id is reused once the GPU finished the frame being recorded
    TAU_ENGINE_API void remove(bindless_h h);
    // debug builds log a removed handle
    TAU_ENGINE_API u32_t index(bindless_h h);

    TAU_ENGINE_API VkDescriptorSetLayout global_layout();
    TAU_ENGINE_API VkDescriptorSet global_set();
    // set 1: the per view globals, one dynamic uniform buffer
    TAU_ENGINE_API VkDescriptorSetLayout frame_layout();
    TAU_ENGINE_API VkDescriptorSet frame_set();
    // shader instances' own sets
    TAU_ENGINE_API VkDescriptorPool instance_pool();
    // by its fixed slot (sampler_type_e)
    TAU_ENGINE_API VkSampler sampler(u32_t slot);

    struct stats_t
    {
        u32_t in_use[static_cast<u32_t>(kind_e::COUNT)] = {};
        u32_t capacity[static_cast<u32_t>(kind_e::COUNT)] = {};
    };
    TAU_ENGINE_API stats_t stats();

    // from the deletion queue
    void release(kind_e kind, u32_t index);
} // namespace tau::renderer::bindless
