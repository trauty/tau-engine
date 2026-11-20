#pragma once

#include "tau/assets/import_settings.h"

#include "json/json.hpp"
#include <span>
#include <string>

namespace tau::cooker::texture
{
    nlohmann::json prepare_meta(const std::string& input_path, const nlohmann::json& project_defaults);

    bool cook_texture(const std::string& input_path, const std::string& output_path, const texture_import_t& settings);

    bool cook_texture_bytes(std::span<const u8_t> bytes, const std::string& extension, const std::string& output_path,
                            const texture_import_t& settings, const std::string& owner);
} // namespace tau::cooker::texture
