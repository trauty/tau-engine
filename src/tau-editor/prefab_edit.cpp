#include "prefab_edit.h"

#include "tau-editor/asset_cook.h"
#include "tau-editor/asset_files.h"
#include "tau-editor/editor_context.h"
#include "tau-editor/history.h"
#include "tau-editor/prefab_stage.h"
#include "tau/asset.h"
#include "tau/assets/prefab.h"
#include "tau/components/prefab.h"
#include "tau/components/tag.h"
#include "tau/components/transform.h"
#include "tau/engine.h"
#include "tau/guid.h"
#include "tau/hash.h"
#include "tau/log.h"
#include "tau/prefab.h"
#include "tau/serialization.h"
#include "tau/systems/guid.h"
#include "tau/world.h"

#include <filesystem>
#include <fstream>
#include <json/json.hpp>
#include <vector>

namespace tau::editor::prefab_edit
{
    namespace
    {
        constexpr const char* TRANSFORM_KEY = "transform_t";

        editor_context_t* g_ctx = nullptr;

        u64_t guid_of(const nlohmann::json& value)
        { return value.is_string() ? guid::from_string(value.get<std::string>()) : 0; }

        nlohmann::json guid_json(u64_t guid)
        { return guid == 0 ? nlohmann::json(nullptr) : nlohmann::json(guid::to_string(guid)); }

        std::string name_of(const world_t& world, ecs::entity_t entity)
        {
            const tag_t* tag = world.registry.try_get<tag_t>(entity);
            return tag && !tag->name.empty() ? tag->name : std::string("Prefab");
        }

        prefab_t* prefab_of(world_t& world, ecs::entity_t root)
        {
            const prefab_instance_t* instance = world.registry.try_get<prefab_instance_t>(root);
            return instance ? assets::get(instance->prefab) : nullptr;
        }

        std::vector<ecs::entity_t> parts_of(world_t& world, u64_t instance)
        {
            std::vector<ecs::entity_t> parts;
            for (auto [entity, part] : world.registry.view<prefab_part_t>().each())
            {
                if (part.instance == instance) { parts.push_back(entity); }
            }
            return parts;
        }

        std::vector<nlohmann::json> states_of(world_t& world, const std::vector<ecs::entity_t>& entities)
        {
            std::vector<nlohmann::json> states;
            for (const ecs::entity_t entity : entities)
            {
                states.push_back(serialization::serialize_entity(world, entity));
            }
            return states;
        }

        void append_subtree(const world_t& world, ecs::entity_t root, std::vector<ecs::entity_t>& out)
        {
            out.push_back(root);
            const transform_t* transform = world.registry.try_get<transform_t>(root);
            if (!transform) { return; }

            for (ecs::entity_t child = transform->get_first_child(); child != ecs::NULL_ENTITY;
                 child = world.registry.get<transform_t>(child).get_next_sibling())
            {
                append_subtree(world, child, out);
            }
        }

        void strip_prefab(nlohmann::json& state)
        {
            for (const char* key : {"prefab_part", "prefab", "removed", "unresolved"}) { state.erase(key); }
        }

        // states -> prefab entities, own guids, refs follow
        nlohmann::json to_prefab_entities(world_t& world, const std::vector<ecs::entity_t>& entities,
                                          const std::unordered_map<u64_t, u64_t>& to_source)
        {
            auto source_of = [&](const nlohmann::json& value) -> nlohmann::json
            {
                const auto it = to_source.find(guid_of(value));
                return it != to_source.end() ? guid_json(it->second) : nlohmann::json(nullptr);
            };

            nlohmann::json out = nlohmann::json::array();
            for (std::size_t i = 0; i < entities.size(); i++)
            {
                nlohmann::json state = serialization::serialize_entity(world, entities[i]);
                strip_prefab(state);
                state["id"] = source_of(state["id"]);
                state["parent"] = i == 0 ? nlohmann::json(nullptr) : source_of(state.value("parent", nlohmann::json{}));
                state["next"] = i == 0 ? nlohmann::json(nullptr) : source_of(state.value("next", nlohmann::json{}));
                serialization::remap_entity_refs(state["components"], to_source);
                out.push_back(std::move(state));
            }
            return out;
        }

        std::vector<history::target_t> entity_targets(const std::vector<u64_t>& guids)
        {
            std::vector<history::target_t> targets;
            for (const u64_t guid : guids) { targets.push_back(history::entity_target(guid)); }
            return targets;
        }

        // apply states, destroy entities in old that are gone
        void apply_states(world_t& world, const std::vector<nlohmann::json>& states, const std::vector<u64_t>& old)
        {
            std::vector<serialization::entity_state_t> batch;
            std::unordered_set<u64_t> kept;
            for (const nlohmann::json& state : states)
            {
                batch.push_back({guid_of(state["id"]), &state});
                kept.insert(guid_of(state["id"]));
            }
            for (const u64_t guid : old)
            {
                if (!kept.contains(guid)) { batch.push_back({guid, nullptr}); }
            }
            serialization::apply_entities(world, batch);
        }

        struct resync_t
        {
            std::vector<u64_t> old_parts;
            std::vector<nlohmann::json> states;
        };

        // instance keeps its overrides vs the old prefab, on top of the new
        resync_t resync(world_t& world, ecs::entity_t root, const nlohmann::json& old_data,
                        const nlohmann::json& new_data)
        {
            resync_t out;
            const std::vector<ecs::entity_t> parts = parts_of(world, guid_system::of(world.registry, root));
            for (const ecs::entity_t part : parts) { out.old_parts.push_back(guid_system::of(world.registry, part)); }

            const nlohmann::json entry = prefab::collapse_instance(&old_data, states_of(world, parts));
            out.states = prefab::expand_instance(entry, &new_data);
            return out;
        }

        std::vector<ecs::entity_t> instances_of(world_t& world, asset_handle_t prefab)
        {
            std::vector<ecs::entity_t> roots;
            for (auto [entity, instance] : world.registry.view<prefab_instance_t>().each())
            {
                if (instance.prefab == prefab) { roots.push_back(entity); }
            }
            return roots;
        }

        // instances other than skip follow new_data, their guids go to the step
        std::vector<resync_t> resync_instances(world_t& world, asset_handle_t prefab, ecs::entity_t skip,
                                               const nlohmann::json& old_data, const nlohmann::json& new_data,
                                               std::vector<u64_t>& guids)
        {
            std::vector<resync_t> out;
            for (const ecs::entity_t other : instances_of(world, prefab))
            {
                if (other == skip) { continue; }

                out.push_back(resync(world, other, old_data, new_data));
                guids.insert(guids.end(), out.back().old_parts.begin(), out.back().old_parts.end());
                for (const nlohmann::json& state : out.back().states) { guids.push_back(guid_of(state["id"])); }
            }
            return out;
        }

        void zero_root_position(nlohmann::json& data)
        {
            nlohmann::json& entities = data["entities"];
            if (entities.empty()) { return; }

            nlohmann::json& transform = entities[0]["components"][TRANSFORM_KEY];
            if (transform.is_object() && transform.contains("local_position"))
            {
                transform["local_position"]["v"] = {0.0, 0.0, 0.0};
            }
        }

        bool blocked_in_stage(const char* what)
        {
            if (!prefab_stage::is_open()) { return false; }
            TAU_LOG_WARN("PREFAB", "{} is not possible while a prefab is open, prefabs cannot hold prefabs yet", what);
            return true;
        }

        std::filesystem::path source_of_vfs(const std::string& vfs)
        {
            constexpr std::string_view game = "game://assets/";
            constexpr std::string_view engine = "engine://assets/";
            if (vfs.starts_with(game)) { return asset_files::source_path(vfs.substr(game.size())); }
            if (vfs.starts_with(engine)) { return asset_cook::engine_assets_dir() / vfs.substr(engine.size()); }
            return {};
        }

        std::vector<u8_t> capture(asset_handle_t handle)
        {
            const prefab_t* prefab = assets::get(asset_t<prefab_t>{handle});
            return prefab ? nlohmann::json::to_cbor(prefab->data) : std::vector<u8_t>{};
        }

        bool apply_data(asset_handle_t handle, std::span<const u8_t> state)
        {
            prefab_t* prefab = assets::get(asset_t<prefab_t>{handle});
            if (!prefab) { return false; }

            prefab->data = nlohmann::json::from_cbor(state, true, false);
            return prefab->data.is_object();
        }

        bool write(asset_handle_t handle, std::filesystem::path& out_source)
        {
            const prefab_t* prefab = assets::get(asset_t<prefab_t>{handle});
            if (!prefab)
            {
                TAU_LOG_ERROR("PREFAB", "Cannot save {}, it is not loaded", assets::path_of(handle));
                return false;
            }
            return write_data(asset_t<prefab_t>{handle}, prefab->data, out_source);
        }
    } // namespace

    bool write_data(asset_t<prefab_t> prefab, const nlohmann::json& data, std::filesystem::path& out_source)
    {
        const std::string vfs = assets::path_of(prefab);
        out_source = source_of_vfs(vfs);
        if (out_source.empty())
        {
            TAU_LOG_ERROR("PREFAB", "Cannot save {}", vfs);
            return false;
        }

        std::ofstream file(out_source);
        if (!file.is_open())
        {
            TAU_LOG_ERROR("PREFAB", "Cannot write {}", out_source.string());
            return false;
        }
        file << data.dump(4) << '\n';
        TAU_LOG_INFO("PREFAB", "Prefab saved to {}", out_source.string());
        return true;
    }

    nlohmann::json data_of(world_t& world, ecs::entity_t root, const nlohmann::json& prefab_guid)
    {
        std::vector<ecs::entity_t> entities;
        append_subtree(world, root, entities);
        for (const ecs::entity_t entity : world.registry.view<ecs::entity_t>())
        {
            if (entity != root && !world.registry.all_of<transform_t>(entity)) { entities.push_back(entity); }
        }

        // guids are the prefab's own already
        std::unordered_map<u64_t, u64_t> same;
        for (const ecs::entity_t entity : entities)
        {
            const u64_t guid = guid_system::ensure(world.registry, entity);
            same[guid] = guid;
        }

        return {
            {"guid",     prefab_guid                              },
            {"entities", to_prefab_entities(world, entities, same)},
        };
    }

    void replace_data(world_t& world, editor_context_t& ctx, asset_t<prefab_t> handle, nlohmann::json new_data,
                      std::string_view label)
    {
        prefab_t* prefab = assets::get(handle);
        if (!prefab) { return; }

        std::vector<u64_t> guids;
        const std::vector<resync_t> others =
            resync_instances(world, handle, ecs::NULL_ENTITY, prefab->data, new_data, guids);

        std::vector<history::target_t> targets = entity_targets(guids);
        targets.push_back(history::asset_target(get_type_id<prefab_t>(), handle));
        history::begin(world, label, targets);

        prefab->data = std::move(new_data);
        for (const resync_t& other : others) { apply_states(world, other.states, other.old_parts); }

        history::end(world);
    }

    void init(editor_context_t& ctx)
    {
        g_ctx = &ctx;
        history::reg_editable(get_type_id<prefab_t>(), &capture, &apply_data, &write);
    }

    std::string create(world_t& world, editor_context_t& ctx, ecs::entity_t root, const std::string& dir)
    {
        ecs::registry_t& reg = world.registry;
        if (!reg.valid(root) || blocked_in_stage("Making a prefab")) { return {}; }

        std::vector<ecs::entity_t> entities;
        append_subtree(world, root, entities);

        std::unordered_map<u64_t, u64_t> to_source;
        std::vector<u64_t> guids;
        for (const ecs::entity_t entity : entities)
        {
            const u64_t guid = guid_system::ensure(reg, entity);
            to_source[guid] = guid::generate();
            guids.push_back(guid);
        }

        const std::string name = name_of(world, root);
        nlohmann::json data = {
            {"guid", guid::to_string(guid::generate())},
            {"entities", to_prefab_entities(world, entities, to_source)},
        };
        // placement belongs to the instance, the prefab opens and spawns at the origin
        zero_root_position(data);
        const std::string rel = asset_files::unique_path(dir, name, ".prefab");

        const u64_t file_entry = asset_files::reserve_entry();
        std::vector<history::target_t> targets = entity_targets(guids);
        targets.push_back({history::PROJECT_FILES, file_entry});
        history::begin(world, "Create Prefab " + name, targets);

        if (asset_files::write_new_file(file_entry, rel, data.dump(4) + "\n"))
        {
            const asset_t<prefab_t> handle = assets::load<prefab_t>(asset_files::vfs_path(rel));
            const u64_t instance = guid_system::of(reg, root);
            for (const ecs::entity_t entity : entities)
            {
                reg.emplace_or_replace<prefab_part_t>(entity,
                                                      prefab_part_t{instance, to_source[guid_system::of(reg, entity)]});
                reg.remove<prefab_instance_t>(entity);
            }
            reg.emplace_or_replace<prefab_instance_t>(root, prefab_instance_t{handle, {}, {}});
        }

        history::end(world);
        return rel;
    }

    ecs::entity_t instantiate(world_t& world, editor_context_t& ctx, const std::string& prefab_path,
                              ecs::entity_t parent, const vec3_t* position)
    {
        if (blocked_in_stage("Placing a prefab")) { return ecs::NULL_ENTITY; }

        const asset_t<prefab_t> handle = assets::load<prefab_t>(prefab_path);
        const prefab_t* prefab = assets::get(handle);
        if (!prefab)
        {
            TAU_LOG_ERROR("PREFAB", "Cannot instance {}, it did not load", prefab_path);
            return ecs::NULL_ENTITY;
        }

        nlohmann::json entry = prefab::new_instance(prefab->data, serialization::asset_ref(handle));
        if (parent != ecs::NULL_ENTITY) { entry["parent"] = guid_json(guid_system::ensure(world.registry, parent)); }
        const std::vector<nlohmann::json> states = prefab::expand_instance(entry, &prefab->data);

        std::vector<u64_t> guids;
        for (const nlohmann::json& state : states) { guids.push_back(guid_of(state["id"])); }

        const std::string name = std::filesystem::path(prefab_path).stem().string();
        history::begin(world, "Create " + name, entity_targets(guids));

        apply_states(world, states, {});
        const ecs::entity_t root = guid_system::find(world.registry, guid_of(entry["id"]));
        if (position && root != ecs::NULL_ENTITY && world.registry.all_of<transform_t>(root))
        {
            world.registry.get<transform_t>(root).local_position = *position;
        }
        if (root != ecs::NULL_ENTITY) { ctx.selection.set(guid_of(entry["id"])); }

        history::end(world);
        return root;
    }

    ecs::entity_t instance_root(world_t& world, ecs::entity_t entity)
    {
        const prefab_part_t* part =
            world.registry.valid(entity) ? world.registry.try_get<prefab_part_t>(entity) : nullptr;
        if (!part) { return ecs::NULL_ENTITY; }

        const ecs::entity_t root = guid_system::find(world.registry, part->instance);
        return root != ecs::NULL_ENTITY && world.registry.all_of<prefab_instance_t>(root) ? root : ecs::NULL_ENTITY;
    }

    std::string prefab_path(world_t& world, ecs::entity_t root)
    {
        const prefab_instance_t* instance = world.registry.try_get<prefab_instance_t>(root);
        return instance ? assets::path_of(instance->prefab) : std::string{};
    }

    void revert(world_t& world, editor_context_t& ctx, ecs::entity_t root)
    {
        const prefab_t* prefab = prefab_of(world, root);
        if (!prefab) { return; }

        const std::vector<ecs::entity_t> parts = parts_of(world, guid_system::of(world.registry, root));
        nlohmann::json entry = prefab::collapse_instance(&prefab->data, states_of(world, parts));

        // root keeps placement, rest follows the prefab
        const std::string root_source = prefab->data["entities"][0]["id"].get<std::string>();
        nlohmann::json kept = nlohmann::json::object();
        const nlohmann::json placement = entry.value("overrides", nlohmann::json::object())
                                             .value(root_source, nlohmann::json::object())
                                             .value("components", nlohmann::json::object())
                                             .value(TRANSFORM_KEY, nlohmann::json{});
        if (!placement.is_null()) { kept[root_source]["components"][TRANSFORM_KEY] = placement; }
        entry["overrides"] = std::move(kept);
        entry.erase("removed");

        const std::vector<nlohmann::json> states = prefab::expand_instance(entry, &prefab->data);

        std::vector<u64_t> guids;
        for (const ecs::entity_t part : parts) { guids.push_back(guid_system::of(world.registry, part)); }
        for (const nlohmann::json& state : states) { guids.push_back(guid_of(state["id"])); }

        history::begin(world, "Revert " + name_of(world, root), entity_targets(guids));
        apply_states(world, states, {});
        history::end(world);
    }

    void apply(world_t& world, editor_context_t& ctx, ecs::entity_t root)
    {
        ecs::registry_t& reg = world.registry;
        prefab_instance_t* instance = reg.try_get<prefab_instance_t>(root);
        prefab_t* prefab = prefab_of(world, root);
        if (!instance || !prefab) { return; }

        const asset_t<prefab_t> handle = instance->prefab;
        const nlohmann::json old_data = prefab->data;
        const u64_t instance_guid = guid_system::of(reg, root);

        std::vector<ecs::entity_t> entities;
        append_subtree(world, root, entities);

        std::unordered_map<u64_t, u64_t> to_source;
        std::vector<u64_t> guids;
        for (const ecs::entity_t entity : entities)
        {
            const prefab_part_t* part = reg.try_get<prefab_part_t>(entity);
            const bool own_part = part && part->instance == instance_guid;
            const u64_t guid = guid_system::ensure(reg, entity);
            to_source[guid] = own_part ? part->source : guid::generate();
            guids.push_back(guid);
        }

        nlohmann::json new_data = {
            {"guid", old_data.value("guid", nlohmann::json{})},
            {"entities", to_prefab_entities(world, entities, to_source)},
        };

        // root placement is the instance's own
        const nlohmann::json& old_root = old_data["entities"][0]["components"];
        if (old_root.contains(TRANSFORM_KEY))
        {
            new_data["entities"][0]["components"][TRANSFORM_KEY] = old_root[TRANSFORM_KEY];
        }

        const std::vector<resync_t> others = resync_instances(world, handle, root, old_data, new_data, guids);

        std::vector<history::target_t> targets = entity_targets(guids);
        targets.push_back(history::asset_target(get_type_id<prefab_t>(), handle));
        history::begin(world, "Apply to " + std::filesystem::path(assets::path_of(handle)).stem().string(), targets);

        prefab->data = std::move(new_data);
        for (const ecs::entity_t entity : entities)
        {
            reg.emplace_or_replace<prefab_part_t>(
                entity, prefab_part_t{instance_guid, to_source[guid_system::of(reg, entity)]});
            if (entity != root) { reg.remove<prefab_instance_t>(entity); }
        }
        reg.get<prefab_instance_t>(root).removed.clear();

        for (const resync_t& other : others) { apply_states(world, other.states, other.old_parts); }

        history::end(world);
    }

    void unpack(world_t& world, editor_context_t& ctx, ecs::entity_t root)
    {
        const std::vector<ecs::entity_t> parts = parts_of(world, guid_system::of(world.registry, root));

        std::vector<u64_t> guids;
        for (const ecs::entity_t part : parts) { guids.push_back(guid_system::of(world.registry, part)); }

        history::begin(world, "Unpack " + name_of(world, root), entity_targets(guids));
        for (const ecs::entity_t part : parts)
        {
            world.registry.remove<prefab_part_t>(part);
            world.registry.remove<prefab_instance_t>(part);
        }
        history::end(world);
    }

    bool reload(world_t& world, const std::string& vfs_path)
    {
        const asset_t<prefab_t> handle{assets::find(vfs_path)};
        prefab_t* prefab = assets::get(handle);
        if (!prefab) { return assets::reload(vfs_path); }

        const nlohmann::json old_data = prefab->data;
        if (!assets::reload(handle)) { return false; }

        for (const ecs::entity_t root : instances_of(world, handle))
        {
            const resync_t synced = resync(world, root, old_data, prefab->data);
            apply_states(world, synced.states, synced.old_parts);
        }
        return true;
    }

    overrides_t overrides_of(world_t& world, ecs::entity_t entity)
    {
        overrides_t out;
        const ecs::entity_t root = instance_root(world, entity);
        const prefab_t* prefab = root != ecs::NULL_ENTITY ? prefab_of(world, root) : nullptr;
        if (!prefab) { return out; }

        const u64_t instance = guid_system::of(world.registry, root);
        std::vector<nlohmann::json> parts;
        for (auto [e, part] : world.registry.view<prefab_part_t>().each())
        {
            if (part.instance != instance) { continue; }
            parts.push_back({
                {"id", guid_json(guid_system::of(world.registry, e))},
                {"prefab_part", {{"source", guid_json(part.source)}}},
            });
        }

        const nlohmann::json state = serialization::serialize_entity(world, entity);
        const nlohmann::json diff = prefab::diff_part(&prefab->data, parts, state);

        const u64_t source = world.registry.get<prefab_part_t>(entity).source;
        nlohmann::json reference = nlohmann::json::object();
        for (const nlohmann::json& e : prefab->data["entities"])
        {
            if (guid_of(e["id"]) == source) { reference = e.value("components", nlohmann::json::object()); }
        }

        const nlohmann::json changed = diff.value("components", nlohmann::json::object());
        for (const auto& [component, fields] : changed.items())
        {
            if (!reference.contains(component)) { out[component].insert("*"); }
            for (const auto& [field, value] : fields.items()) { out[component].insert(field); }
        }
        return out;
    }
} // namespace tau::editor::prefab_edit
