#include "scene_cooker.h"

#include "tau/asset.h"
#include "tau/assets/scene.h"
#include "tau/log.h"
#include "tau/prefab.h"
#include "tau/serialization.h"

#include "json/json.hpp"
#include <fstream>
#include <unordered_map>

namespace tau::cooker::scene
{
    namespace
    {
        nlohmann::json read_json(const std::string& path, const char* what)
        {
            std::ifstream file(path);
            if (!file.is_open())
            {
                TAU_LOG_ERROR("SCENE_COOKER", "Failed to open {}: {}", what, path);
                return nlohmann::json::value_t::discarded;
            }

            nlohmann::json data = nlohmann::json::parse(file, nullptr, false);
            if (data.is_discarded()) { TAU_LOG_ERROR("SCENE_COOKER", "{} is not valid JSON: {}", what, path); }
            return data;
        }
    } // namespace

    bool cook_scene(const std::string& input_path, const std::string& output_path, const resolve_fn_t& resolve,
                    std::vector<std::filesystem::path>& deps)
    {
        TAU_LOG_INFO("SCENE_COOKER", "Cooking scene: {} -> {}", input_path, output_path);

        const nlohmann::json scene_json = read_json(input_path, "scene");
        if (scene_json.is_discarded()) { return false; }

        std::unordered_map<std::string, nlohmann::json> prefabs;
        const tau::prefab::lookup_t lookup = [&](asset_guid_t guid, const std::string& path) -> const nlohmann::json*
        {
            const std::filesystem::path source = resolve(tau::assets::resolve(guid, path));
            if (source.empty()) { return nullptr; }

            auto [it, inserted] = prefabs.try_emplace(source.generic_string());
            if (inserted)
            {
                it->second = read_json(source.generic_string(), "prefab");
                deps.push_back(source);
            }
            return it->second.is_object() ? &it->second : nullptr;
        };

        const tau::scene_t scene = tau::serialization::scene_from_json(tau::prefab::expand(scene_json, lookup));

        std::filesystem::create_directories(std::filesystem::path(output_path).parent_path());
        tau::serialization::write_scene_binary(scene, output_path);
        return true;
    }

    bool cook_prefab(const std::string& input_path, const std::string& output_path)
    {
        TAU_LOG_INFO("SCENE_COOKER", "Cooking prefab: {} -> {}", input_path, output_path);

        const nlohmann::json data = read_json(input_path, "prefab");
        if (data.is_discarded()) { return false; }
        if (!data.is_object() || !data.value("entities", nlohmann::json{}).is_array() || data["entities"].empty())
        {
            TAU_LOG_ERROR("SCENE_COOKER", "A prefab needs an entities array with its root first: {}", input_path);
            return false;
        }

        std::filesystem::create_directories(std::filesystem::path(output_path).parent_path());
        const std::vector<std::uint8_t> bytes = nlohmann::json::to_cbor(data);
        std::ofstream out(output_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return out.good();
    }
} // namespace tau::cooker::scene
