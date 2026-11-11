#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace tau::cooker::scene
{
    using resolve_fn_t = std::function<std::filesystem::path(const std::string& vfs_path)>;

    // prefab instances are expanded into plain entities, the prefabs read become dependencies
    bool cook_scene(const std::string& input_path, const std::string& output_path, const resolve_fn_t& resolve,
                    std::vector<std::filesystem::path>& deps);

    bool cook_prefab(const std::string& input_path, const std::string& output_path);
} // namespace tau::cooker::scene
