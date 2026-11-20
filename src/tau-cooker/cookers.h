#pragma once

#include "tau/defines.h"

#include "json/json.hpp"
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tau::cooker
{
    struct cook_env_t
    {
        std::string name_space;
        std::vector<std::string> input_dirs;
        std::vector<std::string> include_dirs;
        // the project's import_defaults, empty for the engine's own assets
        nlohmann::json import_defaults = nlohmann::json::object();
    };

    struct cook_result_t
    {
        bool ok = false;
        bool produced_output = true;
        std::vector<std::filesystem::path> deps;
        // files written beside `out` (a models sub assets)
        std::vector<std::filesystem::path> outputs;
    };

    using cook_fn_t = cook_result_t (*)(const cook_env_t& env, const std::string& in, const std::string& out);

    using prepare_fn_t = nlohmann::json (*)(const cook_env_t& env, const std::string& in);

    // a source with sub assets: their names and guids, before any cook so references resolve
    using list_fn_t = std::vector<std::pair<std::string, u64_t>> (*)(const cook_env_t& env, const std::string& in);

    struct cooker_t
    {
        cook_fn_t cook = nullptr;
        prepare_fn_t prepare = nullptr;
        list_fn_t list = nullptr;
    };

    cooker_t cooker_for(std::string_view type_key);

    std::filesystem::path resolve_source(const cook_env_t& env, const std::string& vfs_path);
    // a file under an input dir to its vfs path, empty elsewhere
    std::string vfs_of(const cook_env_t& env, const std::filesystem::path& file);
} // namespace tau::cooker
