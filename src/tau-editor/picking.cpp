#include "picking.h"

#include "tau-editor/editor_context.h"
#include "tau/asset.h"
#include "tau/assets/shader.h"
#include "tau/ecs.h"
#include "tau/engine.h"
#include "tau/hash.h"
#include "tau/render/frame.h"
#include "tau/rhi/frame_upload.h"
#include "tau/rhi/readback.h"
#include "tau/rhi/renderer.h"
#include "tau/rhi/renderer_constants.h"
#include "tau/rhi/rendergraph.h"
#include "tau/systems/guid.h"
#include "tau/world.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace tau::editor::picking
{
    namespace
    {
        constexpr const char* IDS_SHADER_PATH = "engine://assets/shaders/editor_ids.slang";
        constexpr const char* SELECTION_SHADER_PATH = "engine://assets/shaders/editor_selection.slang";
        constexpr const char* OUTLINE_SHADER_PATH = "engine://assets/shaders/editor_outline.slang";
        constexpr u32_t VIEW = "PrimaryView"_h;
        constexpr i32 FEATURE_ORDER_PICKING = 500;

        struct pick_t
        {
            renderer::readback_ticket_t ticket;
            bool additive;
        };

        editor_context_t* g_ctx = nullptr;
        asset_t<shader_t> ids_shader;
        asset_t<shader_t> selection_shader;
        asset_t<shader_t> outline_shader;
        std::vector<pick_t> picks;

        std::vector<u32_t> selected_ids;
        VkDeviceAddress selected_address = 0;

        void build_picking(renderer::rendergraph_t& graph, ecs::registry_t& reg)
        {
            selected_ids.clear();
            if (!g_ctx || g_ctx->cur_mode != editor_mode_e::EDIT) { return; }

            for (const u64_t guid : g_ctx->selection.guids)
            {
                const ecs::entity_t entity = guid_system::find(reg, guid);
                if (entity != ecs::NULL_ENTITY)
                {
                    selected_ids.push_back(static_cast<u32_t>(entt::to_integral(entity)) + 1);
                }
            }

            const VkExtent2D extent = renderer::ctx.render_extent;

            if (!selected_ids.empty())
            {
                selected_address =
                    renderer::upload_frame_data(selected_ids.data(), selected_ids.size() * sizeof(u32_t));

                const renderer::image_desc_t mask_desc = {
                    .width = extent.width,
                    .height = extent.height,
                    .format = VK_FORMAT_R8_UNORM,
                    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                    .color_clear = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}},
                };

                renderer::rg_pass_t& pass = graph.add_pass("EditorSelectionMask"_h, "EditorSelectionMask");
                pass.color_writes = {graph.create_image("EditorSelectionMask"_h, "EditorSelectionMask", mask_desc)};
                pass.execute_callback = [](VkCommandBuffer cmd, ecs::registry_t&)
                {
                    shader_t* shader = assets::get(selection_shader);
                    if (!shader) { return; }

                    const gpu::PushConstants pc = {0, selected_address, static_cast<u32_t>(selected_ids.size()), 0};
                    renderer::draw_view_objects(cmd, VIEW, shader->get_pipeline(pipeline_variant_e::FORWARD),
                                                shader->pipeline_layout, pc);
                };
            }

            if (!g_ctx->click_pending) { return; }
            g_ctx->click_pending = false;

            const renderer::image_desc_t ids_desc = {
                .width = extent.width,
                .height = extent.height,
                .format = VK_FORMAT_R32_UINT,
                .usage =
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                .color_clear = {.uint32 = {0, 0, 0, 0}},
            };
            const renderer::image_desc_t depth_desc = {
                .width = extent.width,
                .height = extent.height,
                .format = renderer::ctx.swapchain.depth_format,
                .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
                .depth_clear = 0.0f,
            };

            const renderer::rg_resource_id ids = graph.create_image("EditorEntityIds"_h, "EditorEntityIds", ids_desc);

            renderer::rg_pass_t& pass = graph.add_pass("EditorEntityIds"_h, "EditorEntityIds");
            pass.color_writes = {ids};
            pass.depth_stencil = graph.create_image("EditorIdDepth"_h, "EditorIdDepth", depth_desc);
            pass.execute_callback = [](VkCommandBuffer cmd, ecs::registry_t&)
            {
                shader_t* shader = assets::get(ids_shader);
                if (!shader) { return; }

                renderer::draw_view_objects(cmd, VIEW, shader->get_pipeline(pipeline_variant_e::FORWARD),
                                            shader->pipeline_layout, {});
            };

            const i32 x = std::clamp(static_cast<i32>(g_ctx->click_pos.x), 0, static_cast<i32>(extent.width) - 1);
            const i32 y = std::clamp(static_cast<i32>(g_ctx->click_pos.y), 0, static_cast<i32>(extent.height) - 1);
            const VkRect2D rect = {
                {x, y},
                {1, 1}
            };
            const bool additive = g_ctx->click_additive;

            renderer::rg_pass_t& pick = graph.add_pass("EditorPick"_h, "EditorPick");
            pick.texture_reads = {ids};
            pick.execute_callback = [&graph, ids, rect, additive](VkCommandBuffer cmd, ecs::registry_t&)
            {
                const renderer::readback_ticket_t ticket =
                    renderer::record_readback(cmd, graph.get_image(ids), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                              VK_IMAGE_ASPECT_COLOR_BIT, sizeof(u32_t), rect);
                if (ticket != renderer::READBACK_NONE) { picks.push_back({ticket, additive}); }
            };
        }

        void record_outline(const renderer::graph::record_ctx_t& rc, const void* data)
        {
            const renderer::graph::image_h mask = *static_cast<const renderer::graph::image_h*>(data);
            shader_t* shader = assets::get(outline_shader);
            const VkPipeline pipeline = shader ? shader->get_pipeline(pipeline_variant_e::FORWARD) : VK_NULL_HANDLE;
            if (pipeline == VK_NULL_HANDLE) { return; }

            vkCmdBindPipeline(rc.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            if (!renderer::bind_view_descriptors(rc.cmd, shader->pipeline_layout, VIEW)) { return; }

            const gpu::PushConstants pc = {0, 0, 0, renderer::graph::sampled_id(*rc.graph, mask)};
            vkCmdPushConstants(rc.cmd, shader->pipeline_layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(gpu::PushConstants),
                               &pc);
            vkCmdDraw(rc.cmd, 3, 1, 0, 0);
        }

        void build_outline(renderer::graph::graph_t& g, const renderer::frame::targets_t& t, ecs::registry_t&)
        {
            if (selected_ids.empty() || !t.final_output.valid()) { return; }

            const renderer::graph::image_h mask = renderer::graph::find_image(g, "EditorSelectionMask");
            if (!mask.valid()) { return; }

            const renderer::graph::pass_h pass = renderer::graph::add_pass(g, "EditorOutline", &record_outline, mask);
            renderer::graph::use(g, pass, mask, renderer::graph::access_e::SAMPLED);
            renderer::graph::use(g, pass, t.final_output, renderer::graph::access_e::COLOR_READ_WRITE);
        }
    } // namespace

    void init(editor_context_t& ctx)
    {
        g_ctx = &ctx;
        ids_shader = assets::pin(assets::load<shader_t>(IDS_SHADER_PATH));
        selection_shader = assets::pin(assets::load<shader_t>(SELECTION_SHADER_PATH));
        outline_shader = assets::pin(assets::load<shader_t>(OUTLINE_SHADER_PATH));

        renderer::register_renderer_feature("EditorPicking"_h, FEATURE_ORDER_PICKING, &build_picking);
        renderer::frame::register_producer("EditorOutline"_h, renderer::FEATURE_ORDER_AFTER_POST, &build_outline);
    }

    void update(world_t& world, editor_context_t& ctx)
    {
        for (auto it = picks.begin(); it != picks.end();)
        {
            std::vector<u8_t> bytes;
            if (!renderer::fetch_readback(it->ticket, bytes))
            {
                ++it;
                continue;
            }

            u32_t id = 0;
            std::memcpy(&id, bytes.data(), sizeof(id));

            const ecs::entity_t entity = id == 0 ? ecs::NULL_ENTITY : static_cast<ecs::entity_t>(id - 1);
            const u64_t guid = world.registry.valid(entity) ? guid_system::ensure(world.registry, entity) : 0;

            if (!it->additive) { ctx.selection.set(guid); }
            else if (guid != 0) { ctx.selection.toggle(guid); }
            ctx.scroll_to_selection = guid != 0;

            it = picks.erase(it);
        }
    }
} // namespace tau::editor::picking
