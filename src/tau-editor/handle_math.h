#pragma once

#include "tau/math.h"

#include <algorithm>
#include <cmath>

namespace tau::editor::handle_math
{
    inline f32 dot(vec3_t a, vec3_t b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline vec3_t cross(vec3_t a, vec3_t b)
    { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

    // closest points of a ray and a line, returns the line's parameter
    inline bool ray_line_param(vec3_t ray_origin, vec3_t ray_dir, vec3_t line_origin, vec3_t line_dir, f32& out_t)
    {
        const vec3_t w = ray_origin - line_origin;
        const f32 a = dot(ray_dir, ray_dir);
        const f32 b = dot(ray_dir, line_dir);
        const f32 c = dot(line_dir, line_dir);
        const f32 d = dot(ray_dir, w);
        const f32 e = dot(line_dir, w);

        const f32 denom = a * c - b * b;
        if (std::fabs(denom) < 1e-6f) { return false; }

        out_t = (a * e - b * d) / denom;
        return true;
    }

    inline bool ray_plane(vec3_t ray_origin, vec3_t ray_dir, vec3_t plane_origin, vec3_t plane_normal, vec3_t& out_hit)
    {
        const f32 denom = dot(plane_normal, ray_dir);
        if (std::fabs(denom) < 1e-6f) { return false; }

        const f32 t = dot(plane_normal, plane_origin - ray_origin) / denom;
        if (t < 0.0f) { return false; }

        out_hit = ray_origin + ray_dir * t;
        return true;
    }

    inline f32 signed_angle(vec3_t a, vec3_t b, vec3_t axis) { return std::atan2(dot(cross(a, b), axis), dot(a, b)); }

    inline f32 segment_distance(vec2_t p, vec2_t a, vec2_t b)
    {
        const f32 abx = b.x - a.x;
        const f32 aby = b.y - a.y;
        const f32 len2 = abx * abx + aby * aby;
        const f32 t = len2 > 0.0f ? std::clamp(((p.x - a.x) * abx + (p.y - a.y) * aby) / len2, 0.0f, 1.0f) : 0.0f;
        const f32 dx = p.x - (a.x + abx * t);
        const f32 dy = p.y - (a.y + aby * t);
        return std::sqrt(dx * dx + dy * dy);
    }

    inline f32 snap(f32 value, f32 step) { return step > 0.0f ? std::round(value / step) * step : value; }
} // namespace tau::editor::handle_math
