#pragma once

#include "tau/assets/scene.h"
#include "tau/defines.h"

#include <SDL3/SDL_iostream.h>
#include <fstream>
#include <string>

// the binary form of scene_property_t, shared by scenes and data assets
namespace tau::value_io
{
    template <typename T>
    bool read_pod(SDL_IOStream* stream, T& out)
    { return SDL_ReadIO(stream, &out, sizeof(T)) == sizeof(T); }

    inline bool read_str(SDL_IOStream* stream, std::string& out)
    {
        u32_t len = 0;
        if (!read_pod(stream, len)) { return false; }
        out.resize(len);
        return len == 0 || SDL_ReadIO(stream, out.data(), len) == len;
    }

    template <typename T>
    void write_pod(std::ofstream& out, const T& v)
    { out.write(reinterpret_cast<const char*>(&v), sizeof(T)); }

    inline void write_str(std::ofstream& out, const std::string& s)
    {
        write_pod(out, static_cast<u32_t>(s.size()));
        out.write(s.data(), static_cast<std::streamsize>(s.size()));
    }

    bool read_property(SDL_IOStream* stream, scene_property_t& prop, u32_t depth = 0);
    void write_property(std::ofstream& out, const scene_property_t& prop);
} // namespace tau::value_io
