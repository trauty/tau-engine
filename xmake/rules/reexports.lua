-- windows only

-- the engine tree this rule ships in; the consuming project's dir is somewhere else
-- entirely once a game includes xmake/tau.lua, so never use os.projectdir() here
local TAU_ENGINE = path.absolute(path.join(os.scriptdir(), "..", ".."))

rule("tau.reexports")

on_load(function(target)
    if not target:is_plat("windows") then
        return
    end
    target:add("shflags", "/DEF:" .. path.join(target:autogendir(), "reexports.def"), { force = true })
end)

before_link(function(target)
    import("lib.detect.find_tool")

    if not target:is_plat("windows") then
        return
    end

    local nm = find_tool("llvm-nm")
    if not nm then
        raise("tau.reexports needs llvm-nm to enumerate bundled entry points")
    end

    local function defined_symbols(files, keep)
        local names = {}
        if #files == 0 then
            return names
        end
        local argv = { "--defined-only", "--extern-only" }
        table.join2(argv, files)
        for line in os.iorunv(nm.program, argv):gmatch("[^\r\n]+") do
            local name = line:match("^%x+%s+T%s+(%S+)$")
            if name and keep(name) then
                table.insert(names, name)
            end
        end
        return names
    end

    local exports = {}

    table.join2(exports, defined_symbols(
        { path.join(TAU_ENGINE, "lib", "SDL3", "windows", "SDL3.lib") },
        function(name) return name:startswith("SDL_") and not name:endswith("_REAL") end))

    local fmt_objs = {}
    local vma_objs = {}
    for _, obj in ipairs(target:objectfiles()) do
        local base = path.filename(obj)
        if base == "format.cc.obj" or base == "os.cc.obj" then
            table.insert(fmt_objs, obj)
        elseif base == "vk_mem_alloc.cpp.obj" then
            table.insert(vma_objs, obj)
        end
    end
    table.join2(exports, defined_symbols(fmt_objs,
        function(name) return name:find("fmt@", 1, true) ~= nil end))
    -- the editor allocates through the engine's allocator, with the engine's copy of VMA
    table.join2(exports, defined_symbols(vma_objs,
        function(name) return name:startswith("vma") end))

    if #exports == 0 then
        raise("tau.reexports found nothing to export, check llvm-nm output format")
    end

    table.sort(exports)

    local deffile = path.join(target:autogendir(), "reexports.def")
    os.mkdir(path.directory(deffile))
    io.writefile(deffile, "EXPORTS\n" .. table.concat(exports, "\n") .. "\n")
    print("tau.reexports: re-exporting %d symbols", #exports)
end)
rule_end()
