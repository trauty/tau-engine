#include "tau/components/physics.h"
#include "tau/components/transform.h"
#include "tau/physics/physics.h"
#include "tau/render/overlay.h"
#include "tau/systems/transform.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace tau::physics
{
    namespace
    {
        constexpr u32_t CIRCLE_SEGMENTS = 32;

        vec4 srgb(f32 r, f32 g, f32 b, f32 a)
        {
            auto linear = [](f32 c)
            {
                c /= 255.0f;
                return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
            };
            return {linear(r), linear(g), linear(b), a};
        }

        vec4 color_of(const rigidbody_t* rb)
        {
            if (!rb) { return srgb(160, 160, 160, 0.6f); }
            if (rb->is_sensor) { return srgb(240, 220, 60, 1.0f); }
            switch (rb->type)
            {
            case body_type_e::STATIC: return srgb(90, 220, 110, 1.0f);
            case body_type_e::KINEMATIC: return srgb(80, 160, 255, 1.0f);
            case body_type_e::DYNAMIC: break;
            }
            return srgb(255, 150, 50, 1.0f);
        }

        // rotation + position without scale, physics scales the shape
        struct frame_t
        {
            vec3 origin;
            vec3 axes[3];
            vec3 scale;

            vec3 at(const vec3& p) const { return origin + axes[0] * p.x + axes[1] * p.y + axes[2] * p.z; }
        };

        frame_t frame_of(const mat4& world)
        {
            frame_t frame;
            frame.origin = translation_of(world);
            for (u32_t i = 0; i < 3; i++)
            {
                const vec3 column = world[i].xyz();
                const f32 length = tau::length(column);
                frame.axes[i] = length > 0.0f ? column * (1.0f / length) : vec3{};
                (&frame.scale.x)[i] = length;
            }
            return frame;
        }

        struct lines_t
        {
            std::vector<renderer::overlay_line_t>& out;
            const frame_t& frame;
            vec4 color;

            void line(const vec3& a, const vec3& b) { out.push_back({frame.at(a), frame.at(b), color}); }

            void arc(const vec3& center, f32 radius, u32_t u, u32_t v, f32 from, f32 to)
            {
                const u32_t segments =
                    std::max(1u, static_cast<u32_t>(CIRCLE_SEGMENTS * (to - from) / (2.0f * std::numbers::pi_v<f32>)));
                vec3 previous;
                for (u32_t i = 0; i <= segments; i++)
                {
                    const f32 angle = from + (to - from) * static_cast<f32>(i) / static_cast<f32>(segments);
                    vec3 point = center;
                    (&point.x)[u] += std::cos(angle) * radius;
                    (&point.x)[v] += std::sin(angle) * radius;
                    if (i > 0) { line(previous, point); }
                    previous = point;
                }
            }

            void circle(const vec3& center, f32 radius, u32_t u, u32_t v)
            { arc(center, radius, u, v, 0.0f, 2.0f * std::numbers::pi_v<f32>); }
        };

        void add_box(lines_t& lines, const vec3& extents, const vec3& offset)
        {
            const vec3 scale = lines.frame.scale;
            const vec3 half = {std::abs(extents.x * scale.x), std::abs(extents.y * scale.y),
                               std::abs(extents.z * scale.z)};
            const vec3 center = offset * scale;

            auto corner = [&](u32_t i)
            {
                return center +
                       vec3{(i & 1) ? half.x : -half.x, (i & 2) ? half.y : -half.y, (i & 4) ? half.z : -half.z};
            };
            for (u32_t i = 0; i < 8; i++)
            {
                for (u32_t bit = 1; bit < 8; bit <<= 1)
                {
                    if (!(i & bit)) { lines.line(corner(i), corner(i | bit)); }
                }
            }
        }

        void add_sphere(lines_t& lines, f32 radius, const vec3& offset)
        {
            const vec3 scale = lines.frame.scale;
            const f32 r = radius * std::max({std::abs(scale.x), std::abs(scale.y), std::abs(scale.z)});
            const vec3 center = offset * scale;
            lines.circle(center, r, 0, 1);
            lines.circle(center, r, 0, 2);
            lines.circle(center, r, 1, 2);
        }

        void add_capsule(lines_t& lines, f32 radius, f32 height, const vec3& offset)
        {
            const vec3 scale = lines.frame.scale;
            const f32 r = radius * std::max(std::abs(scale.x), std::abs(scale.z));
            const f32 half = std::max(height * std::abs(scale.y) * 0.5f - r, 0.0f);
            const vec3 center = offset * scale;
            const vec3 top = center + vec3{0.0f, half, 0.0f};
            const vec3 bottom = center - vec3{0.0f, half, 0.0f};
            constexpr f32 pi = std::numbers::pi_v<f32>;

            lines.circle(top, r, 0, 2);
            lines.circle(bottom, r, 0, 2);
            lines.arc(top, r, 0, 1, 0.0f, pi);
            lines.arc(bottom, r, 0, 1, pi, 2.0f * pi);
            lines.arc(top, r, 2, 1, 0.0f, pi);
            lines.arc(bottom, r, 2, 1, pi, 2.0f * pi);
            lines.line(top + vec3{r, 0.0f, 0.0f}, bottom + vec3{r, 0.0f, 0.0f});
            lines.line(top - vec3{r, 0.0f, 0.0f}, bottom - vec3{r, 0.0f, 0.0f});
            lines.line(top + vec3{0.0f, 0.0f, r}, bottom + vec3{0.0f, 0.0f, r});
            lines.line(top - vec3{0.0f, 0.0f, r}, bottom - vec3{0.0f, 0.0f, r});
        }
    } // namespace

    void draw_colliders(ecs::registry_t& reg, u32_t view_hash)
    {
        std::vector<renderer::overlay_line_t> out;

        for (const ecs::entity_t entity : reg.view<transform_t>())
        {
            const box_collider_t* box = reg.try_get<box_collider_t>(entity);
            const sphere_collider_t* sphere = reg.try_get<sphere_collider_t>(entity);
            const capsule_collider_t* capsule = reg.try_get<capsule_collider_t>(entity);
            const rigidbody_t* rb = reg.try_get<rigidbody_t>(entity);
            if (!box && !sphere && !capsule && !rb) { continue; }

            const frame_t frame = frame_of(transform_system::world_matrix(reg, entity));
            lines_t lines{out, frame, color_of(rb)};
            if (box) { add_box(lines, box->extents, box->offset); }
            else if (sphere) { add_sphere(lines, sphere->radius, sphere->offset); }
            else if (capsule) { add_capsule(lines, capsule->radius, capsule->height, capsule->offset); }
            else
            {
                add_box(lines, box_collider_t{}.extents, {});
            }
        }

        if (!out.empty()) { renderer::submit_overlay_lines(view_hash, out, 1.5f, renderer::overlay_depth_e::ON_TOP); }
    }
} // namespace tau::physics
