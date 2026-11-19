#include "play_mode.h"

#include "tau-editor/commands.h"
#include "tau-editor/editor_context.h"
#include "tau-editor/events.h"
#include "tau-editor/prefab_stage.h"
#include "tau/asset.h"
#include "tau/serialization.h"
#include "tau/systems/guid.h"
#include "tau/world.h"

#include "json/json.hpp"

namespace tau::editor::play_mode
{
    namespace
    {
        nlohmann::json g_backup;

        void start(world_t& world, editor_context_t& ctx)
        {
            events::emit(events::event_e::PLAY_STARTING, world, ctx);

            g_backup = serialization::serialize_scene(world);
            world.begin_play();
            ctx.cur_mode = editor_mode_e::PLAY;
        }

        void stop(world_t& world, editor_context_t& ctx)
        {
            world.end_play();

            serialization::clear_scene(world);
            serialization::deserialize_scene(world, g_backup);
            g_backup = nullptr;
            assets::collect();

            ctx.cur_mode = editor_mode_e::EDIT;
            // entities made during Play are gone
            std::erase_if(ctx.selection.guids,
                          [&](u64_t guid) { return guid_system::find(world.registry, guid) == ecs::NULL_ENTITY; });
            events::emit(events::event_e::PLAY_STOPPED, world, ctx);
        }
    } // namespace

    void init(editor_context_t&)
    {
        using commands::command_e;
        using commands::request_t;

        commands::set(
            command_e::PLAY,
            {.label = "Play",
             .enabled = [](const editor_context_t& ctx)
             { return ctx.project_loaded && ctx.cur_mode == editor_mode_e::EDIT && !prefab_stage::is_open(); },
             .run = [](world_t& world, editor_context_t& ctx, const request_t&) { start(world, ctx); }});
        commands::set(command_e::STOP,
                      {.label = "Stop",
                       .enabled = [](const editor_context_t& ctx) { return ctx.cur_mode != editor_mode_e::EDIT; },
                       .run = [](world_t& world, editor_context_t& ctx, const request_t&) { stop(world, ctx); }});

        // the old world went with its project, nothing to restore
        events::on(events::event_e::PROJECT_OPENED,
                   [](world_t&, editor_context_t& ctx)
                   {
                       ctx.cur_mode = editor_mode_e::EDIT;
                       g_backup = nullptr;
                   });
    }
} // namespace tau::editor::play_mode
