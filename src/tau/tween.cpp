#include "tween.h"

#include "tau/world.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace tau::tween
{
    namespace
    {
        constexpr f32 PI = 3.14159265358979323846f;

        constexpr f32 BACK_C1 = 1.70158f;
        constexpr f32 BACK_C2 = BACK_C1 * 1.525f;
        constexpr f32 BACK_C3 = BACK_C1 + 1.0f;
        constexpr f32 ELASTIC_C4 = (2.0f * PI) / 3.0f;
        constexpr f32 ELASTIC_C5 = (2.0f * PI) / 4.5f;

        f32 bounce_out(f32 t)
        {
            constexpr f32 N = 7.5625f;
            constexpr f32 D = 2.75f;

            if (t < 1.0f / D) { return N * t * t; }
            if (t < 2.0f / D)
            {
                t -= 1.5f / D;
                return N * t * t + 0.75f;
            }
            if (t < 2.5f / D)
            {
                t -= 2.25f / D;
                return N * t * t + 0.9375f;
            }
            t -= 2.625f / D;
            return N * t * t + 0.984375f;
        }

        using detail::value_kind_e;

        constexpr u32_t MAX_COMPONENTS = 4;

        u32_t component_count(value_kind_e kind)
        {
            switch (kind)
            {
            case value_kind_e::F32: return 1;
            case value_kind_e::VEC2: return 2;
            case value_kind_e::VEC3: return 3;
            case value_kind_e::COLOR: return 4;
            }
            return 1;
        }

        struct tween_t
        {
            ecs::entity_t entity = ecs::NULL_ENTITY;
            detail::resolve_fn_t resolve = nullptr;
            value_kind_e kind = value_kind_e::F32;
            ease_e ease = ease_e::LINEAR;

            f32 from[MAX_COMPONENTS] = {};
            f32 to[MAX_COMPONENTS] = {};

            f32 duration = 0.0f;
            f32 elapsed = 0.0f;
            f32 delay = 0.0f;
            f32 speed = 1.0f;

            u32_t loops_total = 1;
            u32_t loops_done = 0;
            bool ping_pong = false;
            bool paused = false;

            bool capture_from = true;
            bool started = false;

            u32_t generation = 0;
            bool active = false;
        };

        struct pool_t
        {
            std::vector<tween_t> tweens;
            std::vector<u32_t> free_slots;
            u32_t next_generation = 1;
        };

        pool_t& pool_of(world_t& world)
        {
            if (pool_t* pool = world.registry.ctx().find<pool_t>()) { return *pool; }
            return world.registry.ctx().emplace<pool_t>();
        }

        tween_t* find(world_t& world, handle_t handle)
        {
            pool_t& pool = pool_of(world);
            if (handle.generation == 0 || handle.index >= pool.tweens.size()) { return nullptr; }

            tween_t& tween = pool.tweens[handle.index];
            if (!tween.active || tween.generation != handle.generation) { return nullptr; }

            return &tween;
        }

        void release(pool_t& pool, tween_t& tween)
        {
            tween.active = false;
            tween.resolve = nullptr;
            pool.free_slots.push_back(static_cast<u32_t>(&tween - pool.tweens.data()));
        }

        // false when the entity or its component is gone
        bool write(ecs::registry_t& reg, const tween_t& tween, f32 progress)
        {
            f32* target = tween.resolve(reg, tween.entity);
            if (target == nullptr) { return false; }

            const u32_t count = component_count(tween.kind);
            for (u32_t i = 0; i < count; i++) { target[i] = tween.from[i] + (tween.to[i] - tween.from[i]) * progress; }
            return true;
        }

        bool capture(ecs::registry_t& reg, tween_t& tween)
        {
            f32* target = tween.resolve(reg, tween.entity);
            if (target == nullptr) { return false; }
            if (!tween.capture_from) { return true; }

            const u32_t count = component_count(tween.kind);
            for (u32_t i = 0; i < count; i++) { tween.from[i] = target[i]; }
            return true;
        }

        // false once finished or its target is gone
        bool step(ecs::registry_t& reg, tween_t& tween, f32 dt)
        {
            if (tween.paused) { return tween.resolve(reg, tween.entity) != nullptr; }

            f32 scaled = dt * tween.speed;

            if (tween.delay > 0.0f)
            {
                tween.delay -= scaled;
                if (tween.delay > 0.0f) { return tween.resolve(reg, tween.entity) != nullptr; }

                scaled = -tween.delay;
                tween.delay = 0.0f;
            }

            if (!tween.started)
            {
                if (!capture(reg, tween)) { return false; }
                tween.started = true;
            }

            tween.elapsed += scaled;

            while (tween.elapsed >= tween.duration)
            {
                const bool infinite = tween.loops_total == INFINITE_LOOPS;
                if (!infinite && tween.loops_done + 1 >= tween.loops_total)
                {
                    write(reg, tween, tween.ping_pong && (tween.loops_done % 2 == 1) ? 0.0f : 1.0f);
                    return false;
                }

                tween.loops_done++;
                tween.elapsed -= tween.duration;
            }

            f32 t = tween.elapsed / tween.duration;
            if (tween.ping_pong && (tween.loops_done % 2 == 1)) { t = 1.0f - t; }

            return write(reg, tween, evaluate(tween.ease, t));
        }
    } // namespace

    f32 evaluate(ease_e ease, f32 t)
    {
        t = std::clamp(t, 0.0f, 1.0f);

        switch (ease)
        {
        case ease_e::LINEAR: return t;

        case ease_e::QUAD_IN: return t * t;
        case ease_e::QUAD_OUT: return 1.0f - (1.0f - t) * (1.0f - t);
        case ease_e::QUAD_IN_OUT: return t < 0.5f ? 2.0f * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 2.0f) / 2.0f;

        case ease_e::CUBIC_IN: return t * t * t;
        case ease_e::CUBIC_OUT: return 1.0f - std::pow(1.0f - t, 3.0f);
        case ease_e::CUBIC_IN_OUT: return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;

        case ease_e::QUART_IN: return t * t * t * t;
        case ease_e::QUART_OUT: return 1.0f - std::pow(1.0f - t, 4.0f);
        case ease_e::QUART_IN_OUT:
            return t < 0.5f ? 8.0f * t * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 4.0f) / 2.0f;

        case ease_e::SINE_IN: return 1.0f - std::cos((t * PI) / 2.0f);
        case ease_e::SINE_OUT: return std::sin((t * PI) / 2.0f);
        case ease_e::SINE_IN_OUT: return -(std::cos(PI * t) - 1.0f) / 2.0f;

        case ease_e::EXPO_IN: return t <= 0.0f ? 0.0f : std::pow(2.0f, 10.0f * t - 10.0f);
        case ease_e::EXPO_OUT: return t >= 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t);
        case ease_e::EXPO_IN_OUT:
            if (t <= 0.0f) { return 0.0f; }
            if (t >= 1.0f) { return 1.0f; }
            return t < 0.5f ? std::pow(2.0f, 20.0f * t - 10.0f) / 2.0f
                            : (2.0f - std::pow(2.0f, -20.0f * t + 10.0f)) / 2.0f;

        case ease_e::CIRC_IN: return 1.0f - std::sqrt(1.0f - t * t);
        case ease_e::CIRC_OUT: return std::sqrt(1.0f - (t - 1.0f) * (t - 1.0f));
        case ease_e::CIRC_IN_OUT:
            return t < 0.5f ? (1.0f - std::sqrt(1.0f - std::pow(2.0f * t, 2.0f))) / 2.0f
                            : (std::sqrt(1.0f - std::pow(-2.0f * t + 2.0f, 2.0f)) + 1.0f) / 2.0f;

        case ease_e::BACK_IN: return BACK_C3 * t * t * t - BACK_C1 * t * t;
        case ease_e::BACK_OUT: return 1.0f + BACK_C3 * std::pow(t - 1.0f, 3.0f) + BACK_C1 * std::pow(t - 1.0f, 2.0f);
        case ease_e::BACK_IN_OUT:
            return t < 0.5f
                       ? (std::pow(2.0f * t, 2.0f) * ((BACK_C2 + 1.0f) * 2.0f * t - BACK_C2)) / 2.0f
                       : (std::pow(2.0f * t - 2.0f, 2.0f) * ((BACK_C2 + 1.0f) * (t * 2.0f - 2.0f) + BACK_C2) + 2.0f) /
                             2.0f;

        case ease_e::ELASTIC_IN:
            if (t <= 0.0f) { return 0.0f; }
            if (t >= 1.0f) { return 1.0f; }
            return -std::pow(2.0f, 10.0f * t - 10.0f) * std::sin((t * 10.0f - 10.75f) * ELASTIC_C4);
        case ease_e::ELASTIC_OUT:
            if (t <= 0.0f) { return 0.0f; }
            if (t >= 1.0f) { return 1.0f; }
            return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * ELASTIC_C4) + 1.0f;
        case ease_e::ELASTIC_IN_OUT:
            if (t <= 0.0f) { return 0.0f; }
            if (t >= 1.0f) { return 1.0f; }
            return t < 0.5f
                       ? -(std::pow(2.0f, 20.0f * t - 10.0f) * std::sin((20.0f * t - 11.125f) * ELASTIC_C5)) / 2.0f
                       : (std::pow(2.0f, -20.0f * t + 10.0f) * std::sin((20.0f * t - 11.125f) * ELASTIC_C5)) / 2.0f +
                             1.0f;

        case ease_e::BOUNCE_IN: return 1.0f - bounce_out(1.0f - t);
        case ease_e::BOUNCE_OUT: return bounce_out(t);
        case ease_e::BOUNCE_IN_OUT:
            return t < 0.5f ? (1.0f - bounce_out(1.0f - 2.0f * t)) / 2.0f : (1.0f + bounce_out(2.0f * t - 1.0f)) / 2.0f;
        }

        return t;
    }

    namespace detail
    {
        handle_t create(world_t& world, ecs::entity_t entity, resolve_fn_t resolve, value_kind_e kind,
                        const f32* destination, f32 duration, ease_e ease)
        {
            if (resolve == nullptr || resolve(world.registry, entity) == nullptr) { return handle_t{}; }

            pool_t& pool = pool_of(world);

            u32_t index;
            if (!pool.free_slots.empty())
            {
                index = pool.free_slots.back();
                pool.free_slots.pop_back();
            }
            else
            {
                index = static_cast<u32_t>(pool.tweens.size());
                pool.tweens.emplace_back();
            }

            tween_t& tween = pool.tweens[index];
            tween = tween_t{};
            tween.entity = entity;
            tween.resolve = resolve;
            tween.kind = kind;
            tween.ease = ease;
            tween.duration = duration;
            tween.generation = pool.next_generation++;
            if (pool.next_generation == 0) { pool.next_generation = 1; }
            tween.active = true;

            const u32_t count = component_count(kind);
            for (u32_t i = 0; i < count; i++) { tween.to[i] = destination[i]; }

            if (duration <= 0.0f)
            {
                capture(world.registry, tween);
                write(world.registry, tween, 1.0f);
                const handle_t dead{index, tween.generation};
                release(pool, tween);
                return dead;
            }

            return handle_t{index, tween.generation};
        }
    } // namespace detail

    builder_t& builder_t::delay(f32 seconds)
    {
        if (tween_t* tween = find(*m_world, m_handle)) { tween->delay = seconds; }
        return *this;
    }

    builder_t& builder_t::loops(u32_t count)
    {
        if (tween_t* tween = find(*m_world, m_handle)) { tween->loops_total = std::max(1u, count); }
        return *this;
    }

    builder_t& builder_t::ping_pong(bool enabled)
    {
        if (tween_t* tween = find(*m_world, m_handle)) { tween->ping_pong = enabled; }
        return *this;
    }

    builder_t& builder_t::speed(f32 scale)
    {
        if (tween_t* tween = find(*m_world, m_handle)) { tween->speed = scale; }
        return *this;
    }

    builder_t& builder_t::from_values(const f32* values, u32_t count)
    {
        if (tween_t* tween = find(*m_world, m_handle))
        {
            for (u32_t i = 0; i < count; i++) { tween->from[i] = values[i]; }
            tween->capture_from = false;
        }
        return *this;
    }

    builder_t& builder_t::from(f32 value) { return from_values(&value, 1); }

    builder_t& builder_t::from(vec2_t value)
    {
        const f32 values[2] = {value.x, value.y};
        return from_values(values, 2);
    }

    builder_t& builder_t::from(vec3_t value)
    {
        const f32 values[3] = {value.x, value.y, value.z};
        return from_values(values, 3);
    }

    builder_t& builder_t::from(color_t value)
    {
        const f32 values[4] = {value.r, value.g, value.b, value.a};
        return from_values(values, 4);
    }

    bool is_active(world_t& world, handle_t handle) { return find(world, handle) != nullptr; }

    void cancel(world_t& world, handle_t handle)
    {
        if (tween_t* tween = find(world, handle)) { release(pool_of(world), *tween); }
    }

    void complete(world_t& world, handle_t handle)
    {
        tween_t* tween = find(world, handle);
        if (tween == nullptr) { return; }

        if (capture(world.registry, *tween))
        {
            const bool ends_at_start =
                tween->ping_pong && tween->loops_total != INFINITE_LOOPS && (tween->loops_total % 2 == 0);
            write(world.registry, *tween, ends_at_start ? 0.0f : 1.0f);
        }

        release(pool_of(world), *tween);
    }

    void pause(world_t& world, handle_t handle, bool paused)
    {
        if (tween_t* tween = find(world, handle)) { tween->paused = paused; }
    }

    void cancel_all(world_t& world)
    {
        pool_t& pool = pool_of(world);
        for (tween_t& tween : pool.tweens)
        {
            if (tween.active) { release(pool, tween); }
        }
    }

    u32_t active_count(world_t& world)
    {
        u32_t count = 0;
        for (const tween_t& tween : pool_of(world).tweens)
        {
            if (tween.active) { count++; }
        }
        return count;
    }

    void update(world_t& world, f32 dt)
    {
        if (dt == 0.0f) { return; }

        pool_t& pool = pool_of(world);
        for (u32_t i = 0; i < pool.tweens.size(); i++)
        {
            if (!pool.tweens[i].active) { continue; }
            if (!step(world.registry, pool.tweens[i], dt)) { release(pool, pool.tweens[i]); }
        }
    }
} // namespace tau::tween
