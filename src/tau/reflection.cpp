#include "reflection.h"

#include "tau/asset.h"
#include "tau/assets/material.h"
#include "tau/assets/mesh.h"
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

#include <entt/entt.hpp>
#include <entt/meta/meta.hpp>
#include <string>
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

        TAU_PIN_TYPE(box_collider_t, "box_collider_t");
        TAU_PIN_MEMBER(&box_collider_t::extents, "extents");
        TAU_PIN_MEMBER(&box_collider_t::offset, "offset");

        TAU_PIN_TYPE(sphere_collider_t, "sphere_collider_t");
        TAU_PIN_MEMBER(&sphere_collider_t::radius, "radius");

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

    void register_types() { register_types_into(get_engine_context()); }

    void register_types_into(ctx_t& ctx)
    {
        factory<std::string>(ctx).type("std::string"_h, "std::string");

        factory<i32>(ctx).type("i32"_h, "i32");
        factory<u32>(ctx).type("u32"_h, "u32");
        factory<bool>(ctx).type("bool"_h, "bool");

        factory<f32>(ctx).type("f32"_h, "f32");
        factory<asset_handle_t>(ctx).type("asset_handle_t"_h, "asset_handle_t");

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
            .field<&rigidbody_t::is_sensor>("Is Sensor");

        component<box_collider_t>(ctx, "Box Collider")
            .field<&box_collider_t::extents>("Extents")
            .field<&box_collider_t::offset>("Offset");

        component<sphere_collider_t>(ctx, "Sphere Collider").field<&sphere_collider_t::radius>("Radius");

        // rendering
        component<post_process_t>(ctx, "Post Process")
            .field<&post_process_t::enabled>("Enabled")
            .field_asset_list<&post_process_t::effects, tau::material_t>("Effects")
            .field_asset<&post_process_t::final_pass, tau::material_t>("Final Pass");

        component<post_volume_t>(ctx, "Post Volume")
            .field_asset<&post_volume_t::overrides, tau::material_t>("Overrides")
            .field<&post_volume_t::is_global>("Global")
            .field<&post_volume_t::extents>("Extents")
            .field<&post_volume_t::blend_distance>("Blend Distance")
            .field<&post_volume_t::weight>("Weight")
            .field<&post_volume_t::priority>("Priority");

        component<mesh_renderer_t>(ctx, "Mesh Renderer")
            .field_asset<&mesh_renderer_t::mesh, tau::mesh_t>("Mesh")
            .field_asset<&mesh_renderer_t::material, tau::material_t>("Material")
            .field<&mesh_renderer_t::active>("Active")
            .field<&mesh_renderer_t::casts_shadow>("Casts Shadow");
    }

    void shutdown() { entt::meta_reset(get_engine_context()); }
} // namespace tau::reflection