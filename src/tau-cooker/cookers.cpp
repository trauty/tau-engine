#include "cookers.h"

#include "tau-cooker/material_cooker.h"
#include "tau-cooker/model_cooker.h"
#include "tau-cooker/scene_cooker.h"
#include "tau-cooker/shader_cooker.h"
#include "tau-cooker/texture_cooker.h"
#include "tau/asset_sources.h"
#include "tau/assets/import_settings.h"
#include "tau/assets/texture.h"
#include "tau/vfs.h"

#include <fstream>
#include <optional>
#include <utility>

namespace tau::cooker
{
    namespace
    {
        namespace fs = std::filesystem;

        template <bool (*Fn)(const std::string&, const std::string&)>
        cook_result_t simple(const cook_env_t&, const std::string& in, const std::string& out)
        { return {Fn(in, out), true, {fs::path(in)}, {}}; }

        cook_result_t shader_asset(const cook_env_t& env, const std::string& in, const std::string& out)
        {
            std::vector<std::string> dirs = env.input_dirs;
            dirs.insert(dirs.end(), env.include_dirs.begin(), env.include_dirs.end());

            std::vector<fs::path> deps;
            const shader::cook_status_e status = shader::cook_shader(in, out, dirs, deps);
            if (deps.empty()) { deps.emplace_back(in); }

            return {
                status != shader::cook_status_e::FAILED, status == shader::cook_status_e::COOKED, std::move(deps), {}};
        }

        model::context_t model_context(const cook_env_t& env, const std::string& in)
        {
            return {&env.import_defaults, vfs_of(env, in), [&env](const fs::path& file) { return vfs_of(env, file); }};
        }

        cook_result_t scene_asset(const cook_env_t& env, const std::string& in, const std::string& out)
        {
            std::vector<fs::path> deps = {fs::path(in)};

            const auto prefab = [&env](const std::string& vfs_path,
                                       std::vector<fs::path>& prefab_deps) -> nlohmann::json
            {
                if (const std::optional<assets::sub_asset_path_t> sub = assets::split_sub_asset(vfs_path))
                {
                    const fs::path model = resolve_source(env, std::string(sub->model));
                    if (model.empty()) { return nlohmann::json::value_t::discarded; }

                    prefab_deps.push_back(model);
                    prefab_deps.emplace_back(asset_sources::meta_path(model.generic_string()));
                    nlohmann::json data = model::prefab_data(model.generic_string(), std::string(sub->name),
                                                             model_context(env, model.generic_string()));
                    return data.is_null() ? nlohmann::json(nlohmann::json::value_t::discarded) : data;
                }

                const fs::path source = resolve_source(env, vfs_path);
                if (source.empty()) { return nlohmann::json::value_t::discarded; }

                prefab_deps.push_back(source);
                std::ifstream file(source);
                return nlohmann::json::parse(file, nullptr, false);
            };

            const bool ok = scene::cook_scene(in, out, prefab, deps);
            return {ok, true, std::move(deps), {}};
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
                    {fs::path(in)},
                    {}};
        }

        nlohmann::json texture_prepare(const cook_env_t& env, const std::string& in)
        { return texture::prepare_meta(in, env.import_defaults); }

        cook_result_t model_asset(const cook_env_t& env, const std::string& in, const std::string& out)
        {
            cook_result_t result{false, true, {fs::path(in)}, {}};
            result.ok = model::cook(in, out, model_context(env, in), result.outputs);
            return result;
        }

        nlohmann::json model_prepare(const cook_env_t& env, const std::string& in)
        { return model::prepare_meta(in, model_context(env, in)); }

        std::vector<std::pair<std::string, u64_t>> model_list(const cook_env_t& env, const std::string& in)
        {
            std::vector<std::pair<std::string, u64_t>> out;
            for (const model::output_t& output : model::list_outputs(in, model_context(env, in)))
            {
                out.emplace_back(output.name, output.guid);
            }
            return out;
        }

        struct entry_t
        {
            std::string_view key;
            cooker_t cooker;
        };

        constexpr entry_t COOKERS[] = {
            {"shader",   {&shader_asset}                            },
            {"model",    {&model_asset, &model_prepare, &model_list}},
            {"material", {&simple<&material::cook_material>}        },
            {"scene",    {&scene_asset}                             },
            {"prefab",   {&simple<&scene::cook_prefab>}             },
            {"texture",  {&texture_asset, &texture_prepare}         },
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

    std::string vfs_of(const cook_env_t& env, const fs::path& file)
    {
        std::error_code ec;
        const fs::path canonical = fs::weakly_canonical(file, ec);
        for (const std::string& dir : env.input_dirs)
        {
            const std::string rel = fs::relative(canonical, fs::weakly_canonical(dir, ec), ec).generic_string();
            if (!ec && !rel.empty() && rel != "." && !rel.starts_with(".."))
            {
                return vfs::asset_path(env.name_space, rel);
            }
        }
        return {};
    }
} // namespace tau::cooker
