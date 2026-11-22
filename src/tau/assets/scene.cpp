#include "scene.h"

#include "tau/assets/scene_format.h"
#include "tau/assets/value_io.h"
#include "tau/hash.h"
#include "tau/log.h"
#include "tau/serialization.h"
#include "tau/vfs.h"

#include <SDL3/SDL_iostream.h>
#include <fstream>
#include <string>
#include <vector>

namespace tau
{
    using namespace value_io;

    std::optional<scene_t> asset_loader_t<scene_t>::load(const std::string& path)
    {
        std::string cooked_path = tau::assets::cooked_path(tau::get_type_id<scene_t>(), path);

        SDL_IOStream* stream = tau::vfs::open_read(cooked_path);
        if (!stream)
        {
            TAU_LOG_ERROR("SCENE", "Scene not found: {}", cooked_path);
            return std::nullopt;
        }

        scene_header_t header;
        if (!read_pod(stream, header) || header.magic != TAU_SCENE_MAGIC)
        {
            TAU_LOG_ERROR("SCENE", "Invalid .tauscene file: {}", cooked_path);
            SDL_CloseIO(stream);
            return std::nullopt;
        }

        if (header.version != TAU_SCENE_VERSION)
        {
            TAU_LOG_ERROR("SCENE", "Unsupported .tauscene version {} (engine expects {}), recook: {}", header.version,
                          TAU_SCENE_VERSION, cooked_path);
            SDL_CloseIO(stream);
            return std::nullopt;
        }

        scene_t scene;
        scene.entities.reserve(header.entity_count);

        bool ok = true;
        for (u32_t e = 0; e < header.entity_count && ok; e++)
        {
            scene_entity_t& entity = scene.entities.emplace_back();

            u32_t component_count = 0;
            if (!read_pod(stream, entity.id) || !read_pod(stream, entity.parent) || !read_pod(stream, component_count))
            {
                ok = false;
                break;
            }

            entity.components.reserve(component_count);
            for (u32_t c = 0; c < component_count && ok; c++)
            {
                scene_component_t& comp = entity.components.emplace_back();

                u32_t prop_count = 0;
                if (!read_pod(stream, comp.type_hash) || !read_pod(stream, prop_count))
                {
                    ok = false;
                    break;
                }

                comp.properties.reserve(prop_count);
                for (u32_t p = 0; p < prop_count && ok; p++)
                {
                    ok = read_property(stream, comp.properties.emplace_back(), 0);
                }
            }
        }

        SDL_CloseIO(stream);

        if (!ok)
        {
            TAU_LOG_ERROR("SCENE", "Truncated or corrupt .tauscene file: {}", cooked_path);
            return std::nullopt;
        }

        return scene;
    }
} // namespace tau

namespace tau::serialization
{
    using namespace value_io;

    void write_scene_binary(const scene_t& scene, const std::string& output_path)
    {
        std::ofstream out(output_path, std::ios::binary);
        if (!out.is_open())
        {
            TAU_LOG_ERROR("SCENE", "Failed to open output for scene: {}", output_path);
            return;
        }

        scene_header_t header;
        header.entity_count = static_cast<u32_t>(scene.entities.size());
        write_pod(out, header);

        for (const scene_entity_t& entity : scene.entities)
        {
            write_pod(out, entity.id);
            write_pod(out, entity.parent);
            write_pod(out, static_cast<u32_t>(entity.components.size()));
            for (const scene_component_t& comp : entity.components)
            {
                write_pod(out, comp.type_hash);
                write_pod(out, static_cast<u32_t>(comp.properties.size()));
                for (const scene_property_t& prop : comp.properties) { write_property(out, prop); }
            }
        }
    }
} // namespace tau::serialization

namespace tau::value_io
{
    constexpr u32_t MAX_VALUE_DEPTH = 64;

    bool read_property(SDL_IOStream* stream, scene_property_t& prop, u32_t depth)
    {
        u8_t type_raw = 0;
        if (depth > MAX_VALUE_DEPTH || !read_pod(stream, prop.prop_hash) || !read_pod(stream, type_raw))
        {
            return false;
        }
        prop.type = static_cast<scene_value_type_e>(type_raw);

        if (prop.type == scene_value_type_e::ENUM)
        {
            prop.type = scene_value_type_e::I32;
            return SDL_ReadIO(stream, prop.pod.data(), sizeof(i32_t)) == sizeof(i32_t) &&
                   read_pod(stream, prop.enum_name);
        }

        if (const u32_t size = pod_size(pod_layout(prop.type)); size > 0)
        {
            return SDL_ReadIO(stream, prop.pod.data(), size) == size;
        }

        switch (prop.type)
        {
        case scene_value_type_e::STRING: return read_str(stream, prop.str);
        case scene_value_type_e::ASSET_REF:
            return read_pod(stream, prop.asset_type) && read_str(stream, prop.str) && read_pod(stream, prop.guid);
        case scene_value_type_e::ASSET_REF_LIST:
        {
            u32_t entry_count = 0;
            bool ok = read_pod(stream, prop.asset_type) && read_pod(stream, entry_count);

            for (u32_t i = 0; i < entry_count && ok; i++)
            {
                ok = read_str(stream, prop.strs.emplace_back()) && read_pod(stream, prop.guids.emplace_back());
            }
            return ok;
        }
        case scene_value_type_e::ENTITY: return read_pod(stream, prop.ref);
        case scene_value_type_e::STRUCT:
        case scene_value_type_e::LIST:
        {
            u32_t count = 0;
            if (!read_pod(stream, count)) { return false; }

            for (u32_t i = 0; i < count; i++)
            {
                if (!read_property(stream, prop.children.emplace_back(), depth + 1)) { return false; }
            }
            return true;
        }
        default: break;
        }

        return false;
    }

    void write_property(std::ofstream& out, const scene_property_t& prop)
    {
        write_pod(out, prop.prop_hash);

        if (prop.type == scene_value_type_e::I32 && prop.enum_name != 0)
        {
            write_pod(out, static_cast<u8_t>(scene_value_type_e::ENUM));
            out.write(reinterpret_cast<const char*>(prop.pod.data()), sizeof(i32_t));
            write_pod(out, prop.enum_name);
            return;
        }

        write_pod(out, static_cast<u8_t>(prop.type));

        if (const u32_t size = pod_size(pod_layout(prop.type)); size > 0)
        {
            out.write(reinterpret_cast<const char*>(prop.pod.data()), size);
            return;
        }

        switch (prop.type)
        {
        case scene_value_type_e::STRING: write_str(out, prop.str); break;
        case scene_value_type_e::ASSET_REF:
            write_pod(out, prop.asset_type);
            write_str(out, prop.str);
            write_pod(out, prop.guid);
            break;
        case scene_value_type_e::ASSET_REF_LIST:
            write_pod(out, prop.asset_type);
            write_pod(out, static_cast<u32_t>(prop.strs.size()));
            for (std::size_t i = 0; i < prop.strs.size(); i++)
            {
                write_str(out, prop.strs[i]);
                write_pod(out, i < prop.guids.size() ? prop.guids[i] : asset_guid_t{0});
            }
            break;
        case scene_value_type_e::ENTITY: write_pod(out, prop.ref); break;
        case scene_value_type_e::STRUCT:
        case scene_value_type_e::LIST:
            write_pod(out, static_cast<u32_t>(prop.children.size()));
            for (const scene_property_t& child : prop.children) { write_property(out, child); }
            break;
        default: break;
        }
    }
} // namespace tau::value_io
