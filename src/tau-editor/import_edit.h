#pragma once

#include "tau/asset.h"

#include "json/json.hpp"
#include <string>

namespace tau
{
    struct world_t;
    struct editor_context_t;
} // namespace tau

// import settings of foreign sources
namespace tau::editor::import_edit
{
    void init(editor_context_t& ctx);
    void draw(world_t& world, const std::string& vfs);

    void revert(const std::string& vfs);

    bool remap_material(world_t& world, const std::string& model_vfs, const std::string& name, asset_handle_t material);

    const nlohmann::json& project_defaults();
    void set_project_defaults(const nlohmann::json& defaults);
} // namespace tau::editor::import_edit
