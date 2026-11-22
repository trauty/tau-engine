#pragma once

#include "tau/defines.h"
#include "tau/hash.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace entt { class meta_ctx; }

namespace tau
{
    using asset_guid_t = u64_t;

    enum class asset_state_e : u8_t
    {
        NONE,
        QUEUED,
        LOADING,
        READY,
        FAILED,
        // in flight like LOADING, which state() reports for them: the IO thread reads the file, its bytes wait
        READING,
        READ,
    };

    struct asset_handle_t
    {
        asset_guid_t guid = 0;
        u32_t slot = UINT32_MAX;

        bool is_valid() const { return guid != 0; }
        explicit operator bool() const { return is_valid(); }
        bool operator==(const asset_handle_t& other) const { return guid == other.guid; }
    };

    template <typename T>
    struct asset_t
    {
        asset_handle_t handle;

        asset_t() = default;
        explicit asset_t(asset_handle_t untyped) : handle(untyped) {}

        asset_guid_t guid() const { return handle.guid; }
        bool is_valid() const { return handle.is_valid(); }
        explicit operator bool() const { return is_valid(); }
        operator asset_handle_t() const { return handle; }
        bool operator==(const asset_t& other) const { return handle == other.handle; }
    };

    enum class asset_dep_e : u8_t
    {
        KEEP,
        RELOAD,
    };

    // specialized next to each asset type:
    //   static std::optional<T> load(const std::string& path,                required, the cooked file's bytes
    //                                std::span<const u8_t> bytes);
    //   static void unload(T& asset);                                        GPU resources and the like
    //   static void deps(const T& asset, std::vector<asset_handle_t>& out);  assets it uses, kept loaded with it,
    //                                                                        runs under the table's lock: copy, call
    //                                                                        nothing
    //   static asset_dep_e dep_reloaded(T& asset, asset_handle_t dep);       one of those reloaded in place
    template <typename T>
    struct asset_loader_t;

    struct asset_type_t
    {
        // name in files and tools, id is its hash
        std::string_view key;
        std::string_view display_name;

        std::string_view cooked_extension;
        // authored extensions, lowercase with dot
        std::vector<std::string_view> source_extensions;

        // engine JSON: guid in a "guid" field
        // foreign formats: guid in a .meta beside them
        bool guid_in_source = false;

        u32_t import_type = 0;

        bool has_sub_assets = false;
        std::string_view sub_extension;

        u64_t id = 0;

        void* (*create)() = nullptr;
        void (*destroy)(void* asset) = nullptr;
        void (*move)(void* to, void* from) = nullptr;
        bool (*load)(void* out, const std::string& path, std::span<const u8_t> bytes) = nullptr;
        void (*unload)(void* asset) = nullptr;
        void (*deps)(const void* asset, std::vector<asset_handle_t>& out) = nullptr;
        asset_dep_e (*dep_reloaded)(void* asset, asset_handle_t dep) = nullptr;
        void (*reflect)(entt::meta_ctx& ctx) = nullptr;
    };
} // namespace tau

// assets live in one table, a handle finds them by guid
// no refcounts: collect() frees what no world, pin or kept asset uses
namespace tau::assets
{
    namespace detail
    {
        TAU_ENGINE_API asset_handle_t load(u64_t type_id, std::string_view path, bool async);
        TAU_ENGINE_API void* get(u64_t type_id, asset_handle_t handle);
    } // namespace detail

    // loads once per path; other threads wait
    template <typename T>
    asset_t<T> load(std::string_view path)
    { return asset_t<T>{detail::load(get_type_id<T>(), path, false)}; }

    // returns at once, a job loads it, state() follows it
    template <typename T>
    asset_t<T> load_async(std::string_view path)
    { return asset_t<T>{detail::load(get_type_id<T>(), path, true)}; }

    // null until loaded or on failure, valid until next collect/reload
    template <typename T>
    T* get(asset_t<T> asset)
    { return static_cast<T*>(detail::get(get_type_id<T>(), asset.handle)); }

    TAU_ENGINE_API asset_handle_t load(u64_t type_id, std::string_view path);

    TAU_ENGINE_API asset_state_e state(asset_handle_t handle);
    // loads started and not finished yet, sync and async
    TAU_ENGINE_API u32_t in_flight();
    TAU_ENGINE_API u64_t type_of(asset_handle_t handle);
    // load path, else guid map path, else empty
    TAU_ENGINE_API std::string path_of(asset_handle_t handle);

    TAU_ENGINE_API asset_handle_t find(asset_guid_t guid);
    TAU_ENGINE_API asset_handle_t find(std::string_view path);

    // survives collect(), for handles outside a world
    TAU_ENGINE_API asset_handle_t pin(asset_handle_t handle);
    TAU_ENGINE_API void unpin(asset_handle_t handle);

    template <typename T>
    asset_t<T> pin(asset_t<T> asset)
    {
        pin(asset.handle);
        return asset;
    }

    // frees assets no pin or root reaches, in flight loads stay
    // for wholesale reference changes (scene open, end of Play)
    TAU_ENGINE_API void collect();

    using roots_fn_t = void (*)(std::vector<asset_handle_t>& out);
    TAU_ENGINE_API void add_roots(roots_fn_t fn);

    // reruns the loader in place, dependents refresh or reload, handles stay valid
    // failed asset retries, failed reload keeps the old one
    TAU_ENGINE_API bool reload(std::string_view path);
    TAU_ENGINE_API bool reload(asset_handle_t handle);

    TAU_ENGINE_API void shutdown();

    // guid map: guid -> path, from guid_map.json, editor keeps it current
    TAU_ENGINE_API void read_guid_map(const std::string& path);
    TAU_ENGINE_API void merge_guid_map(const std::string& json_text);

    TAU_ENGINE_API asset_guid_t guid_of(std::string_view path);
    // reference as stored: guid first, path as fallback
    TAU_ENGINE_API std::string resolve(asset_guid_t guid, std::string_view recorded_path);
    // source moved/added/removed: map and loaded asset follow
    TAU_ENGINE_API void set_location(std::string_view path, asset_guid_t guid);
    TAU_ENGINE_API void remove_location(std::string_view path);
    TAU_ENGINE_API std::vector<std::pair<std::string, asset_guid_t>> locations(std::string_view prefix = {});

    // 16 hex digits, old UUID text maps to its old id
    TAU_ENGINE_API asset_guid_t parse_guid(std::string_view text);
    TAU_ENGINE_API std::string guid_text(asset_guid_t guid);

    TAU_ENGINE_API std::span<const asset_type_t> types();
    TAU_ENGINE_API const asset_type_t* type_by_id(u64_t id);
    TAU_ENGINE_API const asset_type_t* type_by_key(std::string_view key);
    TAU_ENGINE_API const asset_type_t* type_by_path(std::string_view path_or_ext);
    TAU_ENGINE_API bool path_is(std::string_view path, u64_t type_id);

    template <typename T>
    bool path_is(std::string_view path)
    { return path_is(path, get_type_id<T>()); }
    TAU_ENGINE_API std::string cooked_path(u64_t type_id, std::string_view source_path);

    struct sub_asset_path_t
    {
        std::string_view model;
        std::string_view name;
    };

    // <model>/<name>, nullopt for any other path
    TAU_ENGINE_API std::optional<sub_asset_path_t> split_sub_asset(std::string_view path);
} // namespace tau::assets
