#pragma once

#include "tau/asset.h"

#include "json/json.hpp"

namespace tau { struct editor_context_t; } // namespace tau

namespace tau::editor::material_edit
{
    void init(editor_context_t& ctx);

    nlohmann::json to_json(asset_handle_t handle);
} // namespace tau::editor::material_edit
