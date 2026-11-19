#pragma once

#include "tau/asset.h"
#include "tau/assets/scene_format.h"
#include "tau/math.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tau
{
    struct scene_property_t
    {
        u32_t prop_hash = 0;
        std::string prop_name;
        scene_value_type_e type = scene_value_type_e::I32;

        std::array<u8_t, MAX_POD_SIZE> pod{};
        std::string str;
        u64_t asset_type = 0;
        std::vector<std::string> strs;

        // asset refs: the guid recorded beside the path in `str` / `strs`
        asset_guid_t guid = 0;
        std::vector<asset_guid_t> guids;

        u64_t ref = 0;
        bool ref_is_index = false;

        // STRUCT fields, LIST elements
        std::vector<scene_property_t> children;
    };

    struct scene_component_t
    {
        u32_t type_hash = 0;
        std::string type_name;
        std::vector<scene_property_t> properties;
        // JSON scenes only: the fields as saved, kept if the type is unknown
        std::string source_json;
    };

    struct scene_prefab_t
    {
        u64_t instance = 0;
        u64_t source = 0;

        bool root = false;
        asset_guid_t guid = 0;
        std::string path;
        std::vector<u64_t> removed;
        std::string unresolved;
    };

    struct scene_entity_t
    {
        u64_t id = 0;
        u64_t parent = 0;
        std::vector<scene_component_t> components;

        scene_prefab_t prefab;
    };

    struct scene_t
    {
        static constexpr std::string_view type_key = "scene";

        std::vector<scene_entity_t> entities;
        // older scene: ids made up
        bool generated_ids = false;
    };

    template <>
    struct TAU_ENGINE_API asset_loader_t<scene_t>
    {
        static std::optional<scene_t> load(const std::string& path);
    };
} // namespace tau
