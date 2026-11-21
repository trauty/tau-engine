#include "prefab_edit.h"

#include "imgui.h"
#include "tau-editor/asset_cook.h"
#include "tau-editor/asset_editors.h"
#include "tau-editor/asset_files.h"
#include "tau-editor/commands.h"
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
#include "tau/guid_json.h"
#include "tau/hash.h"
#include "tau/log.h"
#include "tau/prefab.h"
#include "tau/serialization.h"
#include "tau/systems/guid.h"
#include "tau/systems/transform.h"
#include "tau/world.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <json/json.hpp>
#include <vector>

namespace tau::editor::prefab_edit
{
    namespace
    {
        constexpr const char* TRANSFORM_KEY = "transform_t";

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

        const nlohmann::json* raw_of(asset_handle_t handle)
        {
            const prefab_t* prefab = assets::get(asset_t<prefab_t>{handle});
            return prefab ? &prefab->data : nullptr;
        }

        // flat lookup with one prefab's raw data swapped, raw outlives the lookup
        prefab::lookup_t lookup_with(asset_handle_t changed, const nlohmann::json* raw)
        {
            return prefab::flattening(
                [changed, raw](asset_guid_t guid, const std::string& path) -> const nlohmann::json*
                {
                    const std::string ref = assets::resolve(guid, path);
                    if (!ref.empty() && assets::find(ref) == changed) { return raw; }
                    return prefab::asset_raw(guid, path);
                });
        }

        bool holds(asset_handle_t outer, asset_handle_t inner)
        {
            const nlohmann::json* outer_raw = raw_of(outer);
            return outer == inner || (outer_raw && prefab::holds(*outer_raw, raw_of(inner), &prefab::asset_raw));
        }

        std::string stem_of(asset_handle_t prefab)
        { return std::filesystem::path(assets::path_of(prefab)).stem().string(); }

        // states under to_source guids (others kept), prefab tags for standing alone:
        // instances inside keep theirs, ones nested in unwrap or in an instance reaching in from outside
        // come back out of that instance's flat data
        std::vector<nlohmann::json> standalone_states(world_t& world, const std::vector<ecs::entity_t>& entities,
                                                      const std::unordered_map<u64_t, u64_t>& to_source,
                                                      ecs::entity_t unwrap, const prefab::lookup_t& lookup)
        {
            ecs::registry_t& reg = world.registry;
            auto mapped = [&](const nlohmann::json& value) -> nlohmann::json
            {
                const auto it = to_source.find(guid::from_json(value));
                return it != to_source.end() ? guid::to_json(it->second) : value;
            };

            struct outer_t
            {
                std::unordered_map<u64_t, const nlohmann::json*> by_source;
                std::unordered_map<u64_t, u64_t> to_world;
            };
            std::unordered_map<u64_t, outer_t> outers;
            auto outer_of = [&](ecs::entity_t root, u64_t instance) -> const outer_t&
            {
                auto [it, inserted] = outers.try_emplace(instance);
                if (!inserted) { return it->second; }

                const nlohmann::json* flat = prefab::lookup_asset(lookup, reg.get<prefab_instance_t>(root).prefab);
                if (flat && flat->contains("entities"))
                {
                    for (const nlohmann::json& e : (*flat)["entities"])
                    {
                        it->second.by_source[guid::from_json(e["id"])] = &e;
                    }
                }
                for (auto [entity, part] : reg.view<prefab_part_t>().each())
                {
                    if (part.instance == instance) { it->second.to_world[part.source] = guid_system::of(reg, entity); }
                }
                return it->second;
            };

            std::vector<nlohmann::json> out;
            for (const ecs::entity_t entity : entities)
            {
                const nlohmann::json state = serialization::serialize_entity(world, entity);
                nlohmann::json standalone = state;
                prefab::strip_tags(standalone);
                standalone["id"] = mapped(state["id"]);
                for (const char* key : {"parent", "next"})
                {
                    if (state.contains(key)) { standalone[key] = mapped(state[key]); }
                }
                serialization::remap_entity_refs(standalone["components"], to_source);

                const prefab_part_t* part = reg.try_get<prefab_part_t>(entity);
                const ecs::entity_t root = part ? guid_system::find(reg, part->instance) : ecs::NULL_ENTITY;
                if (root != ecs::NULL_ENTITY && reg.all_of<prefab_instance_t>(root))
                {
                    if (root != unwrap && to_source.contains(part->instance))
                    {
                        standalone.update(prefab::tags_of(state, to_source.at(part->instance)));
                    }
                    else
                    {
                        const outer_t& outer = outer_of(root, part->instance);
                        const auto source = outer.by_source.find(part->source);
                        const nlohmann::json* tagged = source != outer.by_source.end() ? source->second : nullptr;
                        if (tagged && tagged->contains("prefab_part"))
                        {
                            const auto nested = outer.to_world.find(
                                guid::from_json((*tagged)["prefab_part"].value("instance", nlohmann::json{})));
                            const auto nested_root =
                                nested != outer.to_world.end() ? to_source.find(nested->second) : to_source.end();
                            if (nested_root != to_source.end())
                            {
                                standalone.update(prefab::tags_of(*tagged, nested_root->second));
                            }
                        }
                    }
                }
                out.push_back(std::move(standalone));
            }
            return out;
        }

        // prefab data from standalone states, root first
        nlohmann::json prefab_data(std::vector<nlohmann::json>& states, const nlohmann::json& guid,
                                   const prefab::lookup_t& lookup)
        {
            if (!states.empty())
            {
                states[0]["parent"] = nullptr;
                states[0]["next"] = nullptr;
            }
            return {
                {"guid", guid},
                {"entities", prefab::collapse(states, lookup)},
            };
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
                batch.push_back({guid::from_json(state["id"]), &state});
                kept.insert(guid::from_json(state["id"]));
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

        // instance keeps its overrides vs the old flat data, on top of the new
        resync_t resync(world_t& world, ecs::entity_t root, const nlohmann::json& old_flat,
                        const nlohmann::json& new_flat)
        {
            resync_t out;
            const std::vector<ecs::entity_t> parts = parts_of(world, guid_system::of(world.registry, root));
            for (const ecs::entity_t part : parts) { out.old_parts.push_back(guid_system::of(world.registry, part)); }

            const nlohmann::json entry = prefab::collapse_instance(&old_flat, states_of(world, parts));
            out.states = prefab::expand_instance(entry, &new_flat);
            return out;
        }

        // instances other than skip whose flat data changes with changed's raw data, nested in theirs included;
        // their guids go to the step
        std::vector<resync_t> resync_dependents(world_t& world, asset_handle_t changed, const nlohmann::json& old_raw,
                                                const nlohmann::json& new_raw, ecs::entity_t skip,
                                                std::vector<u64_t>& guids)
        {
            const prefab::lookup_t before = lookup_with(changed, &old_raw);
            const prefab::lookup_t after = lookup_with(changed, &new_raw);

            std::vector<std::pair<asset_handle_t, std::vector<ecs::entity_t>>> by_prefab;
            for (auto [entity, instance] : world.registry.view<prefab_instance_t>().each())
            {
                if (!instance.prefab.is_valid() || entity == skip) { continue; }

                const auto it = std::find_if(by_prefab.begin(), by_prefab.end(),
                                             [&](const auto& p) { return p.first == instance.prefab.handle; });
                if (it != by_prefab.end()) { it->second.push_back(entity); }
                else
                {
                    by_prefab.push_back({instance.prefab.handle, {entity}});
                }
            }

            std::vector<resync_t> out;
            for (const auto& [handle, roots] : by_prefab)
            {
                const nlohmann::json* old_flat = prefab::lookup_asset(before, handle);
                const nlohmann::json* new_flat = prefab::lookup_asset(after, handle);
                if (!old_flat || !new_flat || *old_flat == *new_flat) { continue; }

                for (const ecs::entity_t root : roots)
                {
                    out.push_back(resync(world, root, *old_flat, *new_flat));
                    guids.insert(guids.end(), out.back().old_parts.begin(), out.back().old_parts.end());
                    for (const nlohmann::json& state : out.back().states)
                    {
                        guids.push_back(guid::from_json(state["id"]));
                    }
                }
            }
            return out;
        }

        void zero_position(nlohmann::json& state)
        {
            nlohmann::json& components = state["components"];
            if (!components.is_object() || !components.contains(TRANSFORM_KEY)) { return; }

            nlohmann::json& transform = components[TRANSFORM_KEY];
            if (transform.is_object() && transform.contains("local_position"))
            {
                transform["local_position"]["v"] = {0.0, 0.0, 0.0};
            }
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
        out_source = asset_files::source_of(vfs);
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

        const prefab::lookup_t lookup = prefab::asset_lookup();
        std::vector<nlohmann::json> states = standalone_states(world, entities, same, ecs::NULL_ENTITY, lookup);
        return prefab_data(states, prefab_guid, lookup);
    }

    void replace_data(world_t& world, asset_t<prefab_t> handle, nlohmann::json new_data, std::string_view label)
    {
        prefab_t* prefab = assets::get(handle);
        if (!prefab) { return; }

        std::vector<u64_t> guids;
        const std::vector<resync_t> others =
            resync_dependents(world, handle, prefab->data, new_data, ecs::NULL_ENTITY, guids);

        std::vector<history::target_t> targets = entity_targets(guids);
        targets.push_back(history::asset_target(get_type_id<prefab_t>(), handle));
        history::begin(world, label, targets);

        prefab->data = std::move(new_data);
        for (const resync_t& other : others) { apply_states(world, other.states, other.old_parts); }

        history::end(world);
    }

    void init(editor_context_t&)
    {
        history::reg_editable(get_type_id<prefab_t>(), &capture, &apply_data, &write);

        asset_editors::add({
            .type_id = get_type_id<prefab_t>(),
            .color = IM_COL32(90, 180, 230, 255),
            .open = [](world_t&, editor_context_t&, const std::string& vfs)
            { commands::push(commands::command_e::OPEN_PREFAB, vfs); },
            .drop_in_viewport = [](world_t& world, editor_context_t& ctx, const std::string& vfs, const vec3& at)
            { instantiate(world, ctx, vfs, ecs::NULL_ENTITY, &at); },
            .drop_on_entity = [](world_t& world, editor_context_t& ctx, const std::string& vfs, ecs::entity_t parent)
            { instantiate(world, ctx, vfs, parent, nullptr); },
            .reload = &reload,
            // the open prefabs edits live in the stage until it closes
            .unsaved = [](const std::string& vfs)
            { return prefab_stage::is_dirty() && assets::find(vfs) == prefab_stage::prefab().handle; },
        });
    }

    std::string create(world_t& world, ecs::entity_t root, const std::string& dir)
    {
        ecs::registry_t& reg = world.registry;
        if (!reg.valid(root)) { return {}; }

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

        // instances inside stay nested, an instance root makes a variant of its prefab
        const prefab::lookup_t lookup = prefab::asset_lookup();
        std::vector<nlohmann::json> states = standalone_states(world, entities, to_source, ecs::NULL_ENTITY, lookup);
        // placement belongs to the instance, the prefab opens and spawns at the origin
        zero_position(states[0]);
        const nlohmann::json data = prefab_data(states, guid::to_string(guid::generate()), lookup);

        const std::string name = name_of(world, root);
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
                              ecs::entity_t parent, const vec3* position)
    {
        const asset_t<prefab_t> handle = assets::load<prefab_t>(prefab_path);
        const prefab::lookup_t lookup = prefab::asset_lookup();
        const nlohmann::json* flat = prefab::lookup_asset(lookup, handle);
        if (!flat)
        {
            TAU_LOG_ERROR("PREFAB", "Cannot instance {}, it did not load", prefab_path);
            return ecs::NULL_ENTITY;
        }

        ecs::entity_t stage_root = ecs::NULL_ENTITY;
        if (prefab_stage::is_open())
        {
            if (holds(handle, prefab_stage::prefab()))
            {
                TAU_LOG_WARN("PREFAB", "{} cannot go into {}, it would hold itself", stem_of(handle),
                             stem_of(prefab_stage::prefab()));
                return ecs::NULL_ENTITY;
            }
            // one root in the stage
            if (parent == ecs::NULL_ENTITY) { stage_root = prefab_stage::root(world); }
        }

        nlohmann::json entry = prefab::new_instance(*flat, serialization::asset_ref(handle));
        if (parent != ecs::NULL_ENTITY)
        {
            entry["parent"] = guid::to_json(guid_system::ensure(world.registry, parent));
        }
        const std::vector<nlohmann::json> states = prefab::expand_instance(entry, flat);

        std::vector<u64_t> guids;
        for (const nlohmann::json& state : states) { guids.push_back(guid::from_json(state["id"])); }

        const std::string name = std::filesystem::path(prefab_path).stem().string();
        history::begin(world, "Create " + name, entity_targets(guids));

        apply_states(world, states, {});
        const ecs::entity_t root = guid_system::find(world.registry, guid::from_json(entry["id"]));
        if (root != ecs::NULL_ENTITY && world.registry.all_of<transform_t>(root))
        {
            if (position) { world.registry.get<transform_t>(root).local_position = *position; }
            if (stage_root != ecs::NULL_ENTITY)
            {
                transform_system::set_parent_keep_world(world.registry, root, stage_root, ecs::NULL_ENTITY);
            }
        }
        if (root != ecs::NULL_ENTITY) { ctx.selection.set(guid::from_json(entry["id"])); }

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

    void revert(world_t& world, ecs::entity_t root)
    {
        const prefab_instance_t* instance = world.registry.try_get<prefab_instance_t>(root);
        const prefab::lookup_t lookup = prefab::asset_lookup();
        const nlohmann::json* flat = instance ? prefab::lookup_asset(lookup, instance->prefab) : nullptr;
        if (!flat || (*flat)["entities"].empty()) { return; }

        const std::vector<ecs::entity_t> parts = parts_of(world, guid_system::of(world.registry, root));
        nlohmann::json entry = prefab::collapse_instance(flat, states_of(world, parts));

        // root keeps placement, rest follows the prefab
        const std::string root_source = (*flat)["entities"][0]["id"].get<std::string>();
        nlohmann::json kept = nlohmann::json::object();
        const nlohmann::json placement = entry.value("overrides", nlohmann::json::object())
                                             .value(root_source, nlohmann::json::object())
                                             .value("components", nlohmann::json::object())
                                             .value(TRANSFORM_KEY, nlohmann::json{});
        if (!placement.is_null()) { kept[root_source]["components"][TRANSFORM_KEY] = placement; }
        entry["overrides"] = std::move(kept);
        entry.erase("removed");

        const std::vector<nlohmann::json> states = prefab::expand_instance(entry, flat);

        std::vector<u64_t> guids;
        for (const ecs::entity_t part : parts) { guids.push_back(guid_system::of(world.registry, part)); }
        for (const nlohmann::json& state : states) { guids.push_back(guid::from_json(state["id"])); }

        history::begin(world, "Revert " + name_of(world, root), entity_targets(guids));
        apply_states(world, states, {});
        history::end(world);
    }

    void apply(world_t& world, ecs::entity_t root)
    {
        ecs::registry_t& reg = world.registry;
        prefab_instance_t* instance = reg.try_get<prefab_instance_t>(root);
        prefab_t* prefab = prefab_of(world, root);
        if (!instance || !prefab) { return; }

        const asset_t<prefab_t> handle = instance->prefab;
        if (assets::split_sub_asset(assets::path_of(handle)))
        {
            TAU_LOG_WARN("PREFAB", "A model's prefab is read-only, nothing applied");
            return;
        }
        // the stage's undo steps end at its exit, another prefab's change would outlive them
        if (prefab_stage::is_open())
        {
            TAU_LOG_WARN("PREFAB", "Apply is not possible inside an open prefab, open {} to edit it", stem_of(handle));
            return;
        }

        const u64_t instance_guid = guid_system::of(reg, root);

        std::vector<ecs::entity_t> entities;
        append_subtree(world, root, entities);

        for (const ecs::entity_t entity : entities)
        {
            const prefab_instance_t* inner = reg.try_get<prefab_instance_t>(entity);
            if (entity != root && inner && inner->prefab.is_valid() && holds(inner->prefab, handle))
            {
                TAU_LOG_ERROR("PREFAB", "Cannot apply to {}, the {} inside would make it hold itself", stem_of(handle),
                              stem_of(inner->prefab));
                return;
            }
        }

        const prefab::lookup_t lookup = prefab::asset_lookup();
        const nlohmann::json* old_flat = prefab::lookup_asset(lookup, handle);
        if (!old_flat) { return; }
        const nlohmann::json old_data = prefab->data;

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

        std::vector<nlohmann::json> states = standalone_states(world, entities, to_source, root, lookup);

        // root placement is the instance's own
        const nlohmann::json old_root = (*old_flat)["entities"].empty()
                                            ? nlohmann::json::object()
                                            : (*old_flat)["entities"][0].value("components", nlohmann::json::object());
        if (old_root.contains(TRANSFORM_KEY)) { states[0]["components"][TRANSFORM_KEY] = old_root[TRANSFORM_KEY]; }

        nlohmann::json new_data = prefab_data(states, old_data.value("guid", nlohmann::json{}), lookup);

        const std::vector<resync_t> others = resync_dependents(world, handle, old_data, new_data, root, guids);

        std::vector<history::target_t> targets = entity_targets(guids);
        targets.push_back(history::asset_target(get_type_id<prefab_t>(), handle));
        history::begin(world, "Apply to " + stem_of(handle), targets);

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

    void unpack(world_t& world, ecs::entity_t root)
    {
        const std::vector<ecs::entity_t> parts = parts_of(world, guid_system::of(world.registry, root));

        std::vector<u64_t> guids;
        std::unordered_map<u64_t, u64_t> same;
        for (const ecs::entity_t part : parts)
        {
            const u64_t guid = guid_system::of(world.registry, part);
            guids.push_back(guid);
            same[guid] = guid;
        }

        // one level: nested instances come back as instances
        const std::vector<nlohmann::json> states = standalone_states(world, parts, same, root, prefab::asset_lookup());

        history::begin(world, "Unpack " + name_of(world, root), entity_targets(guids));
        apply_states(world, states, {});
        history::end(world);
    }

    bool reload(world_t& world, const std::string& vfs_path)
    {
        const asset_t<prefab_t> handle{assets::find(vfs_path)};
        const prefab_t* prefab = assets::get(handle);
        if (!prefab) { return assets::reload(vfs_path); }

        const nlohmann::json old_data = prefab->data;
        if (!assets::reload(handle)) { return false; }

        const nlohmann::json* new_data = raw_of(handle);
        if (!new_data) { return false; }

        std::vector<u64_t> guids;
        for (const resync_t& synced : resync_dependents(world, handle, old_data, *new_data, ecs::NULL_ENTITY, guids))
        {
            apply_states(world, synced.states, synced.old_parts);
        }
        return true;
    }

    overrides_t overrides_of(world_t& world, ecs::entity_t entity)
    {
        overrides_t out;
        const ecs::entity_t root = instance_root(world, entity);
        if (root == ecs::NULL_ENTITY) { return out; }

        const prefab::lookup_t lookup = prefab::asset_lookup();
        const nlohmann::json* flat = prefab::lookup_asset(lookup, world.registry.get<prefab_instance_t>(root).prefab);
        if (!flat) { return out; }

        const u64_t instance = guid_system::of(world.registry, root);
        std::vector<nlohmann::json> parts;
        for (auto [e, part] : world.registry.view<prefab_part_t>().each())
        {
            if (part.instance != instance) { continue; }
            parts.push_back({
                {"id", guid::to_json(guid_system::of(world.registry, e))},
                {"prefab_part", {{"source", guid::to_json(part.source)}}},
            });
        }

        const nlohmann::json state = serialization::serialize_entity(world, entity);
        const nlohmann::json diff = prefab::diff_part(flat, parts, state);

        const u64_t source = world.registry.get<prefab_part_t>(entity).source;
        nlohmann::json reference = nlohmann::json::object();
        for (const nlohmann::json& e : (*flat)["entities"])
        {
            if (guid::from_json(e["id"]) == source) { reference = e.value("components", nlohmann::json::object()); }
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
