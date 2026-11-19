#include "asset_editors.h"

#include "imgui.h"
#include "tau-editor/asset_files.h"
#include "tau-editor/commands.h"
#include "tau-editor/editor_context.h"
#include "tau-editor/history.h"
#include "tau-editor/prefab_stage.h"
#include "tau/assets/material.h"
#include "tau/assets/mesh.h"
#include "tau/assets/scene.h"
#include "tau/assets/shader.h"
#include "tau/assets/texture.h"
#include "tau/components/renderer.h"
#include "tau/components/tag.h"
#include "tau/components/transform.h"
#include "tau/guid.h"
#include "tau/rendering/renderer_constants.h"
#include "tau/systems/guid.h"
#include "tau/systems/transform.h"
#include "tau/world.h"

#include <algorithm>
#include <filesystem>
#include <vector>

namespace tau::editor::asset_editors
{
    namespace
    {
        std::vector<asset_editor_t> g_editors;

        // texture

        void texture_inspector(world_t&, editor_context_t&, const std::string&, asset_handle_t handle)
        {
            const texture_t* tex = assets::get(asset_t<texture_t>{handle});
            if (!tex) { return; }

            ImGui::Text("%d x %d", tex->width, tex->height);
            const f32 width = std::min(ImGui::GetContentRegionAvail().x, 512.0f);
            if (tex->width > 0)
            {
                ImGui::Image((ImTextureID)(intptr_t)tex->bindless_id,
                             ImVec2(width, width * static_cast<f32>(tex->height) / static_cast<f32>(tex->width)));
            }
        }

        asset_handle_t texture_preview_load(const std::string& vfs) { return assets::load_async<texture_t>(vfs); }

        u32_t texture_preview_image(asset_handle_t handle)
        {
            const texture_t* tex = assets::get(asset_t<texture_t>{handle});
            return tex && tex->width > 0 ? tex->bindless_id : renderer::BINDLESS_NULL_HANDLE;
        }

        // mesh

        constexpr const char* DEFAULT_MATERIAL = "engine://assets/materials/default.mat";

        void place_mesh(world_t& world, editor_context_t& ctx, const std::string& vfs, const vec3_t& at)
        {
            const u64_t guid = guid::generate();
            const history::target_t target = history::entity_target(guid);
            const std::string name = std::filesystem::path(vfs).stem().string();
            history::begin(world, "Create " + name, {&target, 1});

            const ecs::entity_t entity = world.registry.create();
            guid_system::assign(world.registry, entity, guid);
            world.registry.emplace<tag_t>(entity, name);
            world.registry.emplace<transform_t>(entity).local_position = at;

            mesh_renderer_t& renderer = world.registry.emplace<mesh_renderer_t>(entity);
            renderer.mesh = assets::load<mesh_t>(vfs);
            renderer.material = assets::load<material_t>(DEFAULT_MATERIAL);

            if (const ecs::entity_t root = prefab_stage::root(world); root != ecs::NULL_ENTITY)
            {
                transform_system::set_parent_keep_world(world.registry, entity, root, ecs::NULL_ENTITY);
            }

            ctx.selection.set(guid);
            history::end(world);
        }

        // scene: only the project's open in the editor

        void open_scene(world_t&, editor_context_t&, const std::string& vfs)
        {
            if (asset_files::rel_of(vfs).empty()) { return; }
            commands::push(commands::command_e::LOAD_SCENE, asset_files::source_of(vfs).string());
        }

        void scene_inspector(world_t& world, editor_context_t& ctx, const std::string& vfs, asset_handle_t)
        {
            if (!asset_files::rel_of(vfs).empty() && ImGui::Button("Open Scene")) { open_scene(world, ctx, vfs); }
        }
    } // namespace

    void add(const asset_editor_t& editor)
    {
        const auto it = std::find_if(g_editors.begin(), g_editors.end(),
                                     [&](const asset_editor_t& e) { return e.type_id == editor.type_id; });
        if (it != g_editors.end()) { *it = editor; }
        else
        {
            g_editors.push_back(editor);
        }
    }

    const asset_editor_t* of(u64_t type_id)
    {
        const auto it = std::find_if(g_editors.begin(), g_editors.end(),
                                     [&](const asset_editor_t& e) { return e.type_id == type_id; });
        return it != g_editors.end() ? &*it : nullptr;
    }

    const asset_editor_t* of_path(std::string_view path)
    {
        const asset_type_t* type = assets::type_by_path(path);
        return type ? of(type->id) : nullptr;
    }

    std::span<const asset_editor_t> all() { return g_editors; }

    void register_builtin(editor_context_t&)
    {
        add({
            .type_id = get_type_id<texture_t>(),
            .color = IM_COL32(90, 150, 210, 255),
            .load_when_inspected = true,
            .draw_inspector = &texture_inspector,
            .preview_load = &texture_preview_load,
            .preview_image = &texture_preview_image,
        });
        add({
            .type_id = get_type_id<mesh_t>(),
            .color = IM_COL32(120, 190, 110, 255),
            .drop_in_viewport = &place_mesh,
        });
        add({
            .type_id = get_type_id<shader_t>(),
            .color = IM_COL32(200, 200, 90, 255),
        });
        add({
            .type_id = get_type_id<scene_t>(),
            .color = IM_COL32(170, 110, 200, 255),
            .create_label = "Scene",
            .create = &asset_files::create_scene,
            .open = &open_scene,
            .draw_inspector = &scene_inspector,
        });
    }
} // namespace tau::editor::asset_editors
