#include "tau/assets/data.h"

#include "tau/assets/value_io.h"
#include "tau/log.h"
#include "tau/os.h"
#include "tau/serialization_detail.h"
#include "tau/vfs.h"

#include "json/json.hpp"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace tau
{
    namespace
    {
        constexpr u32_t DATA_MAGIC = 0x54414454; // "TDAT"
        constexpr u32_t DATA_VERSION = 1;

        struct entry_t
        {
            void* value = nullptr;
            void (*destroy)(void*) = nullptr;
            u32_t type_hash = 0;
            u32_t version = 0;
        };

        std::mutex g_mutex;
        std::vector<data::data_type_t> g_types;
        std::vector<entry_t> g_entries;
        std::vector<u32_t> g_free;
        std::unordered_set<u64_t> g_warned;
        std::atomic<u32_t> g_version{1};

        void drop_value(entry_t& entry)
        {
            if (entry.value != nullptr && entry.destroy != nullptr) { entry.destroy(entry.value); }
            entry.value = nullptr;
        }

        // once per asset and reason
        bool first_warning(asset_handle_t asset, u32_t reason)
        { return g_warned.insert(asset.guid ^ (static_cast<u64_t>(reason) << 32)).second; }

        void collect_paths(const std::vector<scene_property_t>& props, u32_t parent, std::vector<u32_t>& out)
        {
            for (const scene_property_t& prop : props)
            {
                const u32_t path = data::path_hash(parent, prop.prop_hash);
                if (prop.type == scene_value_type_e::STRUCT) { collect_paths(prop.children, path, out); }
                else
                {
                    out.push_back(path);
                }
            }
        }

        void collect_names(const std::vector<scene_property_t>& props, std::map<u32_t, std::string>& out)
        {
            for (const scene_property_t& prop : props)
            {
                if (!prop.prop_name.empty()) { out.emplace(prop.prop_hash, prop.prop_name); }
                collect_names(prop.children, out);
            }
        }

        void apply_names(std::vector<scene_property_t>& props, const std::unordered_map<u32_t, std::string>& names)
        {
            for (scene_property_t& prop : props)
            {
                if (const auto it = names.find(prop.prop_hash); it != names.end()) { prop.prop_name = it->second; }
                apply_names(prop.children, names);
            }
        }

        void load_refs(const std::vector<scene_property_t>& props, std::vector<asset_handle_t>& out)
        {
            for (const scene_property_t& prop : props)
            {
                if (prop.type == scene_value_type_e::ASSET_REF)
                {
                    const std::string path = assets::resolve(prop.guid, prop.str);
                    if (!path.empty()) { out.push_back(assets::load(prop.asset_type, path)); }
                }
                else if (prop.type == scene_value_type_e::ASSET_REF_LIST)
                {
                    for (std::size_t i = 0; i < prop.strs.size(); i++)
                    {
                        const std::string path =
                            assets::resolve(i < prop.guids.size() ? prop.guids[i] : asset_guid_t{0}, prop.strs[i]);
                        if (!path.empty()) { out.push_back(assets::load(prop.asset_type, path)); }
                    }
                }
                load_refs(prop.children, out);
            }
        }
    } // namespace

    std::optional<data_asset_t> asset_loader_t<data_asset_t>::load(const std::string& path, std::span<const u8_t> bytes)
    {
        using namespace value_io;

        const std::string cooked = assets::cooked_path(get_type_id<data_asset_t>(), path);
        SDL_IOStream* stream = SDL_IOFromConstMem(bytes.data(), bytes.size());
        if (!stream)
        {
            TAU_LOG_ERROR("DATA", "Data asset not found: {}", cooked);
            return std::nullopt;
        }

        data_asset_t asset;
        u32_t magic = 0;
        u32_t version = 0;
        u32_t count = 0;
        bool ok = read_pod(stream, magic) && magic == DATA_MAGIC && read_pod(stream, version) &&
                  version == DATA_VERSION && read_pod(stream, asset.type_hash) && read_str(stream, asset.type_name) &&
                  read_pod(stream, count);

        for (u32_t i = 0; i < count && ok; i++) { ok = read_property(stream, asset.fields.emplace_back()); }

        u32_t name_count = 0;
        std::unordered_map<u32_t, std::string> names;
        ok = ok && read_pod(stream, name_count);
        for (u32_t i = 0; i < name_count && ok; i++)
        {
            u32_t hash = 0;
            std::string name;
            ok = read_pod(stream, hash) && read_str(stream, name);
            names.emplace(hash, std::move(name));
        }
        SDL_CloseIO(stream);

        if (!ok)
        {
            TAU_LOG_ERROR("DATA", "Invalid or old .taudata file, recook: {}", cooked);
            return std::nullopt;
        }

        apply_names(asset.fields, names);
        asset.field_set = data::field_set_of(asset.fields);
        load_refs(asset.fields, asset.refs);
        asset.version = g_version.fetch_add(1, std::memory_order_relaxed);
        return asset;
    }

    void asset_loader_t<data_asset_t>::unload(data_asset_t& asset)
    {
        if (asset.cache == UINT32_MAX) { return; }

        std::scoped_lock lock(g_mutex);
        drop_value(g_entries[asset.cache]);
        g_entries[asset.cache] = {};
        g_free.push_back(asset.cache);
        asset.cache = UINT32_MAX;
    }

    void asset_loader_t<data_asset_t>::deps(const data_asset_t& asset, std::vector<asset_handle_t>& out)
    { out.insert(out.end(), asset.refs.begin(), asset.refs.end()); }

    // the decoded value holds handles, they stay valid
    asset_dep_e asset_loader_t<data_asset_t>::dep_reloaded(data_asset_t&, asset_handle_t) { return asset_dep_e::KEEP; }
} // namespace tau

namespace tau::data
{
    namespace detail
    {
        void add_type(const data_type_t& type)
        {
            std::scoped_lock lock(g_mutex);
            for (data_type_t& existing : g_types)
            {
                if (existing.hash == type.hash)
                {
                    existing = type;
                    return;
                }
            }
            g_types.push_back(type);
        }

        const void* get(asset_handle_t handle, u32_t type_hash)
        {
            data_asset_t* asset = assets::get(asset_t<data_asset_t>{handle});
            if (asset == nullptr) { return nullptr; }

            std::scoped_lock lock(g_mutex);
            if (asset->type_hash != type_hash)
            {
                if (first_warning(handle, 1))
                {
                    TAU_LOG_ERROR("DATA", "{} holds '{}', read as another type", assets::path_of(handle),
                                  asset->type_name);
                }
                return nullptr;
            }

            if (asset->cache == UINT32_MAX)
            {
                if (g_free.empty())
                {
                    asset->cache = static_cast<u32_t>(g_entries.size());
                    g_entries.emplace_back();
                }
                else
                {
                    asset->cache = g_free.back();
                    g_free.pop_back();
                }
            }

            entry_t& entry = g_entries[asset->cache];
            if (entry.value != nullptr && entry.version == asset->version) { return entry.value; }
            drop_value(entry);

            const auto type = std::ranges::find(g_types, type_hash, &data_type_t::hash);
            reflection::ctx_t& ctx = reflection::get_engine_context();
            const reflection::type_t meta = reflection::resolve(ctx, type_hash);
            if (type == g_types.end() || !meta)
            {
                if (first_warning(handle, 2))
                {
                    TAU_LOG_ERROR("DATA", "{}: '{}' is no registered data type", assets::path_of(handle),
                                  asset->type_name);
                }
                return nullptr;
            }

            void* value = type->construct();
            reflection::any_t object = meta.from_void(value);
            serialization::detail::apply_fields(ctx, object, meta, asset->fields, {}, assets::path_of(handle));

            entry = {value, type->destroy, type_hash, asset->version};
            return value;
        }
    } // namespace detail

    bool has_field(asset_handle_t handle, u32_t path)
    {
        const data_asset_t* asset = assets::get(asset_t<data_asset_t>{handle});
        return asset != nullptr && std::ranges::binary_search(asset->field_set, path);
    }

    std::vector<data_type_t> types()
    {
        std::scoped_lock lock(g_mutex);
        return g_types;
    }

    std::optional<data_type_t> type_by_hash(u32_t hash)
    {
        std::scoped_lock lock(g_mutex);
        const auto it = std::ranges::find(g_types, hash, &data_type_t::hash);
        return it != g_types.end() ? std::optional<data_type_t>{*it} : std::nullopt;
    }

    u32_t remove_types_of(void* module_base)
    {
        std::scoped_lock lock(g_mutex);
        auto from_module = [module_base](void (*destroy)(void*))
        { return destroy != nullptr && os::module_base_of(reinterpret_cast<const void*>(destroy)) == module_base; };

        for (entry_t& entry : g_entries)
        {
            if (from_module(entry.destroy))
            {
                drop_value(entry);
                entry.destroy = nullptr;
            }
        }
        return static_cast<u32_t>(
            std::erase_if(g_types, [&](const data_type_t& type) { return from_module(type.destroy); }));
    }

    std::vector<u32_t> field_set_of(const std::vector<scene_property_t>& fields)
    {
        std::vector<u32_t> out;
        collect_paths(fields, 0, out);
        std::ranges::sort(out);
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    bool cook(const std::string& source_path, const std::string& output_path)
    {
        using namespace value_io;

        std::ifstream file(source_path);
        const nlohmann::json source = nlohmann::json::parse(file, nullptr, false);
        if (source.is_discarded() || !source.is_object() || !source.value("type", nlohmann::json{}).is_string())
        {
            TAU_LOG_ERROR("DATA", "A data asset is a JSON object with a \"type\": {}", source_path);
            return false;
        }

        const std::string type_name = source["type"].get<std::string>();
        const std::vector<scene_property_t> fields = parse_fields(source.value("fields", nlohmann::json::object()));

        // sorted, so a cook is byte for byte the same
        std::map<u32_t, std::string> names;
        collect_names(fields, names);

        std::filesystem::create_directories(std::filesystem::path(output_path).parent_path());
        std::ofstream out(output_path, std::ios::binary);
        write_pod(out, DATA_MAGIC);
        write_pod(out, DATA_VERSION);
        write_pod(out, hash_string(type_name));
        write_str(out, type_name);
        write_pod(out, static_cast<u32_t>(fields.size()));
        for (const scene_property_t& prop : fields) { write_property(out, prop); }
        write_pod(out, static_cast<u32_t>(names.size()));
        for (const auto& [hash, name] : names)
        {
            write_pod(out, hash);
            write_str(out, name);
        }
        return out.good();
    }

    std::vector<scene_property_t> parse_fields(const nlohmann::json& fields)
    {
        std::vector<scene_property_t> out;
        if (!fields.is_object()) { return out; }
        for (const auto& [key, value] : fields.items())
        {
            scene_property_t prop;
            prop.prop_hash = serialization::detail::key_to_hash(key);
            if (!serialization::detail::is_numeric_key(key)) { prop.prop_name = key; }
            if (serialization::detail::parse_value(value, key, prop)) { out.push_back(std::move(prop)); }
        }
        return out;
    }

    bool decode(u32_t type_hash, void* value, const std::vector<scene_property_t>& fields, std::string_view owner)
    {
        reflection::ctx_t& ctx = reflection::get_engine_context();
        const reflection::type_t meta = reflection::resolve(ctx, type_hash);
        if (!meta || value == nullptr) { return false; }

        reflection::any_t object = meta.from_void(value);
        serialization::detail::apply_fields(ctx, object, meta, fields, {}, std::string(owner));
        return true;
    }

    nlohmann::json encode(u32_t type_hash, void* value)
    {
        reflection::ctx_t& ctx = reflection::get_engine_context();
        const reflection::type_t meta = reflection::resolve(ctx, type_hash);
        if (!meta || value == nullptr) { return nlohmann::json::object(); }

        reflection::any_t object = meta.from_void(value);
        return serialization::detail::encode_fields(ctx, meta, object, {});
    }

    namespace
    {
        bool is_struct(const reflection::type_t& type)
        {
            return !type.is_enum() && !type.is_sequence_container() && !reflection::pod_tag(type) &&
                   type.data().begin() != type.data().end();
        }

        void unknown_in(const reflection::type_t& type, const std::vector<scene_property_t>& fields,
                        const std::string& prefix, std::vector<std::string>& out)
        {
            for (const scene_property_t& prop : fields)
            {
                const std::string name = prop.prop_name.empty() ? std::to_string(prop.prop_hash) : prop.prop_name;
                const reflection::data_t data = type.data(prop.prop_hash);
                if (!data) { out.push_back(prefix + name); }
                else if (prop.type == scene_value_type_e::STRUCT && is_struct(data.type()))
                {
                    unknown_in(data.type(), prop.children, prefix + name + ".", out);
                }
            }
        }
    } // namespace

    std::vector<std::string> unknown_fields(u32_t type_hash, const std::vector<scene_property_t>& fields)
    {
        std::vector<std::string> out;
        const reflection::type_t meta = reflection::resolve(reflection::get_engine_context(), type_hash);
        if (meta) { unknown_in(meta, fields, {}, out); }
        return out;
    }

    u32_t set_fields(asset_handle_t handle, std::vector<scene_property_t> fields)
    {
        data_asset_t* asset = assets::get(asset_t<data_asset_t>{handle});
        if (asset == nullptr) { return 0; }

        std::vector<asset_handle_t> refs;
        load_refs(fields, refs);

        std::scoped_lock lock(g_mutex);
        asset->field_set = field_set_of(fields);
        asset->fields = std::move(fields);
        asset->refs = std::move(refs);
        asset->version = g_version.fetch_add(1, std::memory_order_relaxed);
        return asset->version;
    }

    u32_t peek_type(std::string_view vfs_path)
    {
        using namespace value_io;

        SDL_IOStream* stream = vfs::open_read(assets::cooked_path(get_type_id<data_asset_t>(), vfs_path));
        if (!stream) { return 0; }

        u32_t magic = 0;
        u32_t version = 0;
        u32_t hash = 0;
        const bool ok = read_pod(stream, magic) && magic == DATA_MAGIC && read_pod(stream, version) &&
                        version == DATA_VERSION && read_pod(stream, hash);
        SDL_CloseIO(stream);
        return ok ? hash : 0;
    }
} // namespace tau::data
