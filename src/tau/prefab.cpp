#include "prefab.h"

#include "tau/asset.h"
#include "tau/assets/prefab.h"
#include "tau/guid.h"
#include "tau/guid_json.h"
#include "tau/hash.h"
#include "tau/log.h"
#include "tau/serialization.h"
#include "tau/systems/guid.h"
#include "tau/world.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace tau::prefab
{
    namespace
    {

        const nlohmann::json& entities_of(const nlohmann::json* prefab_data)
        {
            static const nlohmann::json none = nlohmann::json::array();
            if (!prefab_data || !prefab_data->contains("entities")) { return none; }
            return (*prefab_data)["entities"];
        }

        // euler/quat round trip float noise is no change
        bool same(const nlohmann::json& a, const nlohmann::json& b)
        {
            if (a.is_object() && b.is_object() && a.contains("ty") && a.contains("v") && b.contains("ty") &&
                b.contains("v") && (a.contains("n") || b.contains("n")))
            {
                if (a.contains("n") && b.contains("n")) { return a["n"] == b["n"]; }
                return a["ty"] == b["ty"] && same(a["v"], b["v"]);
            }
            if (a.is_number() && b.is_number())
            {
                const f64 x = a.get<f64>();
                const f64 y = b.get<f64>();
                return std::fabs(x - y) <= 1e-4 * std::max({1.0, std::fabs(x), std::fabs(y)});
            }
            // asset ref = guid, path goes stale on rename
            // old guid spellings compared parsed
            if (a.is_object() && b.is_object() && a.contains("t") && a.contains("g") && b.contains("g"))
            {
                const asset_guid_t ga = assets::parse_guid(a.value("g", std::string{}));
                return ga != 0 ? ga == assets::parse_guid(b.value("g", std::string{}))
                               : a.value("p", "") == b.value("p", "");
            }
            if (a.type() != b.type() || a.size() != b.size()) { return false; }
            if (a.is_array())
            {
                for (std::size_t i = 0; i < a.size(); i++)
                {
                    if (!same(a[i], b[i])) { return false; }
                }
                return true;
            }
            if (a.is_object())
            {
                for (const auto& [key, value] : a.items())
                {
                    if (!b.contains(key) || !same(value, b[key])) { return false; }
                }
                return true;
            }
            return a == b;
        }

        nlohmann::json diff_components(const nlohmann::json& reference, const nlohmann::json& components)
        {
            nlohmann::json changed = nlohmann::json::object();
            for (const auto& [key, fields] : components.items())
            {
                if (!reference.contains(key))
                {
                    changed[key] = fields;
                    continue;
                }
                for (const auto& [field, value] : fields.items())
                {
                    const nlohmann::json& ref = reference[key];
                    if (!ref.contains(field) || !same(ref[field], value)) { changed[key][field] = value; }
                }
            }

            nlohmann::json removed = nlohmann::json::array();
            for (const auto& [key, fields] : reference.items())
            {
                if (!components.contains(key)) { removed.push_back(key); }
            }

            nlohmann::json out = nlohmann::json::object();
            if (!changed.empty()) { out["components"] = std::move(changed); }
            if (!removed.empty()) { out["removed"] = std::move(removed); }
            return out;
        }

        void apply_overrides(nlohmann::json& components, const nlohmann::json& overrides)
        {
            const nlohmann::json changed = overrides.value("components", nlohmann::json::object());
            for (const auto& [key, fields] : changed.items())
            {
                if (!components.contains(key)) { components[key] = fields; }
                else
                {
                    for (const auto& [field, value] : fields.items()) { components[key][field] = value; }
                }
            }
            for (const nlohmann::json& key : overrides.value("removed", nlohmann::json::array()))
            {
                if (key.is_string()) { components.erase(key.get<std::string>()); }
            }
        }

        // part with no stored id: same id on every expansion
        u64_t derived_id(u64_t instance, u64_t source)
        {
            u64_t x = instance ^ (source + 0x9e3779b97f4a7c15ull + (instance << 6) + (instance >> 2));
            x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
            x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
            x ^= x >> 31;
            return x != 0 ? x : 1;
        }

        std::unordered_set<u64_t> removed_sources(const nlohmann::json& entities, const nlohmann::json& removed)
        {
            std::unordered_set<u64_t> out;
            for (const nlohmann::json& g : removed) { out.insert(guid::from_json(g)); }

            for (const nlohmann::json& e : entities)
            {
                if (out.contains(guid::from_json(e.value("parent", nlohmann::json{}))))
                {
                    out.insert(guid::from_json(e["id"]));
                }
            }
            return out;
        }

        struct part_map_t
        {
            u64_t root_source = 0;
            std::unordered_map<u64_t, u64_t> to_instance;
            std::unordered_map<u64_t, const nlohmann::json*> by_source;
        };

        // prefab entity as a part with no overrides, nested tags belong to the prefab
        nlohmann::json reference_state(const nlohmann::json& e, const part_map_t& map)
        {
            nlohmann::json state = e;
            strip_tags(state);
            serialization::remap_entity_refs(state["components"], map.to_instance);
            return state;
        }

        u64_t part_instance(const nlohmann::json& state)
        {
            return guid::from_json(
                state.value("prefab_part", nlohmann::json::object()).value("instance", nlohmann::json{}));
        }

        u64_t part_source(const nlohmann::json& state)
        {
            return guid::from_json(
                state.value("prefab_part", nlohmann::json::object()).value("source", nlohmann::json{}));
        }

        part_map_t map_parts(const nlohmann::json* prefab_data, std::span<const nlohmann::json> parts)
        {
            part_map_t map;
            const nlohmann::json& entities = entities_of(prefab_data);
            if (!entities.empty()) { map.root_source = guid::from_json(entities[0]["id"]); }

            for (const nlohmann::json& part : parts)
            {
                const u64_t source = part_source(part);
                map.to_instance[source] = guid::from_json(part["id"]);
                map.by_source[source] = &part;
            }
            return map;
        }
    } // namespace

    namespace
    {
        constexpr const char* TAG_KEYS[] = {"prefab_part", "prefab", "removed", "unresolved"};

        struct flat_cache_t
        {
            raw_lookup_t raw;
            std::unordered_map<const nlohmann::json*, nlohmann::json> done;
            std::unordered_set<const nlohmann::json*> busy;
        };

        const nlohmann::json* flat_of(flat_cache_t& cache, asset_guid_t guid, const std::string& path)
        {
            const nlohmann::json* raw = cache.raw(guid, path);
            if (!raw) { return nullptr; }
            if (const auto it = cache.done.find(raw); it != cache.done.end()) { return &it->second; }
            if (cache.busy.contains(raw))
            {
                TAU_LOG_ERROR("PREFAB", "Prefab {} holds itself", path);
                return nullptr;
            }

            cache.busy.insert(raw);
            nlohmann::json flat =
                expand(*raw, [&cache](asset_guid_t g, const std::string& p) { return flat_of(cache, g, p); });
            cache.busy.erase(raw);
            return &cache.done.emplace(raw, std::move(flat)).first->second;
        }

        const nlohmann::json* entry_target(const nlohmann::json& entry, const raw_lookup_t& lookup)
        {
            const nlohmann::json ref = entry.value("prefab", nlohmann::json::object());
            if (!ref.is_object()) { return nullptr; }
            return lookup(assets::parse_guid(ref.value("g", std::string{})), ref.value("p", std::string{}));
        }

        bool holds_in(const nlohmann::json& data, const nlohmann::json* target, const raw_lookup_t& raw,
                      std::unordered_set<const nlohmann::json*>& seen)
        {
            for (const nlohmann::json& e : entities_of(&data))
            {
                if (!is_instance_entry(e)) { continue; }

                const nlohmann::json* inner = entry_target(e, raw);
                if (!inner) { continue; }
                if (inner == target) { return true; }
                if (seen.insert(inner).second && holds_in(*inner, target, raw, seen)) { return true; }
            }
            return false;
        }

        bool is_root_state(const nlohmann::json& state)
        {
            if (!state.contains("prefab")) { return false; }
            const u64_t instance = part_instance(state);
            return instance == 0 || instance == guid::from_json(state.value("id", nlohmann::json{}));
        }
    } // namespace

    const nlohmann::json* asset_raw(asset_guid_t guid, const std::string& path)
    {
        const std::string ref = assets::resolve(guid, path);
        if (ref.empty()) { return nullptr; }

        const prefab_t* prefab = assets::get(assets::load<prefab_t>(ref));
        return prefab ? &prefab->data : nullptr;
    }

    lookup_t flattening(raw_lookup_t raw)
    {
        auto cache = std::make_shared<flat_cache_t>();
        cache->raw = std::move(raw);
        return [cache](asset_guid_t guid, const std::string& path) { return flat_of(*cache, guid, path); };
    }

    lookup_t asset_lookup() { return flattening(&asset_raw); }

    const nlohmann::json* lookup_asset(const lookup_t& lookup, asset_handle_t prefab)
    { return prefab.is_valid() ? lookup(prefab.guid, assets::path_of(prefab)) : nullptr; }

    bool holds(const nlohmann::json& prefab_data, const nlohmann::json* target, const raw_lookup_t& raw)
    {
        std::unordered_set<const nlohmann::json*> seen;
        return target && holds_in(prefab_data, target, raw, seen);
    }

    void collect_nested(const nlohmann::json& prefab_data, std::vector<asset_handle_t>& out)
    {
        // loaded ones only, nothing loads here
        std::unordered_map<const nlohmann::json*, asset_handle_t> handles;
        const raw_lookup_t loaded = [&](asset_guid_t guid, const std::string& path) -> const nlohmann::json*
        {
            const std::string ref = assets::resolve(guid, path);
            const asset_handle_t handle = ref.empty() ? asset_handle_t{} : assets::find(ref);
            const prefab_t* prefab = handle ? assets::get(asset_t<prefab_t>{handle}) : nullptr;
            if (prefab) { handles.emplace(&prefab->data, handle); }
            return prefab ? &prefab->data : nullptr;
        };

        std::unordered_set<const nlohmann::json*> seen;
        holds_in(prefab_data, nullptr, loaded, seen);
        for (const nlohmann::json* data : seen) { out.push_back(handles.at(data)); }
    }

    nlohmann::json tags_of(const nlohmann::json& state, u64_t instance)
    {
        nlohmann::json tags = nlohmann::json::object();
        for (const char* key : TAG_KEYS)
        {
            if (state.contains(key)) { tags[key] = state[key]; }
        }
        if (tags.contains("prefab_part")) { tags["prefab_part"]["instance"] = guid::to_json(instance); }
        return tags;
    }

    void strip_tags(nlohmann::json& state)
    {
        for (const char* key : TAG_KEYS) { state.erase(key); }
    }

    bool is_instance_entry(const nlohmann::json& entity)
    { return entity.is_object() && entity.contains("prefab") && !entity.contains("components"); }

    nlohmann::json expand(const nlohmann::json& scene_data, const lookup_t& lookup)
    {
        nlohmann::json out = scene_data;
        if (!scene_data.contains("entities")) { return out; }

        nlohmann::json entities = nlohmann::json::array();
        for (const nlohmann::json& entity : scene_data["entities"])
        {
            if (!is_instance_entry(entity))
            {
                entities.push_back(entity);
                continue;
            }

            const nlohmann::json ref = entity["prefab"];
            const nlohmann::json* data =
                lookup(assets::parse_guid(ref.value("g", std::string{})), ref.value("p", std::string{}));
            if (!data) { TAU_LOG_ERROR("PREFAB", "Prefab {} is missing, its instance stays empty", ref.dump()); }

            for (nlohmann::json& part : expand_instance(entity, data)) { entities.push_back(std::move(part)); }
        }

        out["entities"] = std::move(entities);
        return out;
    }

    std::vector<nlohmann::json> expand_instance(const nlohmann::json& entry, const nlohmann::json* prefab_data)
    {
        const u64_t root_id = guid::from_json(entry["id"]);
        const nlohmann::json& entities = entities_of(prefab_data);
        const nlohmann::json parts = entry.value("parts", nlohmann::json::object());
        const nlohmann::json overrides = entry.value("overrides", nlohmann::json::object());

        auto tag = [&](nlohmann::json& state, u64_t source)
        {
            state["prefab_part"] = {
                {"instance", guid::to_json(root_id)},
                {"source",   guid::to_json(source) }
            };
        };

        // no prefab: root only, entry kept for saving
        if (entities.empty())
        {
            nlohmann::json root = {
                {"id", entry["id"]},
                {"parent", entry.value("parent", nlohmann::json{})},
                {"components", nlohmann::json::object()},
                {"prefab", entry["prefab"]},
                {"removed", entry.value("removed", nlohmann::json::array())},
                {"unresolved", entry},
            };
            tag(root, 0);
            return {root};
        }

        const std::unordered_set<u64_t> removed =
            removed_sources(entities, entry.value("removed", nlohmann::json::array()));

        part_map_t map;
        map.root_source = guid::from_json(entities[0]["id"]);
        for (const nlohmann::json& e : entities)
        {
            const u64_t source = guid::from_json(e["id"]);
            map.by_source[source] = &e;
            if (removed.contains(source)) { continue; }

            if (source == map.root_source) { map.to_instance[source] = root_id; }
            else
            {
                const u64_t known = guid::from_json(parts.value(guid::to_string(source), nlohmann::json{}));
                map.to_instance[source] = known != 0 ? known : derived_id(root_id, source);
            }
        }

        auto next_kept = [&](const nlohmann::json& e)
        {
            u64_t next = guid::from_json(e.value("next", nlohmann::json{}));
            while (next != 0 && removed.contains(next))
            {
                const auto it = map.by_source.find(next);
                next = it != map.by_source.end() ? guid::from_json(it->second->value("next", nlohmann::json{})) : 0;
            }
            return next != 0 ? map.to_instance[next] : 0;
        };

        std::vector<nlohmann::json> states;
        for (const nlohmann::json& e : entities)
        {
            const u64_t source = guid::from_json(e["id"]);
            if (removed.contains(source)) { continue; }

            nlohmann::json state = reference_state(e, map);
            state["id"] = guid::to_json(map.to_instance[source]);

            if (source == map.root_source)
            {
                state["parent"] = entry.value("parent", nlohmann::json{});
                state["next"] = entry.value("next", nlohmann::json{});
                state["prefab"] = entry["prefab"];
                state["removed"] = entry.value("removed", nlohmann::json::array());
            }
            else
            {
                const u64_t parent = guid::from_json(e.value("parent", nlohmann::json{}));
                state["parent"] = guid::to_json(parent != 0 ? map.to_instance[parent] : root_id);
                state["next"] = guid::to_json(next_kept(e));
            }

            apply_overrides(state["components"], overrides.value(guid::to_string(source), nlohmann::json::object()));
            tag(state, source);
            states.push_back(std::move(state));
        }
        return states;
    }

    nlohmann::json collapse_instance(const nlohmann::json* prefab_data, std::span<const nlohmann::json> parts)
    {
        const auto root_it =
            std::find_if(parts.begin(), parts.end(), [](const nlohmann::json& p) { return p.contains("prefab"); });
        if (root_it == parts.end()) { return {}; }
        const nlohmann::json& root = *root_it;

        if (root.contains("unresolved") && entities_of(prefab_data).empty())
        {
            nlohmann::json entry = root["unresolved"];
            entry["parent"] = root.value("parent", nlohmann::json{});
            entry["next"] = root.value("next", nlohmann::json{});
            return entry;
        }

        nlohmann::json entry = {
            {"id", root["id"]},
            {"parent", root.value("parent", nlohmann::json{})},
            {"next", root.value("next", nlohmann::json{})},
            {"prefab", root["prefab"]},
        };

        const part_map_t map = map_parts(prefab_data, parts);
        const nlohmann::json& entities = entities_of(prefab_data);

        nlohmann::json part_ids = nlohmann::json::object();
        nlohmann::json overrides = nlohmann::json::object();
        nlohmann::json removed = nlohmann::json::array();
        for (const nlohmann::json& source : root.value("removed", nlohmann::json::array()))
        {
            const u64_t id = guid::from_json(source);
            if (std::any_of(entities.begin(), entities.end(),
                            [id](const nlohmann::json& e) { return guid::from_json(e["id"]) == id; }))
            {
                removed.push_back(source);
            }
        }
        const std::unordered_set<u64_t> already_removed = removed_sources(entities, removed);

        for (const nlohmann::json& e : entities)
        {
            const u64_t source = guid::from_json(e["id"]);
            if (already_removed.contains(source)) { continue; }

            const auto part = map.by_source.find(source);
            if (part == map.by_source.end())
            {
                // part deleted in the scene, unless its parent went too
                const u64_t parent = guid::from_json(e.value("parent", nlohmann::json{}));
                if (parent == 0 || map.by_source.contains(parent)) { removed.push_back(guid::to_json(source)); }
                continue;
            }

            if (source != map.root_source) { part_ids[guid::to_string(source)] = part->second->at("id"); }

            const nlohmann::json diff = diff_components(reference_state(e, map)["components"],
                                                        part->second->value("components", nlohmann::json::object()));
            if (!diff.empty()) { overrides[guid::to_string(source)] = diff; }
        }

        if (!part_ids.empty()) { entry["parts"] = std::move(part_ids); }
        if (!overrides.empty()) { entry["overrides"] = std::move(overrides); }
        if (!removed.empty()) { entry["removed"] = std::move(removed); }
        return entry;
    }

    nlohmann::json collapse(std::span<const nlohmann::json> states, const lookup_t& lookup)
    {
        std::unordered_set<u64_t> roots;
        for (const nlohmann::json& state : states)
        {
            if (is_root_state(state)) { roots.insert(guid::from_json(state.value("id", nlohmann::json{}))); }
        }

        nlohmann::json out = nlohmann::json::array();
        std::unordered_map<u64_t, std::vector<nlohmann::json>> groups;
        std::vector<std::pair<std::size_t, u64_t>> slots;
        for (const nlohmann::json& state : states)
        {
            const u64_t id = guid::from_json(state.value("id", nlohmann::json{}));
            if (roots.contains(id))
            {
                slots.emplace_back(out.size(), id);
                out.push_back(nullptr);
                groups[id].push_back(state);
                continue;
            }
            if (const u64_t instance = part_instance(state); instance != 0 && roots.contains(instance))
            {
                groups[instance].push_back(state);
                continue;
            }

            nlohmann::json plain = state;
            strip_tags(plain);
            out.push_back(std::move(plain));
        }

        for (const auto& [index, id] : slots)
        {
            const std::vector<nlohmann::json>& parts = groups[id];
            const auto root =
                std::find_if(parts.begin(), parts.end(), [](const nlohmann::json& p) { return p.contains("prefab"); });
            out[index] = collapse_instance(entry_target(*root, lookup), parts);
        }
        return out;
    }

    nlohmann::json new_instance(const nlohmann::json& prefab_data, const nlohmann::json& prefab_ref)
    {
        nlohmann::json entry = {
            {"id",     guid::to_json(guid::generate())},
            {"prefab", prefab_ref                     },
        };

        nlohmann::json parts = nlohmann::json::object();
        const nlohmann::json& entities = entities_of(&prefab_data);
        for (std::size_t i = 1; i < entities.size(); i++)
        {
            parts[guid::to_string(guid::from_json(entities[i]["id"]))] = guid::to_json(guid::generate());
        }
        if (!parts.empty()) { entry["parts"] = std::move(parts); }
        return entry;
    }

    nlohmann::json diff_part(const nlohmann::json* prefab_data, std::span<const nlohmann::json> parts,
                             const nlohmann::json& part)
    {
        const part_map_t map = map_parts(prefab_data, parts);
        const u64_t source = part_source(part);

        for (const nlohmann::json& e : entities_of(prefab_data))
        {
            if (guid::from_json(e["id"]) != source) { continue; }
            return diff_components(reference_state(e, map)["components"],
                                   part.value("components", nlohmann::json::object()));
        }
        return nlohmann::json::object();
    }

    ecs::entity_t spawn(world_t& world, asset_t<prefab_t> prefab)
    {
        const lookup_t lookup = asset_lookup();
        const nlohmann::json* data = lookup_asset(lookup, prefab);
        if (!data) { return ecs::NULL_ENTITY; }

        const nlohmann::json entry = new_instance(*data, serialization::asset_ref(prefab));
        const nlohmann::json scene = {
            {"entities", expand_instance(entry, data)}
        };

        const std::vector<ecs::entity_t> created = serialization::deserialize_scene(world, scene);
        return created.empty() ? ecs::NULL_ENTITY : created.front();
    }
} // namespace tau::prefab
