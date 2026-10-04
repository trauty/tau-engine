includes(path.join(os.scriptdir(), "rules", "*.lua"))
includes(path.join(os.scriptdir(), "tasks", "*.lua"))
add_moduledirs(path.join(os.scriptdir(), "modules"))

option("standalone")
set_default(false)
set_showmenu(true)
set_description("Build a static standalone game binary (no editor)")
option_end()
