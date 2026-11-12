#pragma once

#include "tau/asset.h"
#include "tau/defines.h"

#include "json/json.hpp"

#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace tau::asset_sources
{
    // .meta beside a foreign source (image, model, shader): its guid and import settings
    // every writer goes through here, so the file keeps one format
    TAU_ENGINE_API std::string meta_path(std::string_view source_path);
    // the object, empty when missing or unreadable
    TAU_ENGINE_API nlohmann::json read_meta(std::string_view source_path);
    TAU_ENGINE_API bool write_meta(std::string_view source_path, const nlohmann::json& meta);

    // guid of a source, made if missing, old .meta beside JSON is folded in
    TAU_ENGINE_API asset_guid_t find_or_create_guid(const std::string& source_path);

    TAU_ENGINE_API asset_guid_t guid_for_writing(const std::string& source_path);

    // merges into the map file after dropping entries under replace_prefix
    TAU_ENGINE_API void write_guid_map(const std::string& output_path,
                                       std::span<const std::pair<std::string, asset_guid_t>> entries,
                                       std::string_view replace_prefix = {});
} // namespace tau::asset_sources
