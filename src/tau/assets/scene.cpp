#include "scene.h"

#include "tau/asset_table.h"
#include "tau/hash.h"
#include "tau/asset_handle.h"
#include "tau/assets/scene_format.h"
#include "tau/log.h"
#include "tau/vfs.h"

#include <SDL3/SDL_iostream.h>
#include <string>
#include <vector>

namespace tau
{
    namespace
    {
        template <typename T>
        bool read_pod(SDL_IOStream* stream, T& out)
        { return SDL_ReadIO(stream, &out, sizeof(T)) == sizeof(T); }

        bool read_str(SDL_IOStream* stream, std::string& out)
        {
            u32_t len = 0;
            if (!read_pod(stream, len)) { return false; }
            out.resize(len);
            return len == 0 || SDL_ReadIO(stream, out.data(), len) == len;
        }

        constexpr u32_t MAX_VALUE_DEPTH = 64;

        bool read_property(SDL_IOStream* stream, scene_property_t& prop, u32_t depth)
        {
            u8_t type_raw = 0;
            if (depth > MAX_VALUE_DEPTH || !read_pod(stream, prop.prop_hash) || !read_pod(stream, type_raw))
            {
                return false;
            }
            prop.type = static_cast<scene_value_type_e>(type_raw);

            switch (prop.type)
            {
            case scene_value_type_e::I32: return read_pod(stream, prop.i);
            case scene_value_type_e::U32: return read_pod(stream, prop.u);
            case scene_value_type_e::F32: return read_pod(stream, prop.f);
            case scene_value_type_e::BOOL:
            {
                u8_t b = 0;
                const bool ok = read_pod(stream, b);
                prop.b = b != 0;
                return ok;
            }
            case scene_value_type_e::VEC3:
                return read_pod(stream, prop.vec.x) && read_pod(stream, prop.vec.y) && read_pod(stream, prop.vec.z);
            case scene_value_type_e::STRING: return read_str(stream, prop.str);
            case scene_value_type_e::ASSET_REF:
                return read_pod(stream, prop.asset_type) && read_str(stream, prop.str) && read_str(stream, prop.guid);
            case scene_value_type_e::ASSET_REF_LIST:
            {
                u32_t entry_count = 0;
                bool ok = read_pod(stream, prop.asset_type) && read_pod(stream, entry_count);

                for (u32_t i = 0; i < entry_count && ok; i++)
                {
                    ok = read_str(stream, prop.strs.emplace_back()) && read_str(stream, prop.guids.emplace_back());
                }
                return ok;
            }
            case scene_value_type_e::ENTITY: return read_pod(stream, prop.u);
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
            }

            return false;
        }
    } // namespace

    std::optional<scene_t> asset_loader_t<scene_t>::load(const std::string& path)
    {
        std::string cooked_path = tau::asset_table::cooked_path(tau::get_type_id<scene_t>(), path);

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
            if (!read_pod(stream, entity.parent) || !read_pod(stream, component_count))
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
