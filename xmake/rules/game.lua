-- the engine this rule ships in, which is the one the project included
local TAU_ENGINE = path.absolute(path.join(os.scriptdir(), "..", ".."))

rule("tau.game")
add_deps("tau.common")

on_load(function(target)
    import("core.project.config")
    import("tau_engine")
    import("tau_project")

    local standalone = config.get("standalone")
    local info = tau_engine.resolve(TAU_ENGINE)
    local proj = tau_project.load(os.projectdir())

    target:add("defines", "TAU_GAME_EXPORT")

    if standalone then
        target:add("files", info.runtime_main)
        target:add("defines", "TAU_STATIC_LINK")

        local plat = config.get("plat") or os.host()
        local arch = config.get("arch") or os.arch()
        local mode = config.get("mode") or "release"
        target:set("targetdir", path.join(os.projectdir(), "build", plat, arch, mode))

        target:set("kind", "binary")

        -- tau-engine-static is built with lto, so its archive holds bitcode, not machine code
        -- the final link must run lto too, else a non-lto linker (GNU ld) cannot read it
        target:set("policy", "build.optimization.lto", true)

        if proj and proj.startup_scene and proj.startup_scene ~= "" then
            target:add("defines", "TAU_STARTUP_SCENE=\"" .. proj.startup_scene .. "\"")
        end
        if proj and proj.project_name and proj.project_name ~= "" then
            target:add("defines", "TAU_GAME_NAME=\"" .. proj.project_name .. "\"")
        end

        local bindir = path.join(os.projectdir(), "bin")
        os.tryrm(path.join(bindir, "lib" .. target:basename() .. ".so"))
        os.tryrm(path.join(bindir, target:basename() .. ".dll"))
    else
        target:set("kind", "shared")
        target:set("targetdir", path.join(os.projectdir(), "bin"))
        if target:is_plat("linux") then
            target:add("shflags", "-Wl,-Bsymbolic")
        end

        -- a library has its own vulkan function pointers, a standalone binary uses the static engine's
        target:add("files", info.volk_source)

        import("tau_stamp")
        local stamp_file = path.join(target:autogendir(), "engine_stamp.cpp")
        os.mkdir(path.directory(stamp_file))
        tau_stamp.write_game_source(stamp_file, info.header_root, info.version_file, tau_stamp.build_mode())
        target:add("files", stamp_file)
    end

    if config.get("mode") == "release" then
        target:set("optimize", "fastest")
        target:set("strip", "all")
    end

    for _, dir in ipairs(info.includedirs) do
        target:add("includedirs", dir)
    end
    target:add("defines", "CGLM_FORCE_LEFT_HANDED", "VK_NO_PROTOTYPES", "CGLM_FORCE_DEPTH_ZERO_TO_ONE")
    target:add("defines", "JPH_OBJECT_LAYER_BITS=16", "JPH_PROFILE_ENABLED",
        "JPH_DEBUG_RENDERER", "JPH_OBJECT_STREAM")
    if not is_mode("debug") then
        target:add("defines", "JPH_NO_DEBUG")
    end
    if target:is_plat("windows") then
        target:add("defines", "KHRONOS_STATIC")
    end

    tau_engine.apply_links(target, info, standalone)
end)

before_link(function(target)
    import("core.project.config")
    import("tau_engine")

    local static = config.get("standalone") and true or false
    local info = tau_engine.resolve(TAU_ENGINE)
    local libfile = tau_engine.library_file(info, target, static)

    if not os.isfile(libfile) then
        tau_engine.missing_artifact_error(info, path.filename(libfile) .. " (looked in " .. info.libdir .. ")",
            static and "static" or "shared")
    end
end)


after_build(function(target)
    import("core.project.config")
    import("core.project.depend")
    import("tau_engine")
    import("tau_project")

    local info = tau_engine.resolve(TAU_ENGINE)
    local proj = tau_project.load(os.projectdir())
    if not proj or not proj.assets_dir or not os.isdir(proj.assets_dir) then
        return
    end

    if not os.isfile(info.cooker) then
        tau_engine.missing_artifact_error(info, "tau-cooker (needed to cook assets)", "cooker")
    end

    local function force_if_missing(dependfile, ...)
        for _, out in ipairs({ ... }) do
            if not os.exists(out) then
                os.tryrm(dependfile)
                return
            end
        end
    end

    local engine_out = path.join(proj.cooked_assets_dir, "engine")
    local game_out = path.join(proj.cooked_assets_dir, "game")
    local guid_map = path.join(proj.cooked_assets_dir, "guid_map.json")

    if not os.isdir(info.engine_cooked) then
        tau_engine.missing_artifact_error(info, "cooked engine assets at " .. info.engine_cooked, "assets")
    end

    local engine_depfile = target:dependfile("tau.game.engine_assets")
    force_if_missing(engine_depfile, engine_out, guid_map)

    depend.on_changed(function()
        import("core.base.json")

        os.tryrm(engine_out)
        os.mkdir(proj.cooked_assets_dir)
        os.cp(info.engine_cooked, proj.cooked_assets_dir)
        os.tryrm(path.join(engine_out, "cooker_cache.json"))

        local merged = {}
        for _, mapfile in ipairs({ guid_map, info.engine_guid_map }) do
            if os.isfile(mapfile) then
                for key, value in pairs(json.loadfile(mapfile)) do
                    merged[key] = value
                end
            end
        end
        json.savefile(guid_map, merged)
    end, {
        files = os.files(path.join(info.engine_cooked, "**")),
        dependfile = engine_depfile
    })

    local depfiles = os.files(path.join(proj.assets_dir, "**"))
    table.insert(depfiles, info.cooker)

    local game_depfile = target:dependfile("tau.game.assets")
    force_if_missing(game_depfile, game_out)

    depend.on_changed(function()
        os.vrunv(info.cooker, { "-i", proj.assets_dir, "-I", info.engine_assets, "-I", info.shader_include,
                                "-o", game_out, "-n", "game" })
    end, { files = depfiles, dependfile = game_depfile })

    if config.get("standalone") then
        local stage = path.join(target:targetdir(), "assets")

        os.tryrm(stage)
        os.mkdir(stage)
        os.cp(engine_out, stage)
        os.cp(game_out, stage)
        os.cp(guid_map, stage)

        for _, junk in ipairs(os.files(path.join(stage, "**", "cooker_cache.json"))) do
            os.rm(junk)
        end
    end
end)
rule_end()
