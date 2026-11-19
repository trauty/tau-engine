#include "reflection.h"

#include "tau/asset.h"
#include "tau/assets/material.h"
#include "tau/assets/mesh.h"
#include "tau/color.h"
#include "tau/components/camera.h"
#include "tau/components/lighting.h"
#include "tau/components/physics.h"
#include "tau/components/post_process.h"
#include "tau/components/renderer.h"
#include "tau/components/tag.h"
#include "tau/components/transform.h"
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
    vec3_t get_transform_rot_euler(const tau::transform_t& transform)
    { return quat_t::to_euler(transform.local_rotation); }

    void set_transform_rot_euler(tau::transform_t& transform, const vec3_t& euler)
    { transform.local_rotation = quat_t::from_euler(euler); }

    namespace
    {
#define TAU_PIN_MEMBER(ptr, lit) static_assert(hash_string(member_name<ptr>()) == lit##_h, "reflection id drift: " lit)
#define TAU_PIN_TYPE(T, lit) static_assert(hash_string(type_name<T>()) == lit##_h, "reflection id drift: " lit)
#define TAU_PIN_ENUM(v, lit) static_assert(hash_string(enum_name<v>()) == lit##_h, "reflection id drift: " lit)

        TAU_PIN_TYPE(tag_t, "tag_t");
        TAU_PIN_MEMBER(&tag_t::name, "name");

        TAU_PIN_TYPE(transform_t, "transform_t");
        TAU_PIN_MEMBER(&transform_t::local_position, "local_position");
        TAU_PIN_MEMBER(&transform_t::local_scale, "local_scale");

        TAU_PIN_TYPE(active_camera_tag, "active_camera_tag");

        TAU_PIN_TYPE(camera_t, "camera_t");
        TAU_PIN_MEMBER(&camera_t::fov_deg, "fov_deg");
        TAU_PIN_MEMBER(&camera_t::near_plane, "near_plane");
        TAU_PIN_MEMBER(&camera_t::far_plane, "far_plane");
        TAU_PIN_MEMBER(&camera_t::aspect, "aspect");

        TAU_PIN_TYPE(directional_light_t, "directional_light_t");
        TAU_PIN_TYPE(point_light_t, "point_light_t");
        TAU_PIN_TYPE(spot_light_t, "spot_light_t");
        TAU_PIN_MEMBER(&directional_light_t::color, "color");
        TAU_PIN_MEMBER(&directional_light_t::intensity, "intensity");
        TAU_PIN_MEMBER(&point_light_t::radius, "radius");
        TAU_PIN_MEMBER(&directional_light_t::casts_shadow, "casts_shadow");
        TAU_PIN_MEMBER(&point_light_t::casts_shadow, "casts_shadow");
        TAU_PIN_MEMBER(&spot_light_t::casts_shadow, "casts_shadow");
        TAU_PIN_MEMBER(&spot_light_t::inner_angle, "inner_angle");
        TAU_PIN_MEMBER(&spot_light_t::outer_angle, "outer_angle");

        TAU_PIN_TYPE(body_type_e, "body_type_e");
        TAU_PIN_ENUM(body_type_e::STATIC, "STATIC");
        TAU_PIN_ENUM(body_type_e::KINEMATIC, "KINEMATIC");
        TAU_PIN_ENUM(body_type_e::DYNAMIC, "DYNAMIC");

        TAU_PIN_TYPE(rigidbody_t, "rigidbody_t");
        TAU_PIN_MEMBER(&rigidbody_t::type, "type");
        TAU_PIN_MEMBER(&rigidbody_t::is_sensor, "is_sensor");
        TAU_PIN_MEMBER(&rigidbody_t::continuous, "continuous");
        TAU_PIN_MEMBER(&rigidbody_t::layer, "layer");
        TAU_PIN_MEMBER(&rigidbody_t::mass, "mass");
        TAU_PIN_MEMBER(&rigidbody_t::friction, "friction");
        TAU_PIN_MEMBER(&rigidbody_t::restitution, "restitution");
        TAU_PIN_MEMBER(&rigidbody_t::linear_damping, "linear_damping");
        TAU_PIN_MEMBER(&rigidbody_t::angular_damping, "angular_damping");
        TAU_PIN_MEMBER(&rigidbody_t::gravity_scale, "gravity_scale");

        TAU_PIN_TYPE(box_collider_t, "box_collider_t");
        TAU_PIN_MEMBER(&box_collider_t::extents, "extents");
        TAU_PIN_MEMBER(&box_collider_t::offset, "offset");

        TAU_PIN_TYPE(sphere_collider_t, "sphere_collider_t");
        TAU_PIN_MEMBER(&sphere_collider_t::radius, "radius");
        TAU_PIN_MEMBER(&sphere_collider_t::offset, "offset");

        TAU_PIN_TYPE(capsule_collider_t, "capsule_collider_t");
        TAU_PIN_MEMBER(&capsule_collider_t::radius, "radius");
        TAU_PIN_MEMBER(&capsule_collider_t::height, "height");
        TAU_PIN_MEMBER(&capsule_collider_t::offset, "offset");

        TAU_PIN_TYPE(post_process_t, "post_process_t");
        TAU_PIN_MEMBER(&post_process_t::effects, "effects");
        TAU_PIN_MEMBER(&post_process_t::final_pass, "final_pass");
        TAU_PIN_MEMBER(&post_process_t::enabled, "enabled");

        TAU_PIN_TYPE(post_volume_t, "post_volume_t");
        TAU_PIN_MEMBER(&post_volume_t::overrides, "overrides");
        TAU_PIN_MEMBER(&post_volume_t::is_global, "is_global");
        TAU_PIN_MEMBER(&post_volume_t::extents, "extents");
        TAU_PIN_MEMBER(&post_volume_t::blend_distance, "blend_distance");
        TAU_PIN_MEMBER(&post_volume_t::weight, "weight");
        TAU_PIN_MEMBER(&post_volume_t::priority, "priority");

        TAU_PIN_TYPE(mesh_renderer_t, "mesh_renderer_t");
        TAU_PIN_MEMBER(&mesh_renderer_t::mesh, "mesh");
        TAU_PIN_MEMBER(&mesh_renderer_t::material, "material");
        TAU_PIN_MEMBER(&mesh_renderer_t::active, "active");
        TAU_PIN_MEMBER(&mesh_renderer_t::casts_shadow, "casts_shadow");

#undef TAU_PIN_MEMBER
#undef TAU_PIN_TYPE
#undef TAU_PIN_ENUM
    } // namespace

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

        component<tag_t>(ctx, "Tag").field<&tag_t::name>("Entity Name");

        component<transform_t>(ctx, "Transform")
            .field<&transform_t::local_position>("Position")
            .accessor<&set_transform_rot_euler, &get_transform_rot_euler>("Rotation", unit_e::RADIANS,
                                                                          stable_id{"local_euler"})
            .field<&transform_t::local_scale>("Scale");

        component<active_camera_tag>(ctx, "Active Camera Tag");

        component<camera_t>(ctx, "Camera")
            .field<&camera_t::fov_deg>("FOV")
            .field<&camera_t::near_plane>("Near Plane")
            .field<&camera_t::far_plane>("Far Plane")
            .field<&camera_t::aspect>("Aspect Ratio");

        // lighting
        component<directional_light_t>(ctx, "Directional Light")
            .field<&directional_light_t::color>("Color")
            .field<&directional_light_t::intensity>("Intensity")
            .field<&directional_light_t::casts_shadow>("Casts Shadow");

        component<point_light_t>(ctx, "Point Light")
            .field<&point_light_t::color>("Color")
            .field<&point_light_t::intensity>("Intensity")
            .field<&point_light_t::radius>("Radius")
            .field<&point_light_t::casts_shadow>("Casts Shadow");

        component<spot_light_t>(ctx, "Spot Light")
            .field<&spot_light_t::color>("Color")
            .field<&spot_light_t::intensity>("Intensity")
            .field<&spot_light_t::radius>("Radius")
            .field<&spot_light_t::inner_angle>("Inner Angle")
            .field<&spot_light_t::outer_angle>("Outer Angle")
            .field<&spot_light_t::casts_shadow>("Casts Shadow");

        // physics
        enumeration<body_type_e>(ctx, "Body Type");

        component<rigidbody_t>(ctx, "Rigidbody")
            .field<&rigidbody_t::type>("Body Type")
            .field<&rigidbody_t::is_sensor>("Is Sensor")
            .field<&rigidbody_t::continuous>("Continuous")
            .field<&rigidbody_t::layer>("Layer")
            .field<&rigidbody_t::mass>("Mass")
            .field<&rigidbody_t::friction>("Friction")
            .field<&rigidbody_t::restitution>("Restitution")
            .field<&rigidbody_t::linear_damping>("Linear Damping")
            .field<&rigidbody_t::angular_damping>("Angular Damping")
            .field<&rigidbody_t::gravity_scale>("Gravity Scale");

        component<box_collider_t>(ctx, "Box Collider")
            .field<&box_collider_t::extents>("Extents")
            .field<&box_collider_t::offset>("Offset");

        component<sphere_collider_t>(ctx, "Sphere Collider")
            .field<&sphere_collider_t::radius>("Radius")
            .field<&sphere_collider_t::offset>("Offset");

        component<capsule_collider_t>(ctx, "Capsule Collider")
            .field<&capsule_collider_t::radius>("Radius")
            .field<&capsule_collider_t::height>("Height")
            .field<&capsule_collider_t::offset>("Offset");

        // rendering
        component<post_process_t>(ctx, "Post Process")
            .field<&post_process_t::enabled>("Enabled")
            .field<&post_process_t::effects>("Effects")
            .field<&post_process_t::final_pass>("Final Pass");

        component<post_volume_t>(ctx, "Post Volume")
            .field<&post_volume_t::overrides>("Overrides")
            .field<&post_volume_t::is_global>("Global")
            .field<&post_volume_t::extents>("Extents")
            .field<&post_volume_t::blend_distance>("Blend Distance")
            .field<&post_volume_t::weight>("Weight")
            .field<&post_volume_t::priority>("Priority");

        component<mesh_renderer_t>(ctx, "Mesh Renderer")
            .field<&mesh_renderer_t::mesh>("Mesh")
            .field<&mesh_renderer_t::material>("Material")
            .field<&mesh_renderer_t::active>("Active")
            .field<&mesh_renderer_t::casts_shadow>("Casts Shadow");
    }

    void shutdown() { entt::meta_reset(get_engine_context()); }
} // namespace tau::reflection