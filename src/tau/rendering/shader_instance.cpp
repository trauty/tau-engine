#include "shader_instance.h"

#include "tau/asset.h"
#include "tau/assets/shader_format.h"
#include "tau/defines.h"
#include "tau/log.h"
#include "tau/rendering/renderer.h"
#include "tau/rendering/renderer_resources.h"
#include "tau/rendering/renderer_types.h"
#include "tau/rendering/vulkan.h"

#include <vector>

namespace tau
{
    shader_instance_t::shader_instance_t(asset_t<shader_t> target_shader) { init(target_shader); }

    shader_instance_t::~shader_instance_t() { shutdown(); }

    void shader_instance_t::init(asset_t<shader_t> target_shader)
    {
        if (!target_shader.is_valid())
        {
            TAU_LOG_ERROR("SHADER_INSTANCE", "Cannot init with a null shader");
            return;
        }

        shader = target_shader;

        for (u32_t i = 0; i < renderer::MAX_FRAMES_IN_FLIGHT; i++) { sets[i].clear(); }

        is_initialized = true;
    }

    void shader_instance_t::shutdown()
    {
        if (!is_initialized || !shader.is_valid()) { return; }

        for (u32_t i = 0; i < renderer::MAX_FRAMES_IN_FLIGHT; i++)
        {
            for (auto [set_id, set] : sets[i])
            {
                if (set != VK_NULL_HANDLE)
                {
                    renderer::res_system.deletion_queue.push_back({
                        .type = renderer::resource_type_e::DESCRIPTOR_SET,
                        .handle = {.descriptor_set = {.pool = renderer::res_system.instance_pool, .set = set}},
                        .bindless_id = renderer::BINDLESS_NULL_HANDLE,
                        .gpu_timeline_value = renderer::deletion_timeline_value(),
                    });
                }
            }
        }

        bound_resources.clear();
        is_initialized = false;
    }

    void shader_instance_t::sync()
    {
        if (!is_initialized || dirty_frames == 0) { return; }

        u32_t cur_frame = renderer::ctx.cur_frame;

        shader_t* program = assets::get(shader);

        if (sets[cur_frame].empty())
        {
            for (auto [set_id, layout] : program->custom_layouts)
            {
                VkDescriptorSetAllocateInfo alloc_info = {
                    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                    .descriptorPool = renderer::res_system.instance_pool,
                    .descriptorSetCount = 1,
                    .pSetLayouts = &layout,
                };

                VkDescriptorSet new_set = VK_NULL_HANDLE;
                vkAllocateDescriptorSets(renderer::ctx.device, &alloc_info, &new_set);
                sets[cur_frame][set_id] = new_set;
            }
        }

        std::vector<VkWriteDescriptorSet> writes;
        std::vector<VkDescriptorBufferInfo> buffer_infos;
        std::vector<VkDescriptorImageInfo> image_infos;

        buffer_infos.reserve(program->descriptor_bindings.size());
        image_infos.reserve(program->descriptor_bindings.size());

        for (const shader_descriptor_binding_t& binding : program->descriptor_bindings)
        {
            const bound_resource_t* it = bound_resources.find(binding.name_hash);
            if (it == nullptr) { continue; }

            const bound_resource_t& res = *it;

            VkWriteDescriptorSet write_info = {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = sets[cur_frame][binding.set],
                .dstBinding = binding.binding,
                .dstArrayElement = 0,
                .descriptorCount = binding.count,
                .descriptorType = vulkan::map_descriptor_type(binding.type),
            };

            if (res.buffer != VK_NULL_HANDLE)
            {
                buffer_infos.push_back({res.buffer, res.offset, res.size});
                write_info.pBufferInfo = &buffer_infos.back();
            }
            else if (res.view != VK_NULL_HANDLE)
            {
                image_infos.push_back({renderer::ctx.samplers[(u32_t)res.sampler], res.view, res.layout});
                write_info.pImageInfo = &image_infos.back();
            }

            writes.push_back(write_info);
        }

        if (!writes.empty())
        {
            vkUpdateDescriptorSets(renderer::ctx.device, static_cast<u32_t>(writes.size()), writes.data(), 0, nullptr);
        }

        if (last_synced_frame != cur_frame)
        {
            dirty_frames--;
            last_synced_frame = cur_frame;
        }
    }

    void shader_instance_t::set_buffer(u32_t name_hash, VkBuffer buffer, VkDeviceSize size, VkDeviceSize offset)
    {
        bound_resources[name_hash] = {
            .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .buffer = buffer,
            .offset = offset,
            .size = size,
        };

        dirty_frames = renderer::MAX_FRAMES_IN_FLIGHT;
    }

    void shader_instance_t::set_image(u32_t name_hash, VkImageView view, sampler_type_e sampler, VkImageLayout layout)
    {
        bound_resources[name_hash] = {
            .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .view = view,
            .sampler = sampler,
            .layout = layout,
        };

        dirty_frames = renderer::MAX_FRAMES_IN_FLIGHT;
    }

    void shader_instance_t::bind(VkCommandBuffer cmd)
    {
        if (!is_initialized) { return; }

        u32_t cur_frame = renderer::ctx.cur_frame;
        if (sets[cur_frame].empty()) { return; }

        shader_t* program = assets::get(shader);

        VkPipelineBindPoint bind_point =
            program->is_compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;

        for (auto [set_id, set] : sets[cur_frame])
        {
            vkCmdBindDescriptorSets(cmd, bind_point, program->pipeline_layout, set_id, 1, &set, 0, nullptr);
        }
    }
} // namespace tau