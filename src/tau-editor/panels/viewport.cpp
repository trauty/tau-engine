#include "viewport.h"

#include "imgui.h"
#include "tau-editor/handle_math.h"
#include "tau-editor/history.h"
#include "tau-editor/panels/asset_browser.h"
#include "tau-editor/prefab_edit.h"
#include "tau-editor/prefab_stage.h"
#include "tau-editor/viewport_input.h"
#include "tau/asset.h"
#include "tau/assets/material.h"
#include "tau/assets/mesh.h"
#include "tau/assets/prefab.h"
#include "tau/components/renderer.h"
#include "tau/components/tag.h"
#include "tau/components/transform.h"
#include "tau/engine.h"
#include "tau/guid.h"
#include "tau/input.h"
#include "tau/rendering/renderer.h"
#include "tau/systems/guid.h"
#include "tau/systems/transform.h"

#include <filesystem>
#include <string>

namespace tau::editor::panels
{
    namespace
    {
        constexpr const char* DEFAULT_MATERIAL = "engine://assets/materials/default.mat";
        constexpr f32 MAX_GROUND_DISTANCE = 500.0f;

        vec3_t drop_point(const editor_context_t& ctx, vec2_t pixel)
        {
            const viewport_input::ray_t ray = viewport_input::ray_through(ctx, pixel);
            vec3_t at = ray.origin + ray.direction * 5.0f;

            vec3_t hit;
            if (handle_math::ray_plane(ray.origin, ray.direction, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, hit))
            {
                const vec3_t to_hit = hit - ray.origin;
                if (handle_math::dot(to_hit, to_hit) < MAX_GROUND_DISTANCE * MAX_GROUND_DISTANCE) { at = hit; }
            }
            return at;
        }

        void place_mesh(world_t& world, editor_context_t& ctx, const std::string& path, vec2_t pixel)
        {
            const vec3_t at = drop_point(ctx, pixel);

            const u64_t guid = guid::generate();
            const history::target_t target = history::entity_target(guid);
            const std::string name = std::filesystem::path(path).stem().string();
            history::begin(world, "Create " + name, {&target, 1});

            const ecs::entity_t entity = world.registry.create();
            guid_system::assign(world.registry, entity, guid);
            world.registry.emplace<tag_t>(entity, name);
            world.registry.emplace<transform_t>(entity).local_position = at;

            mesh_renderer_t& renderer = world.registry.emplace<mesh_renderer_t>(entity);
            renderer.mesh = assets::load<mesh_t>(path);
            renderer.material = assets::load<material_t>(DEFAULT_MATERIAL);

            if (const ecs::entity_t root = prefab_stage::root(world); root != ecs::NULL_ENTITY)
            {
                transform_system::set_parent_keep_world(world.registry, entity, root, ecs::NULL_ENTITY);
            }

            ctx.selection.set(guid);
            history::end(world);
        }
    } // namespace

    void draw_viewport(world_t& world, editor_context_t& ctx)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0, 0});
        if (ImGui::Begin("Scene Viewport"))
        {
            ctx.is_viewport_hovered = ImGui::IsWindowHovered();
            ctx.is_viewport_focused = ImGui::IsWindowFocused();

            prefab_stage::draw_bar(world, ctx);

            if (ImGui::GetMousePos().y < ImGui::GetCursorScreenPos().y) { ctx.is_viewport_hovered = false; }

            ImVec2 viewport_size = ImGui::GetContentRegionAvail();
            ImVec2 viewport_pos = ImGui::GetCursorScreenPos();

            if (viewport_size.x > 0.0f && viewport_size.y > 0.0f)
            {
                ctx.viewport_width = (u32_t)viewport_size.x;
                ctx.viewport_height = (u32_t)viewport_size.y;
            }

            ctx.viewport_screen_pos = {viewport_pos.x, viewport_pos.y};
            ctx.viewport_screen_size = {viewport_size.x, viewport_size.y};

            tau::input::set_viewport_offset(viewport_pos.x, viewport_pos.y);
            tau::input::set_viewport_size(viewport_size.x, viewport_size.y);

            u32_t tex_id = tau::renderer::get_target_texture_id("EditorViewport"_h);
            if (tex_id != tau::renderer::BINDLESS_NULL_HANDLE)
            {
                ImGui::Image((ImTextureID)(intptr_t)(tex_id), viewport_size);

                if (ctx.cur_mode == editor_mode_e::EDIT && ImGui::BeginDragDropTarget())
                {
                    const ImGuiPayload* peek = ImGui::GetDragDropPayload();
                    const char* path =
                        peek && peek->IsDataType(ASSET_PAYLOAD) ? static_cast<const char*>(peek->Data) : "";
                    const bool mesh = assets::path_is<mesh_t>(path);
                    const bool prefab = assets::path_is<prefab_t>(path) && !prefab_stage::is_open();
                    if (mesh || prefab)
                    {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(ASSET_PAYLOAD))
                        {
                            const ImVec2 mouse = ImGui::GetMousePos();
                            const vec2_t pixel = {mouse.x - viewport_pos.x, mouse.y - viewport_pos.y};
                            const std::string dropped = static_cast<const char*>(payload->Data);
                            if (mesh) { place_mesh(world, ctx, dropped, pixel); }
                            else
                            {
                                const vec3_t at = drop_point(ctx, pixel);
                                prefab_edit::instantiate(world, ctx, dropped, ecs::NULL_ENTITY, &at);
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
            }
        }

        ImGui::End();
        ImGui::PopStyleVar();
    }
} // namespace tau::editor::panels