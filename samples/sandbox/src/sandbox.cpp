// The smallest game the engine runs. One component of its own, so opening the sample exercises
// game code, reflection, the inspector and hot reload, not only the renderer: edit bob_t's
// update, press Recompile, and the sphere moves differently without restarting.
#include "tau/components/transform.h"
#include "tau/ecs.h"
#include "tau/game.h"
#include "tau/input.h"
#include "tau/reflection.h"
#include "tau/time.h"
#include "tau/world.h"

#include <cmath>

using namespace tau;

// moves an entity up and down around the height it was placed at
struct bob_t
{
    f32 height = 0.5f;
    f32 speed = 1.5f;
    f32 base_y = 1.0f;
};

TAU_REFLECT()
{
    reflection::component<bob_t>(ctx, "Bob")
        .field<&bob_t::height>("Height")
        .field<&bob_t::speed>("Speed")
        .field<&bob_t::base_y>("Base Y");
}

namespace { bool g_paused = false; }

static void update_bob(world_t& world)
{
    if (g_paused) { return; }

    const f32 t = static_cast<f32>(time::get_time());

    for (auto [entity, bob, transform] : world.registry.view<bob_t, transform_t>().each())
    {
        transform.local_position.y = bob.base_y + bob.height * std::sin(t * bob.speed);
    }
}

// Space pauses the bobbing in Play. Registered per load, not in tau_game_init, which runs at every Play:
// the engine drops a library's input listeners when a hot reload unloads it, and this registers the new code's.
TAU_ON_LOAD()
{
    input::bind_button("pause_bob", input::key_e::Space);
    input::on_action("pause_bob", input::input_state_t::PRESSED,
                     [](const input::input_context_t&) { g_paused = !g_paused; });

    world.register_update_system(&update_bob);
}

void tau_game_register_types(world_t* world) { reflection::run_registrations(*world->reflection_ctx); }

void tau_game_init(world_t* world) { g_paused = false; }

void tau_game_update(world_t* world) {}

void tau_game_shutdown(world_t* world) {}

TAU_GAME_ENTRY()
