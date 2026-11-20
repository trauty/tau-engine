#pragma once

#include "tau-editor/editor_context.h"
#include "tau/reflection.h"
#include "tau/world.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tau::editor::panels
{
    void draw_inspector(world_t& world, editor_context_t& ctx);
    bool draw_fields(world_t& world, reflection::any_t& value, std::span<const std::string_view> skip = {});

    using slot_labels_fn_t = std::vector<std::string> (*)(const reflection::any_t& instance);
    void add_slot_labels(u32_t type_id, u32_t field_id, slot_labels_fn_t labels);
} // namespace tau::editor::panels