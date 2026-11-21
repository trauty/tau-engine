#include "viewport_input.h"

#include "imgui.h"
#include "tau-editor/editor_context.h"
#include "tau-editor/handle_math.h"
#include "tau-editor/keymap.h"
#include "tau/asset.h"
#include "tau/assets/mesh.h"
#include "tau/components/renderer.h"
#include "tau/components/transform.h"
#include "tau/engine.h"
#include "tau/input.h"
#include "tau/systems/guid.h"
#include "tau/systems/transform.h"
#include "tau/world.h"

#include <algorithm>
#include <cmath>

namespace tau::editor::viewport_input
{
    namespace
    {
        constexpr f32 CLICK_SLOP_PX = 4.0f;

        bool pressed_in_viewport = false;
        vec2 press_pos;

        vec2 mouse_in_viewport(const editor_context_t& ctx)
        {
            const ImVec2 mouse = ImGui::GetMousePos();
            return {mouse.x - ctx.viewport_screen_pos.x, mouse.y - ctx.viewport_screen_pos.y};
        }

        bool inside(const editor_context_t& ctx, vec2 pixel)
        {
            return pixel.x >= 0.0f && pixel.y >= 0.0f && pixel.x < ctx.viewport_screen_size.x &&
                   pixel.y < ctx.viewport_screen_size.y;
        }
    } // namespace

    ray_t ray_through(const editor_context_t& ctx, vec2 pixel)
    {
        const f32 ndc_x = pixel.x / ctx.viewport_screen_size.x * 2.0f - 1.0f;
        const f32 ndc_y = pixel.y / ctx.viewport_screen_size.y * 2.0f - 1.0f;

        // reverse z: the near plane is depth 1
        const vec4 world = inverse(ctx.view_proj) * vec4{ndc_x, ndc_y, 1.0f, 1.0f};
        const vec3 on_near = world.xyz() / world.w;
        const vec3 eye = translation_of(inverse(ctx.view));

        return {eye, normalize(on_near - eye)};
    }

    bool mouse_pixel(const editor_context_t& ctx, vec2& out)
    {
        out = mouse_in_viewport(ctx);
        return inside(ctx, out);
    }

    bool to_pixel(const editor_context_t& ctx, vec3 world, vec2& out)
    {
        const vec4 clip = ctx.view_proj * vec4{world, 1.0f};
        if (clip.w <= 1e-5f) { return false; }

        out = {(clip.x / clip.w * 0.5f + 0.5f) * ctx.viewport_screen_size.x,
               (clip.y / clip.w * 0.5f + 0.5f) * ctx.viewport_screen_size.y};
        return true;
    }

    bool mouse_ray(const editor_context_t& ctx, ray_t& out)
    {
        const vec2 pixel = mouse_in_viewport(ctx);
        if (!inside(ctx, pixel)) { return false; }

        out = ray_through(ctx, pixel);
        return true;
    }

    void frame_entities(world_t& world, editor_context_t& ctx, std::span<const ecs::entity_t> entities)
    {
        bool any = false;
        vec3 center = {0.0f, 0.0f, 0.0f};
        f32 radius = 0.0f;
        auto merge = [&](vec3 c, f32 r)
        {
            const vec3 offset = c - center;
            const f32 distance = std::sqrt(dot(offset, offset));
            if (!any || distance + radius <= r)
            {
                center = c;
                radius = r;
                any = true;
            }
            else if (distance + r > radius)
            {
                const f32 merged = (distance + radius + r) * 0.5f;
                center = center + offset * ((merged - radius) / distance);
                radius = merged;
            }
        };

        for (const ecs::entity_t entity : entities)
        {
            if (!world.registry.valid(entity) || !world.registry.all_of<transform_t, mesh_renderer_t>(entity))
            {
                continue;
            }

            const mesh_t* mesh = assets::get(world.registry.get<mesh_renderer_t>(entity).mesh);
            if (!mesh) { continue; }

            const vec3 c = transform_point(world.registry.get<transform_t>(entity).world_mat, mesh->local_center);
            const vec3 scale = transform_system::get_world_scale(world.registry, entity);
            merge(c, std::max(mesh->local_radius * std::max({scale.x, scale.y, scale.z}), 0.01f));
        }

        for (const ecs::entity_t entity : entities)
        {
            if (any) { break; }
            if (world.registry.valid(entity) && world.registry.all_of<transform_t>(entity))
            {
                merge(transform_system::get_world_position(world.registry, entity), 1.0f);
            }
        }
        if (!any) { return; }

        editor_camera_t& cam = ctx.editor_camera;
        const f32 half_fov = radians(cam.fov_deg) * 0.5f;
        const f32 distance = radius / std::sin(half_fov) * 1.1f;
        cam.position = center - tau::forward(cam.rotation) * distance;
    }

    void frame_selection(world_t& world, editor_context_t& ctx)
    {
        const ecs::entity_t entity = guid_system::find(world.registry, ctx.selection.primary());
        if (entity != ecs::NULL_ENTITY) { frame_entities(world, ctx, {&entity, 1}); }
    }

    void update(world_t& world, editor_context_t& ctx)
    {
        if (ctx.cur_mode != editor_mode_e::EDIT) { return; }

        ImGuiIO& io = ImGui::GetIO();
        const bool flying = tau::input::get_mouse_button(tau::input::mouse_button_e::Right);
        const bool over_viewport = ctx.is_viewport_hovered || ctx.is_viewport_focused;

        if (over_viewport && !flying && !io.WantTextInput && !ctx.handle_dragging)
        {
            using keymap::action_e;
            if (keymap::pressed(action_e::SELECT_TOOL)) { ctx.handle_mode = handle_mode_e::NONE; }
            if (keymap::pressed(action_e::MOVE_TOOL)) { ctx.handle_mode = handle_mode_e::MOVE; }
            if (keymap::pressed(action_e::ROTATE_TOOL)) { ctx.handle_mode = handle_mode_e::ROTATE; }
            if (keymap::pressed(action_e::SCALE_TOOL)) { ctx.handle_mode = handle_mode_e::SCALE; }
            if (keymap::pressed(action_e::FRAME_SELECTION)) { frame_selection(world, ctx); }
            if (keymap::pressed(action_e::DESELECT)) { ctx.selection.clear(); }
        }

        // press in the viewport, release within the slop: a click
        const vec2 pixel = mouse_in_viewport(ctx);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ctx.is_viewport_hovered && !flying && !ctx.handle_hovered &&
            inside(ctx, pixel))
        {
            pressed_in_viewport = true;
            press_pos = pixel;
        }

        if (pressed_in_viewport && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            pressed_in_viewport = false;

            const f32 dx = pixel.x - press_pos.x;
            const f32 dy = pixel.y - press_pos.y;
            if (dx * dx + dy * dy <= CLICK_SLOP_PX * CLICK_SLOP_PX)
            {
                ctx.click_pending = true;
                ctx.click_pos = press_pos;
                ctx.click_additive = io.KeyCtrl;
            }
        }
    }
} // namespace tau::editor::viewport_input
