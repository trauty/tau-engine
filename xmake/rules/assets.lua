local engine_dir = path.absolute(path.join(os.scriptdir(), "..", ".."))

rule("tau.assets")
on_buildcmd(function(target, batchcmds, opt)
    local cooker        = target:dep("tau-cooker"):targetfile()
    local engine_assets = path.join(engine_dir, "assets")
    -- shaders #include "tau/rhi/shader_shared.h" out of the engine header root
    local engine_include = path.join(engine_dir, "src")
    local engine_out    = path.join(target:dep("tau-cooker"):targetdir(), "assets", "engine")

    batchcmds:show("cooking engine assets")
    batchcmds:vrunv(cooker, { "-i", engine_assets, "-I", engine_include, "-o", engine_out, "-n", "engine" })

    local depfiles = os.files(path.join(engine_assets, "**"))
    table.insert(depfiles, cooker)
    table.sort(depfiles)

    batchcmds:add_depfiles(depfiles)
    batchcmds:add_depvalues(table.concat(depfiles, ";"))
    batchcmds:set_depcache(target:dependfile("tau-assets"))
end)
rule_end()
