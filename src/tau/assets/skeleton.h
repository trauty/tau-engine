#pragma once

#include "tau/asset.h"
#include "tau/defines.h"
#include "tau/math.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tau
{
    struct TAU_ENGINE_API joint_t
    {
        std::string name;
        // -1 for a root
        i32 parent = -1;
        vec3 translation;
        quat rotation;
        vec3 scale = {1.0f, 1.0f, 1.0f};
        mat4 inverse_bind;
    };

    // joints are pose arrays, not entities; in the skin's order, which the mesh's joint indices use
    struct TAU_ENGINE_API skeleton_t
    {
        static constexpr std::string_view type_key = "skeleton";

        std::vector<joint_t> joints;
    };

    template <>
    struct TAU_ENGINE_API asset_loader_t<skeleton_t>
    {
        static std::optional<skeleton_t> load(const std::string& path, std::span<const u8_t> bytes);
    };
} // namespace tau
