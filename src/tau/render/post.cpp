#include "post.h"

#include "tau/asset.h"
#include "tau/assets/material.h"
#include "tau/assets/shader.h"
#include "tau/components/camera.h"
#include "tau/components/profile_volume.h"
#include "tau/components/transform.h"
#include "tau/ecs.h"
#include "tau/hash.h"
#include "tau/log.h"
#include "tau/render/view_profile.h"
#include "tau/rhi/bindless.h"
#include "tau/rhi/context.h"
#include "tau/rhi/gpu_structs.h"
#include "tau/rhi/material_heap.h"
#include "tau/rhi/renderer_constants.h"
#include "tau/systems/camera.h"
#include "tau/systems/transform.h"

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

        constexpr const char* FINAL_PASS_PATH = "engine://assets/materials/uberpost.mat";
        constexpr u32_t FINAL_PASS_SLOT = 0xFFFFFFFFu;

        asset_t<material_t> g_final_pass;
        // per view and slot, a base material with the volumes' overrides
        std::unordered_map<u64_t, std::unique_ptr<material_t>> g_resolved;

        // scratch kept for its capacity
        std::vector<volume_sample_t> post_volumes;
        std::vector<const material_t*> bound_materials;
        std::vector<const volume_sample_t*> influences;

        f32 volume_influence(const profile_volume_t& volume, const vec3& center_pos, const vec3& point)
        {
            if (volume.is_global) { return 1.0f; }

            const f32 center[3] = {center_pos.x, center_pos.y, center_pos.z};
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

            auto view = reg.view<profile_volume_t, transform_t>();
            for (const ecs::entity_t entity : view)
            {
                const profile_volume_t& volume = view.get<profile_volume_t>(entity);
                const vec3 center = volume.is_global ? vec3{} : transform_system::get_world_position(reg, entity);
                const f32 w = volume_influence(volume, center, view_pos) * volume.weight;
                if (w <= 0.0f) { continue; }

                for (const asset_t<material_t> handle : volume.effect_overrides)
                {
                    material_t* over = assets::get(handle);
                    if (!over || !over->shader.is_valid() || over->authored_properties.empty()) { continue; }
                    samples.push_back({over, over->shader, std::min(w, 1.0f), volume.priority});
                }
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

        // the profile's values into the final pass, until part 11's LUT bake
        void apply_profile(material_t& final_pass, const view_profile_t& profile)
        {
            final_pass.set_property_if_present("exposure"_h, view_profile::exposure(profile));
            final_pass.set_property_if_present("tonemap_op"_h, static_cast<u32_t>(profile.tonemap));
            final_pass.set_property_if_present("contrast"_h, profile.contrast);
            final_pass.set_property_if_present("saturation"_h, profile.saturation);
            final_pass.set_property_if_present("vignette_strength"_h, profile.vignette);
            final_pass.set_property_if_present("vignette_radius"_h, profile.vignette_radius);
            final_pass.set_property_if_present("grain_strength"_h, profile.grain);
        }

        bool target_matches(const material_t* effect, VkFormat dst_format, u32_t slot)
        {
            shader_t* shader = assets::get(effect->shader);
            if (!shader || shader->target_formats.empty()) { return true; }
            if (shader->target_formats[0] == dst_format) { return true; }

            static std::mutex warned_mutex;
            static std::unordered_set<u32_t> warned;
            {
                std::scoped_lock lock(warned_mutex);
                if (!warned.insert(slot).second) { return false; }
            }

            TAU_LOG_ERROR("RENDERER",
                          "Post effect in slot {} renders to format {} but the chain runs at {}. A post effect has to "
                          "declare [RenderTarget(\"RGBA16_FLOAT\")], skipping it.",
                          slot, static_cast<i32>(shader->target_formats[0]), static_cast<i32>(dst_format));
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

    void init() { g_final_pass = assets::pin(assets::load<material_t>(FINAL_PASS_PATH)); }

    void shutdown()
    {
        assets::unpin(g_final_pass);
        g_resolved.clear();
    }

    void build(graph::graph_t& g, const frame::targets_t& t, ecs::registry_t& reg, u32_t view_hash,
               const vec3& view_pos, bool enabled)
    {
        // post off (the editor's toggle): the default profile, no effects, no volumes
        static const view_profile_t default_profile;
        const camera_t* camera = nullptr;
        if (enabled)
        {
            const ecs::entity_t cam = camera_system::get_active_camera(reg);
            if (cam != ecs::NULL_ENTITY) { camera = reg.try_get<camera_t>(cam); }
        }
        const view_profile_t& profile = camera ? camera->profile : default_profile;

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

        if (camera)
        {
            for (u32_t slot = 0; slot < static_cast<u32_t>(camera->effects.size()); slot++)
            {
                material_t* effect = resolve_unique(camera->effects[slot], slot);
                if (!effect || !effect->shader.is_valid()) { continue; }

                if (!ping.valid())
                {
                    ping = graph::create_image(g, "PostPing", chain_desc);
                    pong = graph::create_image(g, "PostPong", chain_desc);
                }
                const graph::image_h dst = chain_src.index == ping.index ? pong : ping;
                if (!target_matches(effect, chain_desc.format, slot)) { continue; }

                add_fullscreen(g, "PostEffect", effect, chain_src, t.scene_depth, dst, view_hash);
                chain_src = dst;
            }
        }

        // an instance per view: the profile writes into it
        material_t* final_mat = resolve_post_effect(g_final_pass, post_volumes, view_hash, FINAL_PASS_SLOT, true);
        if (final_mat && final_mat->shader.is_valid())
        {
            apply_profile(*final_mat, profile);
            add_fullscreen(g, "UberPost", final_mat, chain_src, t.scene_depth, t.final_output, view_hash);
        }
    }
} // namespace tau::renderer::post
