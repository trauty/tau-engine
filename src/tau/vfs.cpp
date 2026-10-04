#include "vfs.h"

#include "SDL3/SDL_iostream.h"
#include "SDL3/SDL_stdinc.h"
#include "tau/engine.h"
#include "tau/log.h"

#include <SDL3/SDL_filesystem.h>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace tau::vfs
{
    namespace
    {
        std::unordered_map<std::string, std::string> mounts;
        std::string cooked_root_dir;
    } // namespace

    void init()
    {
        // a tool that keeps its own user data, like the editor, mounts user:// before init
        if (!mounts.contains("user://"))
        {
            char* pref_path = SDL_GetPrefPath("tau-engine", tau::engine::get_game_name().c_str());
            if (pref_path)
            {
                mount("user://", pref_path);
                SDL_free(pref_path);
            }
        }

        const char* base_path = SDL_GetBasePath();
        if (base_path)
        {
            namespace fs = std::filesystem;

            std::string bp = base_path;
            while (bp.size() > 1 && (bp.back() == '/' || bp.back() == '\\')) { bp.pop_back(); }
            const fs::path exe_dir = bp;

            if (fs::exists(exe_dir / "assets" / "engine"))
            {
                cooked_root_dir = (exe_dir / "assets").generic_string();
                mount("engine://assets/", (exe_dir / "assets" / "engine").generic_string() + "/");
                mount("game://assets/", (exe_dir / "assets" / "game").generic_string() + "/");
            }
            else
            {
                const fs::path engine_assets = exe_dir.parent_path() / "share" / "tau" / "engine-assets";
                cooked_root_dir = engine_assets.generic_string();
                mount("engine://assets/", (engine_assets / "engine").generic_string() + "/");
            }
        }
    }

    void shutdown()
    {
        mounts.clear();
        cooked_root_dir.clear();
    }

    const std::string& cooked_root() { return cooked_root_dir; }

    path_parts_t split_protocol(std::string_view virtual_path)
    {
        const std::size_t proto = virtual_path.find("://");
        if (proto == std::string_view::npos) { return {{}, virtual_path}; }

        return {virtual_path.substr(0, proto + 3), virtual_path.substr(proto + 3)};
    }

    void mount(const std::string& alias, const std::string& physical_path) { mounts[alias] = physical_path; }

    std::string resolve(std::string_view virtual_path)
    {
        std::string_view best_alias = "";
        std::string_view best_physical = "";

        for (const auto& pair : mounts)
        {
            if (virtual_path.starts_with(pair.first))
            {
                if (pair.first.length() > best_alias.length())
                {
                    best_alias = pair.first;
                    best_physical = pair.second;
                }
            }
        }

        if (!best_alias.empty())
        {
            std::string_view relative = virtual_path.substr(best_alias.length());

            if (!best_physical.empty() && best_physical.back() != '/' && best_physical.back() != '\\' &&
                !relative.empty() && relative.front() != '/' && relative.front() != '\\')
            {
                return std::string(best_physical) + "/" + std::string(relative);
            }

            return std::string(best_physical) + std::string(relative);
        }

        if (virtual_path.find("://") != std::string_view::npos)
        {
            TAU_LOG_WARN("VFS", "No mount matches '{}'", virtual_path);
            return {};
        }

        return std::string(virtual_path);
    }

    bool exists(const std::string& virtual_path)
    {
        SDL_IOStream* stream = open_read(virtual_path);
        if (stream)
        {
            SDL_CloseIO(stream);
            return true;
        }

        return false;
    }

    std::vector<u8_t> read_bytes(const std::string& virtual_path)
    {
        SDL_IOStream* stream = open_read(virtual_path);
        if (!stream) return {};

        i64_t size = SDL_GetIOSize(stream);
        if (size <= 0)
        {
            SDL_CloseIO(stream);
            return {};
        }

        std::vector<u8_t> buffer(static_cast<std::size_t>(size));
        const std::size_t got = SDL_ReadIO(stream, buffer.data(), static_cast<std::size_t>(size));
        SDL_CloseIO(stream);

        if (got != static_cast<std::size_t>(size))
        {
            TAU_LOG_ERROR("VFS", "Short read on '{}': got {} of {} bytes", virtual_path, got, size);
            return {};
        }

        return buffer;
    }

    std::string read_text(const std::string& virtual_path)
    {
        std::vector<u8_t> bytes = read_bytes(virtual_path);
        return std::string(bytes.begin(), bytes.end());
    }

    SDL_IOStream* open_read(const std::string& virtual_path)
    {
        std::string physical_path = resolve(virtual_path);
        SDL_IOStream* stream = SDL_IOFromFile(physical_path.c_str(), "rb");
        return stream;
    }

    SDL_IOStream* open_write(const std::string& virtual_path)
    {
        std::string physical_path = resolve(virtual_path);
        SDL_IOStream* stream = SDL_IOFromFile(physical_path.c_str(), "wb");
        return stream;
    }
} // namespace tau::vfs