#pragma once
#include "tau/asset.h"
#include "tau/assets/shader.h"
#include "tau/assets/texture.h"
#include "tau/containers/flat_map.h"
#include "tau/defines.h"
#include "tau/log.h"
#include "tau/rhi/renderer_constants.h"
#include "tau/rhi/samplers.h"

#include <climits>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tau
{
    constexpr u32_t NULL_SHADER_MODULE = UINT_MAX;

    struct TAU_ENGINE_API material_t
    {
        static constexpr std::string_view type_key = "material";

        asset_t<shader_t> shader;
        std::vector<u8> data;

        std::vector<u32_t> authored_properties;

        // failed texture stays bound, draws fallback
        flat_map_t<asset_t<texture_t>> textures;

        u32_t heap_offset[renderer::MAX_FRAMES_IN_FLIGHT];
        u32_t dirty_frames = renderer::MAX_FRAMES_IN_FLIGHT;
        u32_t last_synced_frame = renderer::BINDLESS_NULL_HANDLE;

        material_t() = default;
        explicit material_t(asset_t<shader_t> target_shader);

        void sync();

        bool set_shader(asset_t<shader_t> new_shader);

        template <typename T>
        void set_property(u32_t name_hash, const T& value)
        {
            const shader_t* layout = assets::get(shader);
            if (!layout) { return; }

            const flat_map_t<shader_member_t>& members = layout->module.members;
            const shader_member_t* it = members.find(name_hash);

            if (it == nullptr)
            {
                TAU_LOG_WARN("MATERIAL", "Property {} not found in shader '{}'", name_hash, layout->module.name);
                return;
            }

            const shader_member_t& member = *it;

            if (sizeof(T) != member.size)
            {
                TAU_LOG_ERROR("MATERIAL", "Size mismatch for '{}', Expected {} bytes, but got {} bytes", name_hash,
                              member.size, sizeof(T));
                return;
            }

            std::memcpy(data.data() + member.offset, &value, sizeof(T));
            dirty_frames = renderer::MAX_FRAMES_IN_FLIGHT;
        }

        void set_texture(u32_t name_hash, asset_t<texture_t> texture);
        void clear_texture(u32_t name_hash);

        // rewrites bindless id, changes every load
        void refresh_texture(u32_t name_hash);

        void set_property_raw(u32_t name_hash, const void* value, u32_t size);

        bool has_property(u32_t name_hash) const;

        template <typename T>
        void set_property_if_present(u32_t name_hash, const T& value)
        {
            if (has_property(name_hash)) { set_property(name_hash, value); }
        }

        void release_heap();

        void set_sampler(u32_t name_hash, sampler_type_e sampler)
        { set_property<u32_t>(name_hash, static_cast<u32_t>(sampler)); }
    };

    template <>
    struct TAU_ENGINE_API asset_loader_t<material_t>
    {
        static std::optional<material_t> load(const std::string& path, std::span<const u8_t> bytes);
        static void unload(material_t& mat);
        static void deps(const material_t& mat, std::vector<asset_handle_t>& out);
        static asset_dep_e dep_reloaded(material_t& mat, asset_handle_t dep);
    };
} // namespace tau