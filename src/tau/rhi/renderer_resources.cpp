#include "renderer_resources.h"

#include "tau/defines.h"
#include "tau/log.h"
#include "tau/rhi/renderer.h"
#include "tau/rhi/renderer_types.h"
#include "tau/rhi/vulkan.h"
#include "vulkan/vulkan_core.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <mutex>

namespace tau::renderer
{
    resource_system_t res_system;

    void descriptor_heap_t::init(u32_t max_indices)
    {
        capacity = max_indices;
        next_unused = 0;
        free_indices.reserve(1024);
    }

    u32_t descriptor_heap_t::acquire()
    {
        if (!free_indices.empty())
        {
            u32_t idx = free_indices.back();
            free_indices.pop_back();
            return idx;
        }

        return (next_unused < capacity) ? next_unused++ : BINDLESS_NULL_HANDLE;
    }

    void descriptor_heap_t::release(u32_t index) { free_indices.push_back(index); }

    void material_heap_t::init(u32_t max_size)
    {
        capacity = max_size;
        allocated_size = 0;

        VkBufferCreateInfo buffer_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = capacity,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        };

        VmaAllocationCreateInfo alloc_info = {
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };

        VmaAllocationInfo mat_alloc_info;
        VK_CHECK(vmaCreateBuffer(ctx.allocator, &buffer_info, &alloc_info, &buffer, &allocation, &mat_alloc_info));

        mapped_mem = mat_alloc_info.pMappedData;

        device_address = get_buffer_address(buffer);
    }

    void material_heap_t::shutdown()
    {
        if (buffer != VK_NULL_HANDLE) { vmaDestroyBuffer(ctx.allocator, buffer, allocation); }
    }

    u32_t material_heap_t::allocate(u32_t size)
    {
        u32_t aligned_size = (size + 255) & ~255;

        for (auto it = free_blocks.begin(); it != free_blocks.end(); ++it)
        {
            if (it->size >= aligned_size)
            {
                u32_t offset = it->offset;

                if (it->size == aligned_size) { free_blocks.erase(it); }
                else
                {
                    it->offset += aligned_size;
                    it->size -= aligned_size;
                }

                return offset;
            }
        }

        if (allocated_size + aligned_size > capacity)
        {
            TAU_LOG_FATAL("MATERIAL", "Material heap out of memory");
            return BINDLESS_NULL_HANDLE;
        }

        u32_t offset = allocated_size;
        allocated_size += aligned_size;
        return offset;
    }

    void material_heap_t::free(u32_t offset, u32_t size)
    {
        u32_t aligned_size = (size + 255) & ~255;

        free_blocks.push_back({offset, aligned_size});

        std::sort(free_blocks.begin(), free_blocks.end(), [](const free_block_t& a_block, const free_block_t& b_block)
                  { return a_block.offset < b_block.offset; });

        for (size_t i = 0; i < free_blocks.size() - 1;)
        {
            u32_t cur_end = free_blocks[i].offset + free_blocks[i].size;
            u32_t next_start = free_blocks[i + 1].offset;

            if (cur_end == next_start)
            {
                free_blocks[i].size += free_blocks[i + 1].size;
                free_blocks.erase(free_blocks.begin() + i + 1);
            }
            else
            {
                i++;
            }
        }
    }

    void material_heap_t::update(u32_t offset, const void* data, u32_t size)
    {
        if (offset != BINDLESS_NULL_HANDLE && mapped_mem)
        {
            std::memcpy(static_cast<u8*>(mapped_mem) + offset, data, size);
        }
    }

    void init_resources()
    {
        VkDescriptorSetLayoutBinding bindings[3] = {};

        bindings[0] = {0, VK_DESCRIPTOR_TYPE_SAMPLER, res_system.max_samplers, VK_SHADER_STAGE_ALL, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, res_system.max_sampled_textures, VK_SHADER_STAGE_ALL,
                       nullptr};
        bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, res_system.max_storage_images, VK_SHADER_STAGE_ALL,
                       nullptr};

        VkDescriptorBindingFlags flags[3] = {
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        };

        VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .bindingCount = 3,
            .pBindingFlags = flags,
        };

        VkDescriptorSetLayoutCreateInfo layout_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = &flags_info,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
            .bindingCount = 3,
            .pBindings = bindings,
        };

        VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &layout_info, nullptr, &res_system.global_layout));

        VkDescriptorSetLayoutBinding frame_binding = {
            0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_ALL, nullptr,
        };

        VkDescriptorSetLayoutCreateInfo frame_layout_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1,
            .pBindings = &frame_binding,
        };

        VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &frame_layout_info, nullptr, &res_system.frame_layout));

        VkDescriptorPoolSize global_pool_sizes[4] = {
            {VK_DESCRIPTOR_TYPE_SAMPLER,                res_system.max_samplers        },
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          res_system.max_sampled_textures},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          res_system.max_storage_images  },
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1                              },
        };

        VkDescriptorPoolCreateInfo global_pool_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
            .maxSets = 2,
            .poolSizeCount = 4,
            .pPoolSizes = global_pool_sizes,
        };

        VK_CHECK(vkCreateDescriptorPool(ctx.device, &global_pool_info, nullptr, &res_system.global_pool));

        VkDescriptorSetLayout alloc_layouts[] = {res_system.global_layout, res_system.frame_layout};
        VkDescriptorSetAllocateInfo alloc_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = res_system.global_pool,
            .descriptorSetCount = 2,
            .pSetLayouts = alloc_layouts,
        };

        VkDescriptorSet allocated_sets[2];
        VK_CHECK(vkAllocateDescriptorSets(ctx.device, &alloc_info, allocated_sets));
        res_system.global_set = allocated_sets[0];
        res_system.frame_set = allocated_sets[1];

        VkDescriptorPoolSize instance_pool_sizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1024},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1024},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1024},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  1024},
        };

        VkDescriptorPoolCreateInfo instance_pool_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
            .maxSets = 2048,
            .poolSizeCount = 4,
            .pPoolSizes = instance_pool_sizes,
        };

        VK_CHECK(vkCreateDescriptorPool(ctx.device, &instance_pool_info, nullptr, &res_system.instance_pool));

        res_system.texture_heap.init(res_system.max_sampled_textures);
        res_system.storage_image_heap.init(res_system.max_storage_images);

        res_system.material_heap.init(res_system.material_heap_size);

        VkSemaphoreTypeCreateInfo sem_type_info = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue = 0,
        };

        VkSemaphoreCreateInfo sem_info = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &sem_type_info,
        };

        VK_CHECK(vkCreateSemaphore(ctx.device, &sem_info, nullptr, &res_system.timeline_semaphore));
    }

    void shutdown_resources()
    {
        vkDestroyDescriptorPool(ctx.device, res_system.global_pool, nullptr);
        vkDestroyDescriptorPool(ctx.device, res_system.instance_pool, nullptr);
        vkDestroyDescriptorSetLayout(ctx.device, res_system.global_layout, nullptr);
        vkDestroyDescriptorSetLayout(ctx.device, res_system.frame_layout, nullptr);

        res_system.texture_heap.free_indices.clear();
        res_system.storage_image_heap.free_indices.clear();

        res_system.material_heap.shutdown();

        vkDestroySemaphore(ctx.device, res_system.timeline_semaphore, nullptr);
    }

    void resource_system_t::process_deletions(u64_t render_completed, u64_t transfer_completed)
    {
        std::scoped_lock lock(deletion_mutex);

        // two clocks, so the queue is not in order of readiness
        // an upload waiting at the front must not hold back render entries behind it, nor the other way round
        for (auto it = deletion_queue.begin(); it != deletion_queue.end();)
        {
            const u64_t completed = it->clock == deletion_clock_e::TRANSFER ? transfer_completed : render_completed;
            if (completed < it->gpu_timeline_value)
            {
                it++;
                continue;
            }

            deferred_delete_t& del = *it;

            switch (del.type)
            {
            case tau::renderer::resource_type_e::TEXTURE:
            {
                if (del.handle.texture.view) { vkDestroyImageView(ctx.device, del.handle.texture.view, nullptr); }
                vmaDestroyImage(ctx.allocator, del.handle.texture.image, del.handle.texture.allocation);
                if (del.bindless_id != BINDLESS_NULL_HANDLE) { texture_heap.release(del.bindless_id); }
                break;
            }

            case tau::renderer::resource_type_e::BUFFER:
            {
                vmaDestroyBuffer(ctx.allocator, del.handle.buffer.buffer, del.handle.buffer.allocation);
                break;
            }

            case tau::renderer::resource_type_e::STORAGE_IMAGE:
            {
                if (del.handle.texture.view) { vkDestroyImageView(ctx.device, del.handle.texture.view, nullptr); }

                vmaDestroyImage(ctx.allocator, del.handle.texture.image, del.handle.texture.allocation);

                if (del.bindless_id != BINDLESS_NULL_HANDLE) { storage_image_heap.release(del.bindless_id); }
                break;
            }

            case tau::renderer::resource_type_e::COMMAND_BUFFER:
            {
                vkFreeCommandBuffers(ctx.device, del.handle.cmd_buffer.pool, 1, &del.handle.cmd_buffer.cmd);
                break;
            }

            case tau::renderer::resource_type_e::PIPELINE:
            {
                if (del.handle.pipeline.pipeline)
                {
                    vkDestroyPipeline(ctx.device, del.handle.pipeline.pipeline, nullptr);
                }

                if (del.handle.pipeline.layout)
                {
                    vkDestroyPipelineLayout(ctx.device, del.handle.pipeline.layout, nullptr);
                }
                break;
            }

            case tau::renderer::resource_type_e::SWAPCHAIN:
            {
                vkDestroySwapchainKHR(ctx.device, del.handle.swapchain, nullptr);
                break;
            }

            case tau::renderer::resource_type_e::IMAGE_VIEW:
            {
                vkDestroyImageView(ctx.device, del.handle.image_view, nullptr);
                break;
            }

            case tau::renderer::resource_type_e::SEMAPHORE:
            {
                vkDestroySemaphore(ctx.device, del.handle.semaphore, nullptr);
                break;
            }

            case tau::renderer::resource_type_e::DESCRIPTOR_SET_LAYOUT:
            {
                vkDestroyDescriptorSetLayout(ctx.device, del.handle.descriptor_set_layout, nullptr);
                break;
            }

            case tau::renderer::resource_type_e::DESCRIPTOR_SET:
            {
                vkFreeDescriptorSets(ctx.device, del.handle.descriptor_set.pool, 1, &del.handle.descriptor_set.set);
                break;
            }
            }

            it = deletion_queue.erase(it);
        }
    }
} // namespace tau::renderer