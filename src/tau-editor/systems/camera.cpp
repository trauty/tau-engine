#include "camera.h"

#include "imgui.h"
#include "tau/input.h"
#include "tau/time.h"

namespace tau::editor::camera_system
{
    static bool is_moving = false;

    void update(editor_camera_t& cam, bool is_viewport_hovered)
    {
        if (is_viewport_hovered && tau::input::get_mouse_button(tau::input::mouse_button_e::Right))
        {
            is_moving = true;
            tau::input::set_mouse_relative_mode(true);
            ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse;
        }

        if (!tau::input::get_mouse_button(tau::input::mouse_button_e::Right))
        {
            is_moving = false;
            tau::input::set_mouse_relative_mode(false);
            ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
        }

        if (!is_moving) { return; }

        vec3_t forward = tau::forward(cam.rotation);
        vec3_t right = tau::right(cam.rotation);

        f32 speed = 10.0f * tau::time::get_dt();
        if (tau::input::get_key(tau::input::key_e::LeftShift)) { speed *= 2.0f; }

        if (tau::input::get_key(tau::input::key_e::W)) { cam.position += forward * speed; }
        if (tau::input::get_key(tau::input::key_e::S)) { cam.position -= forward * speed; }
        if (tau::input::get_key(tau::input::key_e::D)) { cam.position += right * speed; }
        if (tau::input::get_key(tau::input::key_e::A)) { cam.position -= right * speed; }

        vec2_t mouse_delta = tau::input::get_mouse_delta();
        f32 mouse_sensitivity = 0.0015f;

        if (mouse_delta.x != 0.0f || mouse_delta.y != 0.0f)
        {
            cam.yaw += mouse_delta.x * mouse_sensitivity;
            cam.pitch += mouse_delta.y * mouse_sensitivity;

            const f32 pitch_limit = tau::radians(90.0f);
            if (cam.pitch > pitch_limit) { cam.pitch = pitch_limit; }
            if (cam.pitch < -pitch_limit) { cam.pitch = -pitch_limit; }

            quat_t q_yaw = angle_axis(cam.yaw, vec3_t{0.0f, 1.0f, 0.0f});
            quat_t q_pitch = angle_axis(cam.pitch, vec3_t{1.0f, 0.0f, 0.0f});

            cam.rotation = q_yaw * q_pitch;
        }
    }
} // namespace tau::editor::camera_system
