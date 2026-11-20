#pragma once

#include "tau/asset.h"

#include "json/json.hpp"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace tau::cooker::model
{
    struct output_t
    {
        std::string name;
        u64_t type_id = 0;
        asset_guid_t guid = 0;
    };

    struct context_t
    {
        const nlohmann::json* project_defaults = nullptr;
        std::string model_vfs;
        std::function<std::string(const std::filesystem::path& file)> vfs_of;
    };

    // sub-asset names and guids
    std::vector<output_t> list_outputs(const std::string& source, const context_t& ctx);

    // external images get their kind from the materials using them
    nlohmann::json prepare_meta(const std::string& source, const context_t& ctx);

    // every sub-asset into <dir of manifest>/<source file name>/, then the manifest
    bool cook(const std::string& source, const std::string& manifest_path, const context_t& ctx,
              std::vector<std::filesystem::path>& outputs);

    // the read-only prefab sub-asset `name`, null when the model has none of that name
    nlohmann::json prefab_data(const std::string& source, const std::string& name, const context_t& ctx);
} // namespace tau::cooker::model
