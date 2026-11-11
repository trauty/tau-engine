#include "scene_io.h"

#include "tau-editor/asset_watch.h"
#include "tau-editor/editor_context.h"
#include "tau-editor/history.h"
#include "tau/asset.h"
#include "tau/asset_sources.h"
#include "tau/log.h"
#include "tau/serialization.h"
#include "tau/world.h"

#include <fstream>
#include <json/json.hpp>

namespace tau::editor::scene_io
{
    bool save(world_t& world, editor_context_t& ctx, const std::string& path)
    {
        nlohmann::json scene_json = serialization::serialize_scene(world);
        // new path = new asset, existing path keeps its guid
        scene_json["guid"] = assets::guid_text(asset_sources::guid_for_writing(path));

        std::ofstream file(path);
        if (!file.is_open())
        {
            TAU_LOG_ERROR("EDITOR", "Cannot write the scene to {}", path);
            return false;
        }
        file << scene_json.dump(4);
        file.close();

        ctx.current_scene_path = path;
        history::mark_saved(history::OPEN_SCENE);
        TAU_LOG_INFO("EDITOR", "Scene saved to {}", path);

        asset_watch::set_open_scene(path);

        // cooked .tauscene is runtime only, cooks in the background
        const std::filesystem::path source = path;
        asset_watch::cook_saved({&source, 1});
        return true;
    }

    bool load(world_t& world, editor_context_t& ctx, const std::string& path)
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            TAU_LOG_ERROR("EDITOR", "Failed to open scene file '{}'", path);
            return false;
        }

        const nlohmann::json scene_json = nlohmann::json::parse(file, nullptr, false);
        if (scene_json.is_discarded())
        {
            TAU_LOG_ERROR("EDITOR", "Failed to parse scene file '{}'", path);
            return false;
        }

        serialization::clear_scene(world);
        serialization::deserialize_scene(world, scene_json);
        assets::collect();

        ctx.current_scene_path = path;
        ctx.selection.clear();
        history::drop(history::OPEN_SCENE);
        asset_watch::set_open_scene(path);

        TAU_LOG_INFO("EDITOR", "Scene loaded from {}", path);
        return true;
    }

    void clear(world_t& world, editor_context_t& ctx)
    {
        serialization::clear_scene(world);
        assets::collect();

        ctx.current_scene_path.clear();
        ctx.selection.clear();
        history::drop(history::OPEN_SCENE);
        asset_watch::set_open_scene({});

        TAU_LOG_INFO("EDITOR", "New scene created");
    }
} // namespace tau::editor::scene_io
