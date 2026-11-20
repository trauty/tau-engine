#pragma once

#include "tau/defines.h"
#include "tau/reflection.h"

#include "json/json.hpp"
#include <string_view>

namespace tau
{
    enum class texture_kind_e : u8_t
    {
        COLOR,
        NORMAL,
        DATA,
    };

    struct texture_import_t
    {
        texture_kind_e kind = texture_kind_e::COLOR;
        bool srgb = true;
        u32_t max_size = 0;
        bool mips = true;
    };

    struct mesh_import_t
    { f32 scale = 1.0f; };
} // namespace tau

namespace tau::import_settings
{
    constexpr const char* META_KEY = "import";

    TAU_ENGINE_API reflection::type_t type_of(u64_t asset_type_id);
    TAU_ENGINE_API reflection::any_t base(u64_t asset_type_id, const nlohmann::json& project_defaults);

    TAU_ENGINE_API reflection::any_t resolve(u64_t asset_type_id, const nlohmann::json& project_defaults,
                                             const nlohmann::json& meta, std::string_view owner);

    template <typename Settings>
    Settings resolve_as(u64_t asset_type_id, const nlohmann::json& project_defaults, const nlohmann::json& meta,
                        std::string_view owner)
    {
        const reflection::any_t value = resolve(asset_type_id, project_defaults, meta, owner);
        const Settings* typed = value.try_cast<const Settings>();
        return typed ? *typed : Settings{};
    }

    TAU_ENGINE_API nlohmann::json meta_object(const reflection::any_t& value, const reflection::any_t& base);

    TAU_ENGINE_API std::string fingerprint(u64_t asset_type_id, const nlohmann::json& project_defaults,
                                           const nlohmann::json& meta, std::string_view owner);
} // namespace tau::import_settings

namespace tau::reflection { void reflect_import_settings(ctx_t& ctx); }
