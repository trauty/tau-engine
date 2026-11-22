#include "legacy.h"

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
#include "tau/ecs.h"
#include "tau/hash.h"
#include "tau/log.h"
#include "tau/os.h"
#include "tau/profiling.h"
#include "tau/rhi/bindless.h"
#include "tau/rhi/context.h"
#include "tau/rhi/deletion.h"
#include "tau/rhi/device.h"
#include "tau/rhi/material_heap.h"
#include "tau/rhi/renderer_constants.h"
#include "tau/rhi/samplers.h"
#include "tau/rhi/shader_instance.h"
#include "tau/rhi/vulkan.h"
#include "tau/systems/camera.h"
#include "tau/time.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_set>
#include <vector>

namespace tau::renderer::legacy
{
    using frame::render_view_t;
    using frame::shadow_tile_e;
    using frame::view_kind_e;

    namespace
    {
        struct active_pipeline_t
        {
            VkPipeline pipeline;
            VkPipeline shadow_pipeline;
            VkPipeline gbuffer_pipeline;
            VkPipelineLayout layout;
            u32_t pipeline_index;
            mesh_pass_e pass;
            u32_t first_object = 0;
            u32_t object_count = 0;
        };

        struct pipeline_range_t
        {
            u32_t begin = 0;
            u32_t end = 0;
        };

        struct view_gpu_resources_t
        {
            VkBuffer indirect_buffer = VK_NULL_HANDLE;
            VmaAllocation indirect_alloc = VK_NULL_HANDLE;
            VkDeviceSize indirect_size = 0;

            VkBuffer draw_counts_buffer = VK_NULL_HANDLE;
            VmaAllocation draw_counts_alloc = VK_NULL_HANDLE;
            VkDeviceSize draw_counts_size = 0;

            u32_t cluster_count = 0;

            VkBuffer cluster_counts_buffer = VK_NULL_HANDLE;
            VmaAllocation cluster_counts_alloc = VK_NULL_HANDLE;
            VkDeviceAddress cluster_counts_address = 0;

            VkBuffer cluster_indices_buffer = VK_NULL_HANDLE;
            VmaAllocation cluster_indices_alloc = VK_NULL_HANDLE;
            VkDeviceAddress cluster_indices_address = 0;

            shader_instance_t culling_instance;
            shader_instance_t cluster_instance;

            u32_t global_data_offset = 0;
            view_kind_e kind = view_kind_e::COLOR;
        };

        struct legacy_frame_t
        {
            // index into ctx.frames
            u32_t slot = 0;

            VkBuffer object_buffer = VK_NULL_HANDLE;
            VmaAllocation object_allocation = VK_NULL_HANDLE;
            gpu::ObjectData* mapped_object_data = nullptr;
            VkDeviceAddress object_buffer_address = 0;

            VkBuffer cull_buffer = VK_NULL_HANDLE;
            VmaAllocation cull_allocation = VK_NULL_HANDLE;
            gpu::CullData* mapped_cull_data = nullptr;

            u32_t object_counter = 0;

            std::vector<active_pipeline_t> active_pipelines;
            pipeline_range_t pass_ranges[MESH_PASS_COUNT];
            flat_map_t<std::unique_ptr<view_gpu_resources_t>> views;

            VkBuffer material_buffer = VK_NULL_HANDLE;
            VmaAllocation material_allocation = VK_NULL_HANDLE;
            u8* mapped_material_data = nullptr;

            transient_pool_t transient_pool;

            VkBuffer dir_light_buffer = VK_NULL_HANDLE;
            VmaAllocation dir_light_allocation = VK_NULL_HANDLE;
            gpu::DirectionalLight* mapped_dir_lights = nullptr;
            VkDeviceAddress dir_light_buffer_address = 0;

            VkBuffer point_light_buffer = VK_NULL_HANDLE;
            VmaAllocation point_light_allocation = VK_NULL_HANDLE;
            gpu::PointLight* mapped_point_lights = nullptr;
            VkDeviceAddress point_light_buffer_address = 0;

            VkBuffer spot_light_buffer = VK_NULL_HANDLE;
            VmaAllocation spot_light_allocation = VK_NULL_HANDLE;
            gpu::SpotLight* mapped_spot_lights = nullptr;
            VkDeviceAddress spot_light_buffer_address = 0;

            VkBuffer shadow_cascade_buffer = VK_NULL_HANDLE;
            VmaAllocation shadow_cascade_allocation = VK_NULL_HANDLE;
            gpu::ShadowTile* mapped_shadow_cascades = nullptr;
            VkDeviceAddress shadow_cascade_buffer_address = 0;

            VkBuffer punctual_shadow_buffer = VK_NULL_HANDLE;
            VmaAllocation punctual_shadow_allocation = VK_NULL_HANDLE;
            gpu::ShadowTile* mapped_punctual_shadows = nullptr;
            VkDeviceAddress punctual_shadow_buffer_address = 0;

            VkBuffer depth_bounds_buffer = VK_NULL_HANDLE;
            VmaAllocation depth_bounds_allocation = VK_NULL_HANDLE;
            u32_t* mapped_depth_bounds = nullptr;
            shader_instance_t depth_reduce_instance;

            f32 depth_bounds_z_near = 0.0f;
            bool depth_bounds_pending = false;
        };

        struct view_depth_bounds_t
        {
            f32 near_z = 0.0f;
            f32 far_z = 0.0f;
            bool valid = false;
        };

        struct state_t
        {
            asset_t<shader_t> culling_shader;
            asset_t<shader_t> cluster_shader;
            asset_t<shader_t> depth_reduce_shader;
            asset_t<material_t> deferred_lighting_material;
            persistent_image_t punctual_shadow_atlas;
            view_depth_bounds_t primary_depth_bounds;
        };

        state_t g_state;
        legacy_frame_t g_frames[MAX_FRAMES_IN_FLIGHT];

        rendergraph_t rendergraph;
        struct renderer_feature_t
        {
            u32_t name_hash;
            i32 order;
            graph_builder_func_t builder;
        };

        std::vector<renderer_feature_t> custom_renderer_features;
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

        VkDescriptorSet sets[] = {bindless::global_set(), bindless::frame_set()};
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
            legacy_frame_t& fd = g_frames[ctx.cur_frame];
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
                material_heap::address(),
            };
            VkDescriptorSet sets[] = {bindless::global_set(), bindless::frame_set()};

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

    void draw_view_objects(VkCommandBuffer cmd, u32_t view_name_hash, VkPipeline pipeline, VkPipelineLayout layout,
                           gpu::PushConstants pc)
    {
        legacy_frame_t& fd = g_frames[ctx.cur_frame];
        if (fd.object_counter == 0 || pipeline == VK_NULL_HANDLE) { return; }

        const std::unique_ptr<view_gpu_resources_t>* found = fd.views.find(view_name_hash);
        if (found == nullptr) { return; }
        view_gpu_resources_t& vgr = **found;
        if (vgr.indirect_buffer == VK_NULL_HANDLE || vgr.draw_counts_buffer == VK_NULL_HANDLE) { return; }

        pc.object_buffer = fd.object_buffer_address;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        frame::bind_view_descriptors(cmd, layout, view_name_hash);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(gpu::PushConstants), &pc);

        for (const active_pipeline_t& p : fd.active_pipelines)
        {
            vkCmdDrawIndirectCount(cmd, vgr.indirect_buffer, p.first_object * 16, vgr.draw_counts_buffer,
                                   p.pipeline_index * sizeof(u32_t), p.object_count, 16);
        }
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

            legacy_frame_t& frame_data = g_frames[ctx.cur_frame];

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

            VkDescriptorSet sets[] = {bindless::global_set(), bindless::frame_set()};

            const u32_t view_offset = frame::view_globals_offset(view_name_hash);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shader->pipeline_layout, 0, 2, sets, 1,
                                    &view_offset);

            gpu::PushConstants pc_data = {
                frame_data.object_buffer_address,
                material_heap::address(),
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

            legacy_frame_t& frame_data = g_frames[ctx.cur_frame];

            const gpu::PushConstants pc_data = {
                frame_data.object_buffer_address,
                material_heap::address(),
                material->heap_offset[ctx.cur_frame],
            };

            dispatch_compute(cmd, *shader, pipeline, frame::view_globals_offset(view_name_hash), nullptr, pc_data,
                             dispatch_x, dispatch_y, dispatch_z);
        };

        return pass;
    }

    // an in-flight frame may still sample it
    static void destroy_persistent_image(persistent_image_t& target)
    {
        if (target.image != VK_NULL_HANDLE)
        {
            deletion::retire(deletion::image(target.image, target.allocation, target.view));
        }
        bindless::remove(target.bindless);
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
            target.bindless = bindless::add_sampled(target.view);
            target.bindless_id = target.bindless.index;
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

        legacy_frame_t* newest = nullptr;
        for (legacy_frame_t& candidate : g_frames)
        {
            if (!candidate.depth_bounds_pending || candidate.mapped_depth_bounds == nullptr) { continue; }
            if (ctx.frames[candidate.slot].target_timeline_value == 0 ||
                ctx.frames[candidate.slot].target_timeline_value > completed_timeline)
            {
                continue;
            }
            if (newest == nullptr ||
                ctx.frames[candidate.slot].target_timeline_value > ctx.frames[newest->slot].target_timeline_value)
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
            g_state.primary_depth_bounds.near_z = pass_z_near / max_depth;
            g_state.primary_depth_bounds.far_z = pass_z_near / min_depth;
            g_state.primary_depth_bounds.valid = true;
        }
        else
        {
            g_state.primary_depth_bounds.valid = false;
        }
    }

    static bool add_depth_reduce_pass(rendergraph_t& graph, legacy_frame_t& frame_data, rg_resource_id depth,
                                      u32_t view_name_hash, VkExtent2D extent)
    {
        if (depth == RG_NULL_ID || extent.width == 0 || extent.height == 0) { return false; }
        if (frame_data.depth_bounds_buffer == VK_NULL_HANDLE || frame_data.mapped_depth_bounds == nullptr)
        {
            return false;
        }

        shader_t* shader = assets::get(g_state.depth_reduce_shader);
        if (shader == nullptr || shader->get_pipeline(pipeline_variant_e::FORWARD) == VK_NULL_HANDLE) { return false; }

        if (!frame_data.depth_reduce_instance.is_initialized)
        {
            frame_data.depth_reduce_instance.init(g_state.depth_reduce_shader);
        }
        frame_data.depth_reduce_instance.set_buffer("depth_bounds"_h, frame_data.depth_bounds_buffer);
        frame_data.depth_reduce_instance.sync();

        rg_pass_t& pass = graph.add_pass("DepthReduce"_h, "DepthReduce");
        pass.texture_reads = {depth};

        pass.execute_callback = [&graph, depth, view_name_hash, extent](VkCommandBuffer cmd, ecs::registry_t&)
        {
            legacy_frame_t& fd = g_frames[ctx.cur_frame];

            shader_t* reduce_shader = assets::get(g_state.depth_reduce_shader);
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
                material_heap::address(),
                0,
                depth_tex_id,
            };

            dispatch_compute(cmd, *reduce_shader, pipeline, frame::view_globals_offset(view_name_hash),
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
    static gpu::GlobalData upload_lights(ecs::registry_t& reg, legacy_frame_t& frame_data,
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
    static u32_t build_draw_list(ecs::registry_t& reg, legacy_frame_t& frame_data)
    {
        draw_items.clear();

        for (auto [entity, transform, renderer] : reg.view<transform_t, mesh_renderer_t>().each())
        {
            if (!renderer.active || !renderer.mesh) { continue; }

            mesh_t* mesh = assets::get(renderer.mesh);
            if (mesh == nullptr)
            {
                report_missing("mesh", renderer.mesh);

                mesh = assets::get(frame::fallbacks().mesh);
                if (mesh == nullptr) { continue; }
            }

            const u32_t submesh_count = std::max<u32_t>(1, static_cast<u32_t>(mesh->submeshes.size()));
            for (u32_t submesh = 0; submesh < submesh_count; submesh++)
            {
                asset_t<material_t> handle = slot_material(renderer, mesh, submesh);
                if (!handle.is_valid()) { handle = frame::fallbacks().default_material; }

                material_t* mat = assets::get(handle);
                shader_t* shader = mat ? assets::get(mat->shader) : nullptr;

                // a material that will not load (usually its shader stopped compiling) draws as the fallback
                if (mat == nullptr || shader == nullptr)
                {
                    report_missing("material", handle);

                    mat = assets::get(frame::fallbacks().material);
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

    void init(void* tracy_vk_ctx)
    {
        rendergraph.profiling_vk_ctx = tracy_vk_ctx;

        for (u32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            legacy_frame_t& frame_data = g_frames[i];
            frame_data.slot = i;

            auto create_mapped_buffer = [](const char* name, VkDeviceSize size, VkBufferUsageFlags usage,
                                           VkBuffer& buffer, VmaAllocation& alloc, void*& mapped_mem)
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
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, frame_data.object_buffer,
                                 frame_data.object_allocation, (void*&)frame_data.mapped_object_data);
            frame_data.object_buffer_address = get_buffer_address(frame_data.object_buffer);

            create_mapped_buffer("CullData", sizeof(gpu::CullData) * ecs::MAX_ENTITIES,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, frame_data.cull_buffer, frame_data.cull_allocation,
                                 (void*&)frame_data.mapped_cull_data);

            create_mapped_buffer("Materials", MAX_MATERIAL_BUFFER_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 frame_data.material_buffer, frame_data.material_allocation,
                                 (void*&)frame_data.mapped_material_data);

            create_mapped_buffer("DirectionalLights", sizeof(gpu::DirectionalLight) * MAX_DIR_LIGHTS,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, frame_data.dir_light_buffer,
                                 frame_data.dir_light_allocation, (void*&)frame_data.mapped_dir_lights);
            frame_data.dir_light_buffer_address = get_buffer_address(frame_data.dir_light_buffer);

            create_mapped_buffer("PointLights", sizeof(gpu::PointLight) * MAX_LIGHTS,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, frame_data.point_light_buffer,
                                 frame_data.point_light_allocation, (void*&)frame_data.mapped_point_lights);
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
        }

        g_state.culling_shader = assets::pin(assets::load<shader_t>("engine://assets/shaders/culling.slang"));

        g_state.cluster_shader = assets::pin(assets::load<shader_t>("engine://assets/shaders/cluster_build.slang"));

        g_state.depth_reduce_shader = assets::pin(assets::load<shader_t>("engine://assets/shaders/depth_reduce.slang"));
        g_state.deferred_lighting_material =
            assets::pin(assets::load<material_t>("engine://assets/materials/deferred_lighting.mat"));
    }

    void reset_assets()
    {
        // every view has its own culling shader instance
        for (legacy_frame_t& frame_data : g_frames)
        {
            // the flat map yields a pair holding a reference, so bind it by value
            for (auto [name_hash, vgr_ptr] : frame_data.views)
            {
                vgr_ptr->culling_instance.shutdown();
                vgr_ptr->cluster_instance.shutdown();
            }

            frame_data.depth_reduce_instance.shutdown();
        }

        assets::unpin(g_state.culling_shader);
        assets::unpin(g_state.cluster_shader);
        assets::unpin(g_state.depth_reduce_shader);
        assets::unpin(g_state.deferred_lighting_material);
        rendergraph.clear();
        custom_renderer_features.clear();
    }

    void shutdown()
    {
        destroy_persistent_image(g_state.punctual_shadow_atlas);

        for (legacy_frame_t& frame_data : g_frames)
        {
            frame_data.transient_pool.shutdown();
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
                vmaDestroyBuffer(ctx.allocator, frame_data.punctual_shadow_buffer,
                                 frame_data.punctual_shadow_allocation);
            }
            if (frame_data.depth_bounds_buffer != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(ctx.allocator, frame_data.depth_bounds_buffer, frame_data.depth_bounds_allocation);
                frame_data.depth_bounds_buffer = VK_NULL_HANDLE;
                frame_data.mapped_depth_bounds = nullptr;
            }
        }
    }

    void record(VkCommandBuffer cmd, ecs::registry_t& reg, std::vector<render_view_t>& frame_views,
                const flat_map_t<u32_t>& frame_punctual_slots, ecs::entity_t frame_cascade_light,
                frame::inputs_t& inputs)
    {
        legacy_frame_t& frame_data = g_frames[ctx.cur_frame];
        frame_data.transient_pool.reset();

        read_depth_bounds();

        gpu::GlobalData frame_globals = upload_lights(reg, frame_data, frame_punctual_slots, frame_cascade_light);

        const u32_t cur_object_id = build_draw_list(reg, frame_data);
        frame_globals.object_count = cur_object_id;
        frame_globals.active_pipeline_count = static_cast<u32_t>(frame_data.active_pipelines.size());

        shader_t* culling_shader = assets::get(g_state.culling_shader);
        shader_t* cluster_shader = assets::get(g_state.cluster_shader);

        for (u32_t view_idx = 0; view_idx < frame_views.size(); view_idx++)
        {
            render_view_t& sub = frame_views[view_idx];
            u32_t view_offset = frame::view_globals_offset_at(view_idx);

            std::unique_ptr<view_gpu_resources_t>& vgr_slot = frame_data.views[sub.name_hash];
            if (vgr_slot == nullptr)
            {
                vgr_slot = std::make_unique<view_gpu_resources_t>();
                vgr_slot->culling_instance.init(g_state.culling_shader);
                vgr_slot->cluster_instance.init(g_state.cluster_shader);
            }
            view_gpu_resources_t& vgr = *vgr_slot;
            vgr.kind = sub.kind;
            vgr.global_data_offset = view_offset;

            gpu::GlobalData* slot = frame::view_globals(view_idx);
            *slot = frame_globals;
            frame::fill_view_camera(*slot, sub);

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
                        material_heap::address(),
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
                material_heap::address(),
                0,
            };

            dispatch_compute(cmd, *culling_shader, culling_shader->get_pipeline(pipeline_variant_e::FORWARD),
                             view_offset, &vgr.culling_instance, pc_data, (cur_object_id + 63) / 64);

            buffer_barrier(cmd, {vgr.indirect_buffer, vgr.draw_counts_buffer}, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                           VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
        }

        rendergraph.clear();

        image_desc_t color_desc = {
            .width = frame::render_extent().width,
            .height = frame::render_extent().height,
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
            .width = frame::render_extent().width,
            .height = frame::render_extent().height,
            .format = ctx.depth_format,
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
                    ensure_persistent_image(g_state.punctual_shadow_atlas, sub.depth_desc, "PunctualShadowAtlas");

                    punctual_id = rendergraph.import_persistent_image(
                        sub.depth_target_hash, "PunctualShadowAtlas", g_state.punctual_shadow_atlas.image,
                        g_state.punctual_shadow_atlas.view, g_state.punctual_shadow_atlas.desc,
                        g_state.punctual_shadow_atlas.bindless_id, g_state.punctual_shadow_atlas.layout);

                    if (g_state.punctual_shadow_atlas.needs_full_clear)
                    {
                        rg_pass_t& clear_pass = rendergraph.add_pass("PunctualAtlasClear"_h, "PunctualAtlasClear");
                        clear_pass.depth_stencil = punctual_id;
                        clear_pass.force_depth_clear = true;
                        g_state.punctual_shadow_atlas.needs_full_clear = false;
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

            material_t* lighting_mat = assets::get(g_state.deferred_lighting_material);
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
                add_depth_reduce_pass(rendergraph, frame_data, depth_id, sub.name_hash, frame::render_extent()))
            {
                frame_data.depth_bounds_z_near = sub.z_near;
                frame_data.depth_bounds_pending = true;
            }
        }

        // the legacy block's features (picking ids, selection mask); everything else is a frame producer
        for (const renderer_feature_t& feature : custom_renderer_features) { feature.builder(rendergraph, reg); }

        rendergraph.compile(frame_data.transient_pool);

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

                gpu::GlobalData* slot = frame::view_globals(view_idx);

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
            g_state.punctual_shadow_atlas.layout = rendergraph.get_layout(punctual_atlas_res);
        }

        auto hand_over = [&](rg_resource_id id, const char* name)
        {
            const rg_resource_t* r = rendergraph.find(id);
            if (r == nullptr || inputs.legacy_count >= frame::inputs_t::MAX_LEGACY) { return; }
            inputs.legacy[inputs.legacy_count++] = {
                name,          r->image,      r->view, r->desc.format, {r->desc.width, r->desc.height},
                r->cur_layout, r->bindless_id
            };
        };

        rg_resource_id post_depth = RG_NULL_ID;
        for (const render_view_t& v : frame_views)
        {
            if (v.kind == view_kind_e::COLOR && v.name_hash == "PrimaryView"_h)
            {
                post_depth = rendergraph.get_resource(v.depth_target_hash);
                break;
            }
        }
        // no colour view (an empty scene has no camera): the primary depth
        if (post_depth == RG_NULL_ID) { post_depth = rendergraph.get_resource("SceneDepth"_h); }

        hand_over(rendergraph.get_resource("SceneColor"_h), "SceneColor");
        hand_over(post_depth, "SceneDepth");
        hand_over(rendergraph.get_resource("EditorSelectionMask"_h), "EditorSelectionMask");
    }

    void forget_missing_asset_reports() { reported_missing.clear(); }

    bool get_primary_depth_bounds(f32& out_near_z, f32& out_far_z)
    {
        if (!g_state.primary_depth_bounds.valid) { return false; }

        out_near_z = g_state.primary_depth_bounds.near_z;
        out_far_z = g_state.primary_depth_bounds.far_z;
        return true;
    }
} // namespace tau::renderer::legacy
