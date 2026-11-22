#include "asset_editors.h"

#include "imgui.h"
#include "tau-editor/asset_files.h"
#include "tau-editor/commands.h"
#include "tau-editor/editor_context.h"
#include "tau-editor/history.h"
#include "tau-editor/panels/inspector.h"
#include "tau-editor/prefab_edit.h"
#include "tau-editor/prefab_stage.h"
#include "tau/assets/clip.h"
#include "tau/assets/material.h"
#include "tau/assets/mesh.h"
#include "tau/assets/model.h"
#include "tau/assets/prefab.h"
#include "tau/assets/scene.h"
#include "tau/assets/shader.h"
#include "tau/assets/skeleton.h"
#include "tau/assets/texture.h"
#include "tau/components/renderer.h"
#include "tau/components/tag.h"
#include "tau/components/transform.h"
#include "tau/guid.h"
#include "tau/rhi/renderer_constants.h"
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

        void place_mesh(world_t& world, editor_context_t& ctx, const std::string& vfs, const vec3& at)
        {
            const u64_t guid = guid::generate();
            const history::target_t target = history::entity_target(guid);
            const std::string name = std::filesystem::path(vfs).stem().string();
            history::begin(world, "Create " + name, {&target, 1});

            const ecs::entity_t entity = world.registry.create();
            guid_system::assign(world.registry, entity, guid);
            world.registry.emplace<tag_t>(entity, name);
            world.registry.emplace<transform_t>(entity).local_position = at;

            world.registry.emplace<mesh_renderer_t>(entity).mesh = assets::load<mesh_t>(vfs);

            if (const ecs::entity_t root = prefab_stage::root(world); root != ecs::NULL_ENTITY)
            {
                transform_system::set_parent_keep_world(world.registry, entity, root, ecs::NULL_ENTITY);
            }

            ctx.selection.set(guid);
            history::end(world);
        }

        std::vector<std::string> material_slots(const reflection::any_t& instance)
        {
            const mesh_renderer_t* renderer = instance.try_cast<const mesh_renderer_t>();
            const mesh_t* mesh = renderer ? assets::get(renderer->mesh) : nullptr;
            if (!mesh) { return {}; }

            std::vector<std::string> labels;
            for (std::size_t i = 0; i < mesh->submeshes.size(); i++)
            {
                const std::string& name = mesh->submeshes[i].material_name;
                labels.push_back(name.empty() ? "Submesh " + std::to_string(i) : name);
            }
            return labels;
        }

        std::string model_prefab(const std::string& vfs)
        {
            const model_t* model = assets::get(assets::load<model_t>(vfs));
            if (!model) { return {}; }
            for (const model_sub_asset_t& sub : model->sub_assets)
            {
                if (sub.type_id == get_type_id<prefab_t>()) { return vfs + "/" + sub.name; }
            }
            return {};
        }

        void place_model(world_t& world, editor_context_t& ctx, const std::string& vfs, const vec3& at)
        {
            const std::string prefab = model_prefab(vfs);
            if (!prefab.empty()) { prefab_edit::instantiate(world, ctx, prefab, ecs::NULL_ENTITY, &at); }
        }

        bool reload_model(world_t& world, const std::string& vfs)
        {
            const bool ok = assets::reload(vfs);

            const auto order = [](const std::string& path)
            {
                const asset_type_t* type = assets::type_by_path(path);
                const u64_t id = type ? type->id : 0;
                if (id == get_type_id<texture_t>()) { return 0; }
                if (id == get_type_id<material_t>()) { return 1; }
                if (id == get_type_id<prefab_t>()) { return 3; }
                return 2;
            };

            std::vector<std::string> parts;
            for (const auto& [path, guid] : assets::locations(vfs + "/"))
            {
                if (assets::find(path).is_valid()) { parts.push_back(path); }
            }
            std::stable_sort(parts.begin(), parts.end(),
                             [&](const std::string& a, const std::string& b) { return order(a) < order(b); });

            for (const std::string& part : parts)
            {
                const asset_editor_t* editor = of_path(part);
                if (editor && editor->reload) { editor->reload(world, part); }
                else
                {
                    assets::reload(part);
                }
            }
            return ok;
        }

        void drop_model_on(world_t& world, editor_context_t& ctx, const std::string& vfs, ecs::entity_t parent)
        {
            const std::string prefab = model_prefab(vfs);
            if (!prefab.empty()) { prefab_edit::instantiate(world, ctx, prefab, parent, nullptr); }
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
        panels::add_slot_labels(hash_string(type_name<mesh_renderer_t>()), hash_string("materials"), &material_slots);

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
            .type_id = get_type_id<model_t>(),
            .color = IM_COL32(150, 200, 120, 255),
            .drop_in_viewport = &place_model,
            .drop_on_entity = &drop_model_on,
            .reload = &reload_model,
        });
        add({
            .type_id = get_type_id<skeleton_t>(),
            .color = IM_COL32(200, 160, 220, 255),
        });
        add({
            .type_id = get_type_id<clip_t>(),
            .color = IM_COL32(220, 180, 120, 255),
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
