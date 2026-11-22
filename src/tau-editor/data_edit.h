#pragma once

#include "tau/defines.h"

#include <filesystem>
#include <string>

namespace tau
{
    struct world_t;
    struct editor_context_t;
} // namespace tau

// data assets: inspector (override per field for partial types), undo, save, New > Data
namespace tau::editor::data_edit
{
    void init(editor_context_t& ctx);

    // a New > Data submenu, returns the picked data type's hash or 0
    u32_t new_menu(bool enabled);
    // New <Label>.data in dir: every field for a full type, none for a partial one; returns rel or empty
    std::string create(world_t& world, const std::string& dir, u32_t type_hash);

    // .data sources under root naming an unknown type or field, logged
    void report_unknown(const std::filesystem::path& root);
} // namespace tau::editor::data_edit
