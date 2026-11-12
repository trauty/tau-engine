#pragma once

#include "tau/asset.h"
#include "tau/assets/scene_format.h"
#include "tau/math.h"

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

        i32_t i = 0;
        u32_t u = 0;
        f32 f = 0.0f;
        bool b = false;
        vec3_t vec = {};
        std::string str;
        u64_t asset_type = 0;
        std::vector<std::string> strs;

        // asset refs: the guid recorded beside the path in `str` / `strs`
        asset_guid_t guid = 0;
        std::vector<asset_guid_t> guids;

        u64_t ref = 0;

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

    struct scene_entity_t
    {
        u64_t id = 0;
        u64_t parent = 0;
        std::vector<scene_component_t> components;

        // prefab instances, expanded in cooked scenes
        u64_t prefab_instance = 0;
        u64_t prefab_source = 0;
        bool prefab_root = false;
        asset_guid_t prefab_guid = 0;
        std::string prefab_path;
        std::vector<u64_t> prefab_removed;
        std::string prefab_unresolved;
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
