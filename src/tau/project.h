#pragma once

#include "defines.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace tau
{
    // 2: "engine" holds only the version the project is made for, no path
    constexpr int TAU_PROJECT_VERSION = 2;

    struct project_t
    {
        std::string project_name;
        std::string game_lib_name;

        int project_version = 0;

        std::string startup_scene;
        // major.minor
        std::string engine_version;

        std::filesystem::path project_dir;
        std::filesystem::path bin_dir;
        std::filesystem::path assets_dir;
        std::filesystem::path cooked_assets_dir;
        std::filesystem::path lib_path;
        std::filesystem::path trigger_path;

        TAU_ENGINE_API static bool load(const std::filesystem::path& project_file, project_t& out);
        // rewrites the file as the current schema, keeping its other fields and their order
        TAU_ENGINE_API static bool set_engine_version(const std::filesystem::path& project_file,
                                                      const std::string& version);
    };

    // "0.1.3" -> "0.1"
    TAU_ENGINE_API std::string major_minor(std::string_view version);
    // compares major.minor only, <0 when a is older
    TAU_ENGINE_API int compare_major_minor(std::string_view a, std::string_view b);
} // namespace tau
