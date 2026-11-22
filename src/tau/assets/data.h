#pragma once

#include "tau/asset.h"
#include "tau/assets/scene.h"
#include "tau/defines.h"
#include "tau/hash.h"
#include "tau/nameof.h"
#include "tau/reflection.h"

#include "json/json.hpp"
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tau
{
    // a reflected struct's values as an asset: <name>.data holds the type name and its fields in the scene encoding
    struct data_asset_t
    {
        static constexpr std::string_view type_key = "data";

        u32_t type_hash = 0;
        std::string type_name;
        std::vector<scene_property_t> fields;
        // leaf paths present in the file, sorted (data::path_hash)
        std::vector<u32_t> field_set;
        // assets the fields reference, kept loaded with it
        std::vector<asset_handle_t> refs;
        // new on every load and edit, the decoded value follows it
        u32_t version = 0;
        // decoded value entry, owned by the data module
        u32_t cache = UINT32_MAX;
    };

    template <>
    struct TAU_ENGINE_API asset_loader_t<data_asset_t>
    {
        static std::optional<data_asset_t> load(const std::string& path, std::span<const u8_t> bytes);
        static void unload(data_asset_t& asset);
        static void deps(const data_asset_t& asset, std::vector<asset_handle_t>& out);
        static asset_dep_e dep_reloaded(data_asset_t& asset, asset_handle_t dep);
    };

    // a field naming a data asset of T: saved as an asset reference, the inspector offers only T's assets
    template <typename T>
    struct data_ref_t
    {
        asset_handle_t handle;

        data_ref_t() = default;
        explicit data_ref_t(asset_handle_t untyped) : handle(untyped) {}

        bool is_valid() const { return handle.is_valid(); }
        explicit operator bool() const { return is_valid(); }
        operator asset_handle_t() const { return handle; }
        bool operator==(const data_ref_t& other) const { return handle == other.handle; }
    };
} // namespace tau

namespace tau::data
{
    template <typename T>
    constexpr u32_t type_hash()
    { return hash_string(type_name<T>()); }

    // a field below its parent's path, 0 is the root
    constexpr u32_t path_hash(u32_t parent, u32_t field)
    {
        if (parent == 0) { return field; }
        u32_t h = parent ^ 0x9e3779b9u;
        h = (h ^ field) * 0x01000193u;
        return h ^ (h >> 15);
    }

    // "shadows.distance_scale"
    constexpr u32_t path_hash(std::string_view dotted)
    {
        u32_t path = 0;
        std::size_t start = 0;
        while (start <= dotted.size())
        {
            const std::size_t dot = dotted.find('.', start);
            const std::size_t end = dot == std::string_view::npos ? dotted.size() : dot;
            path = path_hash(path, hash_string(dotted.substr(start, end - start)));
            if (dot == std::string_view::npos) { break; }
            start = dot + 1;
        }
        return path;
    }

    struct data_type_t
    {
        u32_t hash = 0;
        std::string_view name;
        const char* label = "";
        // the inspector shows an override toggle per field, unset fields keep the struct's defaults
        bool partial = false;
        void* (*construct)() = nullptr;
        void (*destroy)(void* value) = nullptr;
    };

    namespace detail
    {
        TAU_ENGINE_API void add_type(const data_type_t& type);
        TAU_ENGINE_API const void* get(asset_handle_t asset, u32_t type_hash);
    } // namespace detail

    // decoded on first use, valid until the asset reloads or is edited, its type's library closes or collect()
    template <typename T>
    const T* get(asset_t<data_asset_t> asset)
    { return static_cast<const T*>(detail::get(asset.handle, type_hash<T>())); }

    template <typename T>
    const T* get(data_ref_t<T> ref)
    { return static_cast<const T*>(detail::get(ref.handle, type_hash<T>())); }

    // the field path is set in the file (partial types)
    TAU_ENGINE_API bool has_field(asset_handle_t asset, u32_t path);

    TAU_ENGINE_API std::vector<data_type_t> types();
    TAU_ENGINE_API std::optional<data_type_t> type_by_hash(u32_t hash);
    // a game library's types and their decoded values, before it is freed
    TAU_ENGINE_API u32_t remove_types_of(void* module_base);

    // leaf paths of a field list
    TAU_ENGINE_API std::vector<u32_t> field_set_of(const std::vector<scene_property_t>& fields);

    // source .data (JSON) -> cooked binary
    TAU_ENGINE_API bool cook(const std::string& source_path, const std::string& output_path);

    // editor and tools: values of a registered type as an untyped pointer (data_type_t::construct)
    TAU_ENGINE_API std::vector<scene_property_t> parse_fields(const nlohmann::json& fields);
    TAU_ENGINE_API bool decode(u32_t type_hash, void* value, const std::vector<scene_property_t>& fields,
                               std::string_view owner);
    // every field, tagged as in the source
    TAU_ENGINE_API nlohmann::json encode(u32_t type_hash, void* value);
    // dotted paths of fields the type does not have
    TAU_ENGINE_API std::vector<std::string> unknown_fields(u32_t type_hash,
                                                           const std::vector<scene_property_t>& fields);
    // replaces a loaded asset's fields (an edit), returns its new version
    TAU_ENGINE_API u32_t set_fields(asset_handle_t asset, std::vector<scene_property_t> fields);
    // the type hash in a cooked file, 0 when unreadable
    TAU_ENGINE_API u32_t peek_type(std::string_view vfs_path);
} // namespace tau::data

namespace tau::reflection
{
    template <typename T>
    struct asset_field_t<data_ref_t<T>>
    {
        static constexpr u64_t type_id = get_type_id<data_asset_t>();
        static constexpr bool is_list = false;
        static constexpr u32_t data_type = data::type_hash<T>();
    };

    // a struct that data assets hold: fields as for structure<T>, no entity fields (a data asset lives in no world)
    template <typename T>
    class data_type_builder_t : public fields_t<T, data_type_builder_t<T>>
    {
      public:
        static constexpr bool NO_ENTITY_FIELDS = true;

        data_type_builder_t(ctx_t& ctx, const char* label) : fields_t<T, data_type_builder_t<T>>(ctx, label)
        {
            factory<data_ref_t<T>>(ctx).template conv<asset_handle_t>();
            factory<asset_handle_t>(ctx).template conv<data_ref_t<T>>();

            m_type = {
                .hash = data::type_hash<T>(),
                .name = type_name<T>(),
                .label = label,
                .construct = []() -> void* { return new T(); },
                .destroy = [](void* value) { delete static_cast<T*>(value); },
            };
            data::detail::add_type(m_type);
        }

        data_type_builder_t& partial()
        {
            m_type.partial = true;
            data::detail::add_type(m_type);
            return *this;
        }

      private:
        data::data_type_t m_type;
    };

    template <typename T>
    data_type_builder_t<T> data_type(ctx_t& ctx, const char* label)
    { return data_type_builder_t<T>{ctx, label}; }
} // namespace tau::reflection
