#pragma once

#include "tau/assets/scene.h"
#include "tau/defines.h"
#include "tau/world.h"

#include "json/json.hpp"
#include <string>
#include <vector>

namespace tau::serialization
{
    // entities in the order they are saved, so a caller can map them to the ones loading creates
    TAU_ENGINE_API nlohmann::json serialize_scene(tau::world_t& world, std::vector<ecs::entity_t>* order = nullptr);
    // returns the created entities in scene order
    TAU_ENGINE_API std::vector<ecs::entity_t> deserialize_scene(tau::world_t& world, const nlohmann::json& scene_data);

    TAU_ENGINE_API scene_t scene_from_json(const nlohmann::json& scene_data);
    TAU_ENGINE_API void write_scene_binary(const scene_t& scene, const std::string& output_path);
    TAU_ENGINE_API std::vector<ecs::entity_t> instantiate_scene(tau::world_t& world, const scene_t& scene);

    TAU_ENGINE_API void clear_scene(tau::world_t& world);
} // namespace tau::serialization
