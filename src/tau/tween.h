#pragma once

#include "tau/color.h"
#include "tau/defines.h"
#include "tau/ecs.h"
#include "tau/math.h"

#include <type_traits>

namespace tau { struct world_t; }

namespace tau::tween
{
    constexpr u32_t INFINITE_LOOPS = 0xFFFFFFFFu;

    enum class ease_e : u8_t
    {
        LINEAR,
        QUAD_IN,
        QUAD_OUT,
        QUAD_IN_OUT,
        CUBIC_IN,
        CUBIC_OUT,
        CUBIC_IN_OUT,
        QUART_IN,
        QUART_OUT,
        QUART_IN_OUT,
        SINE_IN,
        SINE_OUT,
        SINE_IN_OUT,
        EXPO_IN,
        EXPO_OUT,
        EXPO_IN_OUT,
        CIRC_IN,
        CIRC_OUT,
        CIRC_IN_OUT,
        BACK_IN,
        BACK_OUT,
        BACK_IN_OUT,
        ELASTIC_IN,
        ELASTIC_OUT,
        ELASTIC_IN_OUT,
        BOUNCE_IN,
        BOUNCE_OUT,
        BOUNCE_IN_OUT,
    };

    TAU_ENGINE_API f32 evaluate(ease_e ease, f32 t);

    struct handle_t
    {
        u32_t index = 0;
        u32_t generation = 0;

        bool operator==(const handle_t& other) const { return index == other.index && generation == other.generation; }
    };

    namespace detail
    {
        enum class value_kind_e : u8_t
        {
            F32,
            VEC2,
            VEC3,
            COLOR,
        };

        template <typename M>
        constexpr value_kind_e kind_of()
        {
            if constexpr (std::is_same_v<M, f32>) { return value_kind_e::F32; }
            else if constexpr (std::is_same_v<M, vec2>) { return value_kind_e::VEC2; }
            else if constexpr (std::is_same_v<M, vec3>) { return value_kind_e::VEC3; }
            else
            {
                static_assert(std::is_same_v<M, color_t>, "tweens take f32, vec2, vec3 or color_t fields");
                return value_kind_e::COLOR;
            }
        }

        template <typename P>
        struct member_of;

        template <typename C, typename M>
        struct member_of<M C::*>
        {
            using component = C;
            using value = M;
        };

        using resolve_fn_t = f32* (*)(ecs::registry_t& reg, ecs::entity_t entity);

        template <auto Member>
        f32* resolve(ecs::registry_t& reg, ecs::entity_t entity)
        {
            using component_t = typename member_of<decltype(Member)>::component;
            component_t* component = reg.try_get<component_t>(entity);
            return component ? reinterpret_cast<f32*>(&(component->*Member)) : nullptr;
        }

        TAU_ENGINE_API handle_t create(world_t& world, ecs::entity_t entity, resolve_fn_t resolve, value_kind_e kind,
                                       const f32* destination, f32 duration, ease_e ease);
    } // namespace detail

    class builder_t
    {
      public:
        builder_t(world_t& world, handle_t handle) : m_world(&world), m_handle(handle) {}

        TAU_ENGINE_API builder_t& delay(f32 seconds);

        TAU_ENGINE_API builder_t& loops(u32_t count);

        TAU_ENGINE_API builder_t& ping_pong(bool enabled = true);

        TAU_ENGINE_API builder_t& speed(f32 scale);

        TAU_ENGINE_API builder_t& from(f32 value);
        TAU_ENGINE_API builder_t& from(vec2 value);
        TAU_ENGINE_API builder_t& from(vec3 value);
        TAU_ENGINE_API builder_t& from(color_t value);

        operator handle_t() const { return m_handle; }
        handle_t handle() const { return m_handle; }

      private:
        builder_t& from_values(const f32* values, u32_t count);

        world_t* m_world;
        handle_t m_handle;
    };

    template <auto Member>
    builder_t to(world_t& world, ecs::entity_t entity,
                 const typename detail::member_of<decltype(Member)>::value& destination, f32 duration,
                 ease_e ease = ease_e::LINEAR)
    {
        using value_t = typename detail::member_of<decltype(Member)>::value;
        static_assert(std::is_standard_layout_v<value_t> && sizeof(value_t) % sizeof(f32) == 0);
        return builder_t{world, detail::create(world, entity, &detail::resolve<Member>, detail::kind_of<value_t>(),
                                               reinterpret_cast<const f32*>(&destination), duration, ease)};
    }

    TAU_ENGINE_API bool is_active(world_t& world, handle_t handle);

    TAU_ENGINE_API void cancel(world_t& world, handle_t handle);

    TAU_ENGINE_API void complete(world_t& world, handle_t handle);

    TAU_ENGINE_API void pause(world_t& world, handle_t handle, bool paused);

    TAU_ENGINE_API void cancel_all(world_t& world);

    TAU_ENGINE_API u32_t active_count(world_t& world);

    TAU_ENGINE_API void update(world_t& world, f32 dt);
} // namespace tau::tween
