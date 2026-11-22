#include "renderer.h"

#include "tau/asset.h"
#include "tau/assets/material.h"
#include "tau/assets/mesh.h"
#include "tau/assets/shader.h"
#include "tau/assets/texture.h"
#include "tau/components/camera.h"
#include "tau/components/lighting.h"
#include "tau/components/post_process.h"
#include "tau/components/renderer.h"
#include "tau/components/transform.h"
#include "tau/containers/flat_map.h"
#include "tau/defines.h"
#include "tau/ecs_fwd.h"
#include "tau/engine.h"
#include "tau/hash.h"
#include "tau/log.h"
#include "tau/math.h"
#include "tau/memory/linear_allocator.h"
#include "tau/os.h"
#include "tau/profiling.h"
#include "tau/render/frame.h"
#include "tau/render/overlay.h"
#include "tau/rhi/device.h"
#include "tau/rhi/frame_upload.h"
#include "tau/rhi/gpu_timers.h"
#include "tau/rhi/pipelines.h"
#include "tau/rhi/readback.h"
#include "tau/rhi/renderdoc.h"
#include "tau/rhi/renderer_constants.h"
#include "tau/rhi/renderer_resources.h"
#include "tau/rhi/renderer_types.h"
#include "tau/rhi/rendergraph.h"
#include "tau/rhi/samplers.h"
#include "tau/rhi/vulkan.h"
#include "tau/systems/camera.h"
#include "tau/time.h"
#include "tau/vfs.h"
#include "tau/window.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <unordered_set>

#ifdef TAU_ENABLE_PROFILING
    #include <tracy/TracyVulkan.hpp>
#endif
#include <optional>
#include <vector>

namespace tau::renderer
{
    render_context_t ctx;
    namespace
    {
        bool frame_slot_ready = false;

        // off: uncapped (IMMEDIATE, else MAILBOX), for timing runs
        bool vsync = true;

        VkPresentModeKHR select_present_mode(VkPhysicalDevice physical_device, VkSurfaceKHR surface, bool want_vsync)
        {
            if (want_vsync) { return VK_PRESENT_MODE_FIFO_KHR; }

            u32_t count = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &count, nullptr);
            std::vector<VkPresentModeKHR> modes(count);
            vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &count, modes.data());

            for (const VkPresentModeKHR wanted : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR})
            {
                if (std::ranges::find(modes, wanted) != modes.end())
                {
                    TAU_LOG_INFO("VULKAN", "Uncapped present mode: {}",
                                 wanted == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : "MAILBOX");
                    return wanted;
                }
            }
            TAU_LOG_WARN("VULKAN", "No uncapped present mode, staying with vsync");
            return VK_PRESENT_MODE_FIFO_KHR;
        }

        bool image_acquired = false;
        u32_t acquired_index = 0;

        bool pending_resize = false;
        // resize waits until the held image is presented
        bool deferred_resize = false;

#ifdef TAU_ENABLE_PROFILING
        TracyVkCtx tracy_vk_ctx = nullptr;
#endif

        rendergraph_t rendergraph;
        struct renderer_feature_t
        {
            u32_t name_hash;
            i32 order;
            graph_builder_func_t builder;
        };

        std::vector<renderer_feature_t> custom_renderer_features;

        std::vector<render_view_t> submitted_views;

        // render() swaps buffers with the submit side, a moved from vector keeps no capacity
        std::vector<render_view_t> frame_views;
    } // namespace

    namespace
    {
        std::vector<output_target_t> submitted_outputs;
        std::vector<output_target_t> frame_outputs;

        flat_map_t<u32_t> submitted_punctual_slots;

        ecs::entity_t submitted_cascade_light = ecs::NULL_ENTITY;

        flat_map_t<debug_view_e> view_debug_views;

        flat_map_t<bool> view_post_processing;

        void recreate_surface()
        {
            vkDeviceWaitIdle(ctx.device);

            if (ctx.swapchain.handle != VK_NULL_HANDLE)
            {
                std::scoped_lock lock(res_system.deletion_mutex);
                const u64_t safe_timeline = deletion_timeline_value();

                for (VkImageView view : ctx.swapchain.views)
                {
                    res_system.deletion_queue.push_back({
                        .type = resource_type_e::IMAGE_VIEW,
                        .handle = {.image_view = view},
                        .bindless_id = BINDLESS_NULL_HANDLE,
                        .gpu_timeline_value = safe_timeline,
                    });
                }
                ctx.swapchain.views.clear();

                res_system.deletion_queue.push_back({
                    .type = resource_type_e::TEXTURE,
                    .handle = {.texture = {ctx.swapchain.depth_image, ctx.swapchain.depth_allocation,
                                           ctx.swapchain.depth_view}},
                    .bindless_id = BINDLESS_NULL_HANDLE,
                    .gpu_timeline_value = safe_timeline,
                });

                vkDestroySwapchainKHR(ctx.device, ctx.swapchain.handle, nullptr);
                ctx.swapchain.handle = VK_NULL_HANDLE;
            }

            if (ctx.surface != VK_NULL_HANDLE)
            {
                vkDestroySurfaceKHR(ctx.instance, ctx.surface, nullptr);
                ctx.surface = VK_NULL_HANDLE;
            }

            if (!SDL_Vulkan_CreateSurface(tau::window::get_window(), ctx.instance, nullptr, &ctx.surface))
            {
                TAU_LOG_WARN("VULKAN", "Failed to recreate Vulkan surface on resume");
            }
        }
    } // namespace

    mesh_pass_e pass_for(const shader_t& shader)
    {
        if (draws_into_gbuffer(shader.module.domain, shader.module.blend_mode, shader.is_deferred_capable()))
        {
            return mesh_pass_e::GBUFFER;
        }

        if (shader.module.domain != shader_domain_e::SURFACE) { return mesh_pass_e::BLENDED; }

        const blend_mode_e blend = shader.module.blend_mode;
        const bool opaque = (blend == blend_mode_e::SOLID || blend == blend_mode_e::CUTOUT);
        return opaque ? mesh_pass_e::FORWARD_OPAQUE : mesh_pass_e::BLENDED;
    }

    // the variant a mesh pass really binds for this shader
    // the draw list bins and sorts by it, so it must agree with add_mesh_pass
    TAU_ENGINE_API VkPipeline binding_pipeline(const shader_t& shader)
    {
        const pipeline_variant_e variant =
            (pass_for(shader) == mesh_pass_e::GBUFFER) ? pipeline_variant_e::GBUFFER : pipeline_variant_e::FORWARD;
        return shader.get_pipeline(variant);
    }

    void register_renderer_feature(u32_t name_hash, i32 order, graph_builder_func_t builder)
    {
        for (renderer_feature_t& existing : custom_renderer_features)
        {
            if (existing.name_hash == name_hash)
            {
                existing.order = order;
                existing.builder = builder;
                return;
            }
        }

        custom_renderer_features.push_back({name_hash, order, builder});
        std::sort(custom_renderer_features.begin(), custom_renderer_features.end(),
                  [](const renderer_feature_t& a, const renderer_feature_t& b)
                  { return a.order != b.order ? a.order < b.order : a.name_hash < b.name_hash; });
    }

    u32_t remove_features_of(void* module_base)
    {
        const auto first = std::remove_if(
            custom_renderer_features.begin(), custom_renderer_features.end(),
            [module_base](const renderer_feature_t& feature)
            { return os::module_base_of(reinterpret_cast<const void*>(feature.builder)) == module_base; });
        const u32_t removed = static_cast<u32_t>(custom_renderer_features.end() - first);
        custom_renderer_features.erase(first, custom_renderer_features.end());
        return removed;
    }

    u32_t count_features_of(void* module_base)
    {
        return static_cast<u32_t>(std::count_if(
            custom_renderer_features.begin(), custom_renderer_features.end(), [module_base](const renderer_feature_t& f)
            { return os::module_base_of(reinterpret_cast<const void*>(f.builder)) == module_base; }));
    }

    // world_mat column 3 is translation, column 2 the forward axis, both used by every light upload
    static vec3 world_position(const transform_t& transform)
    { return {transform.world_mat[3][0], transform.world_mat[3][1], transform.world_mat[3][2]}; }

    static vec3 world_forward(const transform_t& transform)
    { return normalize({transform.world_mat[2][0], transform.world_mat[2][1], transform.world_mat[2][2]}); }

    // x is the punctual tile this light owns, negative when it casts no shadow
    static vec4 shadow_params(const flat_map_t<u32_t>& slots, ecs::entity_t entity)
    {
        const u32_t* slot = slots.find(static_cast<u32_t>(entity));
        return {slot ? static_cast<f32>(*slot) : -1.0f, 0.0f, 0.0f, 0.0f};
    }

    // a cascade and a punctual tile are the same record, the caller derives texel_world_size
    static void pack_shadow_tile(gpu::ShadowTile& dst, const render_view_t& sub, f32 atlas_w, f32 atlas_h,
                                 f32 texel_world_size)
    {
        dst.view_proj = sub.view_proj;

        const VkRect2D& rect = sub.shadow_atlas_rect;
        dst.uv_offset_scale = {static_cast<f32>(rect.offset.x) / atlas_w, static_cast<f32>(rect.offset.y) / atlas_h,
                               static_cast<f32>(rect.extent.width) / atlas_w,
                               static_cast<f32>(rect.extent.height) / atlas_h};

        dst.texel_world_size = texel_world_size;
        dst.normal_bias = sub.shadow_normal_bias;
        dst.depth_bias = sub.shadow_depth_bias;
        dst.split_far = sub.shadow_split_far;
    }

    // every compute site: bind, both descriptor sets at the view's dynamic offset, the shader's own set, push, dispatch
    static void dispatch_compute(VkCommandBuffer cmd, const shader_t& shader, VkPipeline pipeline, u32_t view_offset,
                                 shader_instance_t* instance, const gpu::PushConstants& pc, u32_t groups_x,
                                 u32_t groups_y = 1, u32_t groups_z = 1)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

        VkDescriptorSet sets[] = {res_system.global_set, res_system.frame_set};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shader.pipeline_layout, 0, 2, sets, 1,
                                &view_offset);

        if (instance != nullptr) { instance->bind(cmd); }

        vkCmdPushConstants(cmd, shader.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(gpu::PushConstants),
                           &pc);

        vkCmdDispatch(cmd, groups_x, groups_y, groups_z);
    }

    // one stage/access transition over a few buffers, whole buffer, a wider barrier is never weaker
    static void buffer_barrier(VkCommandBuffer cmd, std::initializer_list<VkBuffer> buffers,
                               VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                               VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access)
    {
        VkBufferMemoryBarrier2 barriers[4] = {};
        u32_t count = 0;

        for (VkBuffer buffer : buffers)
        {
            if (buffer == VK_NULL_HANDLE || count >= 4) { continue; }

            barriers[count++] = {
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                .srcStageMask = src_stage,
                .srcAccessMask = src_access,
                .dstStageMask = dst_stage,
                .dstAccessMask = dst_access,
                .buffer = buffer,
                .offset = 0,
                .size = VK_WHOLE_SIZE,
            };
        }

        if (count == 0) { return; }

        VkDependencyInfo dep_info = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = count,
            .pBufferMemoryBarriers = barriers,
        };
        vkCmdPipelineBarrier2(cmd, &dep_info);
    }

    TAU_ENGINE_API rg_pass_t& add_mesh_pass(rendergraph_t& graph, u32_t name_hash, const char* debug_name,
                                            u32_t view_name_hash, mesh_pass_e mesh_pass, bool depth_only,
                                            const std::vector<rg_resource_id>& reads,
                                            const std::vector<rg_resource_id>& writes, rg_resource_id depth)
    {
        rg_pass_t& pass = graph.add_pass(name_hash, debug_name);
        pass.texture_reads = reads;
        pass.color_writes = writes;
        pass.depth_stencil = depth;

        pass.execute_callback = [view_name_hash, mesh_pass, depth_only](VkCommandBuffer cmd, ecs::registry_t&)
        {
            per_frame_t& fd = ctx.per_frame_objects[ctx.cur_frame];
            if (fd.object_counter == 0) { return; }

            pipeline_range_t range = fd.pass_ranges[static_cast<u32_t>(mesh_pass)];
            if (depth_only)
            {
                range.begin = fd.pass_ranges[static_cast<u32_t>(mesh_pass_e::GBUFFER)].begin;
                range.end = fd.pass_ranges[static_cast<u32_t>(mesh_pass_e::FORWARD_OPAQUE)].end;
            }
            if (range.begin == range.end) { return; }

            const std::unique_ptr<view_gpu_resources_t>* found = fd.views.find(view_name_hash);
            if (found == nullptr) { return; }
            view_gpu_resources_t& vgr = **found;
            if (vgr.indirect_buffer == VK_NULL_HANDLE || vgr.draw_counts_buffer == VK_NULL_HANDLE) { return; }

            gpu::PushConstants pc_data = {
                fd.object_buffer_address,
                res_system.material_heap.device_address,
            };
            VkDescriptorSet sets[] = {res_system.global_set, res_system.frame_set};

            for (u32_t i = range.begin; i < range.end; i++)
            {
                const active_pipeline_t& p = fd.active_pipelines[i];

                VkPipeline pipe = p.pipeline;
                if (depth_only) { pipe = p.shadow_pipeline; }
                else if (mesh_pass == mesh_pass_e::GBUFFER) { pipe = p.gbuffer_pipeline; }
                if (pipe == VK_NULL_HANDLE) { continue; } // e.g. non caster shader in a shadow pass

                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout, 0, 2, sets, 1,
                                        &vgr.global_data_offset);
                vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(gpu::PushConstants), &pc_data);

                u32_t indirect_offset = p.first_object * 16;
                u32_t counts_offset = p.pipeline_index * sizeof(u32_t);
                vkCmdDrawIndirectCount(cmd, vgr.indirect_buffer, indirect_offset, vgr.draw_counts_buffer, counts_offset,
                                       p.object_count, 16);
            }
        };

        return pass;
    }

    bool bind_view_descriptors(VkCommandBuffer cmd, VkPipelineLayout layout, u32_t view_name_hash)
    {
        per_frame_t& fd = ctx.per_frame_objects[ctx.cur_frame];
        const std::unique_ptr<view_gpu_resources_t>* found = fd.views.find(view_name_hash);
        if (found == nullptr) { return false; }

        const VkDescriptorSet sets[] = {res_system.global_set, res_system.frame_set};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, sets, 1,
                                &(*found)->global_data_offset);
        return true;
    }

    void draw_view_objects(VkCommandBuffer cmd, u32_t view_name_hash, VkPipeline pipeline, VkPipelineLayout layout,
                           gpu::PushConstants pc)
    {
        per_frame_t& fd = ctx.per_frame_objects[ctx.cur_frame];
        if (fd.object_counter == 0 || pipeline == VK_NULL_HANDLE) { return; }

        const std::unique_ptr<view_gpu_resources_t>* found = fd.views.find(view_name_hash);
        if (found == nullptr) { return; }
        view_gpu_resources_t& vgr = **found;
        if (vgr.indirect_buffer == VK_NULL_HANDLE || vgr.draw_counts_buffer == VK_NULL_HANDLE) { return; }

        pc.object_buffer = fd.object_buffer_address;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        bind_view_descriptors(cmd, layout, view_name_hash);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(gpu::PushConstants), &pc);

        for (const active_pipeline_t& p : fd.active_pipelines)
        {
            vkCmdDrawIndirectCount(cmd, vgr.indirect_buffer, p.first_object * 16, vgr.draw_counts_buffer,
                                   p.pipeline_index * sizeof(u32_t), p.object_count, 16);
        }
    }

    static u32_t view_global_offset(const per_frame_t& fd, u32_t view_name_hash)
    {
        const std::unique_ptr<view_gpu_resources_t>* found =
            fd.views.find(view_name_hash ? view_name_hash : "PrimaryView"_h);

        return (found != nullptr) ? (*found)->global_data_offset : fd.global_data_offset;
    }

    TAU_ENGINE_API rg_pass_t& add_fullscreen_pass(rendergraph_t& graph, u32_t name_hash, const char* debug_name,
                                                  tau::material_t* material, std::span<const rg_resource_id> reads,
                                                  std::span<const rg_resource_id> writes,
                                                  pass_execute_func_t on_execute, u32_t view_name_hash,
                                                  rg_resource_id depth_tex_res)
    {
        rg_pass_t& pass = graph.add_pass(name_hash, debug_name);

        // assign, not construct: add_pass keeps these vectors' capacity between frames
        pass.texture_reads.assign(reads.begin(), reads.end());
        pass.color_writes.assign(writes.begin(), writes.end());

        const rg_resource_id input_res = reads.empty() ? RG_NULL_ID : reads.front();

        pass.execute_callback = [&graph, material, on_execute, input_res, view_name_hash,
                                 depth_tex_res](VkCommandBuffer cmd, ecs::registry_t&)
        {
            if (!material || !material->shader.is_valid()) { return; }

            if (input_res != RG_NULL_ID)
            {
                const u32_t input_id = graph.get_bindless_id(input_res);
                material->set_property_if_present("input_tex"_h, input_id);
                material->set_property_if_present("scene_color_tex"_h, input_id);
            }

            if (depth_tex_res != RG_NULL_ID)
            {
                material->set_property_if_present("depth_tex"_h, graph.get_bindless_id(depth_tex_res));
            }

            if (on_execute) { on_execute(graph, *material); }

            material->sync();

            shader_t* shader = assets::get(material->shader);

            VkPipeline pipeline = shader ? shader->get_pipeline(pipeline_variant_e::FORWARD) : VK_NULL_HANDLE;
            if (pipeline == VK_NULL_HANDLE) { return; }

            per_frame_t& frame_data = ctx.per_frame_objects[ctx.cur_frame];

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

            VkDescriptorSet sets[] = {res_system.global_set, res_system.frame_set};

            const u32_t view_offset = view_global_offset(frame_data, view_name_hash);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shader->pipeline_layout, 0, 2, sets, 1,
                                    &view_offset);

            gpu::PushConstants pc_data = {
                frame_data.object_buffer_address,
                res_system.material_heap.device_address,
                material->heap_offset[ctx.cur_frame],
            };

            vkCmdPushConstants(cmd, shader->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(gpu::PushConstants), &pc_data);

            vkCmdDraw(cmd, 3, 1, 0, 0);
        };

        return pass;
    }

    TAU_ENGINE_API rg_pass_t& add_compute_pass(rendergraph_t& graph, u32_t name_hash, const char* debug_name,
                                               tau::material_t* material, u32_t dispatch_x, u32_t dispatch_y,
                                               u32_t dispatch_z, const std::vector<rg_resource_id>& reads,
                                               const std::vector<rg_resource_id>& writes,
                                               pass_execute_func_t on_execute, u32_t view_name_hash)
    {
        rg_pass_t& pass = graph.add_pass(name_hash, debug_name);
        pass.texture_reads = reads;
        pass.storage_writes = writes;

        pass.execute_callback = [&graph, material, dispatch_x, dispatch_y, dispatch_z, on_execute,
                                 view_name_hash](VkCommandBuffer cmd, ecs::registry_t&)
        {
            if (!material || !material->shader.is_valid()) { return; }

            if (on_execute) { on_execute(graph, *material); }

            material->sync();

            shader_t* shader = assets::get(material->shader);

            VkPipeline pipeline = shader ? shader->get_pipeline(pipeline_variant_e::FORWARD) : VK_NULL_HANDLE;
            if (pipeline == VK_NULL_HANDLE) { return; }

            per_frame_t& frame_data = ctx.per_frame_objects[ctx.cur_frame];

            const gpu::PushConstants pc_data = {
                frame_data.object_buffer_address,
                res_system.material_heap.device_address,
                material->heap_offset[ctx.cur_frame],
            };

            dispatch_compute(cmd, *shader, pipeline, view_global_offset(frame_data, view_name_hash), nullptr, pc_data,
                             dispatch_x, dispatch_y, dispatch_z);
        };

        return pass;
    }

    static void destroy_persistent_image(persistent_image_t& target)
    {
        if (target.view != VK_NULL_HANDLE) { vkDestroyImageView(ctx.device, target.view, nullptr); }
        if (target.image != VK_NULL_HANDLE) { vmaDestroyImage(ctx.allocator, target.image, target.allocation); }
        if (target.bindless_id != BINDLESS_NULL_HANDLE) { res_system.texture_heap.release(target.bindless_id); }

        target = persistent_image_t{};
    }

    static void ensure_persistent_image(persistent_image_t& target, const image_desc_t& desc, const char* name)
    {
        if (target.image != VK_NULL_HANDLE && target.desc == desc) { return; }

        destroy_persistent_image(target);

        VkImageCreateInfo image_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = desc.format,
            .extent = {desc.width, desc.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = desc.usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        VmaAllocationCreateInfo alloc_info = {
            .usage = VMA_MEMORY_USAGE_AUTO,
            .priority = 1.0f,
        };

        VK_CHECK(vmaCreateImage(ctx.allocator, &image_info, &alloc_info, &target.image, &target.allocation, nullptr));
        device::name(target.image, VK_OBJECT_TYPE_IMAGE, name);

        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = target.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = desc.format,
            .subresourceRange = {
                                 .aspectMask = desc.aspect,
                                 .baseMipLevel = 0,
                                 .levelCount = 1,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1,
                                 }
        };

        VK_CHECK(vkCreateImageView(ctx.device, &view_info, nullptr, &target.view));

        if (desc.usage & VK_IMAGE_USAGE_SAMPLED_BIT)
        {
            target.bindless_id = res_system.texture_heap.acquire();

            VkDescriptorImageInfo bindless_info = {
                .sampler = VK_NULL_HANDLE,
                .imageView = target.view,
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            };

            VkWriteDescriptorSet write_desc = {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = res_system.global_set,
                .dstBinding = TEXTURES_BINDING_POINT,
                .dstArrayElement = target.bindless_id,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                .pImageInfo = &bindless_info,
            };

            vkUpdateDescriptorSets(ctx.device, 1, &write_desc, 0, nullptr);
        }

        target.desc = desc;
        target.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        target.needs_full_clear = true;
    }

    // sdsm (sample distribution shadow maps): reduce the frame depth to a min/max on the gpu
    // read it back a frame or two later and feed the cascade fitting in shadow_system, both halves live here

    // pick up the newest depth reduction the gpu finished and turn it into a view space near/far
    // reverse z, so the near plane divides by the max encoded depth
    static void read_depth_bounds()
    {
        u64_t completed_timeline = 0;
        vkGetSemaphoreCounterValue(ctx.device, ctx.timeline_semaphore, &completed_timeline);

        per_frame_t* newest = nullptr;
        for (per_frame_t& candidate : ctx.per_frame_objects)
        {
            if (!candidate.depth_bounds_pending || candidate.mapped_depth_bounds == nullptr) { continue; }
            if (candidate.target_timeline_value == 0 || candidate.target_timeline_value > completed_timeline)
            {
                continue;
            }
            if (newest == nullptr || candidate.target_timeline_value > newest->target_timeline_value)
            {
                newest = &candidate;
            }
        }

        if (newest == nullptr) { return; }

        const u32_t min_bits = newest->mapped_depth_bounds[0];
        const u32_t max_bits = newest->mapped_depth_bounds[1];
        const f32 pass_z_near = newest->depth_bounds_z_near;
        newest->depth_bounds_pending = false;

        f32 min_depth = 0.0f;
        f32 max_depth = 0.0f;
        std::memcpy(&min_depth, &min_bits, sizeof(f32));
        std::memcpy(&max_depth, &max_bits, sizeof(f32));

        if (max_bits != 0 && min_bits <= max_bits && min_depth > 0.0f && pass_z_near > 0.0f)
        {
            ctx.primary_depth_bounds.near_z = pass_z_near / max_depth;
            ctx.primary_depth_bounds.far_z = pass_z_near / min_depth;
            ctx.primary_depth_bounds.valid = true;
        }
        else
        {
            ctx.primary_depth_bounds.valid = false;
        }
    }

    static bool add_depth_reduce_pass(rendergraph_t& graph, per_frame_t& frame_data, rg_resource_id depth,
                                      u32_t view_name_hash, VkExtent2D extent)
    {
        if (depth == RG_NULL_ID || extent.width == 0 || extent.height == 0) { return false; }
        if (frame_data.depth_bounds_buffer == VK_NULL_HANDLE || frame_data.mapped_depth_bounds == nullptr)
        {
            return false;
        }

        shader_t* shader = assets::get(ctx.depth_reduce_shader);
        if (shader == nullptr || shader->get_pipeline(pipeline_variant_e::FORWARD) == VK_NULL_HANDLE) { return false; }

        if (!frame_data.depth_reduce_instance.is_initialized)
        {
            frame_data.depth_reduce_instance.init(ctx.depth_reduce_shader);
        }
        frame_data.depth_reduce_instance.set_buffer("depth_bounds"_h, frame_data.depth_bounds_buffer);
        frame_data.depth_reduce_instance.sync();

        rg_pass_t& pass = graph.add_pass("DepthReduce"_h, "DepthReduce");
        pass.texture_reads = {depth};

        pass.execute_callback = [&graph, depth, view_name_hash, extent](VkCommandBuffer cmd, ecs::registry_t&)
        {
            per_frame_t& fd = ctx.per_frame_objects[ctx.cur_frame];

            shader_t* reduce_shader = assets::get(ctx.depth_reduce_shader);
            VkPipeline pipeline =
                reduce_shader ? reduce_shader->get_pipeline(pipeline_variant_e::FORWARD) : VK_NULL_HANDLE;
            if (pipeline == VK_NULL_HANDLE || fd.depth_bounds_buffer == VK_NULL_HANDLE) { return; }

            const u32_t depth_tex_id = graph.get_bindless_id(depth);
            if (depth_tex_id == BINDLESS_NULL_HANDLE) { return; }

            vkCmdFillBuffer(cmd, fd.depth_bounds_buffer, 0, sizeof(u32_t), 0xffffffffu);
            vkCmdFillBuffer(cmd, fd.depth_bounds_buffer, sizeof(u32_t), sizeof(u32_t), 0u);

            buffer_barrier(cmd, {fd.depth_bounds_buffer}, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

            const gpu::PushConstants pc_data = {
                fd.object_buffer_address,
                res_system.material_heap.device_address,
                0,
                depth_tex_id,
            };

            dispatch_compute(cmd, *reduce_shader, pipeline, view_global_offset(fd, view_name_hash),
                             &fd.depth_reduce_instance, pc_data, (extent.width + 7) / 8, (extent.height + 7) / 8);

            buffer_barrier(cmd, {fd.depth_bounds_buffer}, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                           VK_ACCESS_2_HOST_READ_BIT);
        };

        return true;
    }

    // the driver compiles every pipeline from shader binaries on launch and on every shader reload
    // a cache on disk, in the per user folder (user://), carries earlier results across runs
    namespace
    {
        constexpr const char* PIPELINE_CACHE_FILE = "user://pipeline_cache.bin";

        // only save once pipeline creation has gone quiet: a scene load creates dozens in a row
        constexpr std::chrono::seconds PIPELINE_CACHE_SETTLE{2};

        // nothing is evicted and every shader edit adds pipelines that will never be built again, so past this size
        // the file is mostly stale and is dropped and rebuilt. the engine's own shaders come to about 250 KB
        constexpr size_t PIPELINE_CACHE_MAX_BYTES = 32u * 1024u * 1024u;

        struct pipeline_cache_stats_t
        {
            std::mutex mutex;
            u32_t created_total = 0;
            f64 milliseconds_total = 0.0;
            u32_t created_since_save = 0;
            std::chrono::steady_clock::time_point last_created;
        };

        pipeline_cache_stats_t g_pipeline_stats;

        // a cache from another GPU or driver is useless, not an error, check the header so no driver sees foreign data
        bool pipeline_cache_matches_device(const std::vector<char>& data)
        {
            VkPipelineCacheHeaderVersionOne header = {};
            if (data.size() < sizeof(header)) { return false; }
            std::memcpy(&header, data.data(), sizeof(header));

            VkPhysicalDeviceProperties props = {};
            vkGetPhysicalDeviceProperties(ctx.physical_device, &props);

            return header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && header.vendorID == props.vendorID &&
                   header.deviceID == props.deviceID &&
                   std::memcmp(header.pipelineCacheUUID, props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
        }

        // the physical file behind user://, or empty when there is no per user folder to use
        std::filesystem::path pipeline_cache_path()
        {
            const std::string resolved = vfs::resolve(PIPELINE_CACHE_FILE);
            if (resolved.empty() || resolved == PIPELINE_CACHE_FILE) { return {}; }
            return std::filesystem::path(resolved);
        }

        void create_pipeline_cache()
        {
            std::vector<char> data;
            const std::filesystem::path path = pipeline_cache_path();
            if (!path.empty())
            {
                std::ifstream file(path, std::ios::binary);
                data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            }

            if (!data.empty() && !pipeline_cache_matches_device(data))
            {
                TAU_LOG_INFO("VULKAN", "Pipeline cache on disk is from another GPU or driver, starting empty");
                data.clear();
            }
            if (data.size() > PIPELINE_CACHE_MAX_BYTES)
            {
                TAU_LOG_INFO("VULKAN", "Pipeline cache grew to {} MB, starting empty", data.size() / (1024 * 1024));
                data.clear();
            }

            VkPipelineCacheCreateInfo info = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                .initialDataSize = data.size(),
                .pInitialData = data.empty() ? nullptr : data.data(),
            };

            if (vkCreatePipelineCache(ctx.device, &info, nullptr, &ctx.pipeline_cache) != VK_SUCCESS)
            {
                // the data was the problem, not the cache: try once more without it
                info.initialDataSize = 0;
                info.pInitialData = nullptr;
                data.clear();
                if (vkCreatePipelineCache(ctx.device, &info, nullptr, &ctx.pipeline_cache) != VK_SUCCESS)
                {
                    ctx.pipeline_cache = VK_NULL_HANDLE; // pipelines still work, just uncached
                    TAU_LOG_WARN("VULKAN", "Could not create a pipeline cache");
                    return;
                }
            }

            if (!data.empty()) { TAU_LOG_INFO("VULKAN", "Pipeline cache: loaded {} KB", data.size() / 1024); }
        }

        void save_pipeline_cache()
        {
            if (ctx.pipeline_cache == VK_NULL_HANDLE) { return; }

            const std::filesystem::path path = pipeline_cache_path();
            if (path.empty()) { return; }

            size_t size = 0;
            if (vkGetPipelineCacheData(ctx.device, ctx.pipeline_cache, &size, nullptr) != VK_SUCCESS || size == 0)
            {
                return;
            }
            std::vector<char> data(size);
            if (vkGetPipelineCacheData(ctx.device, ctx.pipeline_cache, &size, data.data()) != VK_SUCCESS) { return; }

            // written beside and then moved over the old one, so a crash mid write leaves the previous cache
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            const std::filesystem::path temp = path.string() + ".tmp";
            {
                std::ofstream file(temp, std::ios::binary | std::ios::trunc);
                file.write(data.data(), static_cast<std::streamsize>(size));
                if (!file) { return; }
            }
            std::filesystem::rename(temp, path, ec);
            if (ec)
            {
                TAU_LOG_WARN("VULKAN", "Could not save the pipeline cache: {}", ec.message());
                return;
            }

            std::scoped_lock lock(g_pipeline_stats.mutex);
            TAU_LOG_INFO("VULKAN", "Saved pipeline cache ({} KB); {} pipelines created this session in {:.0f} ms",
                         size / 1024, g_pipeline_stats.created_total, g_pipeline_stats.milliseconds_total);
        }

        void save_pipeline_cache_when_settled()
        {
            {
                std::scoped_lock lock(g_pipeline_stats.mutex);
                if (g_pipeline_stats.created_since_save == 0) { return; }
                if (std::chrono::steady_clock::now() - g_pipeline_stats.last_created < PIPELINE_CACHE_SETTLE)
                {
                    return;
                }
                g_pipeline_stats.created_since_save = 0;
            }
            save_pipeline_cache();
        }
    } // namespace

    void note_pipeline_created(f64 milliseconds)
    {
        std::scoped_lock lock(g_pipeline_stats.mutex);
        g_pipeline_stats.created_total++;
        g_pipeline_stats.milliseconds_total += milliseconds;
        g_pipeline_stats.created_since_save++;
        g_pipeline_stats.last_created = std::chrono::steady_clock::now();
    }

    bool init(const context_config_t& config, SDL_Window* window)
    {
        vsync = config.vsync;

        const device::device_config_t device_config = {
            .app_name = config.app_name,
            .validation = config.validation,
            .force_compat = config.force_compat,
            .headless = config.headless,
            .pipeline_properties = config.pipeline_properties,
            .instance_exts = config.required_instance_exts,
        };
        if (!device::create(device_config, window)) { return false; }

        create_pipeline_cache();

        {
            VkPhysicalDeviceSubgroupProperties sg_props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
            VkPhysicalDeviceProperties2 sg_props2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sg_props};
            vkGetPhysicalDeviceProperties2(ctx.physical_device, &sg_props2);

            auto has_op = [&](VkSubgroupFeatureFlagBits bit) { return (sg_props.supportedOperations & bit) != 0; };
            TAU_LOG_INFO("VULKAN",
                         "Subgroup: size={} compute_stage={} basic={} vote={} ballot={} arithmetic={} shuffle={}",
                         sg_props.subgroupSize, (sg_props.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0,
                         has_op(VK_SUBGROUP_FEATURE_BASIC_BIT), has_op(VK_SUBGROUP_FEATURE_VOTE_BIT),
                         has_op(VK_SUBGROUP_FEATURE_BALLOT_BIT), has_op(VK_SUBGROUP_FEATURE_ARITHMETIC_BIT),
                         has_op(VK_SUBGROUP_FEATURE_SHUFFLE_BIT));
        }

        {
            VkPhysicalDeviceDescriptorIndexingProperties idx_props = {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
            VkPhysicalDeviceProperties2 props2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idx_props};
            vkGetPhysicalDeviceProperties2(ctx.physical_device, &props2);

            auto clamp_cap = [](const char* name, u32_t desired, u32_t set_limit, u32_t stage_limit)
            {
                u32_t hw = std::min(set_limit, stage_limit);
                if (desired > hw)
                {
                    TAU_LOG_WARN("VULKAN", "Bindless capacity '{}' clamped from {} to device limit {}", name, desired,
                                 hw);
                    return hw;
                }
                return desired;
            };

            const bindless_limits_t& want = config.bindless_limits;
            res_system.max_samplers =
                clamp_cap("samplers", want.max_samplers, idx_props.maxDescriptorSetUpdateAfterBindSamplers,
                          idx_props.maxPerStageDescriptorUpdateAfterBindSamplers);
            res_system.max_sampled_textures = clamp_cap("sampled_textures", want.max_sampled_textures,
                                                        idx_props.maxDescriptorSetUpdateAfterBindSampledImages,
                                                        idx_props.maxPerStageDescriptorUpdateAfterBindSampledImages);
            res_system.max_storage_images = clamp_cap("storage_images", want.max_storage_images,
                                                      idx_props.maxDescriptorSetUpdateAfterBindStorageImages,
                                                      idx_props.maxPerStageDescriptorUpdateAfterBindStorageImages);
            res_system.material_heap_size = want.material_heap_size;
        }

        init_resources();

        if (ctx.caps.headless)
        {
            ctx.swapchain.depth_format = find_depth_format();
            ctx.swapchain.format = VK_FORMAT_B8G8R8A8_SRGB;
            ctx.cur_surface_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        }
        else
        {
            init_swapchain();
        }

        ctx.per_frame_objects.resize(MAX_FRAMES_IN_FLIGHT);
        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) { init_per_frame(ctx.per_frame_objects[i]); }

        VkCommandPoolCreateInfo transfer_pool_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = ctx.queue_fam_indices.transfer_family.value(),
        };

        VK_CHECK(vkCreateCommandPool(ctx.device, &transfer_pool_info, nullptr, &ctx.transfer_command_pool));

        VkSemaphoreTypeCreateInfo timeline_type_info = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue = 0,
        };

        VkSemaphoreCreateInfo timeline_sem_info = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &timeline_type_info,
        };

        VK_CHECK(vkCreateSemaphore(ctx.device, &timeline_sem_info, nullptr, &ctx.timeline_semaphore));
        ctx.timeline_value = 0;

        VkSampler linear_repeat = create_sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT);
        VkSampler linear_clamp = create_sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        VkSampler nearest_repeat = create_sampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT);
        VkSampler nearest_clamp = create_sampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        VkSampler shadow_cmp = create_shadow_sampler();

        ctx.samplers.push_back(linear_repeat);
        ctx.samplers.push_back(linear_clamp);
        ctx.samplers.push_back(nearest_repeat);
        ctx.samplers.push_back(nearest_clamp);
        ctx.samplers.push_back(shadow_cmp);

        constexpr u32_t sampler_write_count = (u32_t)sampler_type_e::COUNT;
        VkDescriptorImageInfo sampler_infos[sampler_write_count] = {};
        sampler_infos[(u32_t)sampler_type_e::LINEAR_REPEAT].sampler = linear_repeat;
        sampler_infos[(u32_t)sampler_type_e::LINEAR_CLAMP].sampler = linear_clamp;
        sampler_infos[(u32_t)sampler_type_e::NEAREST_REPEAT].sampler = nearest_repeat;
        sampler_infos[(u32_t)sampler_type_e::NEAREST_CLAMP].sampler = nearest_clamp;
        sampler_infos[(u32_t)sampler_type_e::SHADOW_CMP].sampler = shadow_cmp;

        VkWriteDescriptorSet samplers_write_desc = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = res_system.global_set,
            .dstBinding = SAMPLERS_BINDING_POINT,
            .dstArrayElement = 0,
            .descriptorCount = sampler_write_count,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
            .pImageInfo = sampler_infos,
        };

        vkUpdateDescriptorSets(ctx.device, 1, &samplers_write_desc, 0, nullptr);

        TAU_LOG_INFO("VULKAN", "Context initialized for GPU: {}", ctx.properties.deviceName);

        gpu_timers::init();

#ifdef TAU_ENABLE_PROFILING
        VkCommandPoolCreateInfo tracy_pool_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = ctx.queue_fam_indices.graphics_family.value(),
        };

        VkCommandPool tracy_pool;
        vkCreateCommandPool(ctx.device, &tracy_pool_info, nullptr, &tracy_pool);

        VkCommandBufferAllocateInfo tracy_alloc_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = tracy_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };

        VkCommandBuffer tracy_cmd;
        vkAllocateCommandBuffers(ctx.device, &tracy_alloc_info, &tracy_cmd);

        tracy_vk_ctx = TracyVkContext(ctx.physical_device, ctx.device, ctx.graphics_queue, tracy_cmd);
        rendergraph.profiling_vk_ctx = tracy_vk_ctx;

        vkDestroyCommandPool(ctx.device, tracy_pool, nullptr);
        frame::init(tracy_vk_ctx);
#else
        frame::init(nullptr);
#endif

        auto create_mapped_buffer = [](const char* name, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer,
                                       VmaAllocation& alloc, void*& mapped_mem)
        {
            VkBufferCreateInfo buf_info = {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = size,
                .usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            };
            VmaAllocationCreateInfo alloc_info = {
                .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                .usage = VMA_MEMORY_USAGE_AUTO,
                .requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            };

            VmaAllocationInfo vma_alloc_info;
            VK_CHECK(vmaCreateBuffer(ctx.allocator, &buf_info, &alloc_info, &buffer, &alloc, &vma_alloc_info));
            device::name(buffer, VK_OBJECT_TYPE_BUFFER, name);
            mapped_mem = vma_alloc_info.pMappedData;
        };

        VkDeviceSize min_alignment = ctx.properties.limits.minUniformBufferOffsetAlignment;
        ctx.global_data_aligned_size =
            static_cast<u32_t>((sizeof(gpu::GlobalData) + min_alignment - 1) & ~(min_alignment - 1));

        void* mapped_global_data;
        create_mapped_buffer("GlobalData", ctx.global_data_aligned_size * MAX_FRAMES_IN_FLIGHT * MAX_VIEWS_PER_FRAME,
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, ctx.global_data_buffer, ctx.global_data_alloc,
                             mapped_global_data);

        ctx.global_data_mapped = static_cast<u8*>(mapped_global_data);

        for (u32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            u32_t base_offset = i * MAX_VIEWS_PER_FRAME * ctx.global_data_aligned_size;
            ctx.per_frame_objects[i].mapped_global_data =
                reinterpret_cast<gpu::GlobalData*>(ctx.global_data_mapped + base_offset);
            ctx.per_frame_objects[i].global_data_offset = base_offset;
        }

        VkDescriptorBufferInfo ubo_info = {
            .buffer = ctx.global_data_buffer,
            .offset = 0,
            .range = sizeof(gpu::GlobalData),
        };

        VkWriteDescriptorSet write_desc = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = res_system.frame_set,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
            .pBufferInfo = &ubo_info,
        };
        vkUpdateDescriptorSets(ctx.device, 1, &write_desc, 0, nullptr);

        pipelines::build_all();

        ctx.culling_shader = assets::pin(assets::load<shader_t>("engine://assets/shaders/culling.slang"));

        ctx.cluster_shader = assets::pin(assets::load<shader_t>("engine://assets/shaders/cluster_build.slang"));

        ctx.depth_reduce_shader = assets::pin(assets::load<shader_t>("engine://assets/shaders/depth_reduce.slang"));

        ctx.uberpost_material = assets::pin(assets::load<material_t>(DEFAULT_FINAL_PASS_PATH));
        ctx.deferred_lighting_material =
            assets::pin(assets::load<material_t>("engine://assets/materials/deferred_lighting.mat"));

        ctx.default_tex = assets::pin(assets::load<texture_t>("engine://assets/textures/default_white.png"));

        ctx.fallback_tex = assets::pin(assets::load<texture_t>(FALLBACK_TEXTURE_PATH));
        ctx.fallback_mesh = assets::pin(assets::load<mesh_t>(FALLBACK_MESH_PATH));
        ctx.fallback_material = assets::pin(assets::load<material_t>(FALLBACK_MATERIAL_PATH));
        ctx.default_material = assets::pin(assets::load<material_t>(DEFAULT_MATERIAL_PATH));

        overlay::init();

        return true;
    }

    void reset_assets()
    {
        // every view has its own culling shader instance
        for (per_frame_t& frame_data : ctx.per_frame_objects)
        {
            // the flat map yields a pair holding a reference, so bind it by value
            for (auto [name_hash, vgr_ptr] : frame_data.views)
            {
                vgr_ptr->culling_instance.shutdown();
                vgr_ptr->cluster_instance.shutdown();
            }

            frame_data.depth_reduce_instance.shutdown();
        }

        assets::unpin(ctx.culling_shader);
        assets::unpin(ctx.cluster_shader);
        assets::unpin(ctx.depth_reduce_shader);
        assets::unpin(ctx.uberpost_material);
        assets::unpin(ctx.deferred_lighting_material);
        assets::unpin(ctx.default_tex);
        assets::unpin(ctx.fallback_tex);
        assets::unpin(ctx.fallback_mesh);
        assets::unpin(ctx.fallback_material);
        assets::unpin(ctx.default_material);
        rendergraph.clear();
        custom_renderer_features.clear();
    }

    void shutdown()
    {
        readback::shutdown();
        gpu_timers::shutdown();
        overlay::shutdown();
        frame_upload::shutdown();
        frame::shutdown();
        pipelines::shutdown();
        save_pipeline_cache();
        if (ctx.pipeline_cache != VK_NULL_HANDLE)
        {
            vkDestroyPipelineCache(ctx.device, ctx.pipeline_cache, nullptr);
            ctx.pipeline_cache = VK_NULL_HANDLE;
        }

        destroy_persistent_image(ctx.punctual_shadow_atlas);

#ifdef TAU_ENABLE_PROFILING
        if (tracy_vk_ctx) { TracyVkDestroy(tracy_vk_ctx); }
#endif

        if (!ctx.samplers.empty())
        {
            for (VkSampler sampler : ctx.samplers) { vkDestroySampler(ctx.device, sampler, nullptr); }
        }

        res_system.process_deletions(UINT64_MAX, UINT64_MAX);

        shutdown_resources();

        if (ctx.timeline_semaphore != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(ctx.device, ctx.timeline_semaphore, nullptr);
        }

        if (ctx.transfer_command_pool != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(ctx.device, ctx.transfer_command_pool, nullptr);
        }

        for (per_frame_t& frame_data : ctx.per_frame_objects) { shutdown_per_frame(frame_data); }

        ctx.per_frame_objects.clear();

        if (ctx.global_data_buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(ctx.allocator, ctx.global_data_buffer, ctx.global_data_alloc);
        }

        for (VkImageView image_view : ctx.swapchain.views) { vkDestroyImageView(ctx.device, image_view, nullptr); }

        for (VkSemaphore sem : ctx.swapchain.release_semaphores) { vkDestroySemaphore(ctx.device, sem, nullptr); }

        if (ctx.swapchain.depth_view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(ctx.device, ctx.swapchain.depth_view, nullptr);
        }

        if (ctx.swapchain.depth_image != VK_NULL_HANDLE)
        {
            vmaDestroyImage(ctx.allocator, ctx.swapchain.depth_image, ctx.swapchain.depth_allocation);
        }

        if (ctx.swapchain.handle != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(ctx.device, ctx.swapchain.handle, nullptr);
        }

        device::destroy();
    }

    namespace
    {
        struct fallback_light_t
        {
            bool enabled = false;
            vec3 direction = {0.0f, -1.0f, 0.0f};
            vec3 color = {1.0f, 1.0f, 1.0f};
            f32 intensity = 1.0f;
        };

        fallback_light_t fallback_light;
    } // namespace

    void set_fallback_light(bool enabled, const vec3& direction, const vec3& color, f32 intensity)
    { fallback_light = {enabled, direction, color, intensity}; }

    // walks the light components into the frame's mapped gpu buffers and returns the globals block describing them
    // the cascade caster goes first so it lands at index 0, the only directional light the shadow path looks at
    static gpu::GlobalData upload_lights(ecs::registry_t& reg, per_frame_t& frame_data,
                                         const flat_map_t<u32_t>& punctual_slots, ecs::entity_t cascade_light)
    {
        gpu::GlobalData globals{};
        globals.time = time::time;
        globals.shadow_atlas_id = BINDLESS_NULL_HANDLE;
        globals.shadow_cascade_count = 0;
        globals.punctual_shadow_atlas_id = BINDLESS_NULL_HANDLE;
        globals.punctual_shadow_count = 0;

        u32_t dir_count = 0;
        auto upload_dir_light = [&](const directional_light_t& light, const transform_t& transform)
        {
            gpu::DirectionalLight& gpu_light = frame_data.mapped_dir_lights[dir_count];

            gpu_light.direction_intensity = vec4(world_forward(transform), light.intensity);
            gpu_light.color = vec4(light.color, 0.0f);

            dir_count++;
        };

        if (cascade_light != ecs::NULL_ENTITY && reg.valid(cascade_light) &&
            reg.all_of<directional_light_t, transform_t>(cascade_light))
        {
            upload_dir_light(reg.get<directional_light_t>(cascade_light), reg.get<transform_t>(cascade_light));
        }

        for (auto [entity, light, transform] : reg.view<directional_light_t, transform_t>().each())
        {
            if (dir_count >= MAX_DIR_LIGHTS) { break; }
            if (entity == cascade_light) { continue; }

            upload_dir_light(light, transform);
        }

        if (dir_count == 0 && fallback_light.enabled)
        {
            gpu::DirectionalLight& gpu_light = frame_data.mapped_dir_lights[dir_count];
            gpu_light.direction_intensity = vec4(fallback_light.direction, fallback_light.intensity);
            gpu_light.color = vec4(fallback_light.color, 0.0f);
            dir_count++;
        }

        u32_t point_count = 0;
        for (auto [entity, light, transform] : reg.view<point_light_t, transform_t>().each())
        {
            if (point_count >= MAX_LIGHTS) { break; }

            gpu::PointLight& gpu_light = frame_data.mapped_point_lights[point_count];

            gpu_light.position_radius = vec4(world_position(transform), light.radius);
            gpu_light.color_intensity = vec4(light.color, light.intensity);
            gpu_light.shadow_params = shadow_params(punctual_slots, entity);

            point_count++;
        }

        u32_t spot_count = 0;
        for (auto [entity, light, transform] : reg.view<spot_light_t, transform_t>().each())
        {
            if (spot_count >= MAX_LIGHTS) { break; }

            gpu::SpotLight& gpu_light = frame_data.mapped_spot_lights[spot_count];

            gpu_light.position_range = vec4(world_position(transform), light.radius);
            gpu_light.direction_intensity = vec4(world_forward(transform), light.intensity);
            gpu_light.color_inner_cos = vec4(light.color, std::cos(radians(light.inner_angle)));
            gpu_light.outer_cos = vec4(std::cos(radians(light.outer_angle)), 0.0f, 0.0f, 0.0f);
            gpu_light.shadow_params = shadow_params(punctual_slots, entity);

            spot_count++;
        }

        globals.dir_light_count = dir_count;
        globals.dir_light_buffer = frame_data.dir_light_buffer_address;

        globals.point_light_count = point_count;
        globals.point_light_buffer = frame_data.point_light_buffer_address;

        globals.spot_light_count = spot_count;
        globals.spot_light_buffer = frame_data.spot_light_buffer_address;

        globals.shadow_cascade_buffer = frame_data.shadow_cascade_buffer_address;
        globals.punctual_shadow_buffer = frame_data.punctual_shadow_buffer_address;

        return globals;
    }

    namespace
    {
        // once per asset, not once per frame: a broken mesh would write sixty lines a second
        std::unordered_set<u64_t> reported_missing;

        void report_missing(const char* what, asset_handle_t handle)
        {
            if (!reported_missing.insert(handle.guid).second) { return; }

            const std::string path = assets::path_of(handle);
            TAU_LOG_ERROR("RENDERER", "Falling back for {} '{}' -- it is not loaded", what,
                          path.empty() ? std::string("<unknown>") : path);
        }

        // one renderable with everything the sort and fill need resolved, so the comparator only compares
        struct draw_item_t
        {
            u32_t sort_key; // pass * 16 + blend mode
            VkPipeline pipeline;
            material_t* material;
            shader_t* shader;
            mesh_t* mesh;
            // null: the whole mesh
            const submesh_t* submesh;
            const transform_t* transform;
            bool casts_shadow;
            u32_t entity;
        };

        std::vector<draw_item_t> draw_items;
    } // namespace

    // three linear passes: gather what each renderable needs, sort so each pipeline's objects are contiguous
    // then pack the object and cull buffers and record run and range starts. returns the object count
    static u32_t build_draw_list(ecs::registry_t& reg, per_frame_t& frame_data)
    {
        draw_items.clear();

        for (auto [entity, transform, renderer] : reg.view<transform_t, mesh_renderer_t>().each())
        {
            if (!renderer.active || !renderer.mesh) { continue; }

            mesh_t* mesh = assets::get(renderer.mesh);
            if (mesh == nullptr)
            {
                report_missing("mesh", renderer.mesh);

                mesh = assets::get(ctx.fallback_mesh);
                if (mesh == nullptr) { continue; }
            }

            const u32_t submesh_count = std::max<u32_t>(1, static_cast<u32_t>(mesh->submeshes.size()));
            for (u32_t submesh = 0; submesh < submesh_count; submesh++)
            {
                asset_t<material_t> handle = slot_material(renderer, mesh, submesh);
                if (!handle.is_valid()) { handle = ctx.default_material; }

                material_t* mat = assets::get(handle);
                shader_t* shader = mat ? assets::get(mat->shader) : nullptr;

                // a material that will not load (usually its shader stopped compiling) draws as the fallback
                if (mat == nullptr || shader == nullptr)
                {
                    report_missing("material", handle);

                    mat = assets::get(ctx.fallback_material);
                    shader = mat ? assets::get(mat->shader) : nullptr;
                    if (mat == nullptr || shader == nullptr) { continue; }
                }

                draw_items.push_back({
                    static_cast<u32_t>(pass_for(*shader)) * 16u + static_cast<u32_t>(shader->module.blend_mode),
                    binding_pipeline(*shader),
                    mat,
                    shader,
                    mesh,
                    submesh < mesh->submeshes.size() ? &mesh->submeshes[submesh] : nullptr,
                    &transform,
                    renderer.casts_shadow,
                    static_cast<u32_t>(entt::to_integral(entity)) + 1,
                });
            }
        }

        // pass and blend first so mesh passes come out as ranges, then pipeline so each bin is one run
        // then material so the sync below sees each material once
        std::sort(draw_items.begin(), draw_items.end(),
                  [](const draw_item_t& lhs, const draw_item_t& rhs)
                  {
                      if (lhs.sort_key != rhs.sort_key) { return lhs.sort_key < rhs.sort_key; }
                      if (lhs.pipeline != rhs.pipeline) { return lhs.pipeline < rhs.pipeline; }

                      return lhs.material < rhs.material;
                  });

        u32_t cur_object_id = 0;
        frame_data.active_pipelines.clear();
        VkPipeline last_pipeline = VK_NULL_HANDLE;
        material_t* last_material = nullptr;

        for (const draw_item_t& item : draw_items)
        {
            // the run is contiguous, so one sync per material rather than one per object using it
            if (item.material != last_material)
            {
                last_material = item.material;
                item.material->sync();
            }

            if (item.pipeline != last_pipeline)
            {
                last_pipeline = item.pipeline;
                frame_data.active_pipelines.push_back({
                    last_pipeline,
                    item.shader->get_pipeline(pipeline_variant_e::SHADOW),
                    item.shader->get_pipeline(pipeline_variant_e::GBUFFER),
                    item.shader->pipeline_layout,
                    static_cast<u32_t>(frame_data.active_pipelines.size()),
                    pass_for(*item.shader),
                    cur_object_id,
                    0,
                });
            }

            const transform_t& transform = *item.transform;
            const mesh_t& mesh = *item.mesh;

            gpu::ObjectData& obj_data = frame_data.mapped_object_data[cur_object_id];
            obj_data.model_matrix = transform.world_mat;
            obj_data.normal_matrix = transpose(inverse(transform.world_mat));

            obj_data.material_offset = item.material->heap_offset[ctx.cur_frame];
            obj_data.entity = item.entity;
            const u32_t first_index = item.submesh ? item.submesh->first_index : 0;
            const u32_t index_count = item.submesh ? item.submesh->index_count : mesh.index_count;
            const vec3 local_center = item.submesh ? item.submesh->local_center : mesh.local_center;
            const f32 local_radius = item.submesh ? item.submesh->local_radius : mesh.local_radius;

            obj_data.vertex_buffer = mesh.vertex_buffer_address;
            // vertices are pulled through the index address, so a submesh is its first index
            obj_data.index_buffer = mesh.index_buffer_address + u64_t(first_index) * sizeof(u32_t);

            const active_pipeline_t& bin = frame_data.active_pipelines.back();
            const bool has_shadow = bin.shadow_pipeline != VK_NULL_HANDLE;

            const vec3 world_center = transform_point(transform.world_mat, local_center);
            const vec3 scale = scale_of(transform.world_mat);
            const f32 max_scale = std::max({scale.x, scale.y, scale.z});

            gpu::CullData& cull = frame_data.mapped_cull_data[cur_object_id];
            cull.bounding_sphere = vec4(world_center, local_radius * max_scale);
            cull.index_count = index_count;
            cull.pipeline_index = bin.pipeline_index;
            cull.flags = (item.casts_shadow && has_shadow) ? 1u : 0u;
            cull.draw_bin_offset = bin.first_object;

            cur_object_id++;
        }

        for (size_t i = 0; i < frame_data.active_pipelines.size(); i++)
        {
            const u32_t run_end = (i + 1 < frame_data.active_pipelines.size())
                                      ? frame_data.active_pipelines[i + 1].first_object
                                      : cur_object_id;
            frame_data.active_pipelines[i].object_count = run_end - frame_data.active_pipelines[i].first_object;
        }

        frame_data.object_counter = cur_object_id;

        {
            const u32_t pipeline_count = static_cast<u32_t>(frame_data.active_pipelines.size());
            u32_t cursor = 0;

            for (u32_t p = 0; p < MESH_PASS_COUNT; p++)
            {
                const u32_t begin = cursor;
                while (cursor < pipeline_count && static_cast<u32_t>(frame_data.active_pipelines[cursor].pass) == p)
                {
                    cursor++;
                }
                frame_data.pass_ranges[p] = {begin, cursor};
            }

            if (cursor != pipeline_count)
            {
                TAU_LOG_WARN("RENDERER",
                             "{} pipelines fell outside their pass range, the sort no longer "
                             "groups by pass, so some geometry will not draw",
                             pipeline_count - cursor);
            }
        }

        return cur_object_id;
    }

    // one submit carrying the swapchain acquire and the resource upload timeline, then present
    // the timeline value signalled here is what the next frame's readback waits on
    // headless: timelines only
    static void submit_headless(VkCommandBuffer cmd, per_frame_t& frame_data)
    {
        const u64_t next_timeline_value = ++ctx.timeline_value;
        const u64_t wait_value = res_system.timeline_value;

        VkTimelineSemaphoreSubmitInfo timeline_sem_info = {
            .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
            .waitSemaphoreValueCount = 1,
            .pWaitSemaphoreValues = &wait_value,
            .signalSemaphoreValueCount = 1,
            .pSignalSemaphoreValues = &next_timeline_value,
        };
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        const VkSubmitInfo submit_info = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = &timeline_sem_info,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &res_system.timeline_semaphore,
            .pWaitDstStageMask = &wait_stage,
            .commandBufferCount = 1,
            .pCommandBuffers = &cmd,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &ctx.timeline_semaphore,
        };
        vkQueueSubmit(ctx.graphics_queue, 1, &submit_info, VK_NULL_HANDLE);
        frame_data.target_timeline_value = next_timeline_value;
    }

    static void submit_and_present(VkCommandBuffer cmd, per_frame_t& frame_data, u32_t image_index)
    {
        VkSemaphore wait_semaphores[] = {
            frame_data.swapchain_acquire_semaphore,
            res_system.timeline_semaphore,
        };

        VkSemaphore signal_semaphores[] = {
            ctx.swapchain.release_semaphores[image_index],
            ctx.timeline_semaphore,
        };

        VkPipelineStageFlags wait_stages[] = {
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        };

        u64_t wait_values[] = {0, res_system.timeline_value};

        u64_t next_timeline_value = ctx.timeline_value + 1;
        ctx.timeline_value = next_timeline_value;
        u64_t signal_values[] = {0, next_timeline_value};

        VkTimelineSemaphoreSubmitInfo timeline_sem_info = {
            .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
            .waitSemaphoreValueCount = 2,
            .pWaitSemaphoreValues = wait_values,
            .signalSemaphoreValueCount = 2,
            .pSignalSemaphoreValues = signal_values,
        };

        VkSubmitInfo submit_info = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = &timeline_sem_info,
            .waitSemaphoreCount = 2,
            .pWaitSemaphores = wait_semaphores,
            .pWaitDstStageMask = wait_stages,
            .commandBufferCount = 1,
            .pCommandBuffers = &cmd,
            .signalSemaphoreCount = 2,
            .pSignalSemaphores = signal_semaphores,
        };

        vkQueueSubmit(ctx.graphics_queue, 1, &submit_info, VK_NULL_HANDLE);

        frame_data.target_timeline_value = next_timeline_value;

        VkPresentInfoKHR present_info = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &ctx.swapchain.release_semaphores[image_index],
            .swapchainCount = 1,
            .pSwapchains = &ctx.swapchain.handle,
            .pImageIndices = &image_index,
        };

        const VkResult res = vkQueuePresentKHR(ctx.present_queue, &present_info);

        if (res == VK_ERROR_SURFACE_LOST_KHR) { recreate_surface(); }
        else if (res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_OUT_OF_DATE_KHR) { resize(); }
    }

    // resources uploaded on the transfer queue need their ownership acquired on the graphics queue before sampling
    static void flush_pending_acquires(VkCommandBuffer cmd)
    {
        std::scoped_lock lock(res_system.pending_mutex);
        if (res_system.pending_acquires.empty()) { return; }

        std::vector<VkImageMemoryBarrier> image_barriers;
        std::vector<VkBufferMemoryBarrier> buffer_barriers;

        for (pending_resource_t& res : res_system.pending_acquires)
        {
            if (res.type == resource_type_e::TEXTURE) { image_barriers.push_back(res.barrier.image_barrier); }
            else
            {
                buffer_barriers.push_back(res.barrier.buffer_barrier);
            }
        }

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, nullptr,
                             static_cast<u32_t>(buffer_barriers.size()), buffer_barriers.data(),
                             static_cast<u32_t>(image_barriers.size()), image_barriers.data());

        res_system.pending_acquires.clear();
    }

    // the swapchain may be rotated relative to the display, so colour views are pre multiplied by the rotation
    static mat4 surface_pre_rotation()
    {
        if (ctx.cur_surface_transform & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) { return rotate_z({}, radians(90.0f)); }
        if (ctx.cur_surface_transform & VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR)
        {
            return rotate_z({}, radians(180.0f));
        }
        if (ctx.cur_surface_transform & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)
        {
            return rotate_z({}, radians(270.0f));
        }
        return {};
    }

    static void create_device_buffer(const char* name, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer,
                                     VmaAllocation& alloc)
    {
        VkBufferCreateInfo buf_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        };
        VmaAllocationCreateInfo alloc_info = {
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        };

        VK_CHECK(vmaCreateBuffer(ctx.allocator, &buf_info, &alloc_info, &buffer, &alloc, nullptr));
        device::name(buffer, VK_OBJECT_TYPE_BUFFER, name);
    }

    void render(ecs::registry_t& reg)
    {
        ZoneScoped;

        frame_views.clear();
        frame_views.swap(submitted_views);

        frame_outputs.clear();
        frame_outputs.swap(submitted_outputs);

        flat_map_t<u32_t> frame_punctual_slots = submitted_punctual_slots;
        submitted_punctual_slots.clear();

        const ecs::entity_t frame_cascade_light = submitted_cascade_light;
        submitted_cascade_light = ecs::NULL_ENTITY;

        if (pending_resize)
        {
            if (image_acquired) { deferred_resize = true; }
            else
            {
                resize();
            }
            pending_resize = false;
        }

        // each entry waits on the timeline its value came from: see deletion_clock_e
        u64_t completed_frame = 0;
        vkGetSemaphoreCounterValue(ctx.device, ctx.timeline_semaphore, &completed_frame);
        u64_t completed_transfer = 0;
        vkGetSemaphoreCounterValue(ctx.device, res_system.timeline_semaphore, &completed_transfer);

        res_system.process_deletions(completed_frame, completed_transfer);

        save_pipeline_cache_when_settled();

        u32_t index = acquired_index;
        if (!image_acquired)
        {
            const VkResult res = acquire_next_image(&index);

            if (res == VK_ERROR_SURFACE_LOST_KHR)
            {
                recreate_surface();
                return;
            }
            else if (res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_OUT_OF_DATE_KHR)
            {
                resize();
                return;
            }
            else if (res != VK_SUCCESS)
            {
                vkQueueWaitIdle(ctx.present_queue);
                return;
            }
        }

        per_frame_t& frame_data = ctx.per_frame_objects[ctx.cur_frame];
        VkCommandBuffer cmd = frame_data.main_command_buffer;

        read_depth_bounds();

        VkCommandBufferBeginInfo cmd_begin_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };

        renderdoc::frame_begin();
        vkBeginCommandBuffer(cmd, &cmd_begin_info);

#ifdef TAU_ENABLE_PROFILING
        TracyVkCollect(tracy_vk_ctx, cmd);
#endif
        gpu_timers::begin_frame(cmd, ctx.cur_frame);

        flush_pending_acquires(cmd);

        const mat4 pre_rot_mat = surface_pre_rotation();

        bool has_primary = false;
        for (const render_view_t& v : frame_views)
        {
            if (v.name_hash == "PrimaryView"_h)
            {
                has_primary = true;
                break;
            }
        }
        if (!has_primary)
        {
            ecs::entity_t active_cam = camera_system::get_active_camera(reg);
            if (active_cam != ecs::NULL_ENTITY)
            {
                camera_t& cam = reg.get<camera_t>(active_cam);
                transform_t& cam_transform = reg.get<transform_t>(active_cam);

                render_view_t v{};
                v.name_hash = "PrimaryView"_h;
                v.kind = view_kind_e::COLOR;
                v.view = cam.view;
                v.projection = cam.projection;
                v.view_proj = cam.view_proj;
                v.position = world_position(cam_transform);
                v.color_target_hash = "SceneColor"_h;
                v.depth_target_hash = "SceneDepth"_h;
                v.extent = ctx.render_extent;
                v.z_near = cam.near_plane;
                v.z_far = cam.far_plane;
                frame_views.push_back(v);
            }
        }

        std::stable_partition(frame_views.begin(), frame_views.end(),
                              [](const render_view_t& v) { return v.kind == view_kind_e::DEPTH_ONLY; });

        gpu::GlobalData frame_globals = upload_lights(reg, frame_data, frame_punctual_slots, frame_cascade_light);

        const u32_t cur_object_id = build_draw_list(reg, frame_data);
        frame_globals.object_count = cur_object_id;
        frame_globals.active_pipeline_count = static_cast<u32_t>(frame_data.active_pipelines.size());

        if (frame_views.size() > MAX_VIEWS_PER_FRAME)
        {
            TAU_LOG_FATAL("RENDERER", "Submitted {} views exceeds MAX_VIEWS_PER_FRAME ({})", frame_views.size(),
                          MAX_VIEWS_PER_FRAME);
            abort();
        }

        shader_t* culling_shader = assets::get(ctx.culling_shader);
        shader_t* cluster_shader = assets::get(ctx.cluster_shader);

        for (u32_t view_idx = 0; view_idx < frame_views.size(); view_idx++)
        {
            render_view_t& sub = frame_views[view_idx];
            u32_t view_offset = frame_data.global_data_offset + view_idx * ctx.global_data_aligned_size;

            std::unique_ptr<view_gpu_resources_t>& vgr_slot = frame_data.views[sub.name_hash];
            if (vgr_slot == nullptr)
            {
                vgr_slot = std::make_unique<view_gpu_resources_t>();
                vgr_slot->culling_instance.init(ctx.culling_shader);
                vgr_slot->cluster_instance.init(ctx.cluster_shader);
            }
            view_gpu_resources_t& vgr = *vgr_slot;
            vgr.kind = sub.kind;
            vgr.global_data_offset = view_offset;

            gpu::GlobalData* slot = reinterpret_cast<gpu::GlobalData*>(ctx.global_data_mapped + view_offset);
            *slot = frame_globals;

            slot->view = sub.view;
            slot->projection = sub.projection;

            const mat4 final_view_proj = sub.kind == view_kind_e::COLOR ? pre_rot_mat * sub.view_proj : sub.view_proj;
            slot->view_proj = final_view_proj;
            slot->inv_view_proj = inverse(final_view_proj);

            const debug_view_e* debug_view = view_debug_views.find(sub.name_hash);
            slot->debug_view = static_cast<u32_t>(debug_view ? *debug_view : debug_view_e::OFF);

            frustum_planes(sub.view_proj, slot->frustum_planes);
            slot->camera_pos = vec4(sub.position, 0.0f);

            slot->cull_flags = (sub.kind == view_kind_e::DEPTH_ONLY) ? 1u : 0u;

            if (sub.kind == view_kind_e::COLOR && cluster_shader != nullptr &&
                cluster_shader->get_pipeline(pipeline_variant_e::FORWARD) != VK_NULL_HANDLE)
            {
                const f32 z_near = sub.z_near;
                const f32 z_far = sub.z_far;

                if (z_near > 0.0f && z_far > z_near)
                {
                    const u32_t tiles_x = cluster_tiles_for(sub.extent.width, MAX_CLUSTER_TILES_X);
                    const u32_t tiles_y = cluster_tiles_for(sub.extent.height, MAX_CLUSTER_TILES_Y);
                    const u32_t cluster_count = tiles_x * tiles_y * CLUSTER_SLICES_Z;

                    if (vgr.cluster_counts_buffer == VK_NULL_HANDLE || vgr.cluster_count != cluster_count)
                    {
                        if (vgr.cluster_counts_buffer != VK_NULL_HANDLE)
                        {
                            vmaDestroyBuffer(ctx.allocator, vgr.cluster_counts_buffer, vgr.cluster_counts_alloc);
                            vmaDestroyBuffer(ctx.allocator, vgr.cluster_indices_buffer, vgr.cluster_indices_alloc);
                        }

                        create_device_buffer("ClusterCounts", cluster_count * sizeof(u32_t),
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                             vgr.cluster_counts_buffer, vgr.cluster_counts_alloc);
                        create_device_buffer(
                            "ClusterIndices",
                            static_cast<VkDeviceSize>(cluster_count) * MAX_LIGHTS_PER_CLUSTER * sizeof(u32_t),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                            vgr.cluster_indices_buffer, vgr.cluster_indices_alloc);

                        vgr.cluster_counts_address = get_buffer_address(vgr.cluster_counts_buffer);
                        vgr.cluster_indices_address = get_buffer_address(vgr.cluster_indices_buffer);
                        vgr.cluster_count = cluster_count;
                    }

                    slot->cluster_z_near = z_near;
                    slot->cluster_z_far = z_far;
                    slot->cluster_tiles_x = tiles_x;
                    slot->cluster_tiles_y = tiles_y;
                    slot->cluster_counts_buffer = vgr.cluster_counts_address;
                    slot->cluster_indices_buffer = vgr.cluster_indices_address;

                    vgr.cluster_instance.set_buffer("cluster_light_counts"_h, vgr.cluster_counts_buffer);
                    vgr.cluster_instance.set_buffer("cluster_light_indices"_h, vgr.cluster_indices_buffer);
                    vgr.cluster_instance.sync();

                    const gpu::PushConstants cluster_pc = {
                        frame_data.object_buffer_address,
                        res_system.material_heap.device_address,
                        0,
                    };

                    dispatch_compute(cmd, *cluster_shader, cluster_shader->get_pipeline(pipeline_variant_e::FORWARD),
                                     view_offset, &vgr.cluster_instance, cluster_pc, (vgr.cluster_count + 63) / 64);

                    buffer_barrier(cmd, {vgr.cluster_counts_buffer, vgr.cluster_indices_buffer},
                                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
                }
            }

            if (!sub.needs_render) { continue; }

            if (cur_object_id == 0 || frame_data.active_pipelines.empty()) { continue; }

            VkDeviceSize req_counts = frame_data.active_pipelines.size() * sizeof(u32_t);
            VkDeviceSize req_indirect = static_cast<VkDeviceSize>(cur_object_id) * 16;

            if (vgr.draw_counts_size < req_counts)
            {
                if (vgr.draw_counts_buffer != VK_NULL_HANDLE)
                {
                    vmaDestroyBuffer(ctx.allocator, vgr.draw_counts_buffer, vgr.draw_counts_alloc);
                }
                create_device_buffer("DrawCounts", req_counts,
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                     vgr.draw_counts_buffer, vgr.draw_counts_alloc);
                vgr.draw_counts_size = req_counts;
            }

            if (vgr.indirect_size < req_indirect)
            {
                if (vgr.indirect_buffer != VK_NULL_HANDLE)
                {
                    vmaDestroyBuffer(ctx.allocator, vgr.indirect_buffer, vgr.indirect_alloc);
                }
                create_device_buffer("DrawIndirect", req_indirect,
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                     vgr.indirect_buffer, vgr.indirect_alloc);
                vgr.indirect_size = req_indirect;
            }

            vkCmdFillBuffer(cmd, vgr.draw_counts_buffer, 0, req_counts, 0);

            buffer_barrier(cmd, {vgr.draw_counts_buffer}, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

            vgr.culling_instance.set_buffer("draw_counts"_h, vgr.draw_counts_buffer);
            vgr.culling_instance.set_buffer("indirect_commands"_h, vgr.indirect_buffer);
            vgr.culling_instance.set_buffer("cull_inputs"_h, frame_data.cull_buffer);
            vgr.culling_instance.sync();

            const gpu::PushConstants pc_data = {
                frame_data.object_buffer_address,
                res_system.material_heap.device_address,
                0,
            };

            dispatch_compute(cmd, *culling_shader, culling_shader->get_pipeline(pipeline_variant_e::FORWARD),
                             view_offset, &vgr.culling_instance, pc_data, (cur_object_id + 63) / 64);

            buffer_barrier(cmd, {vgr.indirect_buffer, vgr.draw_counts_buffer}, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                           VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
        }

        tau::active_arena = &frame_data.frame_allocator;

        rendergraph.clear();

        // headless: offscreen target, size from the runner
        const output_target_t default_output = ctx.caps.headless ? output_target_t{"HeadlessOutput"_h, {1280, 720}}
                                                                 : output_target_t{"Swapchain"_h, ctx.swapchain.extent};
        output_target_t final_output = frame_outputs.empty() ? default_output : frame_outputs[0];
        bool output_to_swapchain = (final_output.target_hash == "Swapchain"_h);
        if (output_to_swapchain || final_output.extent.width == 0 || final_output.extent.height == 0)
        {
            final_output.extent = ctx.swapchain.extent;
        }
        ctx.render_extent = final_output.extent;

        image_desc_t color_desc = {
            .width = ctx.render_extent.width,
            .height = ctx.render_extent.height,
            .format = VK_FORMAT_R16G16B16A16_SFLOAT,
            // transfer source: the runner reads the HDR back
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        };

        // the primary targets, a colour view naming different targets gets its own pair
        // created at its own extent inside the view loop below
        rendergraph.create_image("SceneColor"_h, "SceneColor", color_desc);

        image_desc_t albedo_desc = color_desc;
        albedo_desc.format = VK_FORMAT_R8G8B8A8_SRGB;

        image_desc_t gbuffer_material_desc = color_desc;
        gbuffer_material_desc.format = VK_FORMAT_R8G8B8A8_UNORM;

        image_desc_t normal_desc = color_desc;
        normal_desc.format = VK_FORMAT_R16G16_SFLOAT;

        image_desc_t depth_desc = {
            .width = ctx.render_extent.width,
            .height = ctx.render_extent.height,
            .format = ctx.swapchain.depth_format,
            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
        };

        rendergraph.create_image("SceneDepth"_h, "SceneDepth", depth_desc);

        rg_resource_id shadow_atlas_res = RG_NULL_ID;
        rg_resource_id punctual_atlas_res = RG_NULL_ID;
        u32_t shadow_cascade_count = 0;
        u32_t punctual_shadow_count = 0;
        f32 shadow_fade_start = 0.0f;
        f32 shadow_fade_end = 0.0f;
        for (const render_view_t& sub : frame_views)
        {
            if (!sub.has_depth_desc) { continue; }

            if (sub.shadow_tile == shadow_tile_e::PUNCTUAL)
            {
                if (sub.shadow_slot >= MAX_PUNCTUAL_SHADOW_TILES) { continue; }

                rg_resource_id punctual_id = rendergraph.get_resource(sub.depth_target_hash);
                if (punctual_id == RG_NULL_ID)
                {
                    ensure_persistent_image(ctx.punctual_shadow_atlas, sub.depth_desc, "PunctualShadowAtlas");

                    punctual_id = rendergraph.import_persistent_image(
                        sub.depth_target_hash, "PunctualShadowAtlas", ctx.punctual_shadow_atlas.image,
                        ctx.punctual_shadow_atlas.view, ctx.punctual_shadow_atlas.desc,
                        ctx.punctual_shadow_atlas.bindless_id, ctx.punctual_shadow_atlas.layout);

                    if (ctx.punctual_shadow_atlas.needs_full_clear)
                    {
                        rg_pass_t& clear_pass = rendergraph.add_pass("PunctualAtlasClear"_h, "PunctualAtlasClear");
                        clear_pass.depth_stencil = punctual_id;
                        clear_pass.force_depth_clear = true;
                        ctx.punctual_shadow_atlas.needs_full_clear = false;
                    }
                }
                if (punctual_atlas_res == RG_NULL_ID) { punctual_atlas_res = punctual_id; }

                // perspective tile: the world width the far plane covers, over the tile's texels
                const f32 punctual_m00 = std::fabs(sub.projection[0][0]);
                const f32 punctual_texel =
                    (punctual_m00 > 0.0f && sub.shadow_atlas_rect.extent.width > 0)
                        ? (2.0f * sub.z_far / punctual_m00) / static_cast<f32>(sub.shadow_atlas_rect.extent.width)
                        : 0.0f;

                pack_shadow_tile(frame_data.mapped_punctual_shadows[sub.shadow_slot], sub,
                                 static_cast<f32>(sub.depth_desc.width), static_cast<f32>(sub.depth_desc.height),
                                 punctual_texel);

                punctual_shadow_count = std::max(punctual_shadow_count, sub.shadow_slot + 1);
                continue;
            }

            if (shadow_cascade_count >= SHADOW_CASCADE_COUNT) { continue; }

            rg_resource_id id = rendergraph.get_resource(sub.depth_target_hash);
            if (id == RG_NULL_ID)
            {
                id = rendergraph.create_image(sub.depth_target_hash, "ShadowAtlas", sub.depth_desc);
            }
            if (shadow_atlas_res == RG_NULL_ID) { shadow_atlas_res = id; }

            // orthographic cascade: the ortho extent is 2/m00 wide, over the tile's texels
            const f32 m00 = std::fabs(sub.projection[0][0]);
            const f32 cascade_texel = (m00 > 0.0f && sub.shadow_atlas_rect.extent.width > 0)
                                          ? (2.0f / m00) / static_cast<f32>(sub.shadow_atlas_rect.extent.width)
                                          : 0.0f;

            pack_shadow_tile(frame_data.mapped_shadow_cascades[shadow_cascade_count], sub,
                             static_cast<f32>(sub.depth_desc.width), static_cast<f32>(sub.depth_desc.height),
                             cascade_texel);

            shadow_fade_end = sub.shadow_split_far;
            shadow_cascade_count++;
        }

        shadow_fade_start = shadow_fade_end * 0.9f;

        // a target the view named itself, created at the view's extent the first time it is asked for
        // two colour views share an image only by naming the same one
        auto ensure_view_target = [&](u32_t name_hash, const char* debug_name, const image_desc_t& base,
                                      VkExtent2D extent) -> rg_resource_id
        {
            const rg_resource_id existing = rendergraph.get_resource(name_hash);
            if (existing != RG_NULL_ID) { return existing; }

            image_desc_t desc = base;
            desc.width = extent.width;
            desc.height = extent.height;
            return rendergraph.create_image(name_hash, debug_name, desc);
        };

        for (const render_view_t& sub : frame_views)
        {
            rg_resource_id depth_id = RG_NULL_ID;
            if (sub.has_depth_desc || sub.depth_target_hash != 0)
            {
                depth_id = rendergraph.get_resource(sub.depth_target_hash);
                if (depth_id == RG_NULL_ID && sub.kind == view_kind_e::COLOR)
                {
                    depth_id = ensure_view_target(sub.depth_target_hash, "ViewDepth", depth_desc, sub.extent);
                }
            }

            rg_resource_id color_id = RG_NULL_ID;
            if (sub.kind == view_kind_e::COLOR && sub.color_target_hash != 0)
            {
                color_id = ensure_view_target(sub.color_target_hash, "ViewColor", color_desc, sub.extent);
            }

            if (sub.kind == view_kind_e::DEPTH_ONLY)
            {
                if (!sub.needs_render) { continue; } // already in the atlas from an earlier frame

                rg_pass_t& shadow_pass = add_mesh_pass(rendergraph, sub.name_hash, "ShadowGeometry", sub.name_hash,
                                                       mesh_pass_e::GBUFFER, true, {}, {}, depth_id);
                shadow_pass.render_rect = sub.shadow_atlas_rect;
                shadow_pass.force_depth_clear = true;
                continue;
            }

            std::vector<rg_resource_id> reads;
            if (shadow_atlas_res != RG_NULL_ID) { reads.push_back(shadow_atlas_res); }
            if (punctual_atlas_res != RG_NULL_ID) { reads.push_back(punctual_atlas_res); }

            std::vector<rg_resource_id> writes;
            if (color_id != RG_NULL_ID) { writes.push_back(color_id); }

            // scoped per view: get_resource() returns the first match by name
            // a shared name would hand the second view the first view's gbuffer
            image_desc_t view_albedo_desc = albedo_desc;
            image_desc_t view_normal_desc = normal_desc;
            image_desc_t view_material_desc = gbuffer_material_desc;
            view_albedo_desc.width = view_normal_desc.width = view_material_desc.width = sub.extent.width;
            view_albedo_desc.height = view_normal_desc.height = view_material_desc.height = sub.extent.height;

            rg_resource_id gbuffer_albedo =
                rendergraph.create_image("GBufferAlbedo"_h ^ sub.name_hash, "GBufferAlbedo", view_albedo_desc);
            rg_resource_id gbuffer_normal =
                rendergraph.create_image("GBufferNormal"_h ^ sub.name_hash, "GBufferNormal", view_normal_desc);
            rg_resource_id gbuffer_material =
                rendergraph.create_image("GBufferMaterial"_h ^ sub.name_hash, "GBufferMaterial", view_material_desc);

            add_mesh_pass(rendergraph, sub.name_hash ^ "gbuf"_h, "GBuffer", sub.name_hash, mesh_pass_e::GBUFFER, false,
                          {}, {gbuffer_albedo, gbuffer_normal, gbuffer_material}, depth_id);

            material_t* lighting_mat = assets::get(ctx.deferred_lighting_material);
            if (lighting_mat && lighting_mat->shader.is_valid())
            {
                std::vector<rg_resource_id> lighting_reads = {gbuffer_albedo, gbuffer_normal, gbuffer_material,
                                                              depth_id};
                if (shadow_atlas_res != RG_NULL_ID) { lighting_reads.push_back(shadow_atlas_res); }
                if (punctual_atlas_res != RG_NULL_ID) { lighting_reads.push_back(punctual_atlas_res); }

                add_fullscreen_pass(
                    rendergraph, sub.name_hash ^ "light"_h, "DeferredLighting", lighting_mat, lighting_reads, writes,
                    [gbuffer_albedo, gbuffer_normal, gbuffer_material, depth_id](rendergraph_t& g, material_t& mat)
                    {
                        mat.set_property("albedo_tex"_h, g.get_bindless_id(gbuffer_albedo));
                        mat.set_property("normal_tex"_h, g.get_bindless_id(gbuffer_normal));
                        mat.set_property("material_tex"_h, g.get_bindless_id(gbuffer_material));
                        mat.set_property("depth_tex"_h, g.get_bindless_id(depth_id));
                    },
                    sub.name_hash, depth_id);
            }

            add_mesh_pass(rendergraph, sub.name_hash ^ "fwd"_h, "ForwardOpaque", sub.name_hash,
                          mesh_pass_e::FORWARD_OPAQUE, false, reads, writes, depth_id);
            add_mesh_pass(rendergraph, sub.name_hash, "Blended", sub.name_hash, mesh_pass_e::BLENDED, false, reads,
                          writes, depth_id);

            if (sub.name_hash == "PrimaryView"_h && sub.z_near > 0.0f &&
                add_depth_reduce_pass(rendergraph, frame_data, depth_id, sub.name_hash, ctx.render_extent))
            {
                frame_data.depth_bounds_z_near = sub.z_near;
                frame_data.depth_bounds_pending = true;
            }
        }

        // the legacy block's features (picking ids, selection mask); everything else is a frame producer
        for (const renderer_feature_t& feature : custom_renderer_features) { feature.builder(rendergraph, reg); }

        rendergraph.compile(frame_data);

        if ((shadow_atlas_res != RG_NULL_ID && shadow_cascade_count > 0) ||
            (punctual_atlas_res != RG_NULL_ID && punctual_shadow_count > 0))
        {
            const u32_t shadow_bindless = (shadow_atlas_res != RG_NULL_ID && shadow_cascade_count > 0)
                                              ? rendergraph.get_bindless_id(shadow_atlas_res)
                                              : BINDLESS_NULL_HANDLE;
            const u32_t punctual_bindless = (punctual_atlas_res != RG_NULL_ID && punctual_shadow_count > 0)
                                                ? rendergraph.get_bindless_id(punctual_atlas_res)
                                                : BINDLESS_NULL_HANDLE;

            for (u32_t view_idx = 0; view_idx < frame_views.size(); view_idx++)
            {
                if (frame_views[view_idx].kind != view_kind_e::COLOR) { continue; }

                u32_t view_offset = frame_data.global_data_offset + view_idx * ctx.global_data_aligned_size;
                gpu::GlobalData* slot = reinterpret_cast<gpu::GlobalData*>(ctx.global_data_mapped + view_offset);

                if (shadow_bindless != BINDLESS_NULL_HANDLE)
                {
                    slot->shadow_atlas_id = shadow_bindless;
                    slot->shadow_cascade_count = shadow_cascade_count;
                    slot->shadow_fade_start = shadow_fade_start;
                    slot->shadow_fade_end = shadow_fade_end;
                }

                slot->punctual_shadow_atlas_id = punctual_bindless;
                slot->punctual_shadow_count = (punctual_bindless != BINDLESS_NULL_HANDLE) ? punctual_shadow_count : 0;
            }
        }

        rendergraph.execute(cmd, reg);

        if (punctual_atlas_res != RG_NULL_ID)
        {
            ctx.punctual_shadow_atlas.layout = rendergraph.get_layout(punctual_atlas_res);
        }

        // hand over to graph v2: post, overlay, editor passes, UI
        frame::inputs_t inputs;
        auto hand_over = [&](rg_resource_id id, const char* name)
        {
            const rg_resource_t* r = rendergraph.find(id);
            if (r == nullptr || inputs.legacy_count >= frame::inputs_t::MAX_LEGACY) { return; }
            inputs.legacy[inputs.legacy_count++] = {
                name,          r->image,      r->view, r->desc.format, {r->desc.width, r->desc.height},
                r->cur_layout, r->bindless_id
            };
        };

        inputs.post_view_hash = "PrimaryView"_h;
        rg_resource_id post_depth = RG_NULL_ID;
        for (const render_view_t& v : frame_views)
        {
            if (v.kind == view_kind_e::COLOR && v.name_hash == "PrimaryView"_h)
            {
                inputs.post_view_pos = v.position;
                post_depth = rendergraph.get_resource(v.depth_target_hash);
                break;
            }
        }
        // no colour view (an empty scene has no camera): the primary depth
        if (post_depth == RG_NULL_ID) { post_depth = rendergraph.get_resource("SceneDepth"_h); }
        const bool* post_flag = view_post_processing.find(inputs.post_view_hash);
        inputs.post_enabled = post_flag == nullptr || *post_flag;

        hand_over(rendergraph.get_resource("SceneColor"_h), "SceneColor");
        hand_over(post_depth, "SceneDepth");
        hand_over(rendergraph.get_resource("EditorSelectionMask"_h), "EditorSelectionMask");

        if (!ctx.caps.headless)
        {
            inputs.swapchain = ctx.swapchain.images[index];
            inputs.swapchain_view = ctx.swapchain.views[index];
        }
        inputs.to_swapchain = output_to_swapchain;
        inputs.output_hash = final_output.target_hash;
        inputs.output_extent = ctx.render_extent;
        frame::record(cmd, reg, inputs);

        gpu_timers::end_frame(cmd);
        vkEndCommandBuffer(cmd);

        tau::active_arena = nullptr;

        if (ctx.caps.headless) { submit_headless(cmd, frame_data); }
        else
        {
            submit_and_present(cmd, frame_data, index);
        }
        renderdoc::frame_end();

        ctx.cur_frame = (ctx.cur_frame + 1) % MAX_FRAMES_IN_FLIGHT;
        frame_slot_ready = false;
        image_acquired = false;

        if (deferred_resize)
        {
            resize();
            deferred_resize = false;
        }
    }

    void on_window_resized() { pending_resize = true; }

    VkFormat find_depth_format()
    {
        VkFormat formats[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT};
        for (VkFormat format : formats)
        {
            VkFormatProperties props;
            vkGetPhysicalDeviceFormatProperties(renderer::ctx.physical_device, format, &props);
            if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) { return format; }
        }

        return VK_FORMAT_UNDEFINED;
    }

    VkSurfaceFormatKHR select_surface_format(VkPhysicalDevice physical_device, VkSurfaceKHR surface,
                                             std::vector<VkFormat> const& preferred_formats)
    {
        u32_t surface_format_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &surface_format_count, nullptr);
        std::vector<VkSurfaceFormatKHR> supported_surface_formats(surface_format_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &surface_format_count,
                                             supported_surface_formats.data());

        auto it =
            std::ranges::find_if(supported_surface_formats,
                                 [&preferred_formats](VkSurfaceFormatKHR surface_format)
                                 {
                                     return std::ranges::any_of(preferred_formats, [&surface_format](VkFormat format)
                                                                { return format == surface_format.format; });
                                 });

        return it != supported_surface_formats.end() ? *it : supported_surface_formats[0];
    }

    void transition_image(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                          VkImageAspectFlags aspect_mask)
    {
        VkImageMemoryBarrier2 barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
            .oldLayout = old_layout,
            .newLayout = new_layout,
            .image = image,
            .subresourceRange = {.aspectMask = aspect_mask,
                                 .baseMipLevel = 0,
                                 .levelCount = VK_REMAINING_MIP_LEVELS,
                                 .baseArrayLayer = 0,
                                 .layerCount = VK_REMAINING_ARRAY_LAYERS},
        };

        if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
        {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.srcAccessMask = 0;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        }
        else if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
                 new_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
        {
            barrier.srcStageMask =
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            barrier.srcAccessMask = 0;
            barrier.dstStageMask =
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            barrier.dstAccessMask =
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        }
        else if (old_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
                 new_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
        {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
            barrier.dstAccessMask = 0;
        }
        else if (old_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
                 new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        }
        else if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
        {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.srcAccessMask = 0;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
            barrier.dstAccessMask = 0;
        }
        else if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                 new_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
        {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        }
        else if (old_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR &&
                 new_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
        {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
            barrier.srcAccessMask = 0;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        }
        else if (old_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
                 new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        }
        else if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                 new_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
        {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
            barrier.dstStageMask =
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            barrier.dstAccessMask =
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        }
        else if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.srcAccessMask = 0;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        }
        else if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                 new_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
        {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        }
        else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
                 new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.srcAccessMask = 0;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        }
        else
        {
            TAU_LOG_WARN("VULKAN", "transition_image: using fallback barrier for image transition. From: {}, To: {}",
                         (u32_t)old_layout, (u32_t)new_layout);
        }

        VkDependencyInfo dep_info = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &barrier,
        };

        vkCmdPipelineBarrier2(cmd, &dep_info);
    }

    bool resize()
    {
        if (ctx.surface == VK_NULL_HANDLE) { return false; }

        VkSurfaceCapabilitiesKHR surface_caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.physical_device, ctx.surface, &surface_caps);

        if (surface_caps.currentExtent.width == 0 && surface_caps.currentExtent.height == 0) { return false; }

        init_swapchain();

        return true;
    }

    void init_swapchain()
    {
        if (ctx.surface == VK_NULL_HANDLE) { return; }

        VkSurfaceCapabilitiesKHR surface_caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.physical_device, ctx.surface, &surface_caps);

        if (surface_caps.currentExtent.width == 0 || surface_caps.currentExtent.height == 0) { return; }

        VkExtent2D logical_extent;
        if (surface_caps.currentExtent.width != UINT32_MAX) { logical_extent = surface_caps.currentExtent; }
        else
        {
            i32 w, h;
            tau::window::get_window_size(&w, &h);

            logical_extent.width =
                std::clamp(static_cast<u32_t>(w), surface_caps.minImageExtent.width, surface_caps.maxImageExtent.width);
            logical_extent.height = std::clamp(static_cast<u32_t>(h), surface_caps.minImageExtent.height,
                                               surface_caps.maxImageExtent.height);
        }

        VkSurfaceTransformFlagBitsKHR pre_transform = surface_caps.currentTransform;
        VkExtent2D physical_extent = logical_extent;

        if (pre_transform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR))
        {
            physical_extent.width = logical_extent.height;
            physical_extent.height = logical_extent.width;
        }

        ctx.cur_surface_transform = pre_transform;
        ctx.swapchain.extent = physical_extent;

        ctx.render_extent = physical_extent;
        ctx.logical_extent = logical_extent;

        VkSurfaceFormatKHR surface_format = select_surface_format(ctx.physical_device, ctx.surface);
        const VkPresentModeKHR present_mode = select_present_mode(ctx.physical_device, ctx.surface, vsync);

        u32_t desired_swapchain_images = surface_caps.minImageCount + 1;
        if ((surface_caps.maxImageCount > 0) && (desired_swapchain_images > surface_caps.maxImageCount))
        {
            desired_swapchain_images = surface_caps.maxImageCount;
        }

        VkSwapchainKHR old_swapchain = ctx.swapchain.handle;

        VkCompositeAlphaFlagBitsKHR composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if (surface_caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
        {
            composite = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        }
        else if (surface_caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
        {
            composite = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
        }
        else if (surface_caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR)
        {
            composite = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
        }

        VkSwapchainCreateInfoKHR swapchain_info = {
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = ctx.surface,
            .minImageCount = desired_swapchain_images,
            .imageFormat = surface_format.format,
            .imageColorSpace = surface_format.colorSpace,
            .imageExtent = physical_extent,
            .imageArrayLayers = 1,
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .preTransform = pre_transform,
            .compositeAlpha = composite,
            .presentMode = present_mode,
            .clipped = true,
            .oldSwapchain = old_swapchain,
        };

        VK_CHECK(vkCreateSwapchainKHR(ctx.device, &swapchain_info, nullptr, &ctx.swapchain.handle));

        if (old_swapchain != VK_NULL_HANDLE)
        {
            std::scoped_lock lock(res_system.deletion_mutex);

            const u64_t safe_timeline = deletion_timeline_value();

            for (VkImageView view : ctx.swapchain.views)
            {
                res_system.deletion_queue.push_back({
                    .type = resource_type_e::IMAGE_VIEW,
                    .handle = {.image_view = view},
                    .bindless_id = BINDLESS_NULL_HANDLE,
                    .gpu_timeline_value = safe_timeline,
                });
            }

            ctx.swapchain.views.clear();

            res_system.deletion_queue.push_back({
                .type = resource_type_e::TEXTURE,
                .handle = {.texture = {ctx.swapchain.depth_image, ctx.swapchain.depth_allocation,
                                       ctx.swapchain.depth_view}},
                .bindless_id = BINDLESS_NULL_HANDLE,
                .gpu_timeline_value = safe_timeline,
            });

            res_system.deletion_queue.push_back({
                .type = resource_type_e::SWAPCHAIN,
                .handle = {.swapchain = old_swapchain},
                .bindless_id = BINDLESS_NULL_HANDLE,
                .gpu_timeline_value = safe_timeline,
            });
        }

        ctx.swapchain.extent = {physical_extent.width, physical_extent.height};
        ctx.swapchain.format = surface_format.format;

        ctx.swapchain.depth_format = find_depth_format();

        VkImageCreateInfo depth_image_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = ctx.swapchain.depth_format,
            .extent = {physical_extent.width, physical_extent.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        VmaAllocationCreateInfo depth_alloc_info = {
            .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
            .priority = 1.0f,
        };

        VK_CHECK(vmaCreateImage(ctx.allocator, &depth_image_info, &depth_alloc_info, &ctx.swapchain.depth_image,
                                &ctx.swapchain.depth_allocation, nullptr));

        VkImageViewCreateInfo depth_view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = ctx.swapchain.depth_image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = ctx.swapchain.depth_format,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                                 .baseMipLevel = 0,
                                 .levelCount = 1,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1}
        };

        if (ctx.swapchain.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
            ctx.swapchain.depth_format == VK_FORMAT_D24_UNORM_S8_UINT)
        {
            depth_view_info.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }

        VK_CHECK(vkCreateImageView(ctx.device, &depth_view_info, nullptr, &ctx.swapchain.depth_view));

        u32_t image_count;
        VK_CHECK(vkGetSwapchainImagesKHR(ctx.device, ctx.swapchain.handle, &image_count, nullptr));
        std::vector<VkImage> swapchain_images(image_count);
        VK_CHECK(vkGetSwapchainImagesKHR(ctx.device, ctx.swapchain.handle, &image_count, swapchain_images.data()));
        ctx.swapchain.images = swapchain_images;
        for (VkImage image : ctx.swapchain.images) { device::name(image, VK_OBJECT_TYPE_IMAGE, "Swapchain"); }
        device::name(ctx.swapchain.depth_image, VK_OBJECT_TYPE_IMAGE, "SwapchainDepth");

        if (!ctx.swapchain.release_semaphores.empty())
        {
            std::scoped_lock lock(res_system.deletion_mutex);

            const u64_t safe_timeline = deletion_timeline_value();

            for (VkSemaphore sem : ctx.swapchain.release_semaphores)
            {
                res_system.deletion_queue.push_back({
                    .type = resource_type_e::SEMAPHORE,
                    .handle = {.semaphore = sem},
                    .bindless_id = BINDLESS_NULL_HANDLE,
                    .gpu_timeline_value = safe_timeline,
                });
            }

            ctx.swapchain.release_semaphores.clear();
        }

        ctx.swapchain.release_semaphores.clear();
        ctx.swapchain.release_semaphores.resize(image_count);

        for (size_t i = 0; i < image_count; i++)
        {
            VkSemaphoreCreateInfo sem_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VK_CHECK(vkCreateSemaphore(ctx.device, &sem_info, nullptr, &ctx.swapchain.release_semaphores[i]));
        }

        for (size_t i = 0; i < image_count; i++)
        {
            VkImageViewCreateInfo view_info = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = swapchain_images[i],
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = ctx.swapchain.format,
                .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                     .baseMipLevel = 0,
                                     .levelCount = 1,
                                     .baseArrayLayer = 0,
                                     .layerCount = 1},
            };

            VkImageView image_view;
            VK_CHECK(vkCreateImageView(ctx.device, &view_info, nullptr, &image_view));

            ctx.swapchain.views.push_back(image_view);
        }
    }

    void begin_frame()
    {
        if (image_acquired || (ctx.swapchain.handle == VK_NULL_HANDLE && !ctx.caps.headless)) { return; }

        // suboptimal still gives an image, rebuild at present
        const VkResult res = acquire_next_image(&acquired_index);
        image_acquired = res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR;
    }

    void wait_for_frame()
    {
        if (frame_slot_ready) { return; }

        per_frame_t& frame_data = ctx.per_frame_objects[ctx.cur_frame];
        if (frame_data.target_timeline_value > 0)
        {
            VkSemaphoreWaitInfo wait_info = {
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                .semaphoreCount = 1,
                .pSemaphores = &ctx.timeline_semaphore,
                .pValues = &frame_data.target_timeline_value,
            };

            vkWaitSemaphores(ctx.device, &wait_info, UINT64_MAX);
        }
        readback::collect(ctx.cur_frame);
        gpu_timers::collect(ctx.cur_frame);
        frame_upload::reset(ctx.cur_frame);

        frame_slot_ready = true;
    }

    VkResult acquire_next_image(u32_t* image_index)
    {
        if (ctx.swapchain.handle == VK_NULL_HANDLE && !ctx.caps.headless) { return VK_ERROR_OUT_OF_DATE_KHR; }

        per_frame_t& frame_data = ctx.per_frame_objects[ctx.cur_frame];
        wait_for_frame();

        if (ctx.caps.headless) { *image_index = 0; }
        else
        {
            const VkResult res =
                vkAcquireNextImageKHR(ctx.device, ctx.swapchain.handle, UINT64_MAX,
                                      frame_data.swapchain_acquire_semaphore, VK_NULL_HANDLE, image_index);
            if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_SURFACE_LOST_KHR)
            {
                return res;
            }
        }

        vkResetCommandPool(ctx.device, frame_data.main_command_pool, 0);

        frame_data.transient_pool.reset();
        frame_data.frame_allocator.reset();

        return VK_SUCCESS;
    }

    void init_per_frame(per_frame_t& frame_data)
    {
        VkCommandPoolCreateInfo cmd_pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cmd_pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        cmd_pool_info.queueFamilyIndex = static_cast<u32_t>(ctx.queue_fam_indices.graphics_family.value());
        VK_CHECK(vkCreateCommandPool(ctx.device, &cmd_pool_info, nullptr, &frame_data.main_command_pool));

        VkCommandBufferAllocateInfo cmd_buf_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cmd_buf_info.commandPool = frame_data.main_command_pool;
        cmd_buf_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_buf_info.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(ctx.device, &cmd_buf_info, &frame_data.main_command_buffer));

        auto create_mapped_buffer = [](const char* name, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer,
                                       VmaAllocation& alloc, void*& mapped_mem)
        {
            VkBufferCreateInfo buf_info = {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = size,
                .usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            };
            VmaAllocationCreateInfo alloc_info = {
                .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                .usage = VMA_MEMORY_USAGE_AUTO,
                .requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            };

            VmaAllocationInfo vma_alloc_info;
            VK_CHECK(vmaCreateBuffer(ctx.allocator, &buf_info, &alloc_info, &buffer, &alloc, &vma_alloc_info));
            device::name(buffer, VK_OBJECT_TYPE_BUFFER, name);
            mapped_mem = vma_alloc_info.pMappedData;
        };

        create_mapped_buffer("ObjectData", sizeof(gpu::ObjectData) * ecs::MAX_ENTITIES,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, frame_data.object_buffer, frame_data.object_allocation,
                             (void*&)frame_data.mapped_object_data);
        frame_data.object_buffer_address = get_buffer_address(frame_data.object_buffer);

        create_mapped_buffer("CullData", sizeof(gpu::CullData) * ecs::MAX_ENTITIES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             frame_data.cull_buffer, frame_data.cull_allocation, (void*&)frame_data.mapped_cull_data);

        create_mapped_buffer("Materials", MAX_MATERIAL_BUFFER_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             frame_data.material_buffer, frame_data.material_allocation,
                             (void*&)frame_data.mapped_material_data);

        VkSemaphoreCreateInfo sem_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(ctx.device, &sem_info, nullptr, &frame_data.swapchain_acquire_semaphore));

        create_mapped_buffer("DirectionalLights", sizeof(gpu::DirectionalLight) * MAX_DIR_LIGHTS,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, frame_data.dir_light_buffer,
                             frame_data.dir_light_allocation, (void*&)frame_data.mapped_dir_lights);
        frame_data.dir_light_buffer_address = get_buffer_address(frame_data.dir_light_buffer);

        create_mapped_buffer("PointLights", sizeof(gpu::PointLight) * MAX_LIGHTS, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             frame_data.point_light_buffer, frame_data.point_light_allocation,
                             (void*&)frame_data.mapped_point_lights);
        frame_data.point_light_buffer_address = get_buffer_address(frame_data.point_light_buffer);

        create_mapped_buffer("SpotLights", sizeof(gpu::SpotLight) * MAX_LIGHTS, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             frame_data.spot_light_buffer, frame_data.spot_light_allocation,
                             (void*&)frame_data.mapped_spot_lights);
        frame_data.spot_light_buffer_address = get_buffer_address(frame_data.spot_light_buffer);

        create_mapped_buffer("ShadowCascades", sizeof(gpu::ShadowTile) * SHADOW_CASCADE_COUNT,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, frame_data.shadow_cascade_buffer,
                             frame_data.shadow_cascade_allocation, (void*&)frame_data.mapped_shadow_cascades);
        frame_data.shadow_cascade_buffer_address = get_buffer_address(frame_data.shadow_cascade_buffer);

        create_mapped_buffer("PunctualShadows", sizeof(gpu::ShadowTile) * MAX_PUNCTUAL_SHADOW_TILES,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, frame_data.punctual_shadow_buffer,
                             frame_data.punctual_shadow_allocation, (void*&)frame_data.mapped_punctual_shadows);
        frame_data.punctual_shadow_buffer_address = get_buffer_address(frame_data.punctual_shadow_buffer);

        {
            VkBufferCreateInfo buf_info = {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = sizeof(u32_t) * 2,
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            };
            VmaAllocationCreateInfo alloc_info = {
                .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                .usage = VMA_MEMORY_USAGE_AUTO,
                .requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            };

            VmaAllocationInfo vma_alloc_info;
            VK_CHECK(vmaCreateBuffer(ctx.allocator, &buf_info, &alloc_info, &frame_data.depth_bounds_buffer,
                                     &frame_data.depth_bounds_allocation, &vma_alloc_info));
            device::name(frame_data.depth_bounds_buffer, VK_OBJECT_TYPE_BUFFER, "DepthBounds");
            frame_data.mapped_depth_bounds = static_cast<u32_t*>(vma_alloc_info.pMappedData);
            frame_data.depth_bounds_pending = false;
        }

        frame_data.frame_allocator.init(2048 * 1024);
    }

    void shutdown_per_frame(per_frame_t& frame_data)
    {
        frame_data.transient_pool.shutdown();

        if (frame_data.main_command_buffer != VK_NULL_HANDLE)
        {
            vkFreeCommandBuffers(ctx.device, frame_data.main_command_pool, 1, &frame_data.main_command_buffer);
            frame_data.main_command_buffer = VK_NULL_HANDLE;
        }

        if (frame_data.main_command_pool != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(ctx.device, frame_data.main_command_pool, nullptr);
            frame_data.main_command_pool = VK_NULL_HANDLE;
        }

        if (frame_data.swapchain_acquire_semaphore != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(ctx.device, frame_data.swapchain_acquire_semaphore, nullptr);
            frame_data.swapchain_acquire_semaphore = VK_NULL_HANDLE;
        }

        if (frame_data.object_buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(ctx.allocator, frame_data.object_buffer, frame_data.object_allocation);
        }

        if (frame_data.cull_buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(ctx.allocator, frame_data.cull_buffer, frame_data.cull_allocation);
        }

        for (auto [name_hash, vgr_ptr] : frame_data.views)
        {
            view_gpu_resources_t& vgr = *vgr_ptr;
            if (vgr.indirect_buffer != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(ctx.allocator, vgr.indirect_buffer, vgr.indirect_alloc);
            }
            if (vgr.draw_counts_buffer != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(ctx.allocator, vgr.draw_counts_buffer, vgr.draw_counts_alloc);
            }
            if (vgr.cluster_counts_buffer != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(ctx.allocator, vgr.cluster_counts_buffer, vgr.cluster_counts_alloc);
            }
            if (vgr.cluster_indices_buffer != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(ctx.allocator, vgr.cluster_indices_buffer, vgr.cluster_indices_alloc);
            }
        }
        frame_data.views.clear();

        if (frame_data.material_buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(ctx.allocator, frame_data.material_buffer, frame_data.material_allocation);
        }

        if (frame_data.dir_light_buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(ctx.allocator, frame_data.dir_light_buffer, frame_data.dir_light_allocation);
        }
        if (frame_data.point_light_buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(ctx.allocator, frame_data.point_light_buffer, frame_data.point_light_allocation);
        }
        if (frame_data.spot_light_buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(ctx.allocator, frame_data.spot_light_buffer, frame_data.spot_light_allocation);
        }
        if (frame_data.shadow_cascade_buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(ctx.allocator, frame_data.shadow_cascade_buffer, frame_data.shadow_cascade_allocation);
        }
        if (frame_data.punctual_shadow_buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(ctx.allocator, frame_data.punctual_shadow_buffer, frame_data.punctual_shadow_allocation);
        }
        if (frame_data.depth_bounds_buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(ctx.allocator, frame_data.depth_bounds_buffer, frame_data.depth_bounds_allocation);
            frame_data.depth_bounds_buffer = VK_NULL_HANDLE;
            frame_data.mapped_depth_bounds = nullptr;
        }
    }

    void create_buffer(VkDeviceSize size, VkBufferUsageFlags buffer_usage, VmaMemoryUsage mem_usage, VkBuffer& buffer,
                       VmaAllocation& allocation)
    {
        VkBufferCreateInfo buffer_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = buffer_usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, // should be a parameter
        };

        VmaAllocationCreateInfo alloc_info = {.usage = mem_usage};

        VK_CHECK(vmaCreateBuffer(ctx.allocator, &buffer_info, &alloc_info, &buffer, &allocation, nullptr));
    }

    VkDeviceAddress get_buffer_address(VkBuffer buffer)
    {
        VkBufferDeviceAddressInfo info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .buffer = buffer,
        };
        return vkGetBufferDeviceAddress(ctx.device, &info);
    }

    void create_image(u32 width, u32 height, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage,
                      VmaMemoryUsage mem_usage, VkImage& image, VmaAllocation& allocation)
    {
        VkImageCreateInfo image_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = tiling,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        VmaAllocationCreateInfo alloc_info = {.usage = mem_usage};

        if (vmaCreateImage(ctx.allocator, &image_info, &alloc_info, &image, &allocation, nullptr) != VK_SUCCESS)
        {
            TAU_LOG_ERROR("VULKAN", "Failed to create image; Width: {}, Height: {}", width, height);
        }
    }

    VkCommandBuffer begin_transfer_commands()
    {
        VkCommandBufferAllocateInfo alloc_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = ctx.transfer_command_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };

        VkCommandBuffer cmd;
        {
            std::scoped_lock lock(ctx.transfer_mutex);
            VK_CHECK(vkAllocateCommandBuffers(ctx.device, &alloc_info, &cmd));
        }

        VkCommandBufferBeginInfo begin_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };

        VK_CHECK(vkBeginCommandBuffer(cmd, &begin_info));

        return cmd;
    }

    u64_t submit_transfer_commands(VkCommandBuffer cmd)
    {
        VK_CHECK(vkEndCommandBuffer(cmd));

        u64_t signal_value;
        {
            // if i add multithread asset loading, this should be locked
            signal_value = ++res_system.timeline_value;
        }

        VkTimelineSemaphoreSubmitInfo timeline_sem_info = {
            .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
            .signalSemaphoreValueCount = 1,
            .pSignalSemaphoreValues = &signal_value,
        };

        VkSubmitInfo submit_info = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = &timeline_sem_info,
            .commandBufferCount = 1,
            .pCommandBuffers = &cmd,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &res_system.timeline_semaphore,
        };

        {
            std::scoped_lock lock(ctx.transfer_mutex);
            VK_CHECK(vkQueueSubmit(ctx.transfer_queue, 1, &submit_info, VK_NULL_HANDLE));
        }

        {
            std::scoped_lock lock(res_system.deletion_mutex);
            res_system.deletion_queue.push_back({
                .type = resource_type_e::COMMAND_BUFFER,
                .handle = {.cmd_buffer = {cmd, ctx.transfer_command_pool}},
                .bindless_id = BINDLESS_NULL_HANDLE,
                .gpu_timeline_value = signal_value,
                .clock = deletion_clock_e::TRANSFER,
            });
        }

        return signal_value;
    }

    VkSampler create_sampler(VkFilter filter, VkSamplerAddressMode address_mode)
    {
        VkSamplerCreateInfo sampler_info = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .flags = 0,
            .magFilter = filter,
            .minFilter = filter,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = address_mode,
            .addressModeV = address_mode,
            .addressModeW = address_mode,
            .mipLodBias = 0.0f,
            .anisotropyEnable = VK_TRUE,
            .maxAnisotropy = ctx.properties.limits.maxSamplerAnisotropy,
            .compareEnable = VK_FALSE,
            .compareOp = VK_COMPARE_OP_ALWAYS,
            .minLod = 0.0f,
            .maxLod = VK_LOD_CLAMP_NONE,
            .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
            .unnormalizedCoordinates = VK_FALSE,
        };

        VkSampler sampler = VK_NULL_HANDLE;
        VK_CHECK(vkCreateSampler(ctx.device, &sampler_info, nullptr, &sampler));

        return sampler;
    }

    VkSampler create_shadow_sampler()
    {
        VkSamplerCreateInfo sampler_info = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .flags = 0,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            .mipLodBias = 0.0f,
            .anisotropyEnable = VK_FALSE,
            .maxAnisotropy = 1.0f,
            .compareEnable = VK_TRUE,
            .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
            .minLod = 0.0f,
            .maxLod = 0.0f,
            // white border == farthest depth, so anything sampled outside the map reads as lit
            .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
            .unnormalizedCoordinates = VK_FALSE,
        };

        VkSampler sampler = VK_NULL_HANDLE;
        VK_CHECK(vkCreateSampler(ctx.device, &sampler_info, nullptr, &sampler));

        return sampler;
    }

    void submit_output_target(u32_t target_hash, VkExtent2D extent)
    {
        if (extent.width == 0 || extent.height == 0) { extent = ctx.swapchain.extent; }

        ctx.render_extent = extent;
        ctx.logical_extent = extent;

        submitted_outputs.push_back({target_hash, extent});
    }

    u32_t get_target_texture_id(u32_t name_hash)
    {
        const u32_t* id = ctx.output_texture_ids.find(name_hash);
        return id ? *id : BINDLESS_NULL_HANDLE;
    }

    void forget_missing_asset_reports() { reported_missing.clear(); }

    u64_t deletion_timeline_value() { return ctx.timeline_value + 1; }

    bool get_primary_depth_bounds(f32& out_near_z, f32& out_far_z)
    {
        if (!ctx.primary_depth_bounds.valid) { return false; }

        out_near_z = ctx.primary_depth_bounds.near_z;
        out_far_z = ctx.primary_depth_bounds.far_z;
        return true;
    }

    void set_view_debug_view(u32_t view_name_hash, debug_view_e debug_view)
    { view_debug_views[view_name_hash] = debug_view; }

    void set_view_post_processing(u32_t view_name_hash, bool enabled)
    { view_post_processing[view_name_hash] = enabled; }

    namespace
    {
        mat4 combine_view_proj(const mat4& view, const mat4& projection) { return projection * view; }

        // every depth only view is a tile in an atlas, differing in the atlas and what the caller remembers
        render_view_t make_shadow_view(u32_t name_hash, u32_t atlas_target_hash, const mat4& view,
                                       const mat4& projection, const image_desc_t& atlas_desc, VkRect2D atlas_rect,
                                       f32 normal_bias_texels, f32 depth_bias_ndc)
        {
            render_view_t v{};
            v.name_hash = name_hash;
            v.kind = view_kind_e::DEPTH_ONLY;
            v.view = view;
            v.projection = projection;
            v.view_proj = combine_view_proj(view, projection);
            v.depth_target_hash = atlas_target_hash;
            v.has_depth_desc = true;
            v.depth_desc = atlas_desc;
            v.extent = atlas_rect.extent;
            v.shadow_atlas_rect = atlas_rect;
            v.shadow_normal_bias = normal_bias_texels;
            v.shadow_depth_bias = depth_bias_ndc;
            return v;
        }
    } // namespace

    void submit_color_view(u32_t name_hash, const mat4& view, const mat4& projection, const mat4& view_proj,
                           const vec3& position, u32_t color_target_hash, u32_t depth_target_hash, VkExtent2D extent,
                           f32 z_near, f32 z_far)
    {
        render_view_t v{};
        v.name_hash = name_hash;
        v.kind = view_kind_e::COLOR;
        v.view = view;
        v.projection = projection;
        v.view_proj = view_proj;
        v.position = position;
        v.color_target_hash = color_target_hash;
        v.depth_target_hash = depth_target_hash;
        v.extent = extent;
        v.z_near = (z_near > 0.0f) ? z_near : projection[3][2];
        v.z_far = z_far;
        submitted_views.push_back(v);
    }

    void submit_shadow_cascade_view(u32_t name_hash, const mat4& view, const mat4& projection, u32_t atlas_target_hash,
                                    const image_desc_t& atlas_desc, VkRect2D atlas_rect, f32 split_far,
                                    f32 normal_bias_texels, f32 depth_bias_ndc)
    {
        render_view_t v = make_shadow_view(name_hash, atlas_target_hash, view, projection, atlas_desc, atlas_rect,
                                           normal_bias_texels, depth_bias_ndc);
        v.shadow_split_far = split_far;
        submitted_views.push_back(v);
    }

    void submit_punctual_shadow_view(u32_t name_hash, const mat4& view, const mat4& projection,
                                     const image_desc_t& atlas_desc, VkRect2D atlas_rect, u32_t shadow_slot, f32 range,
                                     bool needs_render, f32 normal_bias_texels, f32 depth_bias_ndc)
    {
        render_view_t v = make_shadow_view(name_hash, "PunctualShadowAtlas"_h, view, projection, atlas_desc, atlas_rect,
                                           normal_bias_texels, depth_bias_ndc);
        v.shadow_tile = shadow_tile_e::PUNCTUAL;
        v.shadow_slot = shadow_slot;
        v.needs_render = needs_render;
        v.z_far = range;
        submitted_views.push_back(v);
    }

    void submit_shadow_cascade_light(ecs::entity_t light) { submitted_cascade_light = light; }

    void submit_punctual_shadow_slot(ecs::entity_t light, u32_t shadow_slot)
    { submitted_punctual_slots[static_cast<u32_t>(light)] = shadow_slot; }

    bool get_primary_camera_position(ecs::registry_t& reg, vec3& out_pos)
    {
        for (const render_view_t& v : submitted_views)
        {
            if (v.name_hash == "PrimaryView"_h)
            {
                out_pos = v.position;
                return true;
            }
        }

        ecs::entity_t cam_e = camera_system::get_active_camera(reg);
        if (cam_e != ecs::NULL_ENTITY)
        {
            transform_t& t = reg.get<transform_t>(cam_e);
            out_pos = {t.world_mat[3][0], t.world_mat[3][1], t.world_mat[3][2]};
            return true;
        }

        return false;
    }

    static void fill_view_info(const render_view_t& v, primary_view_info_t& out_info)
    {
        out_info.position = v.position;
        out_info.forward = normalize(vec3{v.view[0][2], v.view[1][2], v.view[2][2]});

        const f32 focal = std::fabs(v.projection[1][1]);
        const f32 focal_over_aspect = std::fabs(v.projection[0][0]);
        out_info.fov_rad = (focal > 1e-6f) ? (2.0f * std::atan(1.0f / focal)) : radians(45.0f);
        out_info.aspect = (focal_over_aspect > 1e-6f) ? (focal / focal_over_aspect) : 1.0f;

        out_info.z_near = (v.z_near > 0.0f) ? v.z_near : 0.1f;
        out_info.z_far = v.z_far;

        vec4 planes[6];
        frustum_planes(v.view_proj, planes);
        for (u32_t i = 0; i < 6; i++) { out_info.frustum_planes[i] = planes[i]; }
        out_info.has_frustum_planes = true;
    }

    bool get_primary_view_info(ecs::registry_t& reg, primary_view_info_t& out_info)
    {
        for (const render_view_t& v : submitted_views)
        {
            if (v.name_hash == "PrimaryView"_h)
            {
                fill_view_info(v, out_info);
                return true;
            }
        }

        ecs::entity_t cam_e = camera_system::get_active_camera(reg);
        if (cam_e == ecs::NULL_ENTITY || !reg.all_of<camera_t, transform_t>(cam_e)) { return false; }

        const camera_t& cam = reg.get<camera_t>(cam_e);
        const transform_t& t = reg.get<transform_t>(cam_e);

        out_info.position = {t.world_mat[3][0], t.world_mat[3][1], t.world_mat[3][2]};
        out_info.forward = normalize({t.world_mat[2][0], t.world_mat[2][1], t.world_mat[2][2]});
        out_info.fov_rad = radians(cam.fov_deg);
        out_info.aspect = cam.aspect;
        out_info.z_near = cam.near_plane;
        out_info.z_far = cam.far_plane;
        return true;
    }
} // namespace tau::renderer