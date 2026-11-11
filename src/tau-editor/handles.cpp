#include "handles.h"

#include "imgui.h"
#include "tau-editor/editor_context.h"
#include "tau-editor/handle_math.h"
#include "tau-editor/history.h"
#include "tau-editor/viewport_input.h"
#include "tau/asset.h"
#include "tau/assets/mesh.h"
#include "tau/components/transform.h"
#include "tau/engine.h"
#include "tau/hash.h"
#include "tau/input.h"
#include "tau/rendering/overlay.h"
#include "tau/rendering/renderer.h"
#include "tau/rendering/renderer_constants.h"
#include "tau/rendering/rendergraph.h"
#include "tau/systems/guid.h"
#include "tau/systems/transform.h"
#include "tau/world.h"

#include <cmath>
#include <optional>
#include <vector>

namespace tau::editor::handles
{
    namespace
    {
        using namespace handle_math;

        constexpr u32_t VIEW = "PrimaryView"_h;

        constexpr f32 HANDLE_PX = 110.0f;
        constexpr f32 HIT_PX = 8.0f;

        constexpr f32 AXIS_START = 0.1f;
        constexpr f32 MOVE_END = 1.0f;
        constexpr f32 SCALE_END = 0.94f;
        constexpr f32 PLANE_MIN = 0.25f;
        constexpr f32 PLANE_MAX = 0.45f;
        constexpr f32 RING_RADIUS = 0.988f;
        constexpr f32 VIEW_RING_RADIUS = 1.15f;
        constexpr u32_t RING_SEGMENTS = 64;

        vec4_t srgb(f32 r, f32 g, f32 b)
        {
            auto linear = [](f32 c)
            {
                c /= 255.0f;
                return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
            };
            return {linear(r), linear(g), linear(b), 1.0f};
        }

        const vec4_t AXIS_COLORS[3] = {srgb(240, 70, 80), srgb(130, 205, 40), srgb(50, 135, 245)};
        const vec4_t HOT_COLOR = srgb(255, 210, 50);
        const vec4_t NEUTRAL_COLOR = srgb(220, 220, 220);

        enum class part_e : u8_t
        {
            NONE,
            AXIS_X,
            AXIS_Y,
            AXIS_Z,
            PLANE_X,
            PLANE_Y,
            PLANE_Z,
            CENTER,
            VIEW_RING,
        };

        part_e axis_part(u32_t axis) { return static_cast<part_e>(static_cast<u32_t>(part_e::AXIS_X) + axis); }
        part_e plane_part(u32_t axis) { return static_cast<part_e>(static_cast<u32_t>(part_e::PLANE_X) + axis); }
        bool is_axis(part_e part) { return part >= part_e::AXIS_X && part <= part_e::AXIS_Z; }
        bool is_plane(part_e part) { return part >= part_e::PLANE_X && part <= part_e::PLANE_Z; }
        u32_t axis_of(part_e part)
        {
            return is_axis(part) ? static_cast<u32_t>(part) - static_cast<u32_t>(part_e::AXIS_X)
                                 : static_cast<u32_t>(part) - static_cast<u32_t>(part_e::PLANE_X);
        }

        struct frame_t
        {
            vec3_t origin;
            vec3_t axes[3];
            vec3_t view_dir;
            f32 size;
        };

        struct target_t
        {
            ecs::entity_t entity;
            vec3_t world_pos;
            quat_t world_rot;
            vec3_t local_scale;
        };

        struct drag_t
        {
            part_e part;
            handle_mode_e mode;
            frame_t frame;
            f32 start_t = 0.0f;
            vec3_t start_hit;
            vec2_t start_pixel;
            f32 start_screen_angle = 0.0f;
            f32 last_angle = 0.0f;
            // set when the ring was grabbed close to edge on
            bool ring_tangent = false;
            vec2_t tangent_px;
            std::vector<target_t> targets;
        };

        struct meshes_t
        {
            asset_t<mesh_t> move;
            asset_t<mesh_t> rotate;
            asset_t<mesh_t> scale;
            asset_t<mesh_t> center;
            asset_t<mesh_t> plane;
        };

        editor_context_t* g_ctx = nullptr;
        meshes_t meshes;
        std::optional<drag_t> drag;
        part_e hovered = part_e::NONE;

        // 0 hidden: the axis points at the camera, or the plane is seen edge on
        f32 axis_visibility(const frame_t& frame, u32_t axis)
        { return std::clamp((0.99f - std::fabs(dot(frame.axes[axis], frame.view_dir))) / 0.04f, 0.0f, 1.0f); }
        f32 plane_visibility(const frame_t& frame, u32_t normal_axis)
        { return std::clamp((std::fabs(dot(frame.axes[normal_axis], frame.view_dir)) - 0.1f) / 0.1f, 0.0f, 1.0f); }

        // cyclic, so a mesh modelled along +y keeps its winding on every axis
        void axis_columns(const frame_t& frame, u32_t axis, vec3_t out[3])
        {
            out[0] = frame.axes[(axis + 2) % 3];
            out[1] = frame.axes[axis];
            out[2] = frame.axes[(axis + 1) % 3];
        }

        mat4_t model_matrix(vec3_t origin, const vec3_t columns[3], f32 scale)
        {
            mat4_t m;
            for (int c = 0; c < 3; c++)
            {
                m[c][0] = columns[c].x * scale;
                m[c][1] = columns[c].y * scale;
                m[c][2] = columns[c].z * scale;
            }
            m[3][0] = origin.x;
            m[3][1] = origin.y;
            m[3][2] = origin.z;
            return m;
        }

        bool selected_ancestor(world_t& world, const editor_context_t& ctx, ecs::entity_t entity)
        {
            for (ecs::entity_t p = world.registry.get<transform_t>(entity).get_parent(); p != ecs::NULL_ENTITY;
                 p = world.registry.get<transform_t>(p).get_parent())
            {
                if (ctx.selection.contains(guid_system::of(world.registry, p))) { return true; }
            }
            return false;
        }

        bool build_frame(ecs::registry_t& reg, const editor_context_t& ctx, frame_t& out)
        {
            const ecs::entity_t primary = guid_system::find(reg, ctx.selection.primary());
            if (primary == ecs::NULL_ENTITY || !reg.all_of<transform_t>(primary)) { return false; }

            out.origin = transform_system::get_world_position(reg, primary);

            if (ctx.handles_local || ctx.handle_mode == handle_mode_e::SCALE)
            {
                const quat_t rot = transform_system::get_world_rotation(reg, primary);
                out.axes[0] = vec3_t::normalize(rot.right());
                out.axes[1] = vec3_t::normalize(rot.up());
                out.axes[2] = vec3_t::normalize(rot.forward());
            }
            else
            {
                out.axes[0] = {1.0f, 0.0f, 0.0f};
                out.axes[1] = {0.0f, 1.0f, 0.0f};
                out.axes[2] = {0.0f, 0.0f, 1.0f};
            }

            const editor_camera_t& cam = ctx.editor_camera;
            const vec3_t to_origin = out.origin - cam.position;
            const f32 distance = std::sqrt(dot(to_origin, to_origin));
            if (distance < 1e-4f || ctx.viewport_screen_size.y <= 0.0f) { return false; }

            out.view_dir = to_origin * (1.0f / distance);
            out.size = distance * std::tan(glm_rad(cam.fov_deg) * 0.5f) * 2.0f * HANDLE_PX / ctx.viewport_screen_size.y;
            return true;
        }

        part_e hit_test(const editor_context_t& ctx, const frame_t& frame, vec2_t mouse,
                        const viewport_input::ray_t& ray)
        {
            part_e best = part_e::NONE;
            f32 best_dist = HIT_PX;
            auto consider = [&](part_e part, f32 dist)
            {
                if (dist < best_dist)
                {
                    best_dist = dist;
                    best = part;
                }
            };

            vec2_t origin_px;
            const bool origin_visible = viewport_input::to_pixel(ctx, frame.origin, origin_px);

            if (ctx.handle_mode == handle_mode_e::MOVE || ctx.handle_mode == handle_mode_e::SCALE)
            {
                const f32 end = ctx.handle_mode == handle_mode_e::MOVE ? MOVE_END : SCALE_END;
                for (u32_t axis = 0; axis < 3; axis++)
                {
                    if (axis_visibility(frame, axis) < 0.5f) { continue; }

                    vec2_t a;
                    vec2_t b;
                    if (viewport_input::to_pixel(ctx, frame.origin + frame.axes[axis] * (frame.size * AXIS_START), a) &&
                        viewport_input::to_pixel(ctx, frame.origin + frame.axes[axis] * (frame.size * end), b))
                    {
                        consider(axis_part(axis), segment_distance(mouse, a, b));
                    }
                }

                if (origin_visible)
                {
                    const f32 dx = mouse.x - origin_px.x;
                    const f32 dy = mouse.y - origin_px.y;
                    consider(part_e::CENTER, std::sqrt(dx * dx + dy * dy) - 2.0f);
                }
            }

            if (ctx.handle_mode == handle_mode_e::MOVE)
            {
                for (u32_t normal = 0; normal < 3; normal++)
                {
                    if (plane_visibility(frame, normal) < 0.5f) { continue; }

                    vec3_t hit;
                    if (!ray_plane(ray.origin, ray.direction, frame.origin, frame.axes[normal], hit)) { continue; }

                    const vec3_t local = hit - frame.origin;
                    const f32 u = dot(local, frame.axes[(normal + 2) % 3]) / frame.size;
                    const f32 v = dot(local, frame.axes[(normal + 1) % 3]) / frame.size;
                    if (u >= PLANE_MIN && u <= PLANE_MAX && v >= PLANE_MIN && v <= PLANE_MAX)
                    {
                        consider(plane_part(normal), 1.0f);
                    }
                }
            }

            if (ctx.handle_mode == handle_mode_e::ROTATE)
            {
                for (u32_t axis = 0; axis < 3; axis++)
                {
                    const vec3_t u = frame.axes[(axis + 1) % 3];
                    const vec3_t v = frame.axes[(axis + 2) % 3];

                    vec2_t prev;
                    bool prev_ok = false;
                    for (u32_t i = 0; i <= RING_SEGMENTS; i++)
                    {
                        const f32 angle = static_cast<f32>(i) / RING_SEGMENTS * 6.2831853f;
                        const vec3_t point =
                            frame.origin + (u * std::cos(angle) + v * std::sin(angle)) * (frame.size * RING_RADIUS);

                        vec2_t px;
                        const bool ok = viewport_input::to_pixel(ctx, point, px);
                        if (ok && prev_ok) { consider(axis_part(axis), segment_distance(mouse, prev, px)); }
                        prev = px;
                        prev_ok = ok;
                    }
                }

                if (origin_visible)
                {
                    vec2_t rim;
                    const vec3_t side = vec3_t::normalize(cross(frame.view_dir, ctx.editor_camera.rotation.up()));
                    if (viewport_input::to_pixel(ctx, frame.origin + side * (frame.size * VIEW_RING_RADIUS), rim))
                    {
                        const f32 radius = std::hypot(rim.x - origin_px.x, rim.y - origin_px.y);
                        const f32 dist = std::hypot(mouse.x - origin_px.x, mouse.y - origin_px.y);
                        consider(part_e::VIEW_RING, std::fabs(dist - radius));
                    }
                }
            }

            return best;
        }

        vec4_t color_for(part_e part, vec4_t base, f32 alpha)
        {
            const bool hot = drag ? drag->part == part : hovered == part;
            vec4_t color = hot ? HOT_COLOR : base;
            color.w *= alpha;
            return color;
        }

        void draw(const editor_context_t& ctx, const frame_t& frame)
        {
            using renderer::overlay_depth_e;

            if (ctx.handle_mode == handle_mode_e::MOVE || ctx.handle_mode == handle_mode_e::SCALE)
            {
                const asset_t<mesh_t> axis_mesh = ctx.handle_mode == handle_mode_e::MOVE ? meshes.move : meshes.scale;
                for (u32_t axis = 0; axis < 3; axis++)
                {
                    const f32 alpha = axis_visibility(frame, axis);
                    if (alpha <= 0.0f) { continue; }

                    vec3_t columns[3];
                    axis_columns(frame, axis, columns);
                    renderer::submit_overlay_mesh(VIEW, axis_mesh, model_matrix(frame.origin, columns, frame.size),
                                                  color_for(axis_part(axis), AXIS_COLORS[axis], alpha),
                                                  overlay_depth_e::ON_TOP);
                }

                const vec3_t identity[3] = {frame.axes[0], frame.axes[1], frame.axes[2]};
                renderer::submit_overlay_mesh(VIEW, meshes.center, model_matrix(frame.origin, identity, frame.size),
                                              color_for(part_e::CENTER, NEUTRAL_COLOR, 1.0f), overlay_depth_e::ON_TOP);
            }

            if (ctx.handle_mode == handle_mode_e::MOVE)
            {
                for (u32_t normal = 0; normal < 3; normal++)
                {
                    const f32 alpha = plane_visibility(frame, normal) * 0.5f;
                    if (alpha <= 0.0f) { continue; }

                    vec3_t columns[3];
                    axis_columns(frame, normal, columns);
                    renderer::submit_overlay_mesh(VIEW, meshes.plane, model_matrix(frame.origin, columns, frame.size),
                                                  color_for(plane_part(normal), AXIS_COLORS[normal], alpha),
                                                  overlay_depth_e::ON_TOP);
                }
            }

            if (ctx.handle_mode == handle_mode_e::ROTATE)
            {
                const asset_t<mesh_t> ring = meshes.rotate;
                for (u32_t axis = 0; axis < 3; axis++)
                {
                    vec3_t columns[3];
                    axis_columns(frame, axis, columns);
                    renderer::submit_overlay_mesh(VIEW, ring, model_matrix(frame.origin, columns, frame.size),
                                                  color_for(axis_part(axis), AXIS_COLORS[axis], 1.0f),
                                                  overlay_depth_e::ON_TOP);
                }

                const vec3_t up = vec3_t::normalize(frame.view_dir * -1.0f);
                const vec3_t right = vec3_t::normalize(cross(up, ctx.editor_camera.rotation.up()));
                const vec3_t forward = cross(right, up);
                const vec3_t columns[3] = {right, up, forward};
                renderer::submit_overlay_mesh(
                    VIEW, ring, model_matrix(frame.origin, columns, frame.size * VIEW_RING_RADIUS / RING_RADIUS),
                    color_for(part_e::VIEW_RING, NEUTRAL_COLOR, 1.0f), overlay_depth_e::ON_TOP);
            }
        }

        // screen dir of a positive turn at the ring point nearest the mouse
        vec2_t ring_tangent(const editor_context_t& ctx, const frame_t& frame, u32_t axis, vec2_t mouse)
        {
            const vec3_t u = frame.axes[(axis + 1) % 3];
            const vec3_t v = frame.axes[(axis + 2) % 3];

            vec3_t nearest = frame.origin + u * frame.size;
            f32 nearest_dist = 1e30f;
            for (u32_t i = 0; i < RING_SEGMENTS; i++)
            {
                const f32 angle = static_cast<f32>(i) / RING_SEGMENTS * 6.2831853f;
                const vec3_t point =
                    frame.origin + (u * std::cos(angle) + v * std::sin(angle)) * (frame.size * RING_RADIUS);

                vec2_t px;
                if (!viewport_input::to_pixel(ctx, point, px)) { continue; }

                const f32 dist = std::hypot(px.x - mouse.x, px.y - mouse.y);
                if (dist < nearest_dist)
                {
                    nearest_dist = dist;
                    nearest = point;
                }
            }

            const vec3_t tangent = vec3_t::normalize(cross(frame.axes[axis], nearest - frame.origin));
            vec2_t a;
            vec2_t b;
            if (!viewport_input::to_pixel(ctx, nearest, a) ||
                !viewport_input::to_pixel(ctx, nearest + tangent * (frame.size * 0.1f), b))
            {
                return {1.0f, 0.0f};
            }

            const f32 len = std::hypot(b.x - a.x, b.y - a.y);
            return len > 1e-4f ? vec2_t{(b.x - a.x) / len, (b.y - a.y) / len} : vec2_t{1.0f, 0.0f};
        }

        void begin_drag(world_t& world, editor_context_t& ctx, const frame_t& frame, part_e part,
                        const viewport_input::ray_t& ray, vec2_t mouse)
        {
            drag_t d;
            d.part = part;
            d.mode = ctx.handle_mode;
            d.frame = frame;
            d.start_pixel = mouse;

            std::vector<history::target_t> undo_targets;
            for (const u64_t guid : ctx.selection.guids)
            {
                const ecs::entity_t entity = guid_system::find(world.registry, guid);
                if (entity == ecs::NULL_ENTITY || !world.registry.all_of<transform_t>(entity)) { continue; }
                if (selected_ancestor(world, ctx, entity)) { continue; }

                d.targets.push_back({entity, transform_system::get_world_position(world.registry, entity),
                                     transform_system::get_world_rotation(world.registry, entity),
                                     world.registry.get<transform_t>(entity).local_scale});
                undo_targets.push_back(history::entity_target(guid));
            }

            constexpr const char* LABELS[] = {"", "Move", "Rotate", "Scale"};
            history::begin(world, LABELS[static_cast<u32_t>(d.mode)], undo_targets);

            if (is_axis(part) && d.mode != handle_mode_e::ROTATE)
            {
                ray_line_param(ray.origin, ray.direction, frame.origin, frame.axes[axis_of(part)], d.start_t);
            }
            else if (is_plane(part))
            {
                ray_plane(ray.origin, ray.direction, frame.origin, frame.axes[axis_of(part)], d.start_hit);
            }
            else if (part == part_e::CENTER && d.mode == handle_mode_e::MOVE)
            {
                ray_plane(ray.origin, ray.direction, frame.origin, frame.view_dir, d.start_hit);
            }
            else if (is_axis(part))
            {
                const vec3_t axis = frame.axes[axis_of(part)];
                d.ring_tangent = std::fabs(dot(axis, frame.view_dir)) < 0.25f ||
                                 !ray_plane(ray.origin, ray.direction, frame.origin, axis, d.start_hit);
                if (d.ring_tangent) { d.tangent_px = ring_tangent(ctx, frame, axis_of(part), mouse); }
            }

            vec2_t origin_px;
            if (viewport_input::to_pixel(ctx, frame.origin, origin_px))
            {
                d.start_screen_angle = std::atan2(mouse.y - origin_px.y, mouse.x - origin_px.x);
            }

            drag = std::move(d);
        }

        void apply_move(world_t& world, vec3_t delta)
        {
            for (const target_t& t : drag->targets)
            {
                transform_system::set_world_position(world.registry, t.entity, t.world_pos + delta);
            }
        }

        void apply_rotation(world_t& world, vec3_t axis, f32 angle)
        {
            const quat_t q = quat_t::angle_axis(angle, axis);
            const vec3_t pivot = drag->frame.origin;

            for (const target_t& t : drag->targets)
            {
                vec3_t offset = t.world_pos - pivot;
                vec3_t rotated;
                glm_quat_rotatev(q, offset, rotated);

                transform_system::set_world_rotation(world.registry, t.entity, q * t.world_rot);
                transform_system::set_world_position(world.registry, t.entity, pivot + rotated);
            }
        }

        void apply_scale(world_t& world, vec3_t factor)
        {
            for (const target_t& t : drag->targets)
            {
                world.registry.get<transform_t>(t.entity).local_scale = {
                    t.local_scale.x * factor.x, t.local_scale.y * factor.y, t.local_scale.z * factor.z};
            }
        }

        void update_drag(world_t& world, const editor_context_t& ctx, const viewport_input::ray_t& ray, vec2_t mouse)
        {
            const frame_t& frame = drag->frame;
            const bool snapping = ImGui::GetIO().KeyCtrl;

            if (drag->mode == handle_mode_e::MOVE)
            {
                vec3_t delta = {0.0f, 0.0f, 0.0f};
                if (is_axis(drag->part))
                {
                    const vec3_t axis = frame.axes[axis_of(drag->part)];
                    f32 t = 0.0f;
                    if (!ray_line_param(ray.origin, ray.direction, frame.origin, axis, t)) { return; }

                    const f32 along = snapping ? snap(t - drag->start_t, ctx.snap_move) : t - drag->start_t;
                    delta = axis * along;
                }
                else
                {
                    const vec3_t normal = is_plane(drag->part) ? frame.axes[axis_of(drag->part)] : frame.view_dir;
                    vec3_t hit;
                    if (!ray_plane(ray.origin, ray.direction, frame.origin, normal, hit)) { return; }

                    delta = hit - drag->start_hit;
                    if (snapping && is_plane(drag->part))
                    {
                        const vec3_t u = frame.axes[(axis_of(drag->part) + 2) % 3];
                        const vec3_t v = frame.axes[(axis_of(drag->part) + 1) % 3];
                        delta = u * snap(dot(delta, u), ctx.snap_move) + v * snap(dot(delta, v), ctx.snap_move);
                    }
                }
                apply_move(world, delta);
            }
            else if (drag->mode == handle_mode_e::ROTATE)
            {
                vec3_t axis = frame.view_dir;
                f32 angle = drag->last_angle;

                if (drag->part == part_e::VIEW_RING)
                {
                    vec2_t origin_px;
                    if (viewport_input::to_pixel(ctx, frame.origin, origin_px))
                    {
                        angle = drag->start_screen_angle - std::atan2(mouse.y - origin_px.y, mouse.x - origin_px.x);
                    }
                }
                else if (drag->ring_tangent)
                {
                    axis = frame.axes[axis_of(drag->part)];
                    const vec2_t moved = mouse - drag->start_pixel;
                    angle = (moved.x * drag->tangent_px.x + moved.y * drag->tangent_px.y) / (HANDLE_PX * RING_RADIUS);
                }
                else
                {
                    axis = frame.axes[axis_of(drag->part)];
                    vec3_t hit;
                    if (ray_plane(ray.origin, ray.direction, frame.origin, axis, hit))
                    {
                        angle = signed_angle(drag->start_hit - frame.origin, hit - frame.origin, axis);
                    }
                }

                drag->last_angle = angle;
                if (snapping) { angle = snap(angle, glm_rad(ctx.snap_rotate_deg)); }
                apply_rotation(world, axis, angle);
            }
            else if (drag->mode == handle_mode_e::SCALE)
            {
                vec3_t factor = {1.0f, 1.0f, 1.0f};
                if (is_axis(drag->part))
                {
                    const u32_t axis = axis_of(drag->part);
                    f32 t = 0.0f;
                    if (!ray_line_param(ray.origin, ray.direction, frame.origin, frame.axes[axis], t)) { return; }

                    f32 amount = (t - drag->start_t) / frame.size;
                    if (snapping) { amount = snap(amount, ctx.snap_scale); }
                    (&factor.x)[axis] = std::max(1.0f + amount, 0.001f);
                }
                else
                {
                    f32 amount = ((mouse.x - drag->start_pixel.x) - (mouse.y - drag->start_pixel.y)) / HANDLE_PX;
                    if (snapping) { amount = snap(amount, ctx.snap_scale); }
                    const f32 uniform = std::max(1.0f + amount, 0.001f);
                    factor = {uniform, uniform, uniform};
                }
                apply_scale(world, factor);
            }
        }

        void end_drag(world_t& world)
        {
            history::end(world);
            drag.reset();
        }

        // drawn at render, after the transform update
        void build_handles(renderer::rendergraph_t&, ecs::registry_t& reg)
        {
            if (!g_ctx || g_ctx->cur_mode != editor_mode_e::EDIT || g_ctx->handle_mode == handle_mode_e::NONE)
            {
                return;
            }

            frame_t frame;
            if (build_frame(reg, *g_ctx, frame)) { draw(*g_ctx, frame); }
        }
    } // namespace

    void init(editor_context_t& ctx)
    {
        g_ctx = &ctx;
        renderer::register_renderer_feature("EditorHandles"_h, renderer::FEATURE_ORDER_OVERLAY - 1, &build_handles);

        meshes.move = assets::pin(assets::load<mesh_t>("engine://assets/models/move.glb"));
        meshes.rotate = assets::pin(assets::load<mesh_t>("engine://assets/models/rotate.glb"));
        meshes.scale = assets::pin(assets::load<mesh_t>("engine://assets/models/scale.glb"));
        meshes.center = assets::pin(assets::load<mesh_t>("engine://assets/models/center.glb"));
        meshes.plane = assets::pin(assets::load<mesh_t>("engine://assets/models/plane.glb"));
    }

    void update(world_t& world, editor_context_t& ctx)
    {
        ctx.handle_hovered = false;
        ctx.handle_dragging = false;

        frame_t frame;
        const bool active = ctx.cur_mode == editor_mode_e::EDIT && ctx.handle_mode != handle_mode_e::NONE &&
                            build_frame(world.registry, ctx, frame);
        if (!active)
        {
            if (drag) { end_drag(world); }
            hovered = part_e::NONE;
            return;
        }

        const bool flying = tau::input::get_mouse_button(tau::input::mouse_button_e::Right);
        vec2_t mouse;
        const bool in_viewport = viewport_input::mouse_pixel(ctx, mouse);
        viewport_input::ray_t ray;
        const bool has_ray = in_viewport && viewport_input::mouse_ray(ctx, ray);

        if (drag)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                history::cancel(world);
                drag.reset();
                ctx.handle_dragging = true;
            }
            else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { end_drag(world); }
            else
            {
                ctx.handle_dragging = true;
                update_drag(world, ctx, viewport_input::ray_through(ctx, mouse), mouse);
            }
        }
        else
        {
            hovered = (has_ray && ctx.is_viewport_hovered && !flying) ? hit_test(ctx, frame, mouse, ray) : part_e::NONE;
            ctx.handle_hovered = hovered != part_e::NONE;

            if (ctx.handle_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                begin_drag(world, ctx, frame, hovered, ray, mouse);
                ctx.handle_dragging = true;
            }
        }
    }
} // namespace tau::editor::handles
