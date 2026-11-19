#pragma once

#include "slang-tag-version.h"
#include "tau/assets/material_format.h"
#include "tau/assets/mesh_format.h"
#include "tau/assets/scene_format.h"
#include "tau/assets/shader_format.h"
#include "tau/defines.h"

#include "json/json.hpp"
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <vector>
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace tau::cooker
{
    inline constexpr int COOKER_VERSION = 8;

    // debug cooks keep debug info and skip optimisation, for RenderDoc
    // part of the cook identity along with the Slang version
#ifdef TAU_COOK_SHADER_DEBUG
    inline constexpr bool COOK_SHADER_DEBUG = true;
#else
    inline constexpr bool COOK_SHADER_DEBUG = false;
#endif

    // fingerprint of the shared struct layouts, folded into every entry by cook_identity()
    // without it a global_data_t change recooks nothing. set once at startup
    inline u64_t g_layout_stamp = 0;

    inline void set_layout_stamp(u64_t stamp) { g_layout_stamp = stamp; }

    inline std::string cook_identity()
    {
        return std::to_string(COOKER_VERSION) + "." + std::to_string(TAU_SHADER_VERSION) + "." +
               std::to_string(TAU_MATERIAL_VERSION) + "." + std::to_string(TAU_MESH_VERSION) + "." +
               std::to_string(TAU_SCENE_VERSION) + "." + std::to_string(g_layout_stamp) +
               (COOK_SHADER_DEBUG ? ".debug" : "") + ".slang-" + SLANG_TAG_VERSION;
    }

    inline u64_t hash_file(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) { return 0; }

        XXH3_state_t* state = XXH3_createState();
        if (!state) { return 0; }

        XXH3_64bits_reset(state);

        constexpr size_t BUFFER_SIZE = 64 * 1024;
        char buffer[BUFFER_SIZE];

        while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0)
        {
            XXH3_64bits_update(state, buffer, file.gcount());
        }

        u64_t final_hash = XXH3_64bits_digest(state);
        XXH3_freeState(state);

        return final_hash;
    }

    struct asset_cache_t
    {
        nlohmann::json data;
        std::string path;

        asset_cache_t(const std::string& path) : path(path) {}

        void load();
        void save();

        // uses the deps the previous cook recorded, else `deps`
        // a cook that finds more than predicted passes the real list to update_cache
        bool needs_cooking(const std::string& out_path, const std::vector<std::filesystem::path>& deps);
        void forget(const std::string& out_path);

        // a source that cooked to nothing on purpose, e.g. an import only .slang
        // recorded so it is not recompiled every run
        bool is_known_non_output(const std::string& source_path) const;
        void mark_non_output(const std::string& source_path, const std::vector<std::filesystem::path>& deps);
        void update_cache(const std::string& out_path, const std::vector<std::filesystem::path>& deps,
                          const std::string& source_path = {});

        // sources whose last cook read any of `files`, via the reverse of the recorded deps
        // outputs cooked before sources were recorded are missed until the next full cook
        std::vector<std::string> sources_depending_on(const std::vector<std::filesystem::path>& files) const;
        void remember_source(const std::string& out_path, const std::string& source_path);

        // drops entries not in `keys` and returns the count. only a full cook knows the live keys
        size_t retain_only(const std::unordered_set<std::string>& keys);

      private:
        static std::string combined_dep_hash(const std::vector<std::filesystem::path>& deps);
    };
} // namespace tau::cooker