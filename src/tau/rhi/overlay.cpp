#include "overlay.h"

#include "tau/asset.h"
#include "tau/assets/mesh.h"
#include "tau/assets/shader.h"
#include "tau/engine.h"
#include "tau/hash.h"
#include "tau/rhi/frame_upload.h"
#include "tau/rhi/renderer.h"
#include "tau/rhi/renderer_constants.h"
#include "tau/rhi/renderer_resources.h"
#include "tau/rhi/rendergraph.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace tau::renderer
{
    namespace
    {
        constexpr const char* SCENE_SHADER_PATH = "engine://assets/shaders/overlay_scene.slang";
        constexpr const char* TOP_SHADER_PATH = "engine://assets/shaders/overlay_top.slang";

        enum class item_mode_e : u32_t
        {
            MESH,
            LINES,
        };

        struct pending_item_t
        {
            u32_t view_hash;
            overlay_depth_e depth;
            item_mode_e mode;
            asset_t<mesh_t> mesh;
            mat4 world;
            vec4 color;
            u32_t first_line;
            u32_t line_count;
            f32 line_width;
        };

        struct draw_t
        {
            u32_t view_hash;
            u32_t vertex_count;
            u32_t item_index;
        };

        std::vector<pending_item_t> pending;
        std::vector<gpu::OverlayLine> pending_lines;
        std::vector<gpu::OverlayItem> gpu_items;
        VkDeviceAddress items_address = 0;

        std::vector<draw_t> scene_draws;
        std::vector<draw_t> top_draws;

        asset_t<shader_t> scene_shader;
        asset_t<shader_t> top_shader;

        void draw(VkCommandBuffer cmd, shader_t* shader, const std::vector<draw_t>& draws)
        {
            const VkPipeline pipeline = shader ? shader->get_pipeline(pipeline_variant_e::FORWARD) : VK_NULL_HANDLE;
            if (pipeline == VK_NULL_HANDLE) { return; }

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

            const gpu::PushConstants pc = {
                items_address,
                res_system.material_heap.device_address,
            };
            vkCmdPushConstants(cmd, shader->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(gpu::PushConstants), &pc);

            u32_t bound_view = 0;
            bool view_found = false;

            for (const draw_t& d : draws)
            {
                if (d.view_hash != bound_view)
                {
                    bound_view = d.view_hash;
                    view_found = bind_view_descriptors(cmd, shader->pipeline_layout, d.view_hash);
                }

                if (view_found && d.vertex_count > 0) { vkCmdDraw(cmd, d.vertex_count, 1, 0, d.item_index); }
            }
        }

        void build(rendergraph_t& graph, ecs::registry_t&)
        {
            scene_draws.clear();
            top_draws.clear();
            if (pending.empty()) { return; }

            const VkDeviceAddress lines_address =
                upload_frame_data(pending_lines.data(), pending_lines.size() * sizeof(gpu::OverlayLine));

            std::stable_sort(pending.begin(), pending.end(), [](const pending_item_t& a, const pending_item_t& b)
                             { return a.view_hash < b.view_hash; });

            gpu_items.resize(pending.size());

            for (u32_t i = 0; i < pending.size(); i++)
            {
                const pending_item_t& item = pending[i];
                gpu::OverlayItem& gpu = gpu_items[i];

                gpu.model = item.world;
                gpu.color = item.color;
                gpu.mode = static_cast<u32_t>(item.mode);
                gpu.line_width = item.line_width;
                gpu.viewport_size = {static_cast<f32>(ctx.render_extent.width),
                                     static_cast<f32>(ctx.render_extent.height)};

                u32_t vertex_count = 0;
                if (item.mode == item_mode_e::MESH)
                {
                    const mesh_t* mesh = assets::get(item.mesh);
                    gpu.vertices = mesh ? mesh->vertex_buffer_address : 0;
                    gpu.indices = mesh ? mesh->index_buffer_address : 0;
                    vertex_count = mesh ? mesh->index_count : 0;
                }
                else
                {
                    gpu.vertices = lines_address + u64_t(item.first_line) * sizeof(gpu::OverlayLine);
                    gpu.indices = 0;
                    vertex_count = item.line_count * 6;
                }

                std::vector<draw_t>& target = item.depth == overlay_depth_e::SCENE ? scene_draws : top_draws;
                target.push_back({item.view_hash, vertex_count, i});
            }

            items_address = upload_frame_data(gpu_items.data(), gpu_items.size() * sizeof(gpu::OverlayItem));
            pending.clear();
            pending_lines.clear();

            const rg_resource_id output = graph.get_resource("FinalOutput"_h);
            if (output == RG_NULL_ID) { return; }

            if (!scene_draws.empty())
            {
                rg_pass_t& pass = graph.add_pass("OverlayScene"_h, "OverlayScene");
                pass.color_writes = {output};
                pass.depth_stencil = graph.get_resource("SceneDepth"_h);
                pass.execute_callback = [](VkCommandBuffer cmd, ecs::registry_t&)
                { draw(cmd, assets::get(scene_shader), scene_draws); };
            }

            if (!top_draws.empty())
            {
                const image_desc_t depth_desc = {
                    .width = ctx.render_extent.width,
                    .height = ctx.render_extent.height,
                    .format = ctx.swapchain.depth_format,
                    .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                    .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
                    .depth_clear = 0.0f,
                };

                rg_pass_t& pass = graph.add_pass("OverlayTop"_h, "OverlayTop");
                pass.color_writes = {output};
                pass.depth_stencil = graph.create_image("OverlayDepth"_h, "OverlayDepth", depth_desc);
                pass.execute_callback = [](VkCommandBuffer cmd, ecs::registry_t&)
                { draw(cmd, assets::get(top_shader), top_draws); };
            }
        }
    } // namespace

    void submit_overlay_mesh(u32_t view_hash, asset_t<mesh_t> mesh, const mat4& world, vec4 color,
                             overlay_depth_e depth)
    { pending.push_back({view_hash, depth, item_mode_e::MESH, mesh, world, color, 0, 0, 0.0f}); }

    void submit_overlay_lines(u32_t view_hash, std::span<const overlay_line_t> lines, f32 width_px,
                              overlay_depth_e depth)
    {
        if (lines.empty()) { return; }

        const u32_t first = static_cast<u32_t>(pending_lines.size());
        for (const overlay_line_t& line : lines)
        {
            pending_lines.push_back({
                {line.a.x,     line.a.y,     line.a.z,     1.0f        },
                {line.b.x,     line.b.y,     line.b.z,     1.0f        },
                {line.color.x, line.color.y, line.color.z, line.color.w},
            });
        }

        pending.push_back({view_hash,
                           depth,
                           item_mode_e::LINES,
                           {},
                           mat4{},
                           vec4(1.0f, 1.0f, 1.0f, 1.0f),
                           first,
                           static_cast<u32_t>(lines.size()),
                           width_px});
    }

    namespace overlay
    {
        void init()
        {
            scene_shader = assets::pin(assets::load<shader_t>(SCENE_SHADER_PATH));
            top_shader = assets::pin(assets::load<shader_t>(TOP_SHADER_PATH));
            register_renderer_feature("Overlay"_h, FEATURE_ORDER_OVERLAY, &build);
        }

        void shutdown()
        {
            assets::unpin(scene_shader);
            assets::unpin(top_shader);
            pending.clear();
            pending_lines.clear();
            scene_draws.clear();
            top_draws.clear();
        }
    } // namespace overlay
} // namespace tau::renderer
