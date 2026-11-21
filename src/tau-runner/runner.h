#pragma once

#include "tau/defines.h"

#include <string>

namespace tau { struct world_t; }

// measurement runs over a project: render size, frame count, HDR dump, GPU pass timings
namespace tau::runner
{
    struct options_t
    {
        std::string scene;
        std::string camera;
        u32_t width = 0;
        u32_t height = 0;
        // exactly this many frames on a fixed 1/60 s clock once loading settled, then exit
        u32_t frames = 0;
        // PREFIX_hdr.bin: the last frame's SceneColor (lit HDR before post), RGBA16F after a 16-byte header
        std::string dump_hdr;
        bool timings = false;
        bool vsync = true;
        // wall clock seconds after loading settled, for timing runs
        f64 quit_after = 0.0;
    };

    // the flags after argv[1] (the project); false on an unknown flag or a bad value
    bool parse(i32 argc, char** argv, i32 first, options_t& out);

    // moves active_camera_tag to the camera tagged `name`
    bool select_camera(world_t& world, const std::string& name);

    // hooks the engine's frame callbacks; call before engine::run
    void start(const options_t& options);

    // after engine::run: false when the run failed (no dump written)
    bool succeeded();
} // namespace tau::runner
