#include "reflection.h"

#include "tau/asset.h"
#include "tau/assets/import_settings.h"
#include "tau/color.h"
#include "tau/components/reflect.h"
#include "tau/ecs_fwd.h"
#include "tau/hash.h"
#include "tau/math.h"
#include "tau/os.h"

#include <array>
#include <entt/entt.hpp>
#include <entt/meta/meta.hpp>
#include <string>
#include <type_traits>
#include <vector>

namespace tau::reflection
{
    namespace
    {
        std::vector<register_func_t>& registrations()
        {
            static std::vector<register_func_t> list;
            return list;
        }
    } // namespace

    void add_registration(register_func_t fn)
    {
        if (fn != nullptr) { registrations().push_back(fn); }
    }

    void run_registrations(ctx_t& ctx)
    {
        for (register_func_t fn : registrations()) { fn(ctx); }
    }

    u32_t remove_registrations_of(void* module_base)
    {
        return static_cast<u32_t>(
            std::erase_if(registrations(), [module_base](register_func_t fn)
                          { return os::module_base_of(reinterpret_cast<const void*>(fn)) == module_base; }));
    }

    namespace
    {
        struct pod_entry_t
        {
            entt::id_type hash = 0;
            scene_value_type_e tag = scene_value_type_e::I32;
        };

        template <typename T, scene_value_type_e Tag>
        constexpr pod_entry_t pod_entry()
        {
            static_assert(std::is_trivially_copyable_v<T> && sizeof(T) == pod_size(pod_layout(Tag)));
            return {entt::type_hash<T>::value(), Tag};
        }

        const std::array POD_TYPES = {
            pod_entry<bool, scene_value_type_e::BOOL>(),   pod_entry<i32, scene_value_type_e::I32>(),
            pod_entry<u32, scene_value_type_e::U32>(),     pod_entry<i64, scene_value_type_e::I64>(),
            pod_entry<i64_t, scene_value_type_e::I64>(),   pod_entry<u64, scene_value_type_e::U64>(),
            pod_entry<u64_t, scene_value_type_e::U64>(),   pod_entry<f32, scene_value_type_e::F32>(),
            pod_entry<f64, scene_value_type_e::F64>(),     pod_entry<vec2_t, scene_value_type_e::VEC2>(),
            pod_entry<vec3_t, scene_value_type_e::VEC3>(), pod_entry<vec4_t, scene_value_type_e::VEC4>(),
            pod_entry<quat_t, scene_value_type_e::QUAT>(), pod_entry<color_t, scene_value_type_e::COLOR>(),
        };
    } // namespace

    std::optional<scene_value_type_e> pod_tag(const type_t& type)
    {
        if (!type) { return std::nullopt; }

        const entt::id_type hash = type.info().hash();
        for (const pod_entry_t& entry : POD_TYPES)
        {
            if (entry.hash == hash) { return entry.tag; }
        }
        return std::nullopt;
    }

    void register_types() { register_types_into(get_engine_context()); }

    void register_types_into(ctx_t& ctx)
    {
        factory<std::string>(ctx).type("std::string"_h, "std::string");

        factory<i32>(ctx).type("i32"_h, "i32");
        factory<u32>(ctx).type("u32"_h, "u32");
        factory<bool>(ctx).type("bool"_h, "bool");

        factory<f32>(ctx).type("f32"_h, "f32");
        factory<asset_handle_t>(ctx).type("asset_handle_t"_h, "asset_handle_t");
        for (const asset_type_t& type : assets::types())
        {
            if (type.reflect) { type.reflect(ctx); }
        }

        factory<vec3_t>{ctx}
            .type("vec3_t"_h, "vec3_t")
            .data<&vec3_t::x>("x"_h)
            .data<&vec3_t::y>("y"_h)
            .data<&vec3_t::z>("z"_h);

        reflect_transform(ctx);
        reflect_camera(ctx);
        reflect_lighting(ctx);
        reflect_physics(ctx);
        reflect_post_process(ctx);
        reflect_mesh_renderer(ctx);
        reflect_import_settings(ctx);
    }

    void shutdown() { entt::meta_reset(get_engine_context()); }
} // namespace tau::reflection