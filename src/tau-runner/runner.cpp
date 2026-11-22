#include "runner.h"

#include "tau-tools/gpu_telemetry.h"
#include "tau/asset.h"
#include "tau/components/camera.h"
#include "tau/components/tag.h"
#include "tau/engine.h"
#include "tau/hash.h"
#include "tau/log.h"
#include "tau/render/frame.h"
#include "tau/rhi/context.h"
#include "tau/rhi/device.h"
#include "tau/rhi/gpu_timers.h"
#include "tau/rhi/pipelines.h"
#include "tau/rhi/readback.h"
#include "tau/rhi/renderdoc.h"
#include "tau/rhi/renderer_constants.h"
#include "tau/time.h"
#include "tau/world.h"

#include <SDL3/SDL_timer.h>
#include <algorithm>
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
        renderer::readback_ticket_t ldr_ticket = renderer::READBACK_NONE;
        VkFormat ldr_format = VK_FORMAT_UNDEFINED;
        bool hdr_written = false;
        bool ldr_written = false;
        VkExtent2D captured_extent = {};
        bool ok = true;

        // over the timed frames; a value < 0 never counts
        struct telemetry_sum_t
        {
            u32_t samples = 0;
            f64 gpu_mhz = 0.0, mem_mhz = 0.0, power_w = 0.0, temp_c = 0.0, power_limited = 0.0;
            f32 gpu_mhz_min = 1e9f, gpu_mhz_max = 0.0f, temp_c_max = 0.0f;
        } telemetry;

        void add_telemetry(const tools::gpu_telemetry::sample_t& s)
        {
            if (s.gpu_mhz < 0.0f) { return; }
            telemetry.samples++;
            telemetry.gpu_mhz += s.gpu_mhz;
            telemetry.mem_mhz += std::max(s.mem_mhz, 0.0f);
            telemetry.power_w += std::max(s.power_w, 0.0f);
            telemetry.temp_c += std::max(s.temp_c, 0.0f);
            telemetry.power_limited += std::max(s.power_limited, 0.0f);
            telemetry.gpu_mhz_min = std::min(telemetry.gpu_mhz_min, s.gpu_mhz);
            telemetry.gpu_mhz_max = std::max(telemetry.gpu_mhz_max, s.gpu_mhz);
            telemetry.temp_c_max = std::max(telemetry.temp_c_max, s.temp_c);
        }

        template <typename T>
        bool parse_number(std::string_view text, T& out)
        {
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
            return ec == std::errc{} && end == text.data() + text.size();
        }

        f64 seconds_since(u64_t start_ns) { return static_cast<f64>(SDL_GetTicksNS() - start_ns) * 1e-9; }

        void record_dump(const renderer::graph::record_ctx_t& rc, const void* data)
        {
            const renderer::graph::image_h color = *static_cast<const renderer::graph::image_h*>(data);
            const VkRect2D rect = {
                {0, 0},
                captured_extent
            };
            ticket =
                renderer::record_readback(rc.cmd, renderer::graph::image(*rc.graph, color),
                                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 8, rect);
        }

        void build_dump_pass(renderer::graph::graph_t& g, const renderer::frame::targets_t& t, ecs::registry_t&)
        {
            if (!capture_this_frame || opts.dump_hdr.empty() || !t.scene_color.valid()) { return; }

            captured_extent = renderer::frame::render_extent();
            const renderer::graph::pass_h pass = renderer::graph::add_pass(
                g, "RunnerHdrDump", &record_dump, t.scene_color, renderer::graph::PASS_SIDE_EFFECT);
            renderer::graph::use(g, pass, t.scene_color, renderer::graph::access_e::TRANSFER_SRC);
        }

        void record_ldr(const renderer::graph::record_ctx_t& rc, const void* data)
        {
            const renderer::graph::image_h output = *static_cast<const renderer::graph::image_h*>(data);
            const VkRect2D rect = {
                {0, 0},
                captured_extent
            };
            ldr_ticket =
                renderer::record_readback(rc.cmd, renderer::graph::image(*rc.graph, output),
                                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 4, rect);
        }

        // after everything, the UI included
        void build_ldr_pass(renderer::graph::graph_t& g, const renderer::frame::targets_t& t, ecs::registry_t&)
        {
            if (!capture_this_frame || opts.dump_ldr.empty() || !t.final_output.valid()) { return; }

            captured_extent = renderer::graph::extent(g, t.final_output);
            ldr_format = renderer::graph::format(g, t.final_output);
            const renderer::graph::pass_h pass = renderer::graph::add_pass(
                g, "RunnerLdrDump", &record_ldr, t.final_output, renderer::graph::PASS_SIDE_EFFECT);
            renderer::graph::use(g, pass, t.final_output, renderer::graph::access_e::TRANSFER_SRC);
        }

        bool write_dump(const std::vector<u8_t>& texels, const std::string& path, u32_t texel_size, u32_t format)
        {
            const u32_t header[4] = {captured_extent.width, captured_extent.height, texel_size, format};
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
                        renderer::frame::render_extent().width, renderer::frame::render_extent().height,
                        results.empty() ? 0u : results.front().frames, TIMING_WARMUP_SECONDS);
            for (const renderer::gpu_timers::result_t& r : results)
            {
                std::printf("timing: %-40s %.3f ms\n", r.name.c_str(), r.mean_ms);
            }

            std::printf("telemetry: %s\n", tools::gpu_telemetry::source().c_str());
            if (telemetry.samples > 0)
            {
                const f64 n = telemetry.samples;
                std::printf("telemetry: gpu %.0f MHz (%.0f-%.0f), mem %.0f MHz, %.1f W, %.0f C (max %.0f), "
                            "power limited %.0f %% of %u frames\n",
                            telemetry.gpu_mhz / n, telemetry.gpu_mhz_min, telemetry.gpu_mhz_max, telemetry.mem_mhz / n,
                            telemetry.power_w / n, telemetry.temp_c / n, telemetry.temp_c_max,
                            telemetry.power_limited / n, telemetry.samples);
            }
        }

        void finish()
        {
            if (opts.timings) { print_timings(); }
            tools::gpu_telemetry::stop();
            if (renderer::ctx.caps.validation)
            {
                const u32_t errors = renderer::device::validation_errors();
                std::printf("validation errors: %u\n", errors);
                if (errors > 0) { ok = false; }
            }

            const u32_t late = renderer::pipelines::late_count();
            std::printf("late pipelines: %u\n", late);
            if (late > 0) { ok = false; }
            if (opts.capture_frame > 0)
            {
                const std::string capture = renderer::renderdoc::last_capture();
                std::printf("capture: %s\n", capture.empty() ? "none" : capture.c_str());
                if (capture.empty()) { ok = false; }
            }
            std::fflush(stdout);
            phase = phase_e::DONE;
            engine::exit();
        }

        void before_frame()
        {
            if (opts.width > 0) { renderer::frame::submit_output_target("RunnerOutput"_h, {opts.width, opts.height}); }

            if (phase == phase_e::LOADING)
            {
                idle_frames = assets::in_flight() == 0 ? idle_frames + 1 : 0;
                if (idle_frames < SETTLE_FRAMES) { return; }

                phase = phase_e::RUNNING;
                run_start_ns = SDL_GetTicksNS();
                renderer::gpu_timers::enable(opts.timings);
            }

            if (phase == phase_e::RUNNING && frame + 1 == opts.capture_frame)
            {
                renderer::renderdoc::capture_next_frame();
            }

            capture_this_frame = phase == phase_e::RUNNING && (!opts.dump_hdr.empty() || !opts.dump_ldr.empty()) &&
                                 opts.frames > 0 && frame + 1 == opts.frames;

            if (phase == phase_e::RUNNING && opts.timings && seconds_since(run_start_ns) >= TIMING_WARMUP_SECONDS)
            {
                renderer::gpu_timers::accumulate(true);
                add_telemetry(tools::gpu_telemetry::latest());
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
                    if ((!opts.dump_hdr.empty() && ticket == renderer::READBACK_NONE) ||
                        (!opts.dump_ldr.empty() && ldr_ticket == renderer::READBACK_NONE))
                    {
                        TAU_LOG_ERROR("RUNNER", "A readback was not recorded (no SceneColor / output this frame?)");
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
                if (!opts.dump_hdr.empty() && !hdr_written)
                {
                    if (!renderer::fetch_readback(ticket, texels)) { return; }
                    ok &= write_dump(texels, opts.dump_hdr + "_hdr.bin", 8, HDR_FORMAT_RGBA16F);
                    hdr_written = true;
                }
                if (!opts.dump_ldr.empty() && !ldr_written)
                {
                    if (!renderer::fetch_readback(ldr_ticket, texels)) { return; }
                    ok &= write_dump(texels, opts.dump_ldr + "_ldr.bin", 4, static_cast<u32_t>(ldr_format));
                    ldr_written = true;
                }
                finish();
            }
        }
    } // namespace

    bool parse(i32 argc, char** argv, i32 first, options_t& out)
    {
        for (i32 i = first; i < argc; i++)
        {
            if (renderer::settings::take_arg(argc, argv, i, out.settings)) { continue; }

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
            else if (key == "--dump-ldr") { out.dump_ldr = value; }
            else if (key == "--timings") { out.timings = true; }
            else if (key == "--headless") { out.headless = true; }
            else if (key == "--compat") { out.compat = true; }
            else if (key == "--validation") { out.validation = true; }
            else if (key == "--renderdoc") { out.renderdoc = true; }
            else if (key == "--capture-frame")
            {
                good = parse_number(value, out.capture_frame) && out.capture_frame > 0;
            }
            else if (key == "--quit-after") { good = parse_number(value, out.quit_after) && out.quit_after > 0.0; }
            else
            {
                good = false;
            }

            const bool is_switch = key == "--timings" || key == "--headless" || key == "--compat" ||
                                   key == "--validation" || key == "--renderdoc";
            if (!good || (eq == std::string_view::npos) != is_switch)
            {
                TAU_LOG_ERROR("RUNNER", "Bad argument: {}", arg);
                return false;
            }
        }

        if ((!out.dump_hdr.empty() || !out.dump_ldr.empty()) && out.frames == 0)
        {
            TAU_LOG_ERROR("RUNNER", "--dump-hdr / --dump-ldr need --frames");
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
        if (opts.timings) { tools::gpu_telemetry::start(renderer::device::identity()); }
        if (opts.capture_frame > 0 && !renderer::renderdoc::active())
        {
            TAU_LOG_ERROR("RUNNER", "--capture-frame needs RenderDoc: --renderdoc, or launch through RenderDoc");
            ok = false;
        }
        if (!opts.dump_hdr.empty())
        {
            renderer::frame::register_producer("RunnerHdrDump"_h, renderer::FEATURE_ORDER_AFTER_POST - 1,
                                               &build_dump_pass);
        }
        if (!opts.dump_ldr.empty())
        {
            renderer::frame::register_producer("RunnerLdrDump"_h, renderer::FEATURE_ORDER_UI + 10, &build_ldr_pass);
        }
        std::printf("runner: GPU %s (%s%s)\n", renderer::ctx.properties.deviceName,
                    renderer::ctx.caps.ray_query ? "ray query" : "compat",
                    renderer::ctx.caps.headless ? ", headless" : "");

        engine::set_ui_callback(&before_frame);
        engine::set_post_render_callback(&after_frame);
    }

    bool succeeded() { return ok && phase == phase_e::DONE; }
} // namespace tau::runner
