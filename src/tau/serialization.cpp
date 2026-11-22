#include "serialization.h"

#include "tau/asset.h"
#include "tau/assets/prefab.h"
#include "tau/assets/scene_format.h"
#include "tau/components/prefab.h"
#include "tau/components/transform.h"
#include "tau/components/unresolved.h"
#include "tau/guid.h"
#include "tau/guid_json.h"
#include "tau/log.h"
#include "tau/prefab.h"
#include "tau/reflection.h"
#include "tau/serialization_detail.h"
#include "tau/systems/guid.h"
#include "tau/systems/transform.h"

#include "json/json.hpp"
#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// whole scenes and entities: world <-> JSON, JSON -> scene_t -> world
namespace tau::serialization
{
    using namespace detail;

    namespace detail
    {
        std::unordered_map<u32_t, tau::reflection::type_t> component_types_by_pool(tau::world_t& world)
        {
            std::unordered_map<u32_t, tau::reflection::type_t> by_pool;
            for (auto [meta_id, type] : tau::reflection::resolve(*world.reflection_ctx))
            {
                if (!type.func("get"_h)) { continue; } // not a component
                by_pool.emplace(static_cast<u32_t>(type.info().hash()), type);
            }
            return by_pool;
        }
    } // namespace detail

    namespace
    {
        // parents before children, siblings in order
        std::vector<tau::ecs::entity_t> hierarchy_order(tau::ecs::registry_t& reg)
        {
            std::vector<tau::ecs::entity_t> roots;
            for (tau::ecs::entity_t root = tau::transform_system::first_root(reg); root != tau::ecs::NULL_ENTITY;
                 root = reg.get<transform_t>(root).get_next_sibling())
            {
                roots.push_back(root);
            }

            // no transform: after the rest, oldest first
            const std::size_t ordered = roots.size();
            for (tau::ecs::entity_t entity : reg.view<tau::ecs::entity_t>())
            {
                if (!reg.all_of<transform_t>(entity)) { roots.push_back(entity); }
            }
            std::reverse(roots.begin() + static_cast<std::ptrdiff_t>(ordered), roots.end());

            std::vector<tau::ecs::entity_t> order;
            std::vector<tau::ecs::entity_t> stack;

            for (tau::ecs::entity_t root : roots)
            {
                stack.push_back(root);
                while (!stack.empty())
                {
                    const tau::ecs::entity_t entity = stack.back();
                    stack.pop_back();
                    order.push_back(entity);

                    const transform_t* transform = reg.try_get<transform_t>(entity);
                    if (!transform) { continue; }

                    const std::size_t first = stack.size();
                    for (tau::ecs::entity_t child = transform->get_first_child(); child != tau::ecs::NULL_ENTITY;
                         child = reg.get<transform_t>(child).get_next_sibling())
                    {
                        stack.push_back(child);
                    }
                    std::reverse(stack.begin() + static_cast<std::ptrdiff_t>(first), stack.end());
                }
            }

            return order;
        }

        nlohmann::json encode_components(tau::world_t& world, tau::ecs::entity_t entity, const entity_refs_t& refs)
        {
            const std::unordered_map<u32_t, tau::reflection::type_t> by_pool = component_types_by_pool(world);
            nlohmann::json components = nlohmann::json::object();

            for (auto [pool_id, pool] : world.registry.storage())
            {
                if (!pool.contains(entity)) { continue; }

                const auto type_it = by_pool.find(static_cast<u32_t>(pool_id));
                if (type_it == by_pool.end()) { continue; }

                const tau::reflection::type_t type = type_it->second;
                tau::reflection::any_t instance =
                    type.func("get"_h).invoke({}, tau::reflection::forward_as_meta(world.registry), entity);
                if (!instance) { continue; }

                components[tau::reflection::meta_key(type.name(), static_cast<u32_t>(type.alias()))] =
                    encode_fields(*world.reflection_ctx, type, instance, refs);
            }

            return components;
        }

        const nlohmann::json* prefab_data_of(const tau::ecs::registry_t& reg, tau::ecs::entity_t root,
                                             const tau::prefab::lookup_t& lookup)
        {
            const prefab_instance_t* instance = reg.try_get<prefab_instance_t>(root);
            return instance ? tau::prefab::lookup_asset(lookup, instance->prefab) : nullptr;
        }

        void add_unresolved(const tau::ecs::registry_t& reg, tau::ecs::entity_t entity, nlohmann::json& components);

        nlohmann::json collapse_instance(tau::world_t& world, tau::ecs::entity_t root,
                                         const tau::prefab::lookup_t& lookup)
        {
            const u64_t instance = tau::guid_system::of(world.registry, root);
            std::vector<nlohmann::json> parts;
            for (auto [entity, part] : world.registry.view<prefab_part_t>().each())
            {
                if (part.instance == instance) { parts.push_back(serialize_entity(world, entity)); }
            }
            return tau::prefab::collapse_instance(prefab_data_of(world.registry, root, lookup), parts);
        }
    } // namespace

    nlohmann::json serialize_scene(tau::world_t& world)
    {
        nlohmann::json scene_data;
        scene_data["entities"] = nlohmann::json::array();

        const std::unordered_map<u32_t, tau::reflection::type_t> by_pool = component_types_by_pool(world);

        const std::vector<tau::ecs::entity_t> entities = hierarchy_order(world.registry);
        for (tau::ecs::entity_t entity : entities) { tau::guid_system::ensure(world.registry, entity); }

        const tau::prefab::lookup_t lookup = tau::prefab::asset_lookup();
        std::unordered_map<tau::ecs::entity_t, std::size_t> slots;
        for (tau::ecs::entity_t entity : entities)
        {
            // instance = one entry at its root
            if (const prefab_part_t* part = world.registry.try_get<prefab_part_t>(entity))
            {
                const tau::ecs::entity_t root = tau::guid_system::find(world.registry, part->instance);
                if (root != tau::ecs::NULL_ENTITY && world.registry.all_of<prefab_instance_t>(root))
                {
                    if (root == entity) { scene_data["entities"].push_back(collapse_instance(world, root, lookup)); }
                    continue;
                }
            }

            nlohmann::json entity_json;
            entity_json["id"] = guid::to_json(tau::guid_system::of(world.registry, entity));

            const transform_t* transform = world.registry.try_get<transform_t>(entity);
            if (transform && transform->get_parent() != tau::ecs::NULL_ENTITY)
            {
                entity_json["parent"] = guid::to_json(tau::guid_system::of(world.registry, transform->get_parent()));
            }

            entity_json["components"] = nlohmann::json::object();

            slots.emplace(entity, scene_data["entities"].size());
            scene_data["entities"].push_back(std::move(entity_json));
        }

        const entity_refs_t refs{.reg = &world.registry};

        for (auto [pool_id, pool] : world.registry.storage())
        {
            const auto type_it = by_pool.find(static_cast<u32_t>(pool_id));
            if (type_it == by_pool.end()) { continue; }

            const tau::reflection::type_t type = type_it->second;
            const tau::reflection::func_t get_func = type.func("get"_h);
            const std::string type_key = tau::reflection::meta_key(type.name(), static_cast<u32_t>(type.alias()));

            for (tau::ecs::entity_t entity : pool)
            {
                const auto slot_it = slots.find(entity);
                if (slot_it == slots.end()) { continue; }

                tau::reflection::any_t instance =
                    get_func.invoke({}, tau::reflection::forward_as_meta(world.registry), entity);
                if (!instance) { continue; }

                scene_data["entities"][slot_it->second]["components"][type_key] =
                    encode_fields(*world.reflection_ctx, type, instance, refs);
            }
        }

        for (const auto& [entity, slot] : slots)
        {
            add_unresolved(world.registry, entity, scene_data["entities"][slot]["components"]);
        }

        return scene_data;
    }

    namespace
    {
        scene_entity_t parse_entity(const nlohmann::json& entity_json)
        {
            scene_entity_t out_entity;
            out_entity.id = guid::from_json(entity_json.value("id", nlohmann::json{}));

            const nlohmann::json parent = entity_json.value("parent", nlohmann::json{});
            out_entity.parent = (parent.is_number_integer() && parent.get<i64_t>() >= 0) ? parent.get<u64_t>() + 1
                                                                                         : guid::from_json(parent);

            const nlohmann::json components = entity_json.value("components", nlohmann::json::object());
            for (const auto& [type_key, props_json] : components.items())
            {
                scene_component_t& out_comp = out_entity.components.emplace_back();
                out_comp.type_hash = key_to_hash(type_key);
                if (!is_numeric_key(type_key)) { out_comp.type_name = type_key; }
                out_comp.source_json = props_json.dump();

                for (const auto& [prop_key, val] : props_json.items())
                {
                    scene_property_t prop;
                    prop.prop_hash = key_to_hash(prop_key);
                    if (!is_numeric_key(prop_key)) { prop.prop_name = prop_key; }

                    if (parse_value(val, prop_key, prop)) { out_comp.properties.push_back(std::move(prop)); }
                }
            }

            if (const auto part = entity_json.find("prefab_part"); part != entity_json.end() && part->is_object())
            {
                out_entity.prefab.instance = guid::from_json(part->value("instance", nlohmann::json{}));
                out_entity.prefab.source = guid::from_json(part->value("source", nlohmann::json{}));
            }
            if (const auto ref = entity_json.find("prefab"); ref != entity_json.end() && ref->is_object())
            {
                out_entity.prefab.root = true;
                out_entity.prefab.guid = ref_guid(*ref);
                out_entity.prefab.path = ref->value("p", std::string{});
                for (const nlohmann::json& source : entity_json.value("removed", nlohmann::json::array()))
                {
                    out_entity.prefab.removed.push_back(guid::from_json(source));
                }
                if (entity_json.contains("unresolved"))
                {
                    out_entity.prefab.unresolved = entity_json["unresolved"].dump();
                }
            }

            return out_entity;
        }
    } // namespace

    nlohmann::json serialize_entity(tau::world_t& world, tau::ecs::entity_t entity)
    {
        const entity_refs_t refs{.reg = &world.registry};

        nlohmann::json entity_json;
        entity_json["id"] = guid::to_json(tau::guid_system::ensure(world.registry, entity));

        if (const transform_t* transform = world.registry.try_get<transform_t>(entity))
        {
            entity_json["parent"] = guid::to_json(refs.encode(transform->get_parent()));
            entity_json["next"] = guid::to_json(refs.encode(transform->get_next_sibling()));
        }

        entity_json["components"] = encode_components(world, entity, refs);
        add_unresolved(world.registry, entity, entity_json["components"]);

        if (const prefab_part_t* part = world.registry.try_get<prefab_part_t>(entity))
        {
            entity_json["prefab_part"] = {
                {"instance", guid::to_json(part->instance)},
                {"source",   guid::to_json(part->source)  }
            };
        }
        if (const prefab_instance_t* instance = world.registry.try_get<prefab_instance_t>(entity))
        {
            if (!instance->unresolved.empty())
            {
                entity_json["unresolved"] = nlohmann::json::parse(instance->unresolved, nullptr, false);
                entity_json["prefab"] = entity_json["unresolved"].value("prefab", nlohmann::json::object());
            }
            else
            {
                entity_json["prefab"] = asset_ref(instance->prefab);
            }

            nlohmann::json removed = nlohmann::json::array();
            for (const u64_t source : instance->removed) { removed.push_back(guid::to_json(source)); }
            entity_json["removed"] = std::move(removed);
        }
        return entity_json;
    }

    scene_t scene_from_json(const nlohmann::json& scene_data)
    {
        scene_t scene;

        if (!scene_data.contains("entities")) { return scene; }

        for (const nlohmann::json& entity_json : scene_data["entities"])
        {
            scene.entities.push_back(parse_entity(entity_json));
        }

        const bool has_ids = std::any_of(scene.entities.begin(), scene.entities.end(),
                                         [](const scene_entity_t& entity) { return entity.id != 0; });
        scene.generated_ids = !has_ids && !scene.entities.empty();
        for (std::size_t i = 0; i < scene.entities.size(); i++)
        {
            if (scene.generated_ids) { scene.entities[i].id = i + 1; }
            for (scene_component_t& comp : scene.entities[i].components)
            {
                adopt_index_refs(comp.properties, scene.generated_ids);
            }
        }

        return scene;
    }

    namespace
    {
        void apply_scene_entity(tau::world_t& world, tau::ecs::entity_t entity, const scene_entity_t& scene_entity,
                                const entity_refs_t& refs)
        {
            nlohmann::json unresolved = nlohmann::json::object();
            nlohmann::json unknown_fields = nlohmann::json::object();
            for (const scene_component_t& comp : scene_entity.components)
            {
                tau::reflection::type_t type = tau::reflection::resolve(*world.reflection_ctx, comp.type_hash);

                if (!type)
                {
                    const std::string key = describe_key(comp.type_name, comp.type_hash);
                    if (comp.source_json.empty())
                    {
                        TAU_LOG_WARN("SCENE", "Unknown component '{}', dropped from entity", key);
                        continue;
                    }

                    if (first_warning(key))
                    {
                        TAU_LOG_WARN("SCENE", "Unknown component '{}', kept as saved until its type is registered",
                                     key);
                    }
                    unresolved[key] = nlohmann::json::parse(comp.source_json, nullptr, false);
                    continue;
                }

                tau::reflection::func_t get_func = type.func("get"_h);
                if (!get_func) { continue; }

                tau::reflection::any_t instance =
                    get_func.invoke({}, tau::reflection::forward_as_meta(world.registry), entity);

                // only if missing: replacing resets e.g. transform links
                if (!instance)
                {
                    if (tau::reflection::func_t add_func = type.func("add"_h); add_func)
                    {
                        add_func.invoke({}, tau::reflection::forward_as_meta(world.registry), entity);
                    }
                    instance = get_func.invoke({}, tau::reflection::forward_as_meta(world.registry), entity);
                }
                if (!instance) { continue; }

                std::vector<std::string> unknown;
                apply_fields(*world.reflection_ctx, instance, type, comp.properties, refs,
                             describe_key(comp.type_name, comp.type_hash),
                             comp.source_json.empty() ? nullptr : &unknown);
                if (!unknown.empty())
                {
                    const nlohmann::json saved = nlohmann::json::parse(comp.source_json, nullptr, false);
                    nlohmann::json kept = nlohmann::json::object();
                    for (const std::string& key : unknown)
                    {
                        if (saved.is_object() && saved.contains(key)) { kept[key] = saved[key]; }
                    }
                    if (!kept.empty())
                    {
                        unknown_fields[tau::reflection::meta_key(type.name(), static_cast<u32_t>(type.alias()))] = kept;
                    }
                }

                if (tau::reflection::func_t on_changed = type.func("on_changed"_h); on_changed)
                {
                    on_changed.invoke({}, tau::reflection::forward_as_meta(world.registry), entity);
                }
            }

            if (unresolved.empty() && unknown_fields.empty())
            {
                world.registry.remove<unresolved_components_t>(entity);
            }
            else
            {
                world.registry.emplace_or_replace<unresolved_components_t>(entity, unresolved.dump(),
                                                                           unknown_fields.dump());
            }
        }

        // written next to the reflected ones, a reflected one of the same key wins
        void add_unresolved(const tau::ecs::registry_t& reg, tau::ecs::entity_t entity, nlohmann::json& components)
        {
            const unresolved_components_t* kept = reg.try_get<unresolved_components_t>(entity);
            if (!kept) { return; }

            const nlohmann::json saved = nlohmann::json::parse(kept->json, nullptr, false);
            if (saved.is_object())
            {
                for (const auto& [key, fields] : saved.items())
                {
                    if (!components.contains(key)) { components[key] = fields; }
                }
            }

            const nlohmann::json unknown = nlohmann::json::parse(kept->fields, nullptr, false);
            if (!unknown.is_object()) { return; }
            for (const auto& [key, fields] : unknown.items())
            {
                const auto component = components.find(key);
                if (component == components.end() || !component->is_object() || !fields.is_object()) { continue; }
                for (const auto& [field, value] : fields.items())
                {
                    if (!component->contains(field)) { (*component)[field] = value; }
                }
            }
        }
    } // namespace

    namespace
    {
        void apply_prefab_tags(tau::world_t& world, tau::ecs::entity_t entity, const scene_entity_t& scene_entity)
        {
            tau::ecs::registry_t& reg = world.registry;

            if (scene_entity.prefab.instance != 0)
            {
                reg.emplace_or_replace<prefab_part_t>(
                    entity, prefab_part_t{scene_entity.prefab.instance, scene_entity.prefab.source});
            }
            else
            {
                reg.remove<prefab_part_t>(entity);
            }

            if (!scene_entity.prefab.root)
            {
                reg.remove<prefab_instance_t>(entity);
                return;
            }

            prefab_instance_t instance;
            instance.removed = scene_entity.prefab.removed;
            instance.unresolved = scene_entity.prefab.unresolved;

            const std::string ref = tau::assets::resolve(scene_entity.prefab.guid, scene_entity.prefab.path);
            if (!ref.empty()) { instance.prefab = tau::assets::load<prefab_t>(ref); }

            reg.emplace_or_replace<prefab_instance_t>(entity, std::move(instance));
        }
    } // namespace

    std::vector<tau::ecs::entity_t> instantiate_scene(tau::world_t& world, const scene_t& scene)
    {
        std::vector<tau::ecs::entity_t> created;
        created.reserve(scene.entities.size());
        std::unordered_map<u64_t, tau::ecs::entity_t> by_file_id;

        for (const scene_entity_t& scene_entity : scene.entities)
        {
            const tau::ecs::entity_t entity = world.registry.create();

            // keep file guid unless made up or taken
            const bool fresh = scene.generated_ids || scene_entity.id == 0 ||
                               tau::guid_system::find(world.registry, scene_entity.id) != tau::ecs::NULL_ENTITY;
            tau::guid_system::assign(world.registry, entity, fresh ? tau::guid::generate() : scene_entity.id);

            by_file_id.emplace(scene_entity.id, entity);
            created.push_back(entity);
        }

        const entity_refs_t refs{.reg = &world.registry, .by_file_id = &by_file_id};
        for (std::size_t i = 0; i < scene.entities.size(); i++)
        {
            apply_scene_entity(world, created[i], scene.entities[i], refs);
            apply_prefab_tags(world, created[i], scene.entities[i]);
        }

        for (std::size_t i = 0; i < created.size(); i++)
        {
            const u64_t parent_id = scene.entities[i].parent;
            if (parent_id == 0) { continue; }

            const tau::ecs::entity_t parent = refs.decode(parent_id);
            if (parent == tau::ecs::NULL_ENTITY || !world.registry.all_of<transform_t>(created[i]) ||
                !world.registry.all_of<transform_t>(parent))
            {
                TAU_LOG_WARN("SCENE", "Entity {} has an invalid parent, left at the root", i);
                continue;
            }

            tau::transform_system::set_parent(world.registry, created[i], parent);
        }

        return created;
    }

    std::vector<tau::ecs::entity_t> deserialize_scene(tau::world_t& world, const nlohmann::json& scene_data)
    { return instantiate_scene(world, scene_from_json(tau::prefab::expand(scene_data, tau::prefab::asset_lookup()))); }

    namespace
    {
        u32_t depth_of(const tau::ecs::registry_t& reg, tau::ecs::entity_t entity)
        {
            u32_t depth = 0;
            for (const transform_t* t = reg.try_get<transform_t>(entity); t && t->get_parent() != tau::ecs::NULL_ENTITY;
                 t = reg.try_get<transform_t>(t->get_parent()))
            {
                depth++;
            }
            return depth;
        }
    } // namespace

    void apply_entities(tau::world_t& world, std::span<const entity_state_t> states)
    {
        tau::ecs::registry_t& reg = world.registry;

        std::unordered_map<u64_t, tau::ecs::entity_t> by_guid;
        for (const entity_state_t& s : states)
        {
            if (!s.state) { continue; }

            tau::ecs::entity_t entity = tau::guid_system::find(reg, s.guid);
            if (entity == tau::ecs::NULL_ENTITY)
            {
                entity = reg.create();
                tau::guid_system::assign(reg, entity, s.guid);
            }
            by_guid.emplace(s.guid, entity);
        }

        const entity_refs_t refs{.reg = &reg, .by_file_id = &by_guid, .search_world = true};
        const std::unordered_map<u32_t, tau::reflection::type_t> by_pool = component_types_by_pool(world);

        std::vector<tau::reflection::type_t> extra;
        for (const entity_state_t& s : states)
        {
            if (!s.state) { continue; }

            const tau::ecs::entity_t entity = by_guid.at(s.guid);
            const scene_entity_t parsed = parse_entity(*s.state);

            // in place: drop components the state lacks
            extra.clear();
            for (auto [pool_id, pool] : reg.storage())
            {
                const auto type_it = by_pool.find(static_cast<u32_t>(pool_id));
                if (type_it == by_pool.end() || !pool.contains(entity)) { continue; }

                const u32_t type_hash = static_cast<u32_t>(type_it->second.alias());
                if (std::none_of(parsed.components.begin(), parsed.components.end(),
                                 [type_hash](const scene_component_t& c) { return c.type_hash == type_hash; }))
                {
                    extra.push_back(type_it->second);
                }
            }
            for (const tau::reflection::type_t& type : extra)
            {
                if (tau::reflection::func_t remove_func = type.func("remove"_h); remove_func)
                {
                    remove_func.invoke({}, tau::reflection::forward_as_meta(reg), entity);
                }
            }

            apply_scene_entity(world, entity, parsed, refs);
            apply_prefab_tags(world, entity, parsed);
        }

        auto place = [&](const entity_state_t& s)
        {
            const tau::ecs::entity_t entity = by_guid.at(s.guid);
            const transform_t* transform = reg.try_get<transform_t>(entity);
            if (!transform) { return; }

            tau::ecs::entity_t parent = refs.decode(guid::from_json(s.state->value("parent", nlohmann::json{})));
            if (parent != tau::ecs::NULL_ENTITY && !reg.all_of<transform_t>(parent)) { parent = tau::ecs::NULL_ENTITY; }
            const tau::ecs::entity_t next = refs.decode(guid::from_json(s.state->value("next", nlohmann::json{})));

            if (transform->get_parent() == parent && transform->get_next_sibling() == next) { return; }
            tau::transform_system::set_parent(reg, entity, parent, next);
        };

        // placed before its next sibling, which goes first if in the batch
        std::vector<const entity_state_t*> unplaced;
        std::unordered_set<u64_t> unplaced_guids;
        for (const entity_state_t& s : states)
        {
            if (!s.state) { continue; }
            unplaced.push_back(&s);
            unplaced_guids.insert(s.guid);
        }

        bool stuck = false;
        while (!unplaced.empty())
        {
            bool progressed = false;
            for (auto it = unplaced.begin(); it != unplaced.end();)
            {
                const u64_t next = guid::from_json((*it)->state->value("next", nlohmann::json{}));
                if (!stuck && next != 0 && unplaced_guids.contains(next))
                {
                    ++it;
                    continue;
                }

                place(**it);
                unplaced_guids.erase((*it)->guid);
                it = unplaced.erase(it);
                progressed = true;
            }
            // cycle: place the rest as they come
            if (!progressed) { stuck = true; }
        }

        // deepest first, a parent takes children a frame later
        std::vector<std::pair<u32_t, tau::ecs::entity_t>> doomed;
        for (const entity_state_t& s : states)
        {
            if (s.state) { continue; }

            const tau::ecs::entity_t entity = tau::guid_system::find(reg, s.guid);
            if (entity != tau::ecs::NULL_ENTITY) { doomed.emplace_back(depth_of(reg, entity), entity); }
        }
        std::sort(doomed.begin(), doomed.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& [depth, entity] : doomed)
        {
            if (reg.valid(entity)) { reg.destroy(entity); }
        }
    }

    void clear_scene(tau::world_t& world)
    {
        auto view = world.registry.view<tau::ecs::entity_t>();

        std::vector<tau::ecs::entity_t> to_destroy;
        to_destroy.insert(to_destroy.end(), view.begin(), view.end());

        world.registry.destroy(to_destroy.begin(), to_destroy.end());
    }
} // namespace tau::serialization
