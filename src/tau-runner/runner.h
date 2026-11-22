#pragma once

#include "tau/defines.h"

#include <string>

namespace tau { struct world_t; }

// measurement runs: fixed size, frame count, HDR dump, pass timings
namespace tau::runner
{
    struct options_t
    {
        std::string scene;
        std::string camera;
        u32_t width = 0;
        u32_t height = 0;
        // fixed 1/60 s clock after loading, then exit
        u32_t frames = 0;
        // PREFIX_hdr.bin: last SceneColor, RGBA16F, 16 B header
        std::string dump_hdr;
        // PREFIX_ldr.bin: the final output after every pass, 4 bytes per texel, 16 B header
        std::string dump_ldr;
        bool timings = false;
        bool vsync = true;
        // wall seconds after loading
        f64 quit_after = 0.0;
        // needs --size
        bool headless = false;
        // no RT
        bool compat = false;
        // + sync, errors fail the run
        bool validation = false;
        // load librenderdoc
        bool renderdoc = false;
        // RenderDoc capture of this counted frame (1-based)
        u32_t capture_frame = 0;
    };

    bool parse(i32 argc, char** argv, i32 first, options_t& out);

    bool select_camera(world_t& world, const std::string& name);

    // before engine::run
    void start(const options_t& options);

    bool succeeded();
} // namespace tau::runner
