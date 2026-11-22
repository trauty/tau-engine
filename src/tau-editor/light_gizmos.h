#pragma once

#include "tau/defines.h"
#include "tau/ecs_fwd.h"

namespace tau { struct editor_context_t; }

namespace tau::editor::light_gizmos { void draw(ecs::registry_t& reg, const editor_context_t& ctx, u32_t view_hash); }