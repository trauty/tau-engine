#pragma once

#include "json/json.hpp"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace tau::cooker::scene
{
    // prefab data by vfs path (discarded when missing), the files it was made from go into deps
    using prefab_fn_t =
        std::function<nlohmann::json(const std::string& vfs_path, std::vector<std::filesystem::path>& deps)>;

    // prefab instances are expanded into plain entities, the prefabs read become dependencies
    bool cook_scene(const std::string& input_path, const std::string& output_path, const prefab_fn_t& prefab,
                    std::vector<std::filesystem::path>& deps);

    bool cook_prefab(const std::string& input_path, const std::string& output_path);
} // namespace tau::cooker::scene
