#pragma once

#include "json/json.hpp"
#include <string>

namespace tau::cooker::material
{
    bool cook_material(const std::string& input_path, const std::string& output_path);
    bool write_material(const nlohmann::json& mat_json, const std::string& output_path, const std::string& owner);
} // namespace tau::cooker::material
