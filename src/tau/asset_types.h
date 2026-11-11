#pragma once

#include "tau/asset.h"
#include "tau/reflection.h"

#include <concepts>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// asset type: T declares type_key, asset_loader_t<T> loads, reg<T> registers
// engine types only: cooker cannot cook game assets, unloading would leave dangling functions
namespace tau::assets
{
    TAU_ENGINE_API void add_type(const asset_type_t& type);

    TAU_ENGINE_API void register_engine_types();

    namespace detail
    {
        template <typename T>
        std::vector<asset_handle_t> untyped_list(const std::vector<asset_t<T>>& list)
        { return {list.begin(), list.end()}; }

        template <typename T>
        std::vector<asset_t<T>> typed_list(const std::vector<asset_handle_t>& list)
        {
            std::vector<asset_t<T>> out;
            out.reserve(list.size());
            for (const asset_handle_t& handle : list) { out.emplace_back(handle); }
            return out;
        }
    } // namespace detail

    template <typename T>
    void reg(asset_type_t type)
    {
        using loader_t = asset_loader_t<T>;

        type.key = T::type_key;
        type.id = get_type_id<T>();

        type.create = []() -> void* { return new T(); };
        type.destroy = [](void* asset) { delete static_cast<T*>(asset); };
        type.move = [](void* to, void* from) { *static_cast<T*>(to) = std::move(*static_cast<T*>(from)); };
        type.load = [](void* out, const std::string& path)
        {
            std::optional<T> loaded = loader_t::load(path);
            if (!loaded) { return false; }

            *static_cast<T*>(out) = std::move(*loaded);
            return true;
        };

        if constexpr (requires(T& asset) { loader_t::unload(asset); })
        {
            type.unload = [](void* asset) { loader_t::unload(*static_cast<T*>(asset)); };
        }

        if constexpr (requires(const T& asset, std::vector<asset_handle_t>& out) { loader_t::deps(asset, out); })
        {
            type.deps = [](const void* asset, std::vector<asset_handle_t>& out)
            { loader_t::deps(*static_cast<const T*>(asset), out); };
        }

        if constexpr (requires(T& asset, asset_handle_t dep) {
                          { loader_t::dep_reloaded(asset, dep) } -> std::same_as<asset_dep_e>;
                      })
        {
            type.dep_reloaded = [](void* asset, asset_handle_t dep)
            { return loader_t::dep_reloaded(*static_cast<T*>(asset), dep); };
        }

        type.reflect = [](entt::meta_ctx& ctx)
        {
            reflection::factory<asset_t<T>>(ctx).template conv<asset_handle_t>();
            reflection::factory<asset_handle_t>(ctx).template conv<asset_t<T>>();
            reflection::factory<std::vector<asset_t<T>>>(ctx).template conv<&detail::untyped_list<T>>();
            reflection::factory<std::vector<asset_handle_t>>(ctx).template conv<&detail::typed_list<T>>();
        };

        add_type(type);
    }
} // namespace tau::assets
