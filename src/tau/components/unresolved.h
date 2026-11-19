#pragma once

#include "tau/defines.h"

#include <string>

namespace tau
{
    // saved data the registered types did not take (game not built, type or field renamed)
    // written back on save, applied by a later load that knows it
    struct TAU_ENGINE_API unresolved_components_t
    {
        std::string json = "{}";
        std::string fields = "{}";
    };
} // namespace tau
