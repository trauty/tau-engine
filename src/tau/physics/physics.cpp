#include "tau/physics/physics.h"

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include "tau/components/physics.h"
#include "tau/components/transform.h"
#include "tau/ecs.h"
#include "tau/log.h"
#include "tau/physics/jolt_job_system_int.h"
#include "tau/physics/physics_internal.h"
#include "tau/rendering/overlay.h"
#include "tau/systems/transform.h"
#include "tau/time.h"
#include "tau/world.h"

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace tau::physics::detail
{
    // object layer: user layer << 1 | moving; broad phase layer: moving or not, static never meets static
    constexpr JPH::BroadPhaseLayer BP_STATIC{0};
    constexpr JPH::BroadPhaseLayer BP_MOVING{1};

    JPH::ObjectLayer object_layer(u32_t layer, bool moving)
    { return static_cast<JPH::ObjectLayer>((std::min(layer, MAX_LAYERS - 1) << 1) | (moving ? 1u : 0u)); }
    u32_t user_layer(JPH::ObjectLayer layer) { return layer >> 1; }
    bool is_moving(JPH::ObjectLayer layer) { return (layer & 1u) != 0; }

    class bp_layers_t final : public JPH::BroadPhaseLayerInterface
    {
      public:
        JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
        JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
        { return is_moving(layer) ? BP_MOVING : BP_STATIC; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
        const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
        { return layer == BP_MOVING ? "MOVING" : "STATIC"; }
#endif
    };

    class object_vs_bp_t final : public JPH::ObjectVsBroadPhaseLayerFilter
    {
      public:
        bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bp) const override
        { return is_moving(layer) || bp == BP_MOVING; }
    };

    class layer_pairs_t final : public JPH::ObjectLayerPairFilter
    {
      public:
        std::array<u32_t, MAX_LAYERS> collides;

        layer_pairs_t() { collides.fill(ALL_LAYERS); }

        bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
        {
            if (!is_moving(a) && !is_moving(b)) { return false; }
            return ((collides[user_layer(a)] >> user_layer(b)) & 1u) != 0;
        }
    };

    struct raw_contact_t
    {
        JPH::BodyID a;
        JPH::BodyID b;
        contact_phase_e phase;
        vec3_t point;
        vec3_t normal;
    };

    class contact_listener_t final : public JPH::ContactListener
    {
      public:
        std::mutex mutex;
        std::vector<raw_contact_t> contacts;

        void OnContactAdded(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold,
                            JPH::ContactSettings&) override
        {
            raw_contact_t contact = {a.GetID(), b.GetID(), contact_phase_e::BEGIN, {}, {}};
            const JPH::Vec3 normal = manifold.mWorldSpaceNormal;
            contact.normal = {normal.GetX(), normal.GetY(), normal.GetZ()};
            if (!manifold.mRelativeContactPointsOn1.empty())
            {
                const JPH::RVec3 point = manifold.GetWorldSpaceContactPointOn1(0);
                contact.point = {static_cast<f32>(point.GetX()), static_cast<f32>(point.GetY()),
                                 static_cast<f32>(point.GetZ())};
            }

            std::scoped_lock lock(mutex);
            contacts.push_back(contact);
        }

        void OnContactRemoved(const JPH::SubShapeIDPair& pair) override
        {
            std::scoped_lock lock(mutex);
            contacts.push_back({pair.GetBody1ID(), pair.GetBody2ID(), contact_phase_e::END, {}, {}});
        }
    };

    struct state_t
    {
        JPH::TempAllocatorImpl temp_allocator{16 * 1024 * 1024};
        jolt_job_system_integration_t jobs;
        bp_layers_t bp_layers;
        object_vs_bp_t object_vs_bp;
        layer_pairs_t layer_pairs;
        contact_listener_t listener;

        std::vector<contact_t> step_contacts;
        std::vector<contact_t> frame_contacts;
        bool in_fixed_step = false;

        bool warned_full = false;

        JPH::PhysicsSystem system;
    };

    struct pose_t
    {
        vec3_t position;
        quat_t rotation;
    };

    // simulated side of a rigidbody, not reflected
    struct body_t
    {
        JPH::BodyID id;
        u64_t shape_key = 0;
        u64_t settings_key = 0;

        // last pose physics wrote/took: local (static, dynamic), world (kinematic)
        // transform differs: someone else moved it
        vec3_t position;
        quat_t rotation;
        bool was_active = false;

        pose_t previous;
        pose_t current;

        bool set_linear = false;
        bool set_angular = false;
        vec3_t linear_velocity;
        vec3_t angular_velocity;
        vec3_t force;
        vec3_t torque;
        vec3_t impulse;
        vec3_t angular_impulse;
    };

    namespace
    {
        u32_t g_jolt_users = 0;

        void trace(const char* format, ...)
        {
            va_list args;
            va_start(args, format);
            char buffer[1024];
            vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            TAU_LOG_WARN("PHYSICS", "{}", buffer);
        }

        // jolt allocator/factory/types are per process
        void acquire_jolt()
        {
            if (g_jolt_users++ > 0) { return; }

            JPH::RegisterDefaultAllocator();
            JPH::Trace = &trace;
            JPH::Factory::sInstance = new JPH::Factory;
            JPH::RegisterTypes();
        }

        void release_jolt()
        {
            if (--g_jolt_users > 0) { return; }

            JPH::UnregisterTypes();
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
        }

        JPH::Vec3 to_jolt(const vec3_t& v) { return {v.x, v.y, v.z}; }
        JPH::Quat to_jolt(const quat_t& q) { return JPH::Quat(q.x, q.y, q.z, q.w).Normalized(); }
        vec3_t from_jolt(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
        quat_t from_jolt(JPH::QuatArg q) { return {q.GetX(), q.GetY(), q.GetZ(), q.GetW()}; }

        u64_t user_data(ecs::entity_t entity) { return (1ull << 32) | static_cast<u64_t>(entt::to_integral(entity)); }
        ecs::entity_t entity_of(u64_t data)
        {
            if ((data >> 32) != 1) { return ecs::NULL_ENTITY; }
            return static_cast<ecs::entity_t>(static_cast<u32_t>(data));
        }

        state_t* state_of(const world_t& world) { return world.physics_state; }

        JPH::EMotionType motion_type(body_type_e type)
        {
            switch (type)
            {
            case body_type_e::STATIC: return JPH::EMotionType::Static;
            case body_type_e::KINEMATIC: return JPH::EMotionType::Kinematic;
            case body_type_e::DYNAMIC: break;
            }
            return JPH::EMotionType::Dynamic;
        }

        u64_t mix(u64_t seed, u64_t value)
        {
            for (u32_t i = 0; i < 8; i++)
            {
                seed ^= (value >> (i * 8)) & 0xffu;
                seed *= 1099511628211ull;
            }
            return seed;
        }

        u64_t mix(u64_t seed, f32 value)
        {
            u32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            return mix(seed, static_cast<u64_t>(bits));
        }

        u64_t mix(u64_t seed, const vec3_t& v) { return mix(mix(mix(seed, v.x), v.y), v.z); }

        // changes needing a body rebuild; shape changes are cheap
        u64_t settings_key(const rigidbody_t& rb)
        {
            u64_t key = 14695981039346656037ull;
            key = mix(key, static_cast<u64_t>(rb.type));
            key = mix(key, static_cast<u64_t>(rb.is_sensor));
            key = mix(key, static_cast<u64_t>(rb.continuous));
            key = mix(key, static_cast<u64_t>(rb.layer));
            key = mix(key, rb.mass);
            key = mix(key, rb.friction);
            key = mix(key, rb.restitution);
            key = mix(key, rb.linear_damping);
            key = mix(key, rb.angular_damping);
            return mix(key, rb.gravity_scale);
        }

        quat_t rotation_of(mat4_t m)
        {
            mat4 axes;
            glm_mat4_copy(m, axes);
            glm_vec3_normalize(axes[0]);
            glm_vec3_normalize(axes[1]);
            glm_vec3_normalize(axes[2]);
            quat_t rotation;
            glm_mat4_quat(axes, rotation);
            return rotation;
        }

        pose_t world_pose(ecs::registry_t& reg, const transform_t& transform)
        {
            const ecs::entity_t parent = transform.get_parent();
            if (parent == ecs::NULL_ENTITY) { return {transform.local_position, transform.local_rotation}; }

            mat4_t parent_world = transform_system::compose_world(reg, parent);
            vec3_t position;
            glm_mat4_mulv3(parent_world, transform.local_position, 1.0f, position);
            return {position, rotation_of(parent_world) * transform.local_rotation};
        }

        vec3_t world_scale(ecs::registry_t& reg, const transform_t& transform)
        {
            const ecs::entity_t parent = transform.get_parent();
            if (parent == ecs::NULL_ENTITY) { return transform.local_scale; }

            mat4_t parent_world = transform_system::compose_world(reg, parent);
            const vec3_t parent_scale = {glm_vec3_norm(parent_world[0]), glm_vec3_norm(parent_world[1]),
                                         glm_vec3_norm(parent_world[2])};
            return parent_scale * transform.local_scale;
        }

        void set_local_pose(ecs::registry_t& reg, transform_t& transform, const pose_t& world)
        {
            const ecs::entity_t parent = transform.get_parent();
            if (parent == ecs::NULL_ENTITY)
            {
                transform.local_position = world.position;
                transform.local_rotation = world.rotation;
                return;
            }

            mat4_t parent_world = transform_system::compose_world(reg, parent);
            mat4 parent_inverse;
            glm_mat4_inv(parent_world, parent_inverse);
            glm_mat4_mulv3(parent_inverse, world.position, 1.0f, transform.local_position);

            quat_t parent_rotation = rotation_of(parent_world);
            glm_quat_inv(parent_rotation, parent_rotation);
            transform.local_rotation = quat_t::normalize(parent_rotation * world.rotation);
        }

        pose_t local_pose(const transform_t& transform) { return {transform.local_position, transform.local_rotation}; }

        bool same_pose(const pose_t& a, const pose_t& b)
        {
            return std::memcmp(&a.position, &b.position, sizeof(vec3_t)) == 0 &&
                   std::memcmp(&a.rotation, &b.rotation, sizeof(quat_t)) == 0;
        }

        pose_t blend(const pose_t& from, const pose_t& to, f32 t)
        {
            const quat_t& q0 = from.rotation;
            const quat_t& q1 = to.rotation;

            const f32 sign = q0.x * q1.x + q0.y * q1.y + q0.z * q1.z + q0.w * q1.w < 0.0f ? -1.0f : 1.0f;
            const f32 u = 1.0f - t;
            const f32 v = t * sign;
            const quat_t rotation = {q0.x * u + q1.x * v, q0.y * u + q1.y * v, q0.z * u + q1.z * v,
                                     q0.w * u + q1.w * v};
            return {from.position + (to.position - from.position) * t, quat_t::normalize(rotation)};
        }

        f32 max_abs(f32 a, f32 b) { return std::max(std::abs(a), std::abs(b)); }

        // first of box, sphere, capsule
        u64_t shape_key(ecs::registry_t& reg, ecs::entity_t entity, const vec3_t& scale)
        {
            u64_t key = mix(14695981039346656037ull, scale);
            if (const box_collider_t* box = reg.try_get<box_collider_t>(entity))
            {
                return mix(mix(mix(key, u64_t{1}), box->extents), box->offset);
            }
            if (const sphere_collider_t* sphere = reg.try_get<sphere_collider_t>(entity))
            {
                return mix(mix(mix(key, u64_t{2}), sphere->radius), sphere->offset);
            }
            if (const capsule_collider_t* capsule = reg.try_get<capsule_collider_t>(entity))
            {
                return mix(mix(mix(mix(key, u64_t{3}), capsule->radius), capsule->height), capsule->offset);
            }
            return mix(key, u64_t{0});
        }

        JPH::RefConst<JPH::Shape> build_shape(ecs::registry_t& reg, ecs::entity_t entity, const vec3_t& scale)
        {
            JPH::RefConst<JPH::Shape> shape;
            vec3_t offset;

            if (const sphere_collider_t* sphere = reg.try_get<sphere_collider_t>(entity);
                sphere && !reg.all_of<box_collider_t>(entity))
            {
                const f32 radius = sphere->radius * std::max(max_abs(scale.x, scale.y), std::abs(scale.z));
                shape = new JPH::SphereShape(std::max(radius, 0.001f));
                offset = sphere->offset * scale;
            }
            else if (const capsule_collider_t* capsule = reg.try_get<capsule_collider_t>(entity);
                     capsule && !reg.all_of<box_collider_t>(entity))
            {
                const f32 radius = std::max(capsule->radius * max_abs(scale.x, scale.z), 0.001f);
                const f32 half_cylinder = std::max(capsule->height * std::abs(scale.y) * 0.5f - radius, 0.0f);
                shape = half_cylinder > 0.0f ? JPH::RefConst<JPH::Shape>(new JPH::CapsuleShape(half_cylinder, radius))
                                             : JPH::RefConst<JPH::Shape>(new JPH::SphereShape(radius));
                offset = capsule->offset * scale;
            }
            else
            {
                const box_collider_t fallback;
                const box_collider_t* box = reg.try_get<box_collider_t>(entity);
                if (!box) { box = &fallback; }

                const vec3_t half = {std::max(std::abs(box->extents.x * scale.x), 0.001f),
                                     std::max(std::abs(box->extents.y * scale.y), 0.001f),
                                     std::max(std::abs(box->extents.z * scale.z), 0.001f)};
                const f32 convex_radius =
                    std::min(JPH::cDefaultConvexRadius, std::min(std::min(half.x, half.y), half.z));
                shape = new JPH::BoxShape(to_jolt(half), convex_radius);
                offset = box->offset * scale;
            }

            if (offset.x != 0.0f || offset.y != 0.0f || offset.z != 0.0f)
            {
                shape = new JPH::RotatedTranslatedShape(to_jolt(offset), JPH::Quat::sIdentity(), shape);
            }
            return shape;
        }

        void on_body_destroyed(ecs::registry_t& reg, ecs::entity_t entity)
        {
            body_t& body = reg.get<body_t>(entity);
            state_t* const* state = reg.ctx().find<state_t*>();
            if (body.id.IsInvalid() || !state || !*state) { return; }

            JPH::BodyInterface& bodies = (*state)->system.GetBodyInterface();
            bodies.RemoveBody(body.id);
            bodies.DestroyBody(body.id);
            body.id = JPH::BodyID();
        }

        void create_body(state_t& state, ecs::registry_t& reg, ecs::entity_t entity, const rigidbody_t& rb,
                         const transform_t& transform, body_t& body, const vec3_t& scale)
        {
            JPH::BodyInterface& bodies = state.system.GetBodyInterface();

            const pose_t pose = world_pose(reg, transform);
            const bool moving = rb.type != body_type_e::STATIC;

            JPH::BodyCreationSettings settings(build_shape(reg, entity, scale), to_jolt(pose.position),
                                               to_jolt(pose.rotation), motion_type(rb.type),
                                               object_layer(rb.layer, moving));
            settings.mIsSensor = rb.is_sensor;
            settings.mCollideKinematicVsNonDynamic = rb.is_sensor && rb.type == body_type_e::KINEMATIC;
            settings.mMotionQuality = rb.continuous ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
            settings.mFriction = rb.friction;
            settings.mRestitution = rb.restitution;
            settings.mLinearDamping = rb.linear_damping;
            settings.mAngularDamping = rb.angular_damping;
            settings.mGravityFactor = rb.gravity_scale;
            settings.mUserData = user_data(entity);
            if (rb.type == body_type_e::DYNAMIC)
            {
                settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                settings.mMassPropertiesOverride.mMass = std::max(rb.mass, 0.001f);
            }

            body.id =
                bodies.CreateAndAddBody(settings, moving ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
            if (body.id.IsInvalid())
            {
                if (!state.warned_full)
                {
                    TAU_LOG_ERROR("PHYSICS", "Out of bodies, {} exist", state.system.GetNumBodies());
                    state.warned_full = true;
                }
                return;
            }

            body.shape_key = shape_key(reg, entity, scale);
            body.settings_key = settings_key(rb);
            body.position = rb.type == body_type_e::KINEMATIC ? pose.position : transform.local_position;
            body.rotation = rb.type == body_type_e::KINEMATIC ? pose.rotation : transform.local_rotation;
            body.was_active = moving;
            body.previous = body.current = local_pose(transform);

            if (rb.type == body_type_e::DYNAMIC)
            {
                if (body.set_linear) { bodies.SetLinearVelocity(body.id, to_jolt(body.linear_velocity)); }
                if (body.set_angular) { bodies.SetAngularVelocity(body.id, to_jolt(body.angular_velocity)); }
                bodies.AddForceAndTorque(body.id, to_jolt(body.force), to_jolt(body.torque));
                bodies.AddImpulse(body.id, to_jolt(body.impulse));
                bodies.AddAngularImpulse(body.id, to_jolt(body.angular_impulse));
            }
            body.set_linear = body.set_angular = false;
            body.force = body.torque = body.impulse = body.angular_impulse = {};
        }

        // live body, or the pending one; null unless dynamic
        body_t* dynamic_body(world_t& world, ecs::entity_t entity)
        {
            if (!state_of(world) || !world.registry.valid(entity)) { return nullptr; }
            const rigidbody_t* rb = world.registry.try_get<rigidbody_t>(entity);
            if (!rb || rb->type != body_type_e::DYNAMIC) { return nullptr; }
            return &world.registry.get_or_emplace<body_t>(entity);
        }

        const body_t* existing_body(const world_t& world, ecs::entity_t entity)
        {
            if (!state_of(world) || !world.registry.valid(entity)) { return nullptr; }
            return world.registry.try_get<body_t>(entity);
        }

        class query_layers_t final : public JPH::ObjectLayerFilter
        {
          public:
            explicit query_layers_t(u32_t mask) : m_mask(mask) {}
            bool ShouldCollide(JPH::ObjectLayer layer) const override
            { return ((m_mask >> user_layer(layer)) & 1u) != 0; }

          private:
            u32_t m_mask;
        };

        class query_bodies_t final : public JPH::BodyFilter
        {
          public:
            query_bodies_t(const world_t& world, const query_filter_t& filter) : m_sensors(filter.sensors)
            {
                if (const body_t* body = existing_body(world, filter.ignore)) { m_ignore = body->id; }
            }
            bool ShouldCollide(const JPH::BodyID& id) const override { return id != m_ignore; }
            bool ShouldCollideLocked(const JPH::Body& body) const override { return m_sensors || !body.IsSensor(); }

          private:
            JPH::BodyID m_ignore;
            bool m_sensors;
        };

        ray_hit_t make_hit(const state_t& state, const JPH::RRayCast& ray, const JPH::BodyID& id,
                           const JPH::SubShapeID& sub_shape, f32 fraction, f32 max_distance)
        {
            ray_hit_t hit;
            const JPH::RVec3 point = ray.GetPointOnRay(fraction);
            hit.point = from_jolt(point);
            hit.distance = fraction * max_distance;

            JPH::BodyLockRead lock(state.system.GetBodyLockInterface(), id);
            if (lock.Succeeded())
            {
                const JPH::Body& body = lock.GetBody();
                hit.entity = entity_of(body.GetUserData());
                hit.normal = from_jolt(body.GetWorldSpaceSurfaceNormal(sub_shape, point));
            }
            return hit;
        }

        std::vector<ecs::entity_t> overlapping(const world_t& world, const JPH::Shape& shape, JPH::RMat44Arg at,
                                               const query_filter_t& filter)
        {
            std::vector<ecs::entity_t> out;
            const state_t* state = state_of(world);
            if (!state) { return out; }

            JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
            const query_layers_t layers(filter.layers);
            const query_bodies_t bodies(world, filter);
            state->system.GetNarrowPhaseQuery().CollideShape(&shape, JPH::Vec3::sReplicate(1.0f), at,
                                                             JPH::CollideShapeSettings(), JPH::RVec3::sZero(),
                                                             collector, {}, layers, bodies);

            const JPH::BodyInterface& body_interface = state->system.GetBodyInterface();
            for (const JPH::CollideShapeResult& hit : collector.mHits)
            {
                const ecs::entity_t entity = entity_of(body_interface.GetUserData(hit.mBodyID2));
                if (entity != ecs::NULL_ENTITY && std::find(out.begin(), out.end(), entity) == out.end())
                {
                    out.push_back(entity);
                }
            }
            return out;
        }
    } // namespace

    void create(world_t& world)
    {
        if (world.physics_state) { return; }
        acquire_jolt();

        state_t* state = new state_t();
        state->system.Init(65536, 0, 65536, 10240, state->bp_layers, state->object_vs_bp, state->layer_pairs);
        state->system.SetContactListener(&state->listener);

        world.physics_state = state;
        world.registry.ctx().emplace<state_t*>(state);
        world.registry.on_destroy<body_t>().connect<&on_body_destroyed>();
    }

    void destroy(world_t& world)
    {
        state_t* state = world.physics_state;
        if (!state) { return; }

        stop(world);
        world.registry.on_destroy<body_t>().disconnect<&on_body_destroyed>();
        world.registry.ctx().erase<state_t*>();
        world.physics_state = nullptr;
        delete state;

        release_jolt();
    }

    void begin_step(world_t& world)
    {
        state_t* state = state_of(world);
        if (!state) { return; }

        ecs::registry_t& reg = world.registry;
        JPH::BodyInterface& bodies = state->system.GetBodyInterface();

        std::vector<ecs::entity_t> orphans;
        for (const ecs::entity_t entity : reg.view<body_t>())
        {
            if (!reg.all_of<rigidbody_t, transform_t>(entity)) { orphans.push_back(entity); }
        }
        reg.remove<body_t>(orphans.begin(), orphans.end());
        reg.remove<shown_pose_t>(orphans.begin(), orphans.end());

        for (auto [entity, rb, transform] : reg.view<rigidbody_t, transform_t>().each())
        {
            body_t& body = reg.get_or_emplace<body_t>(entity);
            const vec3_t scale = world_scale(reg, transform);

            if (!body.id.IsInvalid() && body.settings_key != settings_key(rb))
            {
                if (!body.set_linear) { body.linear_velocity = from_jolt(bodies.GetLinearVelocity(body.id)); }
                if (!body.set_angular) { body.angular_velocity = from_jolt(bodies.GetAngularVelocity(body.id)); }
                body.set_linear = body.set_angular = true;
                bodies.RemoveBody(body.id);
                bodies.DestroyBody(body.id);
                body.id = JPH::BodyID();
            }

            if (body.id.IsInvalid())
            {
                create_body(*state, reg, entity, rb, transform, body, scale);
                continue;
            }

            const u64_t shape = shape_key(reg, entity, scale);
            if (body.shape_key != shape)
            {
                bodies.SetShape(body.id, build_shape(reg, entity, scale), true,
                                rb.type == body_type_e::STATIC ? JPH::EActivation::DontActivate
                                                               : JPH::EActivation::Activate);
                body.shape_key = shape;
            }

            body.previous = local_pose(transform);
        }

        state->in_fixed_step = true;
    }

    void step(world_t& world, f32 dt)
    {
        state_t* state = state_of(world);
        if (!state) { return; }
        state->in_fixed_step = false;

        ecs::registry_t& reg = world.registry;
        JPH::BodyInterface& bodies = state->system.GetBodyInterfaceNoLock();

        for (auto [entity, rb, transform, body] : reg.view<rigidbody_t, transform_t, body_t>().each())
        {
            if (body.id.IsInvalid()) { continue; }

            if (rb.type == body_type_e::KINEMATIC)
            {
                const pose_t pose = world_pose(reg, transform);
                const bool moved = !glm_vec3_eqv(pose.position, body.position) ||
                                   std::memcmp(&pose.rotation, &body.rotation, sizeof(quat_t)) != 0;
                if (moved || bodies.IsActive(body.id))
                {
                    bodies.MoveKinematic(body.id, to_jolt(pose.position), to_jolt(pose.rotation), dt);
                }
                body.position = pose.position;
                body.rotation = pose.rotation;
                continue;
            }

            const bool moved = !glm_vec3_eqv(transform.local_position, body.position) ||
                               std::memcmp(&transform.local_rotation, &body.rotation, sizeof(quat_t)) != 0;
            if (moved)
            {
                const pose_t pose = world_pose(reg, transform);
                bodies.SetPositionAndRotation(body.id, to_jolt(pose.position), to_jolt(pose.rotation),
                                              rb.type == body_type_e::STATIC ? JPH::EActivation::DontActivate
                                                                             : JPH::EActivation::Activate);
                body.position = transform.local_position;
                body.rotation = transform.local_rotation;

                body.previous = local_pose(transform);
            }
        }

        state->system.Update(dt, 1, &state->temp_allocator, &state->jobs);

        // where the step moved them (inclusing bodies falling asleep)
        for (auto [entity, rb, transform, body] : reg.view<rigidbody_t, transform_t, body_t>().each())
        {
            if (body.id.IsInvalid() || rb.type != body_type_e::DYNAMIC) { continue; }

            const bool active = bodies.IsActive(body.id);
            if (active || body.was_active)
            {
                JPH::RVec3 position;
                JPH::Quat rotation;
                bodies.GetPositionAndRotation(body.id, position, rotation);
                set_local_pose(reg, transform, {from_jolt(position), from_jolt(rotation)});
                body.position = transform.local_position;
                body.rotation = transform.local_rotation;
            }
            body.was_active = active;
        }

        for (auto [entity, transform, body] : reg.view<transform_t, body_t>().each())
        {
            body.current = local_pose(transform);
        }

        std::vector<raw_contact_t> raw;
        {
            std::scoped_lock lock(state->listener.mutex);
            raw.swap(state->listener.contacts);
        }

        state->step_contacts.clear();
        for (const raw_contact_t& contact : raw)
        {
            state->step_contacts.push_back({entity_of(bodies.GetUserData(contact.a)),
                                            entity_of(bodies.GetUserData(contact.b)), contact.phase, contact.point,
                                            contact.normal});
        }
        state->frame_contacts.insert(state->frame_contacts.end(), state->step_contacts.begin(),
                                     state->step_contacts.end());
    }

    void interpolate(world_t& world)
    {
        if (!state_of(world)) { return; }

        ecs::registry_t& reg = world.registry;
        const f32 alpha = static_cast<f32>(std::clamp(time::get_fixed_alpha(), 0.0, 1.0));
        std::vector<ecs::entity_t> snapped;

        for (auto [entity, rb, transform, body] : reg.view<rigidbody_t, transform_t, body_t>().each())
        {
            if (!rb.interpolate || rb.type == body_type_e::STATIC || body.id.IsInvalid() ||
                !same_pose(local_pose(transform), body.current))
            {
                snapped.push_back(entity);
                continue;
            }

            const pose_t shown = blend(body.previous, body.current, alpha);
            reg.emplace_or_replace<shown_pose_t>(entity, shown.position, shown.rotation);
        }
        reg.remove<shown_pose_t>(snapped.begin(), snapped.end());
    }

    void end_frame(world_t& world)
    {
        if (state_t* state = state_of(world)) { state->frame_contacts.clear(); }
    }

    void stop(world_t& world)
    {
        state_t* state = state_of(world);
        if (!state) { return; }

        world.registry.clear<body_t>();
        world.registry.clear<shown_pose_t>();
        state->step_contacts.clear();
        state->frame_contacts.clear();
        state->in_fixed_step = false;

        std::scoped_lock lock(state->listener.mutex);
        state->listener.contacts.clear();
    }
} // namespace tau::physics::detail

namespace tau::physics
{
    using detail::body_t;
    using detail::state_t;

    std::span<const contact_t> contacts(const world_t& world)
    {
        const state_t* state = detail::state_of(world);
        if (!state) { return {}; }
        return state->in_fixed_step ? state->step_contacts : state->frame_contacts;
    }

    bool raycast(const world_t& world, const vec3_t& origin, const vec3_t& direction, f32 max_distance, ray_hit_t& out,
                 const query_filter_t& filter)
    {
        const state_t* state = detail::state_of(world);
        const f32 length = direction.length();
        if (!state || max_distance <= 0.0f || length <= 0.0f) { return false; }

        const JPH::RRayCast ray{detail::to_jolt(origin), detail::to_jolt(direction * (max_distance / length))};
        JPH::RayCastResult result;
        const detail::query_layers_t layers(filter.layers);
        const detail::query_bodies_t bodies(world, filter);
        if (!state->system.GetNarrowPhaseQuery().CastRay(ray, result, {}, layers, bodies)) { return false; }

        out = detail::make_hit(*state, ray, result.mBodyID, result.mSubShapeID2, result.mFraction, max_distance);
        return true;
    }

    std::vector<ray_hit_t> raycast_all(const world_t& world, const vec3_t& origin, const vec3_t& direction,
                                       f32 max_distance, const query_filter_t& filter)
    {
        std::vector<ray_hit_t> out;
        const state_t* state = detail::state_of(world);
        const f32 length = direction.length();
        if (!state || max_distance <= 0.0f || length <= 0.0f) { return out; }

        const JPH::RRayCast ray{detail::to_jolt(origin), detail::to_jolt(direction * (max_distance / length))};
        JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
        const detail::query_layers_t layers(filter.layers);
        const detail::query_bodies_t bodies(world, filter);
        state->system.GetNarrowPhaseQuery().CastRay(ray, JPH::RayCastSettings(), collector, {}, layers, bodies);
        collector.Sort();

        for (const JPH::RayCastResult& result : collector.mHits)
        {
            out.push_back(
                detail::make_hit(*state, ray, result.mBodyID, result.mSubShapeID2, result.mFraction, max_distance));
        }
        return out;
    }

    std::vector<ecs::entity_t> overlap_sphere(const world_t& world, const vec3_t& center, f32 radius,
                                              const query_filter_t& filter)
    {
        JPH::SphereShape sphere(std::max(radius, 0.001f));
        sphere.SetEmbedded();
        return detail::overlapping(world, sphere, JPH::RMat44::sTranslation(detail::to_jolt(center)), filter);
    }

    std::vector<ecs::entity_t> overlap_box(const world_t& world, const vec3_t& center, const vec3_t& half_extents,
                                           const quat_t& rotation, const query_filter_t& filter)
    {
        const vec3_t half = {std::max(std::abs(half_extents.x), 0.001f), std::max(std::abs(half_extents.y), 0.001f),
                             std::max(std::abs(half_extents.z), 0.001f)};
        JPH::BoxShape box(detail::to_jolt(half),
                          std::min(JPH::cDefaultConvexRadius, std::min(std::min(half.x, half.y), half.z)));
        box.SetEmbedded();
        return detail::overlapping(
            world, box, JPH::RMat44::sRotationTranslation(detail::to_jolt(rotation), detail::to_jolt(center)), filter);
    }

    vec3_t linear_velocity(const world_t& world, ecs::entity_t entity)
    {
        const body_t* body = detail::existing_body(world, entity);
        if (!body) { return {}; }
        if (body->id.IsInvalid()) { return body->set_linear ? body->linear_velocity : vec3_t{}; }
        return detail::from_jolt(world.physics_state->system.GetBodyInterface().GetLinearVelocity(body->id));
    }

    void set_linear_velocity(world_t& world, ecs::entity_t entity, const vec3_t& velocity)
    {
        body_t* body = detail::dynamic_body(world, entity);
        if (!body) { return; }
        if (body->id.IsInvalid())
        {
            body->linear_velocity = velocity;
            body->set_linear = true;
            return;
        }
        world.physics_state->system.GetBodyInterface().SetLinearVelocity(body->id, detail::to_jolt(velocity));
    }

    vec3_t angular_velocity(const world_t& world, ecs::entity_t entity)
    {
        const body_t* body = detail::existing_body(world, entity);
        if (!body) { return {}; }
        if (body->id.IsInvalid()) { return body->set_angular ? body->angular_velocity : vec3_t{}; }
        return detail::from_jolt(world.physics_state->system.GetBodyInterface().GetAngularVelocity(body->id));
    }

    void set_angular_velocity(world_t& world, ecs::entity_t entity, const vec3_t& velocity)
    {
        body_t* body = detail::dynamic_body(world, entity);
        if (!body) { return; }
        if (body->id.IsInvalid())
        {
            body->angular_velocity = velocity;
            body->set_angular = true;
            return;
        }
        world.physics_state->system.GetBodyInterface().SetAngularVelocity(body->id, detail::to_jolt(velocity));
    }

    void add_force(world_t& world, ecs::entity_t entity, const vec3_t& force)
    {
        body_t* body = detail::dynamic_body(world, entity);
        if (!body) { return; }
        if (body->id.IsInvalid()) { body->force += force; }
        else
        {
            world.physics_state->system.GetBodyInterface().AddForce(body->id, detail::to_jolt(force));
        }
    }

    void add_torque(world_t& world, ecs::entity_t entity, const vec3_t& torque)
    {
        body_t* body = detail::dynamic_body(world, entity);
        if (!body) { return; }
        if (body->id.IsInvalid()) { body->torque += torque; }
        else
        {
            world.physics_state->system.GetBodyInterface().AddTorque(body->id, detail::to_jolt(torque));
        }
    }

    void add_impulse(world_t& world, ecs::entity_t entity, const vec3_t& impulse)
    {
        body_t* body = detail::dynamic_body(world, entity);
        if (!body) { return; }
        if (body->id.IsInvalid()) { body->impulse += impulse; }
        else
        {
            world.physics_state->system.GetBodyInterface().AddImpulse(body->id, detail::to_jolt(impulse));
        }
    }

    void add_angular_impulse(world_t& world, ecs::entity_t entity, const vec3_t& impulse)
    {
        body_t* body = detail::dynamic_body(world, entity);
        if (!body) { return; }
        if (body->id.IsInvalid()) { body->angular_impulse += impulse; }
        else
        {
            world.physics_state->system.GetBodyInterface().AddAngularImpulse(body->id, detail::to_jolt(impulse));
        }
    }

    void set_layers_collide(world_t& world, u32_t a, u32_t b, bool collide)
    {
        state_t* state = detail::state_of(world);
        if (!state || a >= MAX_LAYERS || b >= MAX_LAYERS) { return; }

        std::array<u32_t, MAX_LAYERS>& collides = state->layer_pairs.collides;
        if (collide)
        {
            collides[a] |= 1u << b;
            collides[b] |= 1u << a;
        }
        else
        {
            collides[a] &= ~(1u << b);
            collides[b] &= ~(1u << a);
        }
    }

    bool layers_collide(const world_t& world, u32_t a, u32_t b)
    {
        const state_t* state = detail::state_of(world);
        if (!state || a >= MAX_LAYERS || b >= MAX_LAYERS) { return false; }
        return ((state->layer_pairs.collides[a] >> b) & 1u) != 0;
    }

    void set_gravity(world_t& world, const vec3_t& gravity)
    {
        if (state_t* state = detail::state_of(world)) { state->system.SetGravity(detail::to_jolt(gravity)); }
    }

    vec3_t gravity(const world_t& world)
    {
        const state_t* state = detail::state_of(world);
        return state ? detail::from_jolt(state->system.GetGravity()) : vec3_t{};
    }
} // namespace tau::physics
