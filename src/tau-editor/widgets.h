#pragma once

#include "tau/asset.h"

#include <string>
#include <string_view>

namespace tau { struct world_t; }

namespace tau::editor::widgets
{
    std::string_view asset_file_name(std::string_view path);

    bool asset_unsaved(asset_handle_t handle);

    bool asset_slot(world_t& world, const char* label, u64_t asset_type_hash, asset_handle_t handle,
                    asset_handle_t& out_handle, const std::string& popup_id, bool* out_remove = nullptr);
} // namespace tau::editor::widgets
