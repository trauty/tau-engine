import("core.base.json")

function load(projectdir)
    local files = os.files(path.join(projectdir, "*.tauproject"))
    if #files == 0 then
        return nil
    end

    local data = json.loadfile(files[1])
    local paths = data.paths or {}

    local engine = data.engine or {}
    local engine_version = engine.version or data.engine_version

    return {
        file                = files[1],
        project_name        = data.project_name or "tau-game",
        game_lib_name       = data.game_lib_name or "tau-game-logic",
        startup_scene       = data.startup_scene or "",
        engine_version      = engine_version and engine_version:match("^(%d+%.%d+)") or engine_version,
        tauproject_version  = data.tauproject_version,
        bin_dir             = path.join(projectdir, paths.bin_dir or "bin"),
        assets_dir          = path.join(projectdir, paths.assets_dir or "assets"),
        cooked_assets_dir   = path.join(projectdir, paths.cooked_assets_dir or ".tau"),
    }
end
