#pragma once

#include "tau/defines.h"

#include <string>

namespace tau
{
    // components whose type was not registered when the entity loaded (game not built, type renamed)
    // kept as saved, so saving writes them back and a later load with the type applies them
    struct TAU_ENGINE_API unresolved_components_t
    {
        // JSON object: component key -> saved fields
        std::string json;
    };
} // namespace tau
