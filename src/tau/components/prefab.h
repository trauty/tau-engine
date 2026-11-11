#pragma once

#include "tau/asset.h"
#include "tau/defines.h"

#include <string>
#include <vector>

namespace tau
{
    struct prefab_t;

    struct TAU_ENGINE_API prefab_part_t
    {
        u64_t instance = 0;
        u64_t source = 0;
    };

    struct TAU_ENGINE_API prefab_instance_t
    {
        asset_t<prefab_t> prefab;
        std::vector<u64_t> removed;
        // scene entry kept as JSON while the prefab is missing
        std::string unresolved;
    };
} // namespace tau
