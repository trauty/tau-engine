#include "tau/project.h"

#include "tau/asset.h"
#include "tau/log.h"
#include "tau/rendering/pipelines.h"
#include "tau/vfs.h"

#include "json/json.hpp"
#include <cstdlib>
#include <fstream>
#include <functional>
#include <utility>

namespace
{
#if TAU_PLATFORM_WIN
    constexpr const char* LIB_PREFIX = "";
    constexpr const char* LIB_EXT = ".dll";
#elif TAU_PLATFORM_LINUX
    constexpr const char* LIB_PREFIX = "lib";
    constexpr const char* LIB_EXT = ".so";
#endif
} // namespace

namespace tau
{
    bool project_t::load(const std::filesystem::path& project_file, project_t& out)
    {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(project_file, ec))
        {
            TAU_LOG_ERROR("PROJECT", "Not a project file: {}", project_file.string());
            return false;
        }

        std::ifstream file(project_file);
        if (!file.is_open())
        {
            TAU_LOG_ERROR("PROJECT", "Could not open project file: {}", project_file.string());
            return false;
        }

        nlohmann::json data = nlohmann::json::parse(file, nullptr, false);
        if (data.is_discarded())
        {
            TAU_LOG_ERROR("PROJECT", "Project file is not valid: {}", project_file.string());
            return false;
        }

        project_t p;
        p.project_name = data.value("project_name", "tau-game");
        p.game_lib_name = data.value("game_lib_name", "tau-game-logic");
        p.startup_scene = data.value("startup_scene", "");
        if (const auto it = data.find("import_defaults"); it != data.end() && it->is_object())
        {
            p.import_defaults = *it;
        }

        if (data.contains("tauproject_version"))
        {
            const nlohmann::json& v = data["tauproject_version"];
            if (v.is_string()) { p.project_version = std::atoi(v.get<std::string>().c_str()); }
            else if (v.is_number()) { p.project_version = v.get<int>(); }
        }
        if (p.project_version > TAU_PROJECT_VERSION)
        {
            TAU_LOG_WARN("PROJECT", "Project '{}' is schema version {}, newer than this engine's {}",
                         project_file.string(), p.project_version, TAU_PROJECT_VERSION);
        }

        // schema 1 also had engine.path, never read
        if (data.contains("engine") && data["engine"].is_object())
        {
            p.engine_version = major_minor(data["engine"].value("version", ""));
        }

        nlohmann::json paths = nlohmann::json::object();
        if (data.contains("paths") && data["paths"].is_object()) { paths = data["paths"]; }

        std::string bin_dir = paths.value("bin_dir", "bin");
        std::string assets_dir = paths.value("assets_dir", "assets");
        std::string cooked_assets_dir = paths.value("cooked_assets_dir", ".tau");

        p.project_dir = std::filesystem::absolute(project_file).parent_path();
        p.bin_dir = p.project_dir / bin_dir;
        p.assets_dir = p.project_dir / assets_dir;
        p.cooked_assets_dir = p.project_dir / cooked_assets_dir;

        std::string lib_file = std::string(LIB_PREFIX) + p.game_lib_name + LIB_EXT;
        p.lib_path = p.bin_dir / lib_file;
        p.trigger_path = p.lib_path.string() + ".trigger";

        out = std::move(p);
        return true;
    }

    namespace
    {
        bool rewrite_project(const std::filesystem::path& project_file,
                             const std::function<void(nlohmann::ordered_json&)>& edit)
        {
            nlohmann::ordered_json data;
            {
                std::ifstream file(project_file);
                data = nlohmann::ordered_json::parse(file, nullptr, false);
            }
            if (data.is_discarded() || !data.is_object())
            {
                TAU_LOG_ERROR("PROJECT", "Project file is not valid: {}", project_file.string());
                return false;
            }

            edit(data);

            std::ofstream out(project_file, std::ios::trunc);
            if (!out.is_open())
            {
                TAU_LOG_ERROR("PROJECT", "Could not write project file: {}", project_file.string());
                return false;
            }
            out << data.dump(4) << "\n";
            return true;
        }
    } // namespace

    bool project_t::set_engine_version(const std::filesystem::path& project_file, const std::string& version)
    {
        return rewrite_project(project_file,
                               [&](nlohmann::ordered_json& data)
                               {
                                   data["tauproject_version"] = std::to_string(TAU_PROJECT_VERSION);
                                   data["engine"] = nlohmann::ordered_json{
                                       {"version", major_minor(version)}
                                   };
                               });
    }

    bool project_t::set_import_defaults(const std::filesystem::path& project_file, const nlohmann::json& defaults)
    {
        return rewrite_project(project_file,
                               [&](nlohmann::ordered_json& data)
                               {
                                   if (defaults.empty()) { data.erase("import_defaults"); }
                                   else
                                   {
                                       data["import_defaults"] = nlohmann::ordered_json::parse(defaults.dump());
                                   }
                               });
    }

    namespace project
    {
        void mount(const project_t& project, bool engine_assets)
        {
            const std::filesystem::path& cooked = project.cooked_assets_dir;
            if (engine_assets)
            {
                vfs::mount(vfs::asset_prefix(vfs::ENGINE_NS), (cooked / "engine").generic_string() + "/");
            }
            vfs::mount(vfs::asset_prefix(vfs::GAME_NS), (cooked / "game").generic_string() + "/");

            // stale game:// entries from another project
            for (const auto& [path, guid] : assets::locations(vfs::asset_prefix(vfs::GAME_NS)))
            {
                assets::remove_location(path);
            }
            assets::read_guid_map((cooked / "guid_map.json").generic_string());

            renderer::pipelines::build_all();
        }

        std::string scene_path(std::string_view ref)
        {
            if (ref.empty() || !vfs::split_protocol(ref).protocol.empty()) { return std::string(ref); }

            const asset_guid_t guid = assets::parse_guid(ref);
            const std::string by_guid = guid != 0 ? assets::path_of(asset_handle_t{guid}) : std::string{};
            return !by_guid.empty() ? by_guid : vfs::asset_path(vfs::GAME_NS, ref);
        }
    } // namespace project

    std::string major_minor(std::string_view version)
    {
        const std::size_t first = version.find('.');
        if (first == std::string_view::npos) { return std::string(version); }

        const std::size_t second = version.find('.', first + 1);
        return std::string(version.substr(0, second));
    }

    int compare_major_minor(std::string_view a, std::string_view b)
    {
        auto parts = [](std::string_view v)
        {
            const std::string mm = major_minor(v);
            const std::size_t dot = mm.find('.');
            const int major = std::atoi(mm.substr(0, dot).c_str());
            const int minor = dot == std::string::npos ? 0 : std::atoi(mm.substr(dot + 1).c_str());
            return std::pair{major, minor};
        };

        const auto [a_major, a_minor] = parts(a);
        const auto [b_major, b_minor] = parts(b);
        if (a_major != b_major) { return a_major < b_major ? -1 : 1; }
        if (a_minor != b_minor) { return a_minor < b_minor ? -1 : 1; }
        return 0;
    }
} // namespace tau
