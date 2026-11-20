#include "cookers.h"

#include "tau-cooker/material_cooker.h"
#include "tau-cooker/mesh_cooker.h"
#include "tau-cooker/scene_cooker.h"
#include "tau-cooker/shader_cooker.h"
#include "tau-cooker/texture_cooker.h"
#include "tau/asset_sources.h"
#include "tau/assets/import_settings.h"
#include "tau/assets/mesh.h"
#include "tau/assets/texture.h"
#include "tau/vfs.h"

#include <optional>
#include <utility>

namespace tau::cooker
{
    namespace
    {
        namespace fs = std::filesystem;

        template <bool (*Fn)(const std::string&, const std::string&)>
        cook_result_t simple(const cook_env_t&, const std::string& in, const std::string& out)
        { return {Fn(in, out), true, {fs::path(in)}}; }

        cook_result_t shader_asset(const cook_env_t& env, const std::string& in, const std::string& out)
        {
            std::vector<std::string> dirs = env.input_dirs;
            dirs.insert(dirs.end(), env.include_dirs.begin(), env.include_dirs.end());

            std::vector<fs::path> deps;
            const shader::cook_status_e status = shader::cook_shader(in, out, dirs, deps);
            if (deps.empty()) { deps.emplace_back(in); }

            return {status != shader::cook_status_e::FAILED, status == shader::cook_status_e::COOKED, std::move(deps)};
        }

        cook_result_t scene_asset(const cook_env_t& env, const std::string& in, const std::string& out)
        {
            std::vector<fs::path> deps = {fs::path(in)};
            const bool ok = scene::cook_scene(
                in, out, [&env](const std::string& vfs_path) { return resolve_source(env, vfs_path); }, deps);
            return {ok, true, std::move(deps)};
        }

        template <typename Asset, typename Settings>
        Settings settings_of(const cook_env_t& env, const std::string& in)
        {
            return import_settings::resolve_as<Settings>(get_type_id<Asset>(), env.import_defaults,
                                                         asset_sources::read_meta(in), {});
        }

        cook_result_t texture_asset(const cook_env_t& env, const std::string& in, const std::string& out)
        {
            return {texture::cook_texture(in, out, settings_of<texture_t, texture_import_t>(env, in)),
                    true,
                    {fs::path(in)}};
        }

        nlohmann::json texture_prepare(const cook_env_t& env, const std::string& in)
        { return texture::prepare_meta(in, env.import_defaults); }

        cook_result_t mesh_asset(const cook_env_t& env, const std::string& in, const std::string& out)
        { return {mesh::cook_mesh(in, out, settings_of<mesh_t, mesh_import_t>(env, in)), true, {fs::path(in)}}; }

        struct entry_t
        {
            std::string_view key;
            cooker_t cooker;
        };

        constexpr entry_t COOKERS[] = {
            {"shader",   {&shader_asset}                    },
            {"mesh",     {&mesh_asset}                      },
            {"material", {&simple<&material::cook_material>}},
            {"scene",    {&scene_asset}                     },
            {"prefab",   {&simple<&scene::cook_prefab>}     },
            {"texture",  {&texture_asset, &texture_prepare} },
        };
    } // namespace

    cooker_t cooker_for(std::string_view type_key)
    {
        for (const entry_t& entry : COOKERS)
        {
            if (entry.key == type_key) { return entry.cooker; }
        }
        return {};
    }

    std::filesystem::path resolve_source(const cook_env_t& env, const std::string& vfs_path)
    {
        const std::optional<vfs::asset_path_parts_t> parts = vfs::split_asset_path(vfs_path);
        if (!parts) { return {}; }

        const std::string rel(parts->rel);
        for (const std::string& dir : parts->ns == env.name_space ? env.input_dirs : env.include_dirs)
        {
            const fs::path candidate = fs::path(dir) / rel;
            if (fs::exists(candidate)) { return candidate; }
        }
        return {};
    }
} // namespace tau::cooker
