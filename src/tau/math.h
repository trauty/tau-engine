#pragma once

#include "defines.h"

#include <cmath>
#include <cstddef>

#if defined(__SSE2__) || defined(_M_X64)
    #include <immintrin.h>
    #define TAU_MATH_SSE 1
#endif

// plain math types and free functions: column-major matrices, column vectors (m * v), quaternions x y z w
namespace tau
{
    constexpr f32 PI = 3.14159265358979323846f;

    constexpr f32 radians(f32 degrees) { return degrees * (PI / 180.0f); }
    constexpr f32 degrees(f32 radians) { return radians * (180.0f / PI); }

    struct vec2
    {
        f32 x = 0.0f, y = 0.0f;

        constexpr vec2() = default;
        constexpr vec2(f32 x_, f32 y_) : x(x_), y(y_) {}

        f32& operator[](int i) { return (&x)[i]; }
        f32 operator[](int i) const { return (&x)[i]; }
    };

    struct vec3
    {
        f32 x = 0.0f, y = 0.0f, z = 0.0f;

        constexpr vec3() = default;
        constexpr vec3(f32 x_, f32 y_, f32 z_) : x(x_), y(y_), z(z_) {}

        f32& operator[](int i) { return (&x)[i]; }
        f32 operator[](int i) const { return (&x)[i]; }
    };

    struct alignas(16) vec4
    {
        f32 x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;

        constexpr vec4() = default;
        constexpr vec4(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}
        constexpr vec4(vec3 v, f32 w_) : x(v.x), y(v.y), z(v.z), w(w_) {}

        f32& operator[](int i) { return (&x)[i]; }
        f32 operator[](int i) const { return (&x)[i]; }
        constexpr vec3 xyz() const { return {x, y, z}; }
    };

    struct alignas(16) quat
    {
        f32 x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;

        constexpr quat() = default;
        constexpr quat(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}
    };

    struct alignas(16) mat4
    {
        vec4 cols[4] = {
            {1.0f, 0.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f, 0.0f},
            {0.0f, 0.0f, 1.0f, 0.0f},
            {0.0f, 0.0f, 0.0f, 1.0f}
        };

        vec4& operator[](int c) { return cols[c]; }
        const vec4& operator[](int c) const { return cols[c]; }

        static constexpr mat4 zero()
        {
            mat4 m;
            for (vec4& c : m.cols) { c = {}; }
            return m;
        }
    };

    // row-major affine 3x4 (VkTransformMatrixKHR, GPU instance transforms)
    struct alignas(16) mat3x4
    {
        vec4 rows[3] = {
            {1.0f, 0.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f, 0.0f},
            {0.0f, 0.0f, 1.0f, 0.0f}
        };
    };

    static_assert(sizeof(vec2) == 8 && alignof(vec2) == 4);
    static_assert(sizeof(vec3) == 12 && alignof(vec3) == 4);
    static_assert(sizeof(vec4) == 16 && alignof(vec4) == 16);
    static_assert(sizeof(quat) == 16 && alignof(quat) == 16 && offsetof(quat, w) == 12);
    static_assert(sizeof(mat4) == 64 && alignof(mat4) == 16);
    static_assert(sizeof(mat3x4) == 48 && alignof(mat3x4) == 16);

    // --- vec2 ---

    constexpr vec2 operator+(vec2 a, vec2 b) { return {a.x + b.x, a.y + b.y}; }
    constexpr vec2 operator-(vec2 a, vec2 b) { return {a.x - b.x, a.y - b.y}; }
    constexpr vec2 operator-(vec2 a) { return {-a.x, -a.y}; }
    constexpr vec2 operator*(vec2 a, vec2 b) { return {a.x * b.x, a.y * b.y}; }
    constexpr vec2 operator*(vec2 a, f32 s) { return {a.x * s, a.y * s}; }
    constexpr vec2 operator*(f32 s, vec2 a) { return {a.x * s, a.y * s}; }
    constexpr vec2 operator/(vec2 a, f32 s) { return {a.x / s, a.y / s}; }
    constexpr vec2& operator+=(vec2& a, vec2 b) { return a = a + b; }
    constexpr vec2& operator-=(vec2& a, vec2 b) { return a = a - b; }
    constexpr vec2& operator*=(vec2& a, f32 s) { return a = a * s; }

    constexpr f32 dot(vec2 a, vec2 b) { return a.x * b.x + a.y * b.y; }
    // z of the 3d cross product
    constexpr f32 cross(vec2 a, vec2 b) { return a.x * b.y - a.y * b.x; }
    inline f32 length_sq(vec2 v) { return dot(v, v); }
    inline f32 length(vec2 v) { return std::sqrt(dot(v, v)); }

    inline vec2 normalize(vec2 v)
    {
        const f32 len = length(v);
        return len > 0.0f ? v * (1.0f / len) : vec2{};
    }

    // --- vec3 ---

    constexpr vec3 operator+(vec3 a, vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    constexpr vec3 operator-(vec3 a, vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    constexpr vec3 operator-(vec3 a) { return {-a.x, -a.y, -a.z}; }
    constexpr vec3 operator*(vec3 a, vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
    constexpr vec3 operator*(vec3 a, f32 s) { return {a.x * s, a.y * s, a.z * s}; }
    constexpr vec3 operator*(f32 s, vec3 a) { return {a.x * s, a.y * s, a.z * s}; }
    constexpr vec3 operator/(vec3 a, f32 s) { return {a.x / s, a.y / s, a.z / s}; }
    constexpr vec3& operator+=(vec3& a, vec3 b) { return a = a + b; }
    constexpr vec3& operator-=(vec3& a, vec3 b) { return a = a - b; }
    constexpr vec3& operator*=(vec3& a, f32 s) { return a = a * s; }
    constexpr bool operator==(vec3 a, vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

    constexpr f32 dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    constexpr vec3 cross(vec3 a, vec3 b)
    { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
    inline f32 length_sq(vec3 v) { return dot(v, v); }
    inline f32 length(vec3 v) { return std::sqrt(dot(v, v)); }
    constexpr vec3 lerp(vec3 a, vec3 b, f32 t) { return a + (b - a) * t; }

    inline vec3 normalize(vec3 v)
    {
        const f32 len = length(v);
        return len > 0.0f ? v * (1.0f / len) : vec3{};
    }

    // --- vec4 ---

    constexpr vec4 operator+(vec4 a, vec4 b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
    constexpr vec4 operator-(vec4 a, vec4 b) { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
    constexpr vec4 operator*(vec4 a, f32 s) { return {a.x * s, a.y * s, a.z * s, a.w * s}; }
    constexpr vec4 operator*(f32 s, vec4 a) { return a * s; }
    constexpr f32 dot(vec4 a, vec4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

    // --- quat ---

    constexpr f32 dot(quat a, quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
    constexpr quat conjugate(quat q) { return {-q.x, -q.y, -q.z, q.w}; }

    inline quat normalize(quat q)
    {
        const f32 len_sq = dot(q, q);
        if (len_sq <= 0.0f) { return {}; }
        const f32 inv = 1.0f / std::sqrt(len_sq);
        return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
    }

    inline quat inverse(quat q)
    {
        const f32 len_sq = dot(q, q);
        if (len_sq <= 0.0f) { return {}; }
        const f32 inv = 1.0f / len_sq;
        return {-q.x * inv, -q.y * inv, -q.z * inv, q.w * inv};
    }

    constexpr quat operator*(quat a, quat b)
    {
        return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    }

    inline quat angle_axis(f32 angle_radians, vec3 axis)
    {
        const vec3 n = normalize(axis);
        const f32 s = std::sin(angle_radians * 0.5f);
        return {n.x * s, n.y * s, n.z * s, std::cos(angle_radians * 0.5f)};
    }

    inline vec3 rotate(quat q, vec3 v)
    {
        const vec3 u = {q.x, q.y, q.z};
        const vec3 t = cross(u, v) * 2.0f;
        return v + t * q.w + cross(u, t);
    }

    inline vec3 right(quat q) { return rotate(q, {1.0f, 0.0f, 0.0f}); }
    inline vec3 up(quat q) { return rotate(q, {0.0f, 1.0f, 0.0f}); }
    // right-handed: forward is -Z
    inline vec3 forward(quat q) { return rotate(q, {0.0f, 0.0f, -1.0f}); }

    // shortest arc, normalized lerp when nearly parallel
    inline quat slerp(quat a, quat b, f32 t)
    {
        f32 cos_theta = dot(a, b);
        if (cos_theta < 0.0f)
        {
            b = {-b.x, -b.y, -b.z, -b.w};
            cos_theta = -cos_theta;
        }
        f32 wa = 1.0f - t;
        f32 wb = t;
        if (cos_theta < 0.9995f)
        {
            const f32 theta = std::acos(cos_theta);
            const f32 inv_sin = 1.0f / std::sin(theta);
            wa = std::sin(wa * theta) * inv_sin;
            wb = std::sin(wb * theta) * inv_sin;
        }
        return normalize(quat{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb});
    }

    // --- mat4 ---

    inline vec4 operator*(const mat4& m, vec4 v)
    {
#ifdef TAU_MATH_SSE
        __m128 r = _mm_mul_ps(_mm_load_ps(&m.cols[0].x), _mm_set1_ps(v.x));
    #ifdef __FMA__
        r = _mm_fmadd_ps(_mm_load_ps(&m.cols[1].x), _mm_set1_ps(v.y), r);
        r = _mm_fmadd_ps(_mm_load_ps(&m.cols[2].x), _mm_set1_ps(v.z), r);
        r = _mm_fmadd_ps(_mm_load_ps(&m.cols[3].x), _mm_set1_ps(v.w), r);
    #else
        r = _mm_add_ps(r, _mm_mul_ps(_mm_load_ps(&m.cols[1].x), _mm_set1_ps(v.y)));
        r = _mm_add_ps(r, _mm_mul_ps(_mm_load_ps(&m.cols[2].x), _mm_set1_ps(v.z)));
        r = _mm_add_ps(r, _mm_mul_ps(_mm_load_ps(&m.cols[3].x), _mm_set1_ps(v.w)));
    #endif
        vec4 out;
        _mm_store_ps(&out.x, r);
        return out;
#else
        return m.cols[0] * v.x + m.cols[1] * v.y + m.cols[2] * v.z + m.cols[3] * v.w;
#endif
    }

    inline mat4 operator*(const mat4& a, const mat4& b)
    {
        mat4 out;
        for (int c = 0; c < 4; c++) { out.cols[c] = a * b.cols[c]; }
        return out;
    }

    inline vec3 transform_point(const mat4& m, vec3 p) { return (m * vec4{p, 1.0f}).xyz(); }
    inline vec3 transform_dir(const mat4& m, vec3 d) { return (m * vec4{d, 0.0f}).xyz(); }

    constexpr vec3 translation_of(const mat4& m) { return m.cols[3].xyz(); }
    inline vec3 scale_of(const mat4& m)
    { return {length(m.cols[0].xyz()), length(m.cols[1].xyz()), length(m.cols[2].xyz())}; }

    constexpr mat4 transpose(const mat4& m)
    {
        mat4 t;
        for (int c = 0; c < 4; c++)
        {
            for (int r = 0; r < 4; r++) { t.cols[c][r] = m.cols[r][c]; }
        }
        return t;
    }

    // general 4x4 inverse (cofactors); a singular matrix gives zero
    inline mat4 inverse(const mat4& m)
    {
        const f32 a = m[0][0], b = m[0][1], c = m[0][2], d = m[0][3];
        const f32 e = m[1][0], f = m[1][1], g = m[1][2], h = m[1][3];
        const f32 i = m[2][0], j = m[2][1], k = m[2][2], l = m[2][3];
        const f32 n = m[3][0], o = m[3][1], p = m[3][2], q = m[3][3];

        const f32 kp_lo = k * q - l * p, jp_ln = j * q - l * o, jo_kn = j * p - k * o;
        const f32 ip_lm = i * q - l * n, io_km = i * p - k * n, in_jm = i * o - j * n;

        mat4 r;
        r[0][0] = f * kp_lo - g * jp_ln + h * jo_kn;
        r[1][0] = -(e * kp_lo - g * ip_lm + h * io_km);
        r[2][0] = e * jp_ln - f * ip_lm + h * in_jm;
        r[3][0] = -(e * jo_kn - f * io_km + g * in_jm);

        r[0][1] = -(b * kp_lo - c * jp_ln + d * jo_kn);
        r[1][1] = a * kp_lo - c * ip_lm + d * io_km;
        r[2][1] = -(a * jp_ln - b * ip_lm + d * in_jm);
        r[3][1] = a * jo_kn - b * io_km + c * in_jm;

        const f32 gp_ho = g * q - h * p, fp_hn = f * q - h * o, fo_gn = f * p - g * o;
        const f32 ep_hm = e * q - h * n, eo_gm = e * p - g * n, en_fm = e * o - f * n;

        r[0][2] = b * gp_ho - c * fp_hn + d * fo_gn;
        r[1][2] = -(a * gp_ho - c * ep_hm + d * eo_gm);
        r[2][2] = a * fp_hn - b * ep_hm + d * en_fm;
        r[3][2] = -(a * fo_gn - b * eo_gm + c * en_fm);

        const f32 gl_hk = g * l - h * k, fl_hj = f * l - h * j, fk_gj = f * k - g * j;
        const f32 el_hi = e * l - h * i, ek_gi = e * k - g * i, ej_fi = e * j - f * i;

        r[0][3] = -(b * gl_hk - c * fl_hj + d * fk_gj);
        r[1][3] = a * gl_hk - c * el_hi + d * ek_gi;
        r[2][3] = -(a * fl_hj - b * el_hi + d * ej_fi);
        r[3][3] = a * fk_gj - b * ek_gi + c * ej_fi;

        const f32 det = a * r[0][0] + b * r[1][0] + c * r[2][0] + d * r[3][0];
        const f32 inv_det = det != 0.0f ? 1.0f / det : 0.0f;
        for (vec4& col : r.cols) { col = col * inv_det; }
        return r;
    }

    // the rotation part of m (unit columns)
    inline mat4 mat_from_quat(quat q)
    {
        const f32 len_sq = dot(q, q);
        const f32 s = len_sq > 0.0f ? 2.0f / len_sq : 0.0f;
        const f32 xx = s * q.x * q.x, yy = s * q.y * q.y, zz = s * q.z * q.z;
        const f32 xy = s * q.x * q.y, xz = s * q.x * q.z, yz = s * q.y * q.z;
        const f32 wx = s * q.w * q.x, wy = s * q.w * q.y, wz = s * q.w * q.z;

        mat4 m;
        m[0] = {1.0f - yy - zz, xy + wz, xz - wy, 0.0f};
        m[1] = {xy - wz, 1.0f - xx - zz, yz + wx, 0.0f};
        m[2] = {xz + wy, yz - wx, 1.0f - xx - yy, 0.0f};
        return m;
    }

    // from the upper 3x3, which must be a rotation (normalize scaled columns first)
    inline quat quat_from_mat(const mat4& m)
    {
        const f32 trace = m[0][0] + m[1][1] + m[2][2];
        if (trace >= 0.0f)
        {
            const f32 r = std::sqrt(1.0f + trace), inv = 0.5f / r;
            return {inv * (m[1][2] - m[2][1]), inv * (m[2][0] - m[0][2]), inv * (m[0][1] - m[1][0]), r * 0.5f};
        }
        if (m[0][0] >= m[1][1] && m[0][0] >= m[2][2])
        {
            const f32 r = std::sqrt(1.0f - m[1][1] - m[2][2] + m[0][0]), inv = 0.5f / r;
            return {r * 0.5f, inv * (m[0][1] + m[1][0]), inv * (m[0][2] + m[2][0]), inv * (m[1][2] - m[2][1])};
        }
        if (m[1][1] >= m[2][2])
        {
            const f32 r = std::sqrt(1.0f - m[0][0] - m[2][2] + m[1][1]), inv = 0.5f / r;
            return {inv * (m[0][1] + m[1][0]), r * 0.5f, inv * (m[1][2] + m[2][1]), inv * (m[2][0] - m[0][2])};
        }
        const f32 r = std::sqrt(1.0f - m[0][0] - m[1][1] + m[2][2]), inv = 0.5f / r;
        return {inv * (m[0][2] + m[2][0]), inv * (m[1][2] + m[2][1]), r * 0.5f, inv * (m[0][1] - m[1][0])};
    }

    // translation * rotation * scale
    inline mat4 trs(vec3 t, quat r, vec3 s)
    {
        mat4 m = mat_from_quat(r);
        m[0] = m[0] * s.x;
        m[1] = m[1] * s.y;
        m[2] = m[2] * s.z;
        m[3] = {t, 1.0f};
        return m;
    }

    // inverse of trs for matrices without shear
    inline void decompose(const mat4& m, vec3& t, quat& r, vec3& s)
    {
        t = translation_of(m);
        s = scale_of(m);
        vec3 x = m[0].xyz() / s.x, y = m[1].xyz() / s.y, z = m[2].xyz() / s.z;
        if (dot(cross(x, y), z) < 0.0f)
        {
            s = -s;
            x = -x;
            y = -y;
            z = -z;
        }
        mat4 rot;
        rot[0] = {x, 0.0f};
        rot[1] = {y, 0.0f};
        rot[2] = {z, 0.0f};
        r = quat_from_mat(rot);
    }

    inline quat rotation_of(const mat4& m)
    {
        mat4 rot;
        rot[0] = {normalize(m[0].xyz()), 0.0f};
        rot[1] = {normalize(m[1].xyz()), 0.0f};
        rot[2] = {normalize(m[2].xyz()), 0.0f};
        return quat_from_mat(rot);
    }

    // m * rotation about z
    inline mat4 rotate_z(const mat4& m, f32 angle_radians)
    {
        const f32 c = std::cos(angle_radians), s = std::sin(angle_radians);
        mat4 r;
        r[0] = {c, s, 0.0f, 0.0f};
        r[1] = {-s, c, 0.0f, 0.0f};
        return m * r;
    }

    inline mat3x4 to_mat3x4(const mat4& m)
    {
        mat3x4 out;
        for (int row = 0; row < 3; row++) { out.rows[row] = {m[0][row], m[1][row], m[2][row], m[3][row]}; }
        return out;
    }

    // --- euler (x, y, z in radians) ---

    inline mat4 mat_from_euler_xyz(vec3 e)
    {
        const f32 sx = std::sin(e.x), cx = std::cos(e.x);
        const f32 sy = std::sin(e.y), cy = std::cos(e.y);
        const f32 sz = std::sin(e.z), cz = std::cos(e.z);

        mat4 m;
        m[0] = {cy * cz, cz * sx * sy + cx * sz, -cx * cz * sy + sx * sz, 0.0f};
        m[1] = {-cy * sz, cx * cz - sx * sy * sz, cz * sx + cx * sy * sz, 0.0f};
        m[2] = {sy, -cy * sx, cx * cy, 0.0f};
        return m;
    }

    inline quat quat_from_euler_xyz(vec3 e) { return quat_from_mat(mat_from_euler_xyz(e)); }

    inline vec3 euler_xyz(quat q)
    {
        const mat4 m = mat_from_quat(q);
        if (m[2][0] < 1.0f)
        {
            if (m[2][0] > -1.0f)
            {
                return {std::atan2(-m[2][1], m[2][2]), std::asin(m[2][0]), std::atan2(-m[1][0], m[0][0])};
            }
            return {-std::atan2(m[0][1], m[1][1]), -PI * 0.5f, 0.0f};
        }
        return {std::atan2(m[0][1], m[1][1]), PI * 0.5f, 0.0f};
    }

    // --- view and projection: right-handed (forward -Z, right +X, up +Y), zero to one depth ---

    inline mat4 look_at_rh(vec3 eye, vec3 center, vec3 up)
    {
        const vec3 f = normalize(center - eye);
        const vec3 s = normalize(cross(f, up));
        const vec3 u = cross(s, f);

        mat4 m;
        m[0] = {s.x, u.x, -f.x, 0.0f};
        m[1] = {s.y, u.y, -f.y, 0.0f};
        m[2] = {s.z, u.z, -f.z, 0.0f};
        m[3] = {-dot(s, eye), -dot(u, eye), dot(f, eye), 1.0f};
        return m;
    }

    inline mat4 perspective_rh_zo(f32 fovy_radians, f32 aspect, f32 near_z, f32 far_z)
    {
        const f32 f = 1.0f / std::tan(fovy_radians * 0.5f);
        const f32 fn = 1.0f / (near_z - far_z);

        mat4 m = mat4::zero();
        m[0][0] = f / aspect;
        m[1][1] = f;
        m[2][2] = far_z * fn;
        m[2][3] = -1.0f;
        m[3][2] = near_z * far_z * fn;
        return m;
    }

    // reverse z
    inline mat4 perspective_reverse_infinite_rh_zo(f32 fovy_radians, f32 aspect, f32 near_z)
    {
        const f32 focal = 1.0f / std::tan(fovy_radians * 0.5f);

        mat4 m = mat4::zero();
        m[0][0] = focal / aspect;
        m[1][1] = focal;
        m[2][3] = -1.0f;
        m[3][2] = near_z;
        return m;
    }

    inline mat4 ortho_rh_zo(f32 left, f32 right, f32 bottom, f32 top, f32 near_z, f32 far_z)
    {
        const f32 rl = 1.0f / (right - left);
        const f32 tb = 1.0f / (top - bottom);
        const f32 fn = -1.0f / (far_z - near_z);

        mat4 m = mat4::zero();
        m[0][0] = 2.0f * rl;
        m[1][1] = 2.0f * tb;
        m[2][2] = fn;
        m[3] = {-(right + left) * rl, -(top + bottom) * tb, near_z * fn, 1.0f};
        return m;
    }

    inline void flip_clip_y(mat4& proj)
    {
        for (vec4& c : proj.cols) { c.y = -c.y; }
    }

    // left, right, bottom, top, near, far; xyz unit normals pointing inside, w the distance
    inline void frustum_planes(const mat4& view_proj, vec4 out[6])
    {
        const mat4 t = transpose(view_proj);
        out[0] = t[3] + t[0];
        out[1] = t[3] - t[0];
        out[2] = t[3] + t[1];
        out[3] = t[3] - t[1];
        out[4] = t[3] + t[2];
        out[5] = t[3] - t[2];
        for (int i = 0; i < 6; i++)
        {
            const f32 len = length(out[i].xyz());
            out[i] = len > 0.0f ? out[i] * (1.0f / len) : vec4{};
        }
    }
} // namespace tau
