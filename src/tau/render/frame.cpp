#include "frame.h"

#include "tau/assets/material.h"
#include "tau/assets/mesh.h"
#include "tau/assets/texture.h"
#include "tau/components/camera.h"
#include "tau/components/transform.h"
#include "tau/containers/flat_map.h"
#include "tau/ecs.h"
#include "tau/hash.h"
#include "tau/log.h"
#include "tau/memory/arena_function.h"
#include "tau/os.h"
#include "tau/profiling.h"
#include "tau/render/legacy.h"
#include "tau/render/overlay.h"
#include "tau/render/post.h"
#include "tau/rhi/bindless.h"
#include "tau/rhi/context.h"
#include "tau/rhi/deletion.h"
#include "tau/rhi/device.h"
#include "tau/rhi/gpu_timers.h"
#include "tau/rhi/material_heap.h"
#include "tau/rhi/renderdoc.h"
#include "tau/rhi/renderer_constants.h"
#include "tau/rhi/swapchain.h"
#include "tau/rhi/transfer.h"
#include "tau/systems/camera.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

#ifdef TAU_ENABLE_PROFILING
    #include <tracy/TracyVulkan.hpp>
#endif

namespace tau::renderer::frame
{
    namespace
    {
        struct producer_t
        {
            u32_t name_hash;
            i32 order;
            producer_fn_t fn;
        };

        std::vector<producer_t> g_producers;
        graph::graph_cache_t* g_cache = nullptr;
        graph::graph_t g_graph;
        bool g_failed_reported = false;
        flat_map_t<u32_t> g_output_texture_ids;

        // render() swaps buffers with the submit side, a moved from vector keeps no capacity
        std::vector<render_view_t> submitted_views;
        std::vector<render_view_t> frame_views;
        std::vector<output_target_t> submitted_outputs;
        std::vector<output_target_t> frame_outputs;
        flat_map_t<u32_t> submitted_punctual_slots;
        ecs::entity_t submitted_cascade_light = ecs::NULL_ENTITY;
        flat_map_t<debug_view_e> view_debug_views;
        flat_map_t<bool> view_post_processing;

        VkExtent2D g_render_extent = {0, 0};
        VkExtent2D g_logical_extent = {0, 0};

        bool image_acquired = false;
        u32_t acquired_index = 0;
        bool pending_resize = false;
        // resize waits until the held image is presented
        bool deferred_resize = false;

        // per view globals: MAX_VIEWS_PER_FRAME slots per frame in flight
        u32_t g_globals_stride = 0;
        VkBuffer g_globals_buffer = VK_NULL_HANDLE;
        VmaAllocation g_globals_alloc = VK_NULL_HANDLE;
        u8* g_globals_mapped = nullptr;
        flat_map_t<u32_t> g_view_offsets[MAX_FRAMES_IN_FLIGHT];
        mat4 g_pre_rotation = {};

        fallbacks_t g_fallbacks;

#ifdef TAU_ENABLE_PROFILING
        TracyVkCtx tracy_vk_ctx = nullptr;
#endif

        vec3 world_position(const transform_t& transform)
        { return {transform.world_mat[3][0], transform.world_mat[3][1], transform.world_mat[3][2]}; }

        // the extents follow a new swapchain; an output target overrides them until the next one
        void resize_swapchain()
        {
            if (!swapchain::resize()) { return; }
            g_render_extent = swapchain::state().extent;
            g_logical_extent = swapchain::state().logical_extent;
        }

        VkResult acquire(u32_t* image_index)
        {
            if (swapchain::state().handle == VK_NULL_HANDLE && !ctx.caps.headless) { return VK_ERROR_OUT_OF_DATE_KHR; }

            frame_slot_t& slot = ctx.frames[ctx.cur_frame];
            wait_for_frame();

            if (ctx.caps.headless) { *image_index = 0; }
            else
            {
                const VkResult res = swapchain::acquire(slot.acquire_semaphore, image_index);
                if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_SURFACE_LOST_KHR)
                {
                    return res;
                }
            }

            vkResetCommandPool(ctx.device, slot.command_pool, 0);
            slot.allocator.reset();
            return VK_SUCCESS;
        }

        // one submit carrying the swapchain acquire and the resource upload timeline, then present
        // the timeline value signalled here is what the next frame's readback waits on
        // headless: timelines only
        void submit_headless(VkCommandBuffer cmd, frame_slot_t& frame_data)
        {
            const u64_t next_timeline_value = ++ctx.timeline_value;
            const u64_t wait_value = transfer::last_value();
            const VkSemaphore upload_timeline = transfer::timeline();

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
                .pWaitSemaphores = &upload_timeline,
                .pWaitDstStageMask = &wait_stage,
                .commandBufferCount = 1,
                .pCommandBuffers = &cmd,
                .signalSemaphoreCount = 1,
                .pSignalSemaphores = &ctx.timeline_semaphore,
            };
            vkQueueSubmit(ctx.graphics_queue, 1, &submit_info, VK_NULL_HANDLE);
            frame_data.target_timeline_value = next_timeline_value;
        }

        void submit_and_present(VkCommandBuffer cmd, frame_slot_t& frame_data, u32_t image_index)
        {
            VkSemaphore wait_semaphores[] = {
                frame_data.acquire_semaphore,
                transfer::timeline(),
            };

            VkSemaphore signal_semaphores[] = {
                swapchain::state().release_semaphores[image_index],
                ctx.timeline_semaphore,
            };

            VkPipelineStageFlags wait_stages[] = {
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            };

            u64_t wait_values[] = {0, transfer::last_value()};

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

            const VkResult res = swapchain::present(image_index);

            if (res == VK_ERROR_SURFACE_LOST_KHR) { swapchain::recreate_surface(); }
            else if (res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_OUT_OF_DATE_KHR) { resize_swapchain(); }
        }
    } // namespace

    void register_producer(u32_t name_hash, i32 order, producer_fn_t fn)
    {
        for (producer_t& p : g_producers)
        {
            if (p.name_hash == name_hash)
            {
                p.order = order;
                p.fn = fn;
                return;
            }
        }
        g_producers.push_back({name_hash, order, fn});
        std::sort(g_producers.begin(), g_producers.end(), [](const producer_t& a, const producer_t& b)
                  { return a.order != b.order ? a.order < b.order : a.name_hash < b.name_hash; });
    }

    u32_t remove_producers_of(void* module_base)
    {
        const auto first =
            std::remove_if(g_producers.begin(), g_producers.end(), [module_base](const producer_t& p)
                           { return os::module_base_of(reinterpret_cast<const void*>(p.fn)) == module_base; });
        const u32_t removed = static_cast<u32_t>(g_producers.end() - first);
        g_producers.erase(first, g_producers.end());
        return removed;
    }

    u32_t count_producers_of(void* module_base)
    {
        return static_cast<u32_t>(
            std::count_if(g_producers.begin(), g_producers.end(), [module_base](const producer_t& p)
                          { return os::module_base_of(reinterpret_cast<const void*>(p.fn)) == module_base; }));
    }

    void init()
    {
        if (!ctx.caps.headless)
        {
            g_render_extent = swapchain::state().extent;
            g_logical_extent = swapchain::state().logical_extent;
        }

        void* tracy = nullptr;
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
        tracy = tracy_vk_ctx;

        vkDestroyCommandPool(ctx.device, tracy_pool, nullptr);
#endif
        g_cache = graph::create_cache();
        graph::set_profiler(g_cache, tracy);

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
        g_globals_stride = static_cast<u32_t>((sizeof(gpu::GlobalData) + min_alignment - 1) & ~(min_alignment - 1));

        void* mapped_global_data;
        create_mapped_buffer("GlobalData", g_globals_stride * MAX_FRAMES_IN_FLIGHT * MAX_VIEWS_PER_FRAME,
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, g_globals_buffer, g_globals_alloc, mapped_global_data);

        g_globals_mapped = static_cast<u8*>(mapped_global_data);

        VkDescriptorBufferInfo ubo_info = {
            .buffer = g_globals_buffer,
            .offset = 0,
            .range = sizeof(gpu::GlobalData),
        };

        VkWriteDescriptorSet write_desc = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = bindless::frame_set(),
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
            .pBufferInfo = &ubo_info,
        };
        vkUpdateDescriptorSets(ctx.device, 1, &write_desc, 0, nullptr);

        legacy::init(tracy);
        post::init();

        g_fallbacks.default_tex = assets::pin(assets::load<texture_t>("engine://assets/textures/default_white.png"));
        g_fallbacks.texture = assets::pin(assets::load<texture_t>(FALLBACK_TEXTURE_PATH));
        g_fallbacks.mesh = assets::pin(assets::load<mesh_t>(FALLBACK_MESH_PATH));
        g_fallbacks.material = assets::pin(assets::load<material_t>(FALLBACK_MATERIAL_PATH));
        g_fallbacks.default_material = assets::pin(assets::load<material_t>(DEFAULT_MATERIAL_PATH));

        overlay::init();
    }

    void reset_assets()
    {
        legacy::reset_assets();
        post::shutdown();
        assets::unpin(g_fallbacks.default_tex);
        assets::unpin(g_fallbacks.texture);
        assets::unpin(g_fallbacks.mesh);
        assets::unpin(g_fallbacks.material);
        assets::unpin(g_fallbacks.default_material);
    }

    void shutdown()
    {
        overlay::shutdown();
        graph::destroy_cache(g_cache);
        g_cache = nullptr;
        legacy::shutdown();

        if (g_globals_buffer != VK_NULL_HANDLE) { vmaDestroyBuffer(ctx.allocator, g_globals_buffer, g_globals_alloc); }
        g_globals_buffer = VK_NULL_HANDLE;

#ifdef TAU_ENABLE_PROFILING
        if (tracy_vk_ctx) { TracyVkDestroy(tracy_vk_ctx); }
        tracy_vk_ctx = nullptr;
#endif
    }

    void begin()
    {
        if (image_acquired || (swapchain::state().handle == VK_NULL_HANDLE && !ctx.caps.headless)) { return; }

        // suboptimal still gives an image, rebuild at present
        const VkResult res = acquire(&acquired_index);
        image_acquired = res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR;
    }

    void on_window_resized() { pending_resize = true; }

    namespace
    {
        void record_graph(VkCommandBuffer cmd, ecs::registry_t& reg, const inputs_t& in)
        {
            graph::graph_t& g = g_graph;
            graph::begin(g, g_cache, ctx.cur_frame);

            targets_t t;
            t.primary_view_hash = in.post_view_hash;
            for (u32_t i = 0; i < in.legacy_count; i++)
            {
                const legacy_image_t& l = in.legacy[i];
                const graph::image_h h =
                    graph::import_image(g, l.name, l.image, l.view,
                                        {.format = l.format, .width = l.extent.width, .height = l.extent.height},
                                        l.layout, VK_IMAGE_LAYOUT_UNDEFINED, l.sampled_id);
                if (std::string_view(l.name) == "SceneColor") { t.scene_color = h; }
                else if (std::string_view(l.name) == "SceneDepth") { t.scene_depth = h; }
            }

            const swapchain::swapchain_t& sc = swapchain::state();
            const VkClearValue grey = {.color = {{0.01f, 0.01f, 0.01f, 1.0f}}};
            if (in.swapchain != VK_NULL_HANDLE)
            {
                t.swapchain = graph::import_image(
                    g, "Swapchain", in.swapchain, in.swapchain_view,
                    {.format = sc.format, .width = sc.extent.width, .height = sc.extent.height, .clear = grey},
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            }
            if (in.to_swapchain && t.swapchain.valid())
            {
                t.final_output = t.swapchain;
                t.final_is_swapchain = true;
            }
            else
            {
                t.final_output = graph::persistent_image(g, "OutputTarget",
                                                         {.format = sc.format,
                                                          .width = in.output_extent.width,
                                                          .height = in.output_extent.height,
                                                          .clear = grey},
                                                         in.output_hash);
            }

            size_t i = 0;
            for (; i < g_producers.size() && g_producers[i].order < FEATURE_ORDER_AFTER_POST; i++)
            {
                g_producers[i].fn(g, t, reg);
            }
            post::build(g, t, reg, in.post_view_hash, in.post_view_pos, in.post_enabled);
            for (; i < g_producers.size(); i++) { g_producers[i].fn(g, t, reg); }

            if (!graph::compile(g))
            {
                if (!g_failed_reported) { TAU_LOG_ERROR("RENDERER", "The frame graph did not compile, see above"); }
                g_failed_reported = true;
                return;
            }

            g_output_texture_ids.clear();
            if (!t.final_is_swapchain) { g_output_texture_ids[in.output_hash] = graph::sampled_id(g, t.final_output); }

            graph::execute(g, cmd);
        }
    } // namespace

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
                resize_swapchain();
            }
            pending_resize = false;
        }

        u64_t completed_frame = 0;
        vkGetSemaphoreCounterValue(ctx.device, ctx.timeline_semaphore, &completed_frame);
        deletion::process(completed_frame, transfer::completed());
        material_heap::begin_frame();

        save_pipeline_cache_when_settled();

        u32_t index = acquired_index;
        if (!image_acquired)
        {
            const VkResult res = acquire(&index);

            if (res == VK_ERROR_SURFACE_LOST_KHR)
            {
                swapchain::recreate_surface();
                return;
            }
            else if (res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_OUT_OF_DATE_KHR)
            {
                resize_swapchain();
                return;
            }
            else if (res != VK_SUCCESS)
            {
                vkQueueWaitIdle(ctx.present_queue);
                return;
            }
        }

        frame_slot_t& frame_data = ctx.frames[ctx.cur_frame];
        VkCommandBuffer cmd = frame_data.cmd;

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

        transfer::flush_acquires(cmd);

        g_pre_rotation = swapchain::pre_rotation();

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
                v.extent = g_render_extent;
                v.z_near = cam.near_plane;
                v.z_far = cam.far_plane;
                frame_views.push_back(v);
            }
        }

        std::stable_partition(frame_views.begin(), frame_views.end(),
                              [](const render_view_t& v) { return v.kind == view_kind_e::DEPTH_ONLY; });

        if (frame_views.size() > MAX_VIEWS_PER_FRAME)
        {
            TAU_LOG_FATAL("RENDERER", "Submitted {} views exceeds MAX_VIEWS_PER_FRAME ({})", frame_views.size(),
                          MAX_VIEWS_PER_FRAME);
            abort();
        }

        flat_map_t<u32_t>& offsets = g_view_offsets[ctx.cur_frame];
        offsets.clear();
        for (u32_t view_idx = 0; view_idx < frame_views.size(); view_idx++)
        {
            offsets[frame_views[view_idx].name_hash] = view_globals_offset_at(view_idx);
        }

        tau::active_arena = &frame_data.allocator;

        // headless: offscreen target, size from the runner
        const output_target_t default_output = ctx.caps.headless ? output_target_t{"HeadlessOutput"_h, {1280, 720}}
                                                                 : output_target_t{"Swapchain"_h, swapchain::state().extent};
        output_target_t final_output = frame_outputs.empty() ? default_output : frame_outputs[0];
        bool output_to_swapchain = (final_output.target_hash == "Swapchain"_h);
        if (output_to_swapchain || final_output.extent.width == 0 || final_output.extent.height == 0)
        {
            final_output.extent = swapchain::state().extent;
        }
        g_render_extent = final_output.extent;

        inputs_t inputs;
        legacy::record(cmd, reg, frame_views, frame_punctual_slots, frame_cascade_light, inputs);

        inputs.post_view_hash = "PrimaryView"_h;
        for (const render_view_t& v : frame_views)
        {
            if (v.kind == view_kind_e::COLOR && v.name_hash == "PrimaryView"_h)
            {
                inputs.post_view_pos = v.position;
                break;
            }
        }
        const bool* post_flag = view_post_processing.find(inputs.post_view_hash);
        inputs.post_enabled = post_flag == nullptr || *post_flag;

        if (!ctx.caps.headless)
        {
            inputs.swapchain = swapchain::state().images[index];
            inputs.swapchain_view = swapchain::state().views[index];
        }
        inputs.to_swapchain = output_to_swapchain;
        inputs.output_hash = final_output.target_hash;
        inputs.output_extent = g_render_extent;
        record_graph(cmd, reg, inputs);

        gpu_timers::end_frame(cmd);
        vkEndCommandBuffer(cmd);

        tau::active_arena = nullptr;

        if (ctx.caps.headless) { submit_headless(cmd, frame_data); }
        else
        {
            submit_and_present(cmd, frame_data, index);
        }
        renderdoc::frame_end();

        advance_frame();
        image_acquired = false;

        if (deferred_resize)
        {
            resize_swapchain();
            deferred_resize = false;
        }
    }

    void submit_output_target(u32_t target_hash, VkExtent2D extent)
    {
        if (extent.width == 0 || extent.height == 0) { extent = swapchain::state().extent; }

        g_render_extent = extent;
        g_logical_extent = extent;

        submitted_outputs.push_back({target_hash, extent});
    }

    u32_t get_target_texture_id(u32_t name_hash)
    {
        const u32_t* id = g_output_texture_ids.find(name_hash);
        return id ? *id : BINDLESS_NULL_HANDLE;
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

    bool bind_view_descriptors(VkCommandBuffer cmd, VkPipelineLayout layout, u32_t view_name_hash)
    {
        const u32_t* offset = g_view_offsets[ctx.cur_frame].find(view_name_hash);
        if (offset == nullptr) { return false; }

        const VkDescriptorSet sets[] = {bindless::global_set(), bindless::frame_set()};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, sets, 1, offset);
        return true;
    }

    u32_t view_globals_offset(u32_t view_name_hash)
    {
        const u32_t* offset = g_view_offsets[ctx.cur_frame].find(view_name_hash ? view_name_hash : "PrimaryView"_h);
        return offset != nullptr ? *offset : first_view_globals_offset();
    }

    u32_t first_view_globals_offset() { return ctx.cur_frame * MAX_VIEWS_PER_FRAME * g_globals_stride; }

    u32_t view_globals_offset_at(u32_t view_index)
    { return first_view_globals_offset() + view_index * g_globals_stride; }

    gpu::GlobalData* view_globals(u32_t view_index)
    { return reinterpret_cast<gpu::GlobalData*>(g_globals_mapped + view_globals_offset_at(view_index)); }

    void fill_view_camera(gpu::GlobalData& globals, const render_view_t& view)
    {
        gpu::GlobalData* slot = &globals;
        const render_view_t& sub = view;
        slot->view = sub.view;
        slot->projection = sub.projection;

        const mat4 final_view_proj = sub.kind == view_kind_e::COLOR ? g_pre_rotation * sub.view_proj : sub.view_proj;
        slot->view_proj = final_view_proj;
        slot->inv_view_proj = inverse(final_view_proj);

        const debug_view_e* debug_view = view_debug_views.find(sub.name_hash);
        slot->debug_view = static_cast<u32_t>(debug_view ? *debug_view : debug_view_e::OFF);

        frustum_planes(sub.view_proj, slot->frustum_planes);
        slot->camera_pos = vec4(sub.position, 0.0f);

        slot->cull_flags = (sub.kind == view_kind_e::DEPTH_ONLY) ? 1u : 0u;
    }

    VkExtent2D render_extent() { return g_render_extent; }
    VkExtent2D logical_extent() { return g_logical_extent; }

    const fallbacks_t& fallbacks() { return g_fallbacks; }
} // namespace tau::renderer::frame
