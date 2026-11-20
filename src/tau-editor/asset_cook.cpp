#include "tau-editor/asset_cook.h"

#include "tau-editor/project_manager.h"
#include "tau/asset.h"
#include "tau/log.h"
#include "tau/os.h"
#include "tau/project.h"
#include "tau/vfs.h"

#include "json/json.hpp"
#include <SDL3/SDL_filesystem.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace tau::editor::asset_cook
{
    namespace
    {
        std::string g_project_file;

        fs::path cooker_path()
        {
            const char* base = SDL_GetBasePath();
            if (!base) { return {}; }

#ifdef TAU_PLATFORM_WIN
            return fs::path(base) / "tau-cooker.exe";
#else
            return fs::path(base) / "tau-cooker";
#endif
        }

        // shaders include tau/rendering/shader_shared.h, so the cooker needs the engine header root
        fs::path shader_include_dir()
        {
            const std::string dir = project_manager::engine_dir();
            if (dir.empty()) { return {}; }

            const fs::path root(dir);

            std::error_code ec;
            if (fs::is_directory(root / "share" / "tau" / "rules", ec)) { return root / "include"; }

            return root / "src";
        }

        // keep the first error, the cooker's own summary line says nothing
        std::string first_error_line(const std::string& output)
        {
            std::size_t begin = 0;
            while (begin < output.size())
            {
                std::size_t end = output.find('\n', begin);
                if (end == std::string::npos) { end = output.size(); }

                // strip colour escapes that survive the pipe
                const std::string clean = tau::log::strip_ansi(std::string_view(output).substr(begin, end - begin));
                if (clean.find("[ERROR]") != std::string::npos || clean.find("[FATAL]") != std::string::npos)
                {
                    return clean;
                }

                begin = end + 1;
            }

            return {};
        }

        // the cooker logs "[time] [LEVEL] [CATEGORY] message", relogged here with its own level and category
        // a line not in that shape (rest of a multi line Slang diagnostic) keeps the previous level
        struct cooker_relay_t
        {
            tau::log::level_e last_level = tau::log::level_e::LOG_INFO;
            std::string last_category = "COOKER";

            static bool take_bracket(std::string_view& rest, std::string_view& out)
            {
                if (rest.size() < 2 || rest.front() != '[') { return false; }
                const std::size_t close = rest.find(']');
                if (close == std::string_view::npos) { return false; }

                out = rest.substr(1, close - 1);
                rest.remove_prefix(close + 1);
                if (!rest.empty() && rest.front() == ' ') { rest.remove_prefix(1); }
                return true;
            }

            static bool parse_level(std::string_view name, tau::log::level_e& out)
            {
                using level_e = tau::log::level_e;
                if (name == "TRACE") { out = level_e::LOG_TRACE; }
                else if (name == "DEBUG") { out = level_e::LOG_DEBUG; }
                else if (name == "INFO") { out = level_e::LOG_INFO; }
                else if (name == "WARN") { out = level_e::LOG_WARN; }
                else if (name == "ERROR") { out = level_e::LOG_ERROR; }
                else if (name == "FATAL") { out = level_e::LOG_FATAL; }
                else
                {
                    return false;
                }
                return true;
            }

            void operator()(std::string_view raw)
            {
                const std::string line = tau::log::strip_ansi(raw);
                if (line.empty()) { return; }

                std::string_view rest = line;
                std::string_view time, level, category;
                tau::log::level_e parsed = last_level;
                if (take_bracket(rest, time) && take_bracket(rest, level) && take_bracket(rest, category) &&
                    parse_level(level, parsed))
                {
                    last_level = parsed;
                    last_category = std::string(category);
                    tau::log::write(last_level, last_category.c_str(), rest);
                    return;
                }

                tau::log::write(last_level, last_category.c_str(), line);
            }
        };

        struct cook_target_t
        {
            fs::path input;
            fs::path output;
            const char* name_space;
            fs::path project_file;
        };

        // unique per run, the watcher and an inspector save can cook concurrently
        fs::path new_report_path()
        {
            static std::atomic<u32_t> counter = 0;

            const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
            std::error_code ec;
            return fs::temp_directory_path(ec) /
                   ("tau-cook-" + std::to_string(ticks) + "-" + std::to_string(counter++) + ".json");
        }

        void read_report(const fs::path& report_path, std::vector<fs::path>& out_cooked)
        {
            std::ifstream file(report_path);
            if (!file.is_open()) { return; }

            const nlohmann::json report = nlohmann::json::parse(file, nullptr, false);
            if (report.is_discarded() || !report.contains("cooked") || !report["cooked"].is_array()) { return; }

            for (const nlohmann::json& source : report["cooked"])
            {
                if (source.is_string()) { out_cooked.emplace_back(source.get<std::string>()); }
            }
        }
    } // namespace

    bool run_cooker(const cook_target_t& target, std::span<const fs::path> source_files, std::string& out_err,
                    std::vector<fs::path>* out_cooked, tau::os::process_cancel_t* cancel = nullptr);

    fs::path engine_assets_dir()
    {
        const std::string dir = project_manager::engine_dir();
        if (dir.empty()) { return {}; }

        const fs::path root(dir);

        std::error_code ec;
        if (fs::is_directory(root / "share" / "tau" / "rules", ec))
        {
            return root / "share" / "tau" / "engine-assets-src";
        }

        return root / "assets";
    }

    void set_project(const std::string& project_file) { g_project_file = project_file; }

    bool cook_asset(const fs::path& source_file, std::string& out_err)
    {
        const fs::path one[] = {source_file};
        return cook_assets(one, out_err);
    }

    static bool project_target(cook_target_t& out, std::string& out_err)
    {
        if (g_project_file.empty())
        {
            out_err = "no project open";
            return false;
        }

        tau::project_t project;
        if (!tau::project_t::load(g_project_file, project))
        {
            out_err = "cannot load project " + g_project_file;
            return false;
        }

        out = {project.assets_dir, project.cooked_assets_dir / "game", "game", g_project_file};
        return true;
    }

    bool cook_assets(std::span<const fs::path> source_files, std::string& out_err, std::vector<fs::path>* out_cooked)
    {
        if (source_files.empty()) { return true; }

        cook_target_t target;
        if (!project_target(target, out_err)) { return false; }

        return run_cooker(target, source_files, out_err, out_cooked);
    }

    bool cook_engine_assets(std::span<const fs::path> source_files, std::string& out_err,
                            std::vector<fs::path>* out_cooked)
    {
        if (source_files.empty()) { return true; }

        // the tree vfs::init mounted engine:// on, the one this editor draws from
        const cook_target_t target = {engine_assets_dir(), fs::path(tau::vfs::cooked_root()) / "engine", "engine", {}};
        return run_cooker(target, source_files, out_err, out_cooked);
    }

    bool cook_project(std::string& out_err, tau::os::process_cancel_t* cancel)
    {
        cook_target_t target;
        if (!project_target(target, out_err)) { return false; }

        return run_cooker(target, {}, out_err, nullptr, cancel);
    }

    // no --file arguments means the whole tree
    bool run_cooker(const cook_target_t& target, std::span<const fs::path> source_files, std::string& out_err,
                    std::vector<fs::path>* out_cooked, tau::os::process_cancel_t* cancel)
    {
        std::error_code ec;
        if (target.input.empty() || !fs::is_directory(target.input, ec))
        {
            out_err = "no asset directory at " + target.input.string();
            return false;
        }

        const fs::path cooker = cooker_path();
        if (cooker.empty() || !fs::exists(cooker, ec))
        {
            out_err = "tau-cooker not found next to the editor";
            return false;
        }

        // argv, not a shell command line: a path with a space is just an argument with a space
        std::vector<std::string> args;
        args.emplace_back("-i");
        args.emplace_back(target.input.generic_string());

        const fs::path engine_assets = engine_assets_dir();
        if (!engine_assets.empty() && fs::is_directory(engine_assets, ec) && engine_assets != target.input)
        {
            args.emplace_back("-I");
            args.emplace_back(engine_assets.generic_string());
        }

        const fs::path shader_include = shader_include_dir();
        if (!shader_include.empty() && fs::is_directory(shader_include, ec))
        {
            args.emplace_back("-I");
            args.emplace_back(shader_include.generic_string());
        }

        args.emplace_back("-o");
        args.emplace_back(target.output.generic_string());
        args.emplace_back("-n");
        args.emplace_back(target.name_space);

        if (!target.project_file.empty())
        {
            args.emplace_back("--project");
            args.emplace_back(target.project_file.generic_string());
        }

        for (const fs::path& source : source_files)
        {
            args.emplace_back("--file");
            args.emplace_back(source.generic_string());
        }

        const fs::path report_path = out_cooked ? new_report_path() : fs::path{};
        if (!report_path.empty())
        {
            args.emplace_back("--report");
            args.emplace_back(report_path.generic_string());
        }

        const tau::os::process_result_t run = tau::os::run_process(cooker.string(), args, {}, cooker_relay_t{}, cancel);

        if (!report_path.empty())
        {
            read_report(report_path, *out_cooked);
            fs::remove(report_path, ec);
        }

        if (run.started && !run.cancelled)
        {
            std::ifstream map(target.output.parent_path() / "guid_map.json");
            const std::string text((std::istreambuf_iterator<char>(map)), std::istreambuf_iterator<char>());
            if (!text.empty()) { tau::assets::merge_guid_map(text); }
        }

        if (!run.started)
        {
            out_err = "could not start " + cooker.string();
            return false;
        }

        if (run.cancelled)
        {
            out_err = "cancelled";
            return false;
        }

        if (run.exit_code != 0)
        {
            out_err = first_error_line(run.output);
            if (out_err.empty())
            {
                out_err = "tau-cooker exited " + std::to_string(run.exit_code) + " without saying why";
            }

            return false;
        }

        return true;
    }
} // namespace tau::editor::asset_cook
