#include "light_gizmos.h"

#include "tau-editor/editor_context.h"
#include "tau/components/light.h"
#include "tau/components/transform.h"
#include "tau/ecs.h"
#include "tau/render/overlay.h"
#include "tau/systems/guid.h"
#include "tau/systems/transform.h"

#include <cmath>
#include <vector>

namespace tau::editor::light_gizmos
{
    namespace
    {
        constexpr u32_t CIRCLE_SEGMENTS = 32;
        constexpr vec4 COLOR = {1.0f, 0.85f, 0.3f, 1.0f};
        constexpr vec4 INNER_COLOR = {1.0f, 0.85f, 0.3f, 0.45f};

        struct frame_t
        {
            vec3 origin;
            vec3 right;
            vec3 up;
            // the light shines along its -Z
            vec3 forward;
        };

        struct lines_t
        {
            std::vector<renderer::overlay_line_t>& out;

            void line(vec3 a, vec3 b, vec4 color = COLOR) { out.push_back({a, b, color}); }

            void circle(vec3 center, vec3 axis_a, vec3 axis_b, f32 radius, vec4 color = COLOR)
            {
                vec3 prev = center + axis_a * radius;
                for (u32_t i = 1; i <= CIRCLE_SEGMENTS; i++)
                {
                    const f32 t = 2.0f * PI * static_cast<f32>(i) / static_cast<f32>(CIRCLE_SEGMENTS);
                    const vec3 p = center + (axis_a * std::cos(t) + axis_b * std::sin(t)) * radius;
                    line(prev, p, color);
                    prev = p;
                }
            }
        };

        frame_t frame_of(const mat4& m)
        {
            return {
                {m[3][0], m[3][1], m[3][2]},
                normalize(vec3{m[0][0], m[0][1], m[0][2]}
                ),
                normalize(vec3{m[1][0], m[1][1], m[1][2]}
                ),
                -normalize(vec3{m[2][0], m[2][1], m[2][2]}
                ),
            };
        }

        void add_sun(lines_t& l, const frame_t& f)
        {
            constexpr f32 RADIUS = 0.4f;
            constexpr f32 LENGTH = 2.0f;
            l.circle(f.origin, f.right, f.up, RADIUS);
            for (const vec3 offset : {f.right, -f.right, f.up, -f.up})
            {
                const vec3 start = f.origin + offset * RADIUS;
                l.line(start, start + f.forward * LENGTH);
            }
        }

        void add_cone(lines_t& l, const frame_t& f, f32 range, f32 angle_deg, vec4 color, bool rays)
        {
            const f32 radius = range * std::tan(radians(std::fmin(angle_deg, 89.0f)));
            const vec3 base = f.origin + f.forward * range;
            l.circle(base, f.right, f.up, radius, color);
            if (!rays) { return; }
            for (const vec3 offset : {f.right, -f.right, f.up, -f.up})
            {
                l.line(f.origin, base + offset * radius, color);
            }
        }

        void add_rect(lines_t& l, const frame_t& f, vec2 size)
        {
            const vec3 x = f.right * (size.x * 0.5f);
            const vec3 y = f.up * (size.y * 0.5f);
            const vec3 corners[4] = {f.origin - x - y, f.origin + x - y, f.origin + x + y, f.origin - x + y};
            for (u32_t i = 0; i < 4; i++) { l.line(corners[i], corners[(i + 1) % 4]); }
            l.line(f.origin, f.origin + f.forward * 0.5f * std::fmax(size.x, size.y));
        }

        void add_tube(lines_t& l, const frame_t& f, vec2 size)
        {
            const vec3 half = f.right * (size.x * 0.5f);
            const f32 radius = size.y;
            l.circle(f.origin - half, f.up, f.forward, radius);
            l.circle(f.origin + half, f.up, f.forward, radius);
            for (const vec3 offset : {f.up, -f.up, f.forward, -f.forward})
            {
                l.line(f.origin - half + offset * radius, f.origin + half + offset * radius);
            }
        }
    } // namespace

    void draw(ecs::registry_t& reg, const editor_context_t& ctx, u32_t view_hash)
    {
        std::vector<renderer::overlay_line_t> out;
        lines_t lines{out};

        auto view = reg.view<light_t, transform_t>();
        for (const ecs::entity_t entity : view)
        {
            const light_t& light = view.get<light_t>(entity);
            if (!ctx.show_lights && !ctx.selection.contains(guid_system::of(reg, entity))) { continue; }

            const frame_t f = frame_of(transform_system::world_matrix(reg, entity));
            switch (light.type)
            {
            case light_type_e::SUN: add_sun(lines, f); break;
            case light_type_e::POINT:
                lines.circle(f.origin, f.right, f.up, light.range);
                lines.circle(f.origin, f.right, f.forward, light.range);
                lines.circle(f.origin, f.up, f.forward, light.range);
                break;
            case light_type_e::SPOT:
                add_cone(lines, f, light.range, light.outer_angle, COLOR, true);
                add_cone(lines, f, light.range, light.inner_angle, INNER_COLOR, false);
                break;
            case light_type_e::RECT: add_rect(lines, f, light.size); break;
            case light_type_e::TUBE: add_tube(lines, f, light.size); break;
            }
        }

        if (!out.empty()) { renderer::submit_overlay_lines(view_hash, out, 1.5f, renderer::overlay_depth_e::ON_TOP); }
    }
} // namespace tau::editor::light_gizmos
