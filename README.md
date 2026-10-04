# tau-engine

## Libs

* tinygltf v2.9.7
* VMA v3.4.0
* ImGui v1.92.8
* meshoptimizer v1.2
* JoltPhysics v5.6.0
* fmt v12.2.0
* stb_image v2.30
* stb_image_resize2 v2.18
* stb_image_write v1.16
* cglm v0.9.6
* nlohmann::json v3.12.0
* entt v3.16.0
* volk 1.4.335
* vulkan-headers 1.4.335
* libktx v4.4.2
* SDL 3.4.12
* slang v2026.19
* tracy v0.13.1

## Building

This project uses [xmake](https://xmake.io).

Build modes (`-m`): `debug` (default), `release`, `releasedbg`.

**Toolchain** (`--toolchain`): Linux defaults to `clang` (also `gcc`). Windows
defaults to `clang-cl` (also `msvc`). Pick one with `xmake f --toolchain=gcc`
/ `xmake f --toolchain=msvc`. To get MSVC without installing Visual Studio, run
`xmake tau-msvc-setup` first (still working on this, so won't work now)

### Engine

```bash
xmake f -m debug          # configure
xmake                     # engine + cooker + editor + runtime; engine assets cook automatically
xmake run tau-editor      # launch the editor
```

The static engine is only needed for standalone game builds, so it is skipped by
a normal build:

```bash
xmake build tau-engine-static
```

Tracy profiling is off by default:

```bash
xmake f -m releasedbg --profiling=y
xmake
```

### Game project

A project builds with the engine that configured it. Opening it in the editor
configures it with that editor's engine. From a terminal, use the `tau` script of
the engine you want (`scripts/tau` in a checkout, `bin/tau` in a packaged engine):

**Bash:**
```bash
/path/to/tau-engine/scripts/tau configure   # once, records the engine in .xmake
xmake                                       # -> bin/<game>.so; game assets cook to .tau/game
```

**Powershell:**
```powershell
xmake f -m debug --tau_engine_dir=C:\path\to\tau-engine
xmake                     # -> bin\<game>.dll
```

The `.tauproject` names the engine version the project is made for
(`"engine": { "version": "0.1" }`), never a path. The editor opens projects of its
own version and offers to update older ones; builds check the same.

To build with an engine that ships with the project, such as a submodule, change
the engine line in the project's `xmake.lua` to
`get_config("tau_engine_dir") or path.join(os.scriptdir(), "tau-engine")`.

Open the project in the editor to run it. The editor loads the game library at
runtime and hot-reloads it whenever you rebuild, so `xmake` in the game project
is the whole edit loop.

The editor reloads the library in Edit mode. A build that finishes during Play
waits until you press Stop, or, with *Replay on reload* checked, stops Play,
reloads and starts Play again. A reload rebuilds the world from the scene, so it
keeps exactly what saving the scene keeps:

* **Reflect what should survive** with `TAU_REFLECT`. Data without it is lost
  on a reload, the same as on save and load.
* **Register callbacks in `TAU_ON_LOAD()`.** Input actions, render features,
  systems and signal listeners point at your code, and `TAU_ON_LOAD()` runs
  after every load, the first and every reload. `tau_game_init` and
  `tau_game_shutdown` run when Play starts and stops, set up game state there.

```cpp
TAU_ON_LOAD()
{
    tau::input::bind_button("jump", tau::input::key_e::Space);
    tau::input::on_action("jump", tau::input::input_state_t::PRESSED, [](const auto&) { /* ... */ });
    world.register_update_system(&update_player);
}
```

Reflected fields can be numbers, bools, strings, `vec3_t`, enums, asset
handles, entity references, structs registered with `reflection::structure`,
and `std::vector`s of any of these. Scenes store entity references as scene
indices, so they survive save and load.

```cpp
struct waypoint_t { tau::vec3_t pos; f32 wait = 0.0f; };
struct patrol_t { std::vector<waypoint_t> points; tau::ecs::entity_t target = tau::ecs::NULL_ENTITY; };

TAU_REFLECT()
{
    tau::reflection::structure<waypoint_t>(ctx, "Waypoint")
        .field<&waypoint_t::pos>("Position")
        .field<&waypoint_t::wait>("Wait");
    tau::reflection::component<patrol_t>(ctx, "Patrol")
        .field<&patrol_t::points>("Points")
        .field<&patrol_t::target>("Target");
}
```

### Standalone (no editor)

Links the static engine, your game code and the runtime entry point into one
binary. Build the static engine once, then build the game with `--standalone=y`:

```bash
# in the engine repo
xmake f -m release && xmake build tau-engine-static

# in the game project
xmake f --standalone=y -m release
xmake                     # -> build/<plat>/<arch>/<mode>/{<game>, assets/}
```

`--standalone` is a game-side option: it selects which engine to link, and is
not accepted in the engine repo.

### Cooking assets

Assets cook as part of a normal build.

The engine cooks its own assets into `build/<plat>/<arch>/<mode>/assets/engine`.
A game project then keeps one cooked root of its own, `<project>/.tau/`, holding
`engine/` (copied in from the engine build), `game/` (cooked from the project's
`assets/`), and a single `guid_map.json` covering both. The editor, the runtime
and a packaged build all read that one root.

The two stay in separate vfs namespaces, `engine://assets/...` and
`game://assets/...`, so the engine can address its own assets by absolute path
from inside a library shared by every game, and generic names like
`shaders/util.slang` cannot collide.

To force just the engine cook step:

```bash
xmake build tau-assets    # engine repo
```

### Sample project

`samples/sandbox/` is a complete game project inside the engine repo: a lit sphere on a
ground plane, and one game component (`bob_t`) that moves it in Play mode. Open it with:

```bash
tau-editor samples/sandbox/sandbox.tauproject
```

The editor builds it on first open. To build it by hand, run from `samples/sandbox/` and pass
`-P .`, otherwise xmake walks up and configures the engine instead:

```bash
xmake f -P . -m debug
xmake build -P .
```

## Packaging & Distribution (experimental)

### Standalone

Bundle the self-contained standalone binary with its cooked assets into
`dist/<project>/`, ready to hand to a player. Run from the game project:

```bash
xmake f --standalone=y -m release   # static build
xmake                               # builds + cooks
xmake tau-package                   # -> dist/<project>/{<game>, assets/}
```

### Package the engine (engine + editor + cooker)

A packaged engine is one self-contained folder with the same layout as a checkout,
so put it anywhere. Its `bin/tau` configures projects with it, and its editor keeps
its state (recent projects, layout, pipeline cache) in `user/` inside the folder.

**Bash:**
```bash
xmake f -m release
xmake install -o /path/to/tau-$(cat VERSION)
```

**Powershell:**
```powershell
xmake f -m release
xmake install -o "C:\path\to\tau-$(Get-Content VERSION)"
```
