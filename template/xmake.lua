set_project("${NAME}")
set_version("0.0.1")

add_rules("plugin.compile_commands.autoupdate", {outputdir = "."})

-- set when the editor or the engine's `tau configure` configures this project
-- for an engine shipped with the project: get_config("tau_engine_dir") or path.join(os.scriptdir(), "tau-engine")
option("tau_engine_dir")
    set_showmenu(true)
    set_description("The tau engine this project builds against")
option_end()

local engine = get_config("tau_engine_dir")

if engine then
    includes(path.join(engine, "xmake", "tau.lua"))

    target("${NAME}")
        add_rules("tau.game", "tau.hotreload")

        add_files("src/**.cpp")
        add_includedirs("src")
    target_end()
else
    target("${NAME}")
        set_kind("phony")
        on_load(function()
            raise("This project has no engine configured. Open it in a tau editor, or run the engine's `tau configure` here.")
        end)
    target_end()
end
