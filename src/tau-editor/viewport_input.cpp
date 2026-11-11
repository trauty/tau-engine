#include "viewport_input.h"

#include "imgui.h"
#include "tau-editor/editor_context.h"
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
        vec2_t press_pos;

        vec2_t mouse_in_viewport(const editor_context_t& ctx)
        {
            const ImVec2 mouse = ImGui::GetMousePos();
            return {mouse.x - ctx.viewport_screen_pos.x, mouse.y - ctx.viewport_screen_pos.y};
        }

        bool inside(const editor_context_t& ctx, vec2_t pixel)
        {
            return pixel.x >= 0.0f && pixel.y >= 0.0f && pixel.x < ctx.viewport_screen_size.x &&
                   pixel.y < ctx.viewport_screen_size.y;
        }
    } // namespace

    ray_t ray_through(const editor_context_t& ctx, vec2_t pixel)
    {
        const f32 ndc_x = pixel.x / ctx.viewport_screen_size.x * 2.0f - 1.0f;
        const f32 ndc_y = pixel.y / ctx.viewport_screen_size.y * 2.0f - 1.0f;

        mat4_t inv_view_proj;
        glm_mat4_inv(const_cast<mat4_t&>(ctx.view_proj), inv_view_proj);

        // reverse z: the near plane is depth 1
        vec4 clip = {ndc_x, ndc_y, 1.0f, 1.0f};
        vec4 world;
        glm_mat4_mulv(inv_view_proj, clip, world);
        const vec3_t on_near = {world[0] / world[3], world[1] / world[3], world[2] / world[3]};

        mat4_t inv_view;
        glm_mat4_inv(const_cast<mat4_t&>(ctx.view), inv_view);
        const vec3_t eye = {inv_view[3][0], inv_view[3][1], inv_view[3][2]};

        return {eye, vec3_t::normalize(on_near - eye)};
    }

    bool mouse_pixel(const editor_context_t& ctx, vec2_t& out)
    {
        out = mouse_in_viewport(ctx);
        return inside(ctx, out);
    }

    bool to_pixel(const editor_context_t& ctx, vec3_t world, vec2_t& out)
    {
        vec4 point = {world.x, world.y, world.z, 1.0f};
        vec4 clip;
        glm_mat4_mulv(const_cast<mat4_t&>(ctx.view_proj), point, clip);
        if (clip[3] <= 1e-5f) { return false; }

        out = {(clip[0] / clip[3] * 0.5f + 0.5f) * ctx.viewport_screen_size.x,
               (clip[1] / clip[3] * 0.5f + 0.5f) * ctx.viewport_screen_size.y};
        return true;
    }

    bool mouse_ray(const editor_context_t& ctx, ray_t& out)
    {
        const vec2_t pixel = mouse_in_viewport(ctx);
        if (!inside(ctx, pixel)) { return false; }

        out = ray_through(ctx, pixel);
        return true;
    }

    void frame_selection(world_t& world, editor_context_t& ctx)
    {
        const ecs::entity_t entity = guid_system::find(world.registry, ctx.selection.primary());
        if (entity == ecs::NULL_ENTITY || !world.registry.all_of<transform_t>(entity)) { return; }

        const transform_t& transform = world.registry.get<transform_t>(entity);
        vec3_t center = transform_system::get_world_position(world.registry, entity);
        f32 radius = 1.0f;

        if (const mesh_renderer_t* renderer = world.registry.try_get<mesh_renderer_t>(entity))
        {
            if (const mesh_t* mesh = assets::get(renderer->mesh))
            {
                vec3_t local_center = mesh->local_center;
                glm_mat4_mulv3(const_cast<mat4_t&>(transform.world_mat), local_center, 1.0f, center);

                const vec3_t scale = transform_system::get_world_scale(world.registry, entity);
                radius = std::max(mesh->local_radius * std::max({scale.x, scale.y, scale.z}), 0.01f);
            }
        }

        editor_camera_t& cam = ctx.editor_camera;
        const f32 half_fov = glm_rad(cam.fov_deg) * 0.5f;
        const f32 distance = radius / std::sin(half_fov) * 1.1f;
        cam.position = center - cam.rotation.forward() * distance;
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
        const vec2_t pixel = mouse_in_viewport(ctx);
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
