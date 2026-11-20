#pragma once

#include "json/json.hpp"
#include <filesystem>
#include <string>
#include <string_view>
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
    };

    using cook_fn_t = cook_result_t (*)(const cook_env_t& env, const std::string& in, const std::string& out);

    using prepare_fn_t = nlohmann::json (*)(const cook_env_t& env, const std::string& in);

    struct cooker_t
    {
        cook_fn_t cook = nullptr;
        prepare_fn_t prepare = nullptr;
    };

    cooker_t cooker_for(std::string_view type_key);

    std::filesystem::path resolve_source(const cook_env_t& env, const std::string& vfs_path);
} // namespace tau::cooker
