#pragma once

#include "tau/containers/flat_map.h"
#include "tau/defines.h"
#include "tau/ecs_fwd.h"
#include "tau/math.h"
#include "tau/memory/arena_function.h"
#include "tau/render/frame.h"
#include "tau/render/legacy_graph.h"
#include "tau/rhi/gpu_structs.h"

#include <span>
#include <vector>

namespace tau { struct shader_t; } // namespace tau

// the old renderer: draw list, culling, clusters, shadows, gbuffer, deferred lighting on the old graph
// deleted at the start of part 3; new passes are frame::register_producer
namespace tau::renderer::legacy
{
    enum class mesh_pass_e : u32_t
    {
        GBUFFER,        // opaque, implements the surface interface, deferred
        FORWARD_OPAQUE, // opaque, writes its own color
        BLENDED,        // anything that blends, always forward
        COUNT,
    };

    constexpr u32_t MESH_PASS_COUNT = static_cast<u32_t>(mesh_pass_e::COUNT);

    TAU_ENGINE_API mesh_pass_e pass_for(const shader_t& shader);
    TAU_ENGINE_API VkPipeline binding_pipeline(const shader_t& shader);

    using graph_builder_func_t = void (*)(rendergraph_t&, ecs::registry_t&);

    // capturing: a pass is handed the resources of its own view, not a lookup by name
    // two colour views own separately named gbuffers
    using pass_execute_func_t = tau::arena_function<void(rendergraph_t&, tau::material_t&)>;

    // passes drawing through the legacy block
    TAU_ENGINE_API void register_renderer_feature(u32_t name_hash, i32 order, graph_builder_func_t builder);
    // features whose builder is code in that module (see os::module_base_of)
    // a game library's go when it is unloaded and it registers them again from TAU_ON_LOAD
    TAU_ENGINE_API u32_t remove_features_of(void* module_base);
    TAU_ENGINE_API u32_t count_features_of(void* module_base);

    TAU_ENGINE_API rg_pass_t& add_mesh_pass(rendergraph_t& graph, u32_t name_hash, const char* debug_name,
                                            u32_t view_name_hash, mesh_pass_e pass, bool depth_only,
                                            const std::vector<rg_resource_id>& reads,
                                            const std::vector<rg_resource_id>& writes, rg_resource_id depth);

    // reads and writes are spans, copied into the pass at once, vectors cost a temporary per call per frame
    TAU_ENGINE_API rg_pass_t& add_fullscreen_pass(rendergraph_t& graph, u32_t name_hash, const char* debug_name,
                                                  tau::material_t* material, std::span<const rg_resource_id> reads,
                                                  std::span<const rg_resource_id> writes,
                                                  pass_execute_func_t on_execute = {}, u32_t view_name_hash = 0,
                                                  rg_resource_id depth_tex_res = RG_NULL_ID);

    TAU_ENGINE_API rg_pass_t& add_compute_pass(rendergraph_t& graph, u32_t name_hash, const char* debug_name,
                                               tau::material_t* material, u32_t dispatch_x, u32_t dispatch_y,
                                               u32_t dispatch_z, const std::vector<rg_resource_id>& reads,
                                               const std::vector<rg_resource_id>& writes,
                                               pass_execute_func_t on_execute = {}, u32_t view_name_hash = 0);

    // every culled object in the view, one pipeline, fills object_buffer
    TAU_ENGINE_API void draw_view_objects(VkCommandBuffer cmd, u32_t view_name_hash, VkPipeline pipeline,
                                          VkPipelineLayout layout, gpu::PushConstants pc);

    TAU_ENGINE_API void set_fallback_light(bool enabled, const vec3& direction = {0.0f, -1.0f, 0.0f},
                                           const vec3& color = {1.0f, 1.0f, 1.0f}, f32 intensity = 1.0f);

    TAU_ENGINE_API bool get_primary_depth_bounds(f32& out_near_z, f32& out_far_z);

    // a missing asset is reported once, not once per frame
    // reloading anything clears that, so breaking the same file twice tells you twice
    TAU_ENGINE_API void forget_missing_asset_reports();

    void init(void* tracy_vk_ctx);
    void reset_assets();
    void shutdown();

    // lights, draw list, culling, clusters, the old graph; hands SceneColor / SceneDepth / the selection mask over
    void record(VkCommandBuffer cmd, ecs::registry_t& reg, std::vector<frame::render_view_t>& frame_views,
                const flat_map_t<u32_t>& frame_punctual_slots, ecs::entity_t frame_cascade_light,
                frame::inputs_t& inputs);
} // namespace tau::renderer::legacy
