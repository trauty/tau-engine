local TAU_ENGINE = path.absolute(path.join(os.scriptdir(), "..", ".."))

-- engine targets only, games stamp themselves in tau.game
rule("tau.engine_stamp")
on_load(function(target)
    import("tau_stamp")

    local file = path.join(target:autogendir(), "build_stamp.cpp")
    os.mkdir(path.directory(file))
    tau_stamp.write_engine_source(file, path.join(TAU_ENGINE, "src"), path.join(TAU_ENGINE, "VERSION"),
                                  tau_stamp.build_mode())
    target:add("files", file)
end)
rule_end()
