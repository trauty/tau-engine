#pragma once

#include "tau/asset.h"
#include "tau/defines.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace tau::asset_sources
{
    // guid of a source, made if missing, old .meta beside JSON is folded in
    TAU_ENGINE_API asset_guid_t find_or_create_guid(const std::string& source_path);

    TAU_ENGINE_API asset_guid_t guid_for_writing(const std::string& source_path);

    // merges into the map file after dropping entries under replace_prefix
    TAU_ENGINE_API void write_guid_map(const std::string& output_path,
                                       std::span<const std::pair<std::string, asset_guid_t>> entries,
                                       std::string_view replace_prefix = {});
} // namespace tau::asset_sources
