#include "runner.h"

#include "tau/asset.h"
#include "tau/components/camera.h"
#include "tau/components/tag.h"
#include "tau/engine.h"
#include "tau/hash.h"
#include "tau/log.h"
#include "tau/rendering/device.h"
#include "tau/rendering/gpu_timers.h"
#include "tau/rendering/pipelines.h"
#include "tau/rendering/readback.h"
#include "tau/rendering/renderer.h"
#include "tau/rendering/renderer_constants.h"
#include "tau/rendering/rendergraph.h"
#include "tau/time.h"
#include "tau/world.h"

#include <SDL3/SDL_timer.h>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <string_view>
#include <vector>

namespace tau::runner
{
    namespace
    {
        constexpr f64 FRAME_STEP = 1.0 / 60.0;
        constexpr f64 TIMING_WARMUP_SECONDS = 3.0;
        // loads can start while the first frames draw, so idle has to hold a while
        constexpr u32_t SETTLE_FRAMES = 10;
        constexpr u32_t HDR_FORMAT_RGBA16F = 97;

        enum class phase_e : u8_t
        {
            LOADING,
            RUNNING,
            WAIT_DUMP,
            DONE,
        };

        options_t opts;
        phase_e phase = phase_e::LOADING;
        u32_t idle_frames = 0;
        u32_t frame = 0;
        u64_t run_start_ns = 0;
        bool capture_this_frame = false;
        renderer::readback_ticket_t ticket = renderer::READBACK_NONE;
        VkExtent2D captured_extent = {};
        bool ok = true;

        template <typename T>
        bool parse_number(std::string_view text, T& out)
        {
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
            return ec == std::errc{} && end == text.data() + text.size();
        }

        f64 seconds_since(u64_t start_ns) { return static_cast<f64>(SDL_GetTicksNS() - start_ns) * 1e-9; }

        void build_dump_pass(renderer::rendergraph_t& graph, ecs::registry_t&)
        {
            if (!capture_this_frame) { return; }

            const renderer::rg_resource_id color = graph.get_resource("SceneColor"_h);
            if (color == renderer::RG_NULL_ID) { return; }

            captured_extent = renderer::ctx.render_extent;
            renderer::rg_pass_t& pass = graph.add_pass("RunnerHdrDump"_h, "RunnerHdrDump");
            pass.texture_reads = {color};
            pass.execute_callback = [&graph, color](VkCommandBuffer cmd, ecs::registry_t&)
            {
                const VkRect2D rect = {
                    {0, 0},
                    captured_extent
                };
                ticket =
                    renderer::record_readback(cmd, graph.get_image(color), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                              VK_IMAGE_ASPECT_COLOR_BIT, 8, rect);
            };
        }

        bool write_dump(const std::vector<u8_t>& texels)
        {
            const std::string path = opts.dump_hdr + "_hdr.bin";
            const u32_t header[4] = {captured_extent.width, captured_extent.height, 8, HDR_FORMAT_RGBA16F};
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(header), sizeof(header));
            file.write(reinterpret_cast<const char*>(texels.data()), static_cast<std::streamsize>(texels.size()));
            if (!file)
            {
                TAU_LOG_ERROR("RUNNER", "Could not write {}", path);
                return false;
            }
            std::printf("runner: wrote %s (%ux%u, frame %u)\n", path.c_str(), captured_extent.width,
                        captured_extent.height, frame);
            return true;
        }

        void print_timings()
        {
            const std::vector<renderer::gpu_timers::result_t> results = renderer::gpu_timers::results();
            std::printf("timing: render %ux%u, means over %u frames after a %.0f s warm-up\n",
                        renderer::ctx.render_extent.width, renderer::ctx.render_extent.height,
                        results.empty() ? 0u : results.front().frames, TIMING_WARMUP_SECONDS);
            for (const renderer::gpu_timers::result_t& r : results)
            {
                std::printf("timing: %-40s %.3f ms\n", r.name.c_str(), r.mean_ms);
            }
        }

        void finish()
        {
            if (opts.timings) { print_timings(); }
            if (renderer::ctx.caps.validation)
            {
                const u32_t errors = renderer::device::validation_errors();
                std::printf("validation errors: %u\n", errors);
                if (errors > 0) { ok = false; }
            }

            const u32_t late = renderer::pipelines::late_count();
            std::printf("late pipelines: %u\n", late);
            if (late > 0) { ok = false; }
            std::fflush(stdout);
            phase = phase_e::DONE;
            engine::exit();
        }

        void before_frame()
        {
            if (opts.width > 0) { renderer::submit_output_target("RunnerOutput"_h, {opts.width, opts.height}); }

            if (phase == phase_e::LOADING)
            {
                idle_frames = assets::in_flight() == 0 ? idle_frames + 1 : 0;
                if (idle_frames < SETTLE_FRAMES) { return; }

                phase = phase_e::RUNNING;
                run_start_ns = SDL_GetTicksNS();
                renderer::gpu_timers::enable(opts.timings);
            }

            capture_this_frame =
                phase == phase_e::RUNNING && !opts.dump_hdr.empty() && opts.frames > 0 && frame + 1 == opts.frames;

            if (phase == phase_e::RUNNING && opts.timings && seconds_since(run_start_ns) >= TIMING_WARMUP_SECONDS)
            {
                renderer::gpu_timers::accumulate(true);
            }
        }

        void after_frame()
        {
            if (phase == phase_e::RUNNING)
            {
                frame++;
                if (capture_this_frame)
                {
                    capture_this_frame = false;
                    if (ticket == renderer::READBACK_NONE)
                    {
                        TAU_LOG_ERROR("RUNNER", "The HDR readback was not recorded (no SceneColor this frame?)");
                        ok = false;
                        finish();
                        return;
                    }
                    phase = phase_e::WAIT_DUMP;
                }
                else if ((opts.frames > 0 && frame >= opts.frames) ||
                         (opts.quit_after > 0.0 && seconds_since(run_start_ns) >= opts.quit_after))
                {
                    finish();
                    return;
                }
            }

            if (phase == phase_e::WAIT_DUMP)
            {
                std::vector<u8_t> texels;
                if (!renderer::fetch_readback(ticket, texels)) { return; }
                ok = write_dump(texels);
                finish();
            }
        }
    } // namespace

    bool parse(i32 argc, char** argv, i32 first, options_t& out)
    {
        for (i32 i = first; i < argc; i++)
        {
            const std::string_view arg = argv[i];
            const std::size_t eq = arg.find('=');
            const std::string_view key = arg.substr(0, eq);
            const std::string_view value = eq == std::string_view::npos ? std::string_view{} : arg.substr(eq + 1);

            bool good = true;
            if (key == "--scene") { out.scene = value; }
            else if (key == "--camera") { out.camera = value; }
            else if (key == "--size")
            {
                const std::size_t x = value.find('x');
                good = x != std::string_view::npos && parse_number(value.substr(0, x), out.width) &&
                       parse_number(value.substr(x + 1), out.height) && out.width > 0 && out.height > 0;
            }
            else if (key == "--frames") { good = parse_number(value, out.frames) && out.frames > 0; }
            else if (key == "--dump-hdr") { out.dump_hdr = value; }
            else if (key == "--timings") { out.timings = true; }
            else if (key == "--no-vsync") { out.vsync = false; }
            else if (key == "--headless") { out.headless = true; }
            else if (key == "--compat") { out.compat = true; }
            else if (key == "--validation") { out.validation = true; }
            else if (key == "--quit-after") { good = parse_number(value, out.quit_after) && out.quit_after > 0.0; }
            else
            {
                good = false;
            }

            const bool is_switch = key == "--timings" || key == "--no-vsync" || key == "--headless" ||
                                   key == "--compat" || key == "--validation";
            if (!good || (eq == std::string_view::npos) != is_switch)
            {
                TAU_LOG_ERROR("RUNNER", "Bad argument: {}", arg);
                return false;
            }
        }

        if (!out.dump_hdr.empty() && out.frames == 0)
        {
            TAU_LOG_ERROR("RUNNER", "--dump-hdr needs --frames");
            return false;
        }
        if (out.headless && out.width == 0)
        {
            TAU_LOG_ERROR("RUNNER", "--headless needs --size");
            return false;
        }
        return true;
    }

    bool select_camera(world_t& world, const std::string& name)
    {
        ecs::entity_t found = ecs::NULL_ENTITY;
        for (auto [entity, camera, tag] : world.registry.view<camera_t, tag_t>().each())
        {
            if (tag.name == name) { found = entity; }
        }
        if (found == ecs::NULL_ENTITY)
        {
            TAU_LOG_ERROR("RUNNER", "No camera tagged '{}' in the scene", name);
            return false;
        }

        world.registry.clear<active_camera_tag>();
        world.registry.emplace<active_camera_tag>(found);
        return true;
    }

    void start(const options_t& options)
    {
        opts = options;
        if (opts.frames > 0) { time::set_frame_step(FRAME_STEP); }
        if (!opts.dump_hdr.empty())
        {
            renderer::register_renderer_feature("RunnerHdrDump"_h, renderer::FEATURE_ORDER_AFTER_POST - 1,
                                                &build_dump_pass);
        }
        std::printf("runner: GPU %s (%s%s)\n", renderer::ctx.properties.deviceName,
                    renderer::ctx.caps.ray_query ? "ray query" : "compat",
                    renderer::ctx.caps.headless ? ", headless" : "");

        engine::set_ui_callback(&before_frame);
        engine::set_post_render_callback(&after_frame);
    }

    bool succeeded() { return ok && phase == phase_e::DONE; }
} // namespace tau::runner
