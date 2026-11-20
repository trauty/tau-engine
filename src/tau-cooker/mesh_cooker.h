#pragma once

#include "tau/assets/import_settings.h"

#include <string>

namespace tau::cooker::mesh
{
    bool cook_mesh(const std::string& input_path, const std::string& output_path, const mesh_import_t& settings);
}
