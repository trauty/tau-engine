#include "post.h"

#include "tau/asset.h"
#include "tau/assets/material.h"
#include "tau/assets/shader.h"
#include "tau/components/post_process.h"
#include "tau/components/transform.h"
#include "tau/ecs.h"
#include "tau/hash.h"
#include "tau/log.h"
#include "tau/rhi/bindless.h"
#include "tau/rhi/context.h"
#include "tau/rhi/gpu_structs.h"
#include "tau/rhi/material_heap.h"
#include "tau/rhi/renderer_constants.h"
#include "tau/systems/camera.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tau::renderer::post
{
    namespace
    {
        struct volume_sample_t
        {
            const material_t* overrides;
            asset_t<shader_t> shader;
            f32 weight;
            i32_t priority;
        };

        asset_t<material_t> g_final_pass;
        // per view and slot, a base material with the volumes' overrides
        std::unordered_map<u64_t, std::unique_ptr<material_t>> g_resolved;

        // scratch kept for its capacity
        std::vector<volume_sample_t> post_volumes;
        std::vector<const material_t*> bound_materials;
        std::vector<const volume_sample_t*> influences;

        f32 volume_influence(const post_volume_t& volume, const transform_t& transform, const vec3& point)
        {
            if (volume.is_global) { return 1.0f; }

            const f32 center[3] = {transform.world_mat[3][0], transform.world_mat[3][1], transform.world_mat[3][2]};
            const f32 half[3] = {std::abs(volume.extents.x), std::abs(volume.extents.y), std::abs(volume.extents.z)};
            const f32 p[3] = {point.x, point.y, point.z};

            f32 dist_sq = 0.0f;
            for (u32_t i = 0; i < 3; i++)
            {
                const f32 outside = std::abs(p[i] - center[i]) - half[i];
                if (outside > 0.0f) { dist_sq += outside * outside; }
            }

            if (dist_sq <= 0.0f) { return 1.0f; }
            if (volume.blend_distance <= 0.0f) { return 0.0f; }

            const f32 t = std::sqrt(dist_sq) / volume.blend_distance;
            return (t >= 1.0f) ? 0.0f : (1.0f - t);
        }

        void blend_property(u8* dst, const u8* src, const shader_member_t& member, f32 w)
        {
            auto lerp_floats = [&](u32_t count)
            {
                for (u32_t i = 0; i < count; i++)
                {
                    f32 a, b;
                    std::memcpy(&a, dst + member.offset + i * sizeof(f32), sizeof(f32));
                    std::memcpy(&b, src + member.offset + i * sizeof(f32), sizeof(f32));
                    const f32 blended = a + (b - a) * w;
                    std::memcpy(dst + member.offset + i * sizeof(f32), &blended, sizeof(f32));
                }
            };

            switch (member.type)
            {
            case shader_member_type_e::FLOAT: lerp_floats(1); break;
            case shader_member_type_e::FLOAT2: lerp_floats(2); break;
            case shader_member_type_e::FLOAT3: lerp_floats(3); break;
            case shader_member_type_e::FLOAT4: lerp_floats(4); break;
            default:
                if (w >= 0.5f) { std::memcpy(dst + member.offset, src + member.offset, member.size); }
                break;
            }
        }

        void gather_post_volumes(ecs::registry_t& reg, const vec3& view_pos, std::vector<volume_sample_t>& samples)
        {
            samples.clear();

            for (auto [entity, volume, transform] : reg.view<post_volume_t, transform_t>().each())
            {
                if (!volume.overrides.is_valid()) { continue; }

                material_t* over = assets::get(volume.overrides);
                if (!over || !over->shader.is_valid()) { continue; }
                if (over->authored_properties.empty()) { continue; }

                const f32 w = volume_influence(volume, transform, view_pos) * volume.weight;
                if (w <= 0.0f) { continue; }

                samples.push_back({over, over->shader, std::min(w, 1.0f), volume.priority});
            }

            std::stable_sort(samples.begin(), samples.end(), [](const volume_sample_t& a, const volume_sample_t& b)
                             { return a.priority < b.priority; });
        }

        material_t* resolve_post_effect(asset_t<material_t> effect_handle, const std::vector<volume_sample_t>& volumes,
                                        u32_t view_name_hash, u32_t slot, bool force_instance)
        {
            material_t* base = assets::get(effect_handle);
            if (!base || !base->shader.is_valid()) { return nullptr; }

            influences.clear();
            for (const volume_sample_t& sample : volumes)
            {
                if (sample.shader == base->shader) { influences.push_back(&sample); }
            }

            if (influences.empty() && !force_instance) { return base; }

            const u64_t key = (static_cast<u64_t>(view_name_hash) << 32) | slot;
            auto it = g_resolved.find(key);
            if (it == g_resolved.end() || it->second->shader != base->shader)
            {
                auto instance = std::make_unique<material_t>(base->shader);
                it = g_resolved.insert_or_assign(key, std::move(instance)).first;
            }

            material_t& resolved = *it->second;
            resolved.data = base->data;

            const shader_t* shader = assets::get(base->shader);
            if (shader)
            {
                for (const volume_sample_t* inf : influences)
                {
                    for (u32_t name_hash : inf->overrides->authored_properties)
                    {
                        const shader_member_t* member = shader->module.members.find(name_hash);
                        if (member == nullptr) { continue; }
                        blend_property(resolved.data.data(), inf->overrides->data.data(), *member, inf->weight);
                    }
                }
            }

            resolved.dirty_frames = MAX_FRAMES_IN_FLIGHT;
            return &resolved;
        }

        bool target_matches(const material_t* effect, VkFormat dst_format, u32_t slot, bool is_final)
        {
            shader_t* shader = assets::get(effect->shader);
            if (!shader || shader->target_formats.empty()) { return true; }
            if (shader->target_formats[0] == dst_format) { return true; }

            static std::mutex warned_mutex;
            static std::unordered_set<u64_t> warned;
            {
                const u64_t warn_key = (static_cast<u64_t>(is_final ? 1u : 0u) << 32) | slot;
                std::scoped_lock lock(warned_mutex);
                if (!warned.insert(warn_key).second) { return false; }
            }

            if (is_final)
            {
                TAU_LOG_ERROR("RENDERER",
                              "Final pass renders to format {} but the display image is {}. A final pass has to "
                              "declare [RenderTarget(\"Swapchain\")], only chain effects target the HDR chain. "
                              "Falling back to the stock final pass.",
                              static_cast<i32>(shader->target_formats[0]), static_cast<i32>(dst_format));
            }
            else
            {
                TAU_LOG_ERROR("RENDERER",
                              "Post effect in slot {} renders to format {} but the chain runs at {}. A chain effect "
                              "has to declare [RenderTarget(\"RGBA16_FLOAT\")], only the final pass targets the "
                              "display, skipping it.",
                              slot, static_cast<i32>(shader->target_formats[0]), static_cast<i32>(dst_format));
            }
            return false;
        }

        struct fullscreen_t
        {
            material_t* material;
            graph::image_h input;
            graph::image_h depth;
            u32_t view_hash;
        };

        void record_fullscreen(const graph::record_ctx_t& rc, const void* data)
        {
            const fullscreen_t& f = *static_cast<const fullscreen_t*>(data);
            material_t* material = f.material;

            if (f.input.valid())
            {
                const u32_t input_id = graph::sampled_id(*rc.graph, f.input);
                material->set_property_if_present("input_tex"_h, input_id);
                material->set_property_if_present("scene_color_tex"_h, input_id);
            }
            if (f.depth.valid())
            {
                material->set_property_if_present("depth_tex"_h, graph::sampled_id(*rc.graph, f.depth));
            }
            material->sync();

            shader_t* shader = assets::get(material->shader);
            const VkPipeline pipeline = shader ? shader->get_pipeline(pipeline_variant_e::FORWARD) : VK_NULL_HANDLE;
            if (pipeline == VK_NULL_HANDLE) { return; }

            vkCmdBindPipeline(rc.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

            // no such view this frame (no camera): the frame's first globals
            const u32_t view_offset = frame::view_globals_offset(f.view_hash);
            const VkDescriptorSet sets[] = {bindless::global_set(), bindless::frame_set()};
            vkCmdBindDescriptorSets(rc.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shader->pipeline_layout, 0, 2, sets, 1,
                                    &view_offset);

            const gpu::PushConstants pc = {
                0,
                material_heap::address(),
                material->heap_offset[ctx.cur_frame],
            };
            vkCmdPushConstants(rc.cmd, shader->pipeline_layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(gpu::PushConstants),
                               &pc);
            vkCmdDraw(rc.cmd, 3, 1, 0, 0);
        }

        void add_fullscreen(graph::graph_t& g, const char* name, material_t* material, graph::image_h input,
                            graph::image_h depth, graph::image_h output, u32_t view_hash)
        {
            const graph::pass_h p =
                graph::add_pass(g, name, &record_fullscreen, fullscreen_t{material, input, depth, view_hash});
            graph::use(g, p, input, graph::access_e::SAMPLED);
            if (depth.valid() && depth.index != input.index) { graph::use(g, p, depth, graph::access_e::SAMPLED); }
            graph::use(g, p, output, graph::access_e::COLOR_WRITE, graph::load_e::CLEAR);
        }
    } // namespace

    void init() { g_final_pass = assets::pin(assets::load<material_t>(DEFAULT_FINAL_PASS_PATH)); }

    void shutdown()
    {
        assets::unpin(g_final_pass);
        g_resolved.clear();
    }

    void build(graph::graph_t& g, const frame::targets_t& t, ecs::registry_t& reg, u32_t view_hash,
               const vec3& view_pos, bool enabled)
    {
        const post_process_t* stack = nullptr;
        const ecs::entity_t cam = camera_system::get_active_camera(reg);
        if (cam != ecs::NULL_ENTITY && reg.all_of<post_process_t>(cam))
        {
            const post_process_t& candidate = reg.get<post_process_t>(cam);
            if (candidate.enabled) { stack = &candidate; }
        }

        post_volumes.clear();
        if (enabled) { gather_post_volumes(reg, view_pos, post_volumes); }

        // a handful of pointers: a flat scan
        bound_materials.clear();
        auto resolve_unique = [&](asset_t<material_t> handle, u32_t slot) -> material_t*
        {
            material_t* mat = resolve_post_effect(handle, post_volumes, view_hash, slot, false);
            if (!mat) { return nullptr; }
            if (std::find(bound_materials.begin(), bound_materials.end(), mat) == bound_materials.end())
            {
                bound_materials.push_back(mat);
                return mat;
            }
            mat = resolve_post_effect(handle, post_volumes, view_hash, slot, true);
            if (mat) { bound_materials.push_back(mat); }
            return mat;
        };

        if (!t.scene_color.valid() || !t.final_output.valid()) { return; }

        graph::image_h chain_src = t.scene_color;
        graph::image_h ping, pong;
        const graph::image_desc_t chain_desc = {.format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                                .width = graph::extent(g, t.scene_color).width,
                                                .height = graph::extent(g, t.scene_color).height,
                                                .clear = {.color = {{0.01f, 0.01f, 0.01f, 1.0f}}}};

        if (stack && enabled)
        {
            for (u32_t slot = 0; slot < static_cast<u32_t>(stack->effects.size()); slot++)
            {
                material_t* effect = resolve_unique(stack->effects[slot], slot);
                if (!effect || !effect->shader.is_valid()) { continue; }

                if (!ping.valid())
                {
                    ping = graph::create_image(g, "PostPing", chain_desc);
                    pong = graph::create_image(g, "PostPong", chain_desc);
                }
                const graph::image_h dst = chain_src.index == ping.index ? pong : ping;
                if (!target_matches(effect, chain_desc.format, slot, false)) { continue; }

                add_fullscreen(g, "PostEffect", effect, chain_src, t.scene_depth, dst, view_hash);
                chain_src = dst;
            }
        }

        const asset_t<material_t> final_handle =
            (enabled && stack && stack->final_pass.is_valid()) ? stack->final_pass : g_final_pass;
        material_t* final_mat =
            enabled ? resolve_unique(final_handle, POST_FINAL_PASS_SLOT) : assets::get(g_final_pass);

        if (final_mat && final_mat->shader.is_valid() &&
            !target_matches(final_mat, graph::format(g, t.final_output), POST_FINAL_PASS_SLOT, true))
        {
            final_mat = assets::get(g_final_pass);
        }
        if (final_mat && final_mat->shader.is_valid())
        {
            add_fullscreen(g, "UberPost", final_mat, chain_src, t.scene_depth, t.final_output, view_hash);
        }
    }
} // namespace tau::renderer::post
