#include "bindless.h"

#include "tau/log.h"
#include "tau/rhi/deletion.h"
#include "tau/rhi/renderer.h"
#include "tau/rhi/samplers.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace tau::renderer::bindless
{
    namespace
    {
        constexpr u32_t SAMPLERS_BINDING = 0;
        constexpr u32_t SAMPLED_BINDING = 1;
        constexpr u32_t STORAGE_BINDING = 2;
        constexpr u32_t KIND_BIT = 0x80000000u;

        kind_e kind_of(bindless_h h) { return (h.generation & KIND_BIT) ? kind_e::STORAGE : kind_e::SAMPLED; }

        struct pool_t
        {
            u32_t capacity = 0;
            u32_t next_unused = 0;
            u32_t in_use = 0;
            std::vector<u32_t> free;
            std::vector<u32_t> generation;
        };

        std::mutex g_mutex;
        pool_t g_pools[static_cast<u32_t>(kind_e::COUNT)];
        u32_t g_max_samplers = 0;

        VkDescriptorSetLayout g_global_layout = VK_NULL_HANDLE;
        VkDescriptorSetLayout g_frame_layout = VK_NULL_HANDLE;
        VkDescriptorPool g_global_pool = VK_NULL_HANDLE;
        VkDescriptorPool g_instance_pool = VK_NULL_HANDLE;
        VkDescriptorSet g_global_set = VK_NULL_HANDLE;
        VkDescriptorSet g_frame_set = VK_NULL_HANDLE;
        VkSampler g_samplers[static_cast<u32_t>(sampler_type_e::COUNT)] = {};

        u32_t clamp(const char* name, u32_t wanted, u32_t set_limit, u32_t stage_limit)
        {
            const u32_t hw = std::min(set_limit, stage_limit);
            if (wanted <= hw) { return wanted; }
            TAU_LOG_WARN("VULKAN", "Bindless capacity '{}' clamped from {} to device limit {}", name, wanted, hw);
            return hw;
        }

        VkSampler make_sampler(VkFilter filter, VkSamplerAddressMode address)
        {
            const VkSamplerCreateInfo info = {
                .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                .magFilter = filter,
                .minFilter = filter,
                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                .addressModeU = address,
                .addressModeV = address,
                .addressModeW = address,
                .anisotropyEnable = VK_TRUE,
                .maxAnisotropy = ctx.properties.limits.maxSamplerAnisotropy,
                .compareOp = VK_COMPARE_OP_ALWAYS,
                .maxLod = VK_LOD_CLAMP_NONE,
                .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
            };
            VkSampler s = VK_NULL_HANDLE;
            VK_CHECK(vkCreateSampler(ctx.device, &info, nullptr, &s));
            return s;
        }

        // white border = farthest depth: outside the map reads as lit
        VkSampler make_shadow_sampler()
        {
            const VkSamplerCreateInfo info = {
                .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                .magFilter = VK_FILTER_LINEAR,
                .minFilter = VK_FILTER_LINEAR,
                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                .maxAnisotropy = 1.0f,
                .compareEnable = VK_TRUE,
                .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
                .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
            };
            VkSampler s = VK_NULL_HANDLE;
            VK_CHECK(vkCreateSampler(ctx.device, &info, nullptr, &s));
            return s;
        }

        bindless_h add(kind_e kind, VkImageView view)
        {
            std::scoped_lock lock(g_mutex);
            pool_t& pool = g_pools[static_cast<u32_t>(kind)];

            u32_t index = BINDLESS_NULL_HANDLE;
            if (!pool.free.empty())
            {
                index = pool.free.back();
                pool.free.pop_back();
            }
            else if (pool.next_unused < pool.capacity) { index = pool.next_unused++; }
            if (index == BINDLESS_NULL_HANDLE)
            {
                TAU_LOG_ERROR("VULKAN", "Bindless {} table full ({})", kind == kind_e::SAMPLED ? "sampled" : "storage",
                              pool.capacity);
                return {};
            }
            pool.in_use++;

            const bool storage = kind == kind_e::STORAGE;
            const VkDescriptorImageInfo info = {
                .imageView = view,
                .imageLayout = storage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            };
            const VkWriteDescriptorSet write = {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = g_global_set,
                .dstBinding = storage ? STORAGE_BINDING : SAMPLED_BINDING,
                .dstArrayElement = index,
                .descriptorCount = 1,
                .descriptorType = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                .pImageInfo = &info,
            };
            // the set is externally synchronised: writes stay under the lock
            vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
            return {index, pool.generation[index]};
        }
    } // namespace

    bool init(const limits_t& wanted)
    {
        VkPhysicalDeviceDescriptorIndexingProperties idx = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
        VkPhysicalDeviceProperties2 props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idx};
        vkGetPhysicalDeviceProperties2(ctx.physical_device, &props);

        g_max_samplers = clamp("samplers", wanted.samplers, idx.maxDescriptorSetUpdateAfterBindSamplers,
                               idx.maxPerStageDescriptorUpdateAfterBindSamplers);
        const u32_t sampled =
            clamp("sampled_textures", wanted.sampled, idx.maxDescriptorSetUpdateAfterBindSampledImages,
                  idx.maxPerStageDescriptorUpdateAfterBindSampledImages);
        const u32_t storage = clamp("storage_images", wanted.storage, idx.maxDescriptorSetUpdateAfterBindStorageImages,
                                    idx.maxPerStageDescriptorUpdateAfterBindStorageImages);

        pool_t& sampled_pool = g_pools[static_cast<u32_t>(kind_e::SAMPLED)];
        pool_t& storage_pool = g_pools[static_cast<u32_t>(kind_e::STORAGE)];
        sampled_pool = {.capacity = sampled};
        storage_pool = {.capacity = storage};
        sampled_pool.generation.assign(sampled, 0);
        // kind in the generation's top bit
        storage_pool.generation.assign(storage, KIND_BIT);

        const VkDescriptorSetLayoutBinding bindings[3] = {
            {SAMPLERS_BINDING, VK_DESCRIPTOR_TYPE_SAMPLER,       g_max_samplers, VK_SHADER_STAGE_ALL, nullptr},
            {SAMPLED_BINDING,  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sampled,        VK_SHADER_STAGE_ALL, nullptr},
            {STORAGE_BINDING,  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storage,        VK_SHADER_STAGE_ALL, nullptr},
        };
        const VkDescriptorBindingFlags flags[3] = {
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        };
        const VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .bindingCount = 3,
            .pBindingFlags = flags,
        };
        const VkDescriptorSetLayoutCreateInfo layout_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = &flags_info,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
            .bindingCount = 3,
            .pBindings = bindings,
        };
        VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &layout_info, nullptr, &g_global_layout));

        const VkDescriptorSetLayoutBinding frame_binding = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
                                                            VK_SHADER_STAGE_ALL, nullptr};
        const VkDescriptorSetLayoutCreateInfo frame_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1,
            .pBindings = &frame_binding,
        };
        VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &frame_info, nullptr, &g_frame_layout));

        const VkDescriptorPoolSize global_sizes[4] = {
            {VK_DESCRIPTOR_TYPE_SAMPLER,                g_max_samplers},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          sampled       },
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          storage       },
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1             },
        };
        const VkDescriptorPoolCreateInfo global_pool_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
            .maxSets = 2,
            .poolSizeCount = 4,
            .pPoolSizes = global_sizes,
        };
        VK_CHECK(vkCreateDescriptorPool(ctx.device, &global_pool_info, nullptr, &g_global_pool));

        const VkDescriptorSetLayout set_layouts[] = {g_global_layout, g_frame_layout};
        const VkDescriptorSetAllocateInfo alloc = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = g_global_pool,
            .descriptorSetCount = 2,
            .pSetLayouts = set_layouts,
        };
        VkDescriptorSet sets[2];
        VK_CHECK(vkAllocateDescriptorSets(ctx.device, &alloc, sets));
        g_global_set = sets[0];
        g_frame_set = sets[1];

        const VkDescriptorPoolSize instance_sizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1024},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1024},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1024},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  1024},
        };
        const VkDescriptorPoolCreateInfo instance_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
            .maxSets = 2048,
            .poolSizeCount = 4,
            .pPoolSizes = instance_sizes,
        };
        VK_CHECK(vkCreateDescriptorPool(ctx.device, &instance_info, nullptr, &g_instance_pool));

        // slots fixed by shader_shared.h
        g_samplers[static_cast<u32_t>(sampler_type_e::LINEAR_REPEAT)] =
            make_sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT);
        g_samplers[static_cast<u32_t>(sampler_type_e::LINEAR_CLAMP)] =
            make_sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        g_samplers[static_cast<u32_t>(sampler_type_e::NEAREST_REPEAT)] =
            make_sampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT);
        g_samplers[static_cast<u32_t>(sampler_type_e::NEAREST_CLAMP)] =
            make_sampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        g_samplers[static_cast<u32_t>(sampler_type_e::SHADOW_CMP)] = make_shadow_sampler();

        constexpr u32_t COUNT = static_cast<u32_t>(sampler_type_e::COUNT);
        VkDescriptorImageInfo sampler_infos[COUNT] = {};
        for (u32_t i = 0; i < COUNT; i++) { sampler_infos[i].sampler = g_samplers[i]; }
        const VkWriteDescriptorSet sampler_write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = g_global_set,
            .dstBinding = SAMPLERS_BINDING,
            .descriptorCount = COUNT,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
            .pImageInfo = sampler_infos,
        };
        vkUpdateDescriptorSets(ctx.device, 1, &sampler_write, 0, nullptr);
        return true;
    }

    void shutdown()
    {
        for (VkSampler& s : g_samplers)
        {
            if (s != VK_NULL_HANDLE) { vkDestroySampler(ctx.device, s, nullptr); }
            s = VK_NULL_HANDLE;
        }
        vkDestroyDescriptorPool(ctx.device, g_global_pool, nullptr);
        vkDestroyDescriptorPool(ctx.device, g_instance_pool, nullptr);
        vkDestroyDescriptorSetLayout(ctx.device, g_global_layout, nullptr);
        vkDestroyDescriptorSetLayout(ctx.device, g_frame_layout, nullptr);
        for (pool_t& p : g_pools) { p = {}; }
    }

    bindless_h add_sampled(VkImageView view) { return add(kind_e::SAMPLED, view); }
    bindless_h add_storage(VkImageView view) { return add(kind_e::STORAGE, view); }

    void remove(bindless_h h)
    {
        if (!h.valid()) { return; }
        const kind_e kind = kind_of(h);
        {
            std::scoped_lock lock(g_mutex);
            pool_t& pool = g_pools[static_cast<u32_t>(kind)];
            if (h.index >= pool.capacity || pool.generation[h.index] != h.generation)
            {
                TAU_LOG_ERROR("VULKAN", "Bindless handle {} removed twice or stale", h.index);
                return;
            }
            // stale from here on, reusable once the GPU is done
            pool.generation[h.index] = ((pool.generation[h.index] + 1) & ~KIND_BIT) | (h.generation & KIND_BIT);
        }
        deletion::entry_t e{.kind = deletion::kind_e::BINDLESS};
        e.handle.bindless = {static_cast<u32_t>(kind), h.index};
        deletion::retire(e);
    }

    u32_t index(bindless_h h)
    {
#ifndef NDEBUG
        if (h.valid())
        {
            const kind_e kind = kind_of(h);
            std::scoped_lock lock(g_mutex);
            const pool_t& pool = g_pools[static_cast<u32_t>(kind)];
            if (h.index < pool.capacity && pool.generation[h.index] != h.generation)
            {
                TAU_LOG_ERROR("VULKAN", "Stale bindless handle {} used", h.index);
            }
        }
#endif
        return h.index;
    }

    void release(kind_e kind, u32_t index)
    {
        std::scoped_lock lock(g_mutex);
        pool_t& pool = g_pools[static_cast<u32_t>(kind)];
        pool.free.push_back(index);
        pool.in_use--;
    }

    VkDescriptorSetLayout global_layout() { return g_global_layout; }
    VkDescriptorSet global_set() { return g_global_set; }
    VkDescriptorSetLayout frame_layout() { return g_frame_layout; }
    VkDescriptorSet frame_set() { return g_frame_set; }
    VkDescriptorPool instance_pool() { return g_instance_pool; }
    VkSampler sampler(u32_t slot)
    { return slot < static_cast<u32_t>(sampler_type_e::COUNT) ? g_samplers[slot] : VK_NULL_HANDLE; }

    stats_t stats()
    {
        std::scoped_lock lock(g_mutex);
        stats_t s;
        for (u32_t k = 0; k < static_cast<u32_t>(kind_e::COUNT); k++)
        {
            s.in_use[k] = g_pools[k].in_use;
            s.capacity[k] = g_pools[k].capacity;
        }
        return s;
    }
} // namespace tau::renderer::bindless
