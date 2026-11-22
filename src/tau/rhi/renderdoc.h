#pragma once

#include "tau/defines.h"

#include <string>

// RenderDoc in-application API, no-op without RenderDoc
namespace tau::renderer::renderdoc
{
    // before the Vulkan instance: an injected RenderDoc, or load it
    bool init(bool load, const std::string& capture_template);

    TAU_ENGINE_API bool active();
    TAU_ENGINE_API void capture_next_frame();
    // path of the newest capture, empty before the first
    TAU_ENGINE_API std::string last_capture();

    void frame_begin();
    void frame_end();
} // namespace tau::renderer::renderdoc
