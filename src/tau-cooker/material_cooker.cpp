#include "material_cooker.h"

#include "tau/asset.h"
#include "tau/assets/material_format.h"
#include "tau/hash.h"
#include "tau/log.h"

#include "json/json.hpp"
#include <filesystem>
#include <fstream>
#include <vector>

namespace tau::cooker::material
{
    bool cook_material(const std::string& input_path, const std::string& output_path)
    {
        TAU_LOG_INFO("MATERIAL_COOKER", "Cooking material: {} -> {}", input_path, output_path);

        std::ifstream file(input_path);
        if (!file.is_open())
        {
            TAU_LOG_ERROR("MATERIAL_COOKER", "Failed to open material file: {}", input_path);
            return false;
        }

        // parse without throwing: a typo in a hand edited .mat is a cook error, not a crash
        const nlohmann::json mat_json = nlohmann::json::parse(file, nullptr, false);
        if (mat_json.is_discarded() || !mat_json.is_object())
        {
            TAU_LOG_ERROR("MATERIAL_COOKER", "Material file is not valid JSON: {}", input_path);
            return false;
        }

        return write_material(mat_json, output_path, input_path);
    }

    bool write_material(const nlohmann::json& mat_json, const std::string& output_path, const std::string& owner)
    {
        const std::string& input_path = owner;

        std::filesystem::create_directories(std::filesystem::path(output_path).parent_path());
        std::ofstream out(output_path, std::ios::binary);

        std::string shader_path = mat_json.value("shader", "");

        // a recorded guid is the real reference, resolve it to where the shader lives now
        // without one (a hand written .mat) name it by path
        asset_guid_t shader_guid = tau::assets::parse_guid(mat_json.value("shader_guid", std::string{}));
        if (shader_guid == 0) { shader_guid = tau::assets::guid_of(shader_path); }
        else
        {
            shader_path = tau::assets::resolve(shader_guid, shader_path);
        }

        std::vector<char> prop_blob;
        u32_t encoded_property_count = 0;

        auto append_blob = [&prop_blob](const void* data, size_t size)
        {
            const char* bytes = static_cast<const char*>(data);
            prop_blob.insert(prop_blob.end(), bytes, bytes + size);
        };

        if (mat_json.contains("properties"))
        {
            for (const auto& [prop_name, prop_val] : mat_json["properties"].items())
            {
                cooked_property_t prop;
                prop.name_hash = tau::hash_string(prop_name);

                if (prop_val.is_number_float())
                {
                    const f32 val = prop_val.get<f32>();
                    prop.data_size = sizeof(f32);
                    append_blob(&prop, sizeof(cooked_property_t));
                    append_blob(&val, sizeof(f32));
                }
                else if (prop_val.is_boolean())
                {
                    const u32_t val = prop_val.get<bool>() ? 1u : 0u;
                    prop.data_size = sizeof(u32_t);
                    append_blob(&prop, sizeof(cooked_property_t));
                    append_blob(&val, sizeof(u32_t));
                }
                else if (prop_val.is_number_unsigned())
                {
                    const u32_t val = prop_val.get<u32_t>();
                    prop.data_size = sizeof(u32_t);
                    append_blob(&prop, sizeof(cooked_property_t));
                    append_blob(&val, sizeof(u32_t));
                }
                else if (prop_val.is_number_integer())
                {
                    const i32 val = prop_val.get<i32>();
                    prop.data_size = sizeof(i32);
                    append_blob(&prop, sizeof(cooked_property_t));
                    append_blob(&val, sizeof(i32));
                }
                else if (prop_val.is_array())
                {
                    std::vector<f32> val_array;
                    for (const auto& val : prop_val) { val_array.push_back(val.get<f32>()); }

                    prop.data_size = static_cast<u32_t>(val_array.size() * sizeof(f32));
                    append_blob(&prop, sizeof(cooked_property_t));
                    append_blob(val_array.data(), prop.data_size);
                }
                else
                {
                    TAU_LOG_ERROR("MATERIAL_COOKER", "Property '{}' in '{}' has an unsupported type ({})", prop_name,
                                  input_path, prop_val.type_name());
                    return false;
                }

                encoded_property_count++;
            }
        }

        material_header_t header = {
            .shader_guid = shader_guid,
            .shader_path_length = static_cast<u32_t>(shader_path.size()),
            .texture_count = static_cast<u32_t>(mat_json.contains("textures") ? mat_json["textures"].size() : 0),
            .sampler_count = static_cast<u32_t>(mat_json.contains("samplers") ? mat_json["samplers"].size() : 0),
            .property_count = encoded_property_count,
        };

        out.write(reinterpret_cast<const char*>(&header), sizeof(material_header_t));
        out.write(shader_path.data(), shader_path.size());

        if (header.texture_count > 0)
        {
            for (const auto& [tex_name, tex_path_json] : mat_json["textures"].items())
            {
                std::string tex_path = tex_path_json.get<std::string>();

                asset_guid_t tex_guid = 0;
                if (mat_json.contains("texture_guids"))
                {
                    tex_guid = tau::assets::parse_guid(mat_json["texture_guids"].value(tex_name, std::string{}));
                }
                if (tex_guid == 0) { tex_guid = tau::assets::guid_of(tex_path); }
                else
                {
                    tex_path = tau::assets::resolve(tex_guid, tex_path);
                }

                cooked_texture_bind_t tex_bind;
                tex_bind.name_hash = tau::hash_string(tex_name);
                tex_bind.path_length = static_cast<u32_t>(tex_path.size());
                tex_bind.guid = tex_guid;

                out.write(reinterpret_cast<const char*>(&tex_bind), sizeof(cooked_texture_bind_t));
                out.write(tex_path.data(), tex_path.size());
            }
        }

        if (header.sampler_count > 0)
        {
            for (const auto& [smp_name, smp_val] : mat_json["samplers"].items())
            {
                cooked_sampler_bind_t smp_bind;
                smp_bind.name_hash = tau::hash_string(smp_name);
                smp_bind.sampler_value = smp_val.get<u32_t>();

                out.write(reinterpret_cast<const char*>(&smp_bind), sizeof(cooked_sampler_bind_t));
            }
        }

        if (!prop_blob.empty()) { out.write(prop_blob.data(), static_cast<std::streamsize>(prop_blob.size())); }

        return true;
    }
} // namespace tau::cooker::material