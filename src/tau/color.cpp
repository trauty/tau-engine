#include "color.h"

namespace tau
{
    color_t::color_t() : r(1.0f), g(1.0f), b(1.0f), a(1.0f) {}
    color_t::color_t(f32 r, f32 g, f32 b) : r(r), g(g), b(b), a(1.0f) {}
    color_t::color_t(f32 r, f32 g, f32 b, f32 a) : r(r), g(g), b(b), a(a) {}
} // namespace tau