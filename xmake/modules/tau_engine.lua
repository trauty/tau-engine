import("core.project.config")

function is_source_tree(engine_dir)
    return os.isfile(path.join(engine_dir, "xmake.lua"))
end

function exe(name)
    if config.get("plat") == "windows" or (not config.get("plat") and os.host() == "windows") then
        return name .. ".exe"
    end
    return name
end

function source_builddir(engine_dir)
    local plat = config.get("plat") or os.host()
    local arch = config.get("arch") or os.arch()
    local mode = config.get("mode") or "debug"
    return path.join(engine_dir, "build", plat, arch, mode)
end

function resolve(engine_dir)
    if not engine_dir or engine_dir == "" or not os.isdir(engine_dir) then
        raise("No tau-engine found at '" .. tostring(engine_dir) .. "'.")
    end

    local info = { dir = engine_dir, source = is_source_tree(engine_dir) }

    if info.source then
        info.libdir = source_builddir(engine_dir)
        info.bindir = info.libdir
        info.includedirs = {
            path.join(engine_dir, "include"),
            path.join(engine_dir, "include", "imgui"),
            path.join(engine_dir, "src"),
            path.join(engine_dir, "lib", "JoltPhysics"),
        }
        info.header_root = path.join(engine_dir, "src")
        info.engine_assets = path.join(engine_dir, "assets")
        -- shaders #include "tau/rendering/shader_shared.h", the same path C++ uses
        info.shader_include = path.join(engine_dir, "src")
        info.engine_cooked = path.join(info.libdir, "assets", "engine")
        info.engine_guid_map = path.join(info.libdir, "assets", "guid_map.json")
        info.version_file = path.join(engine_dir, "VERSION")
    else
        info.libdir = path.join(engine_dir, "lib")
        info.bindir = path.join(engine_dir, "bin")
        info.includedirs = {
            path.join(engine_dir, "include"),
            path.join(engine_dir, "include", "imgui"),
        }
        info.header_root = path.join(engine_dir, "include")
        info.engine_assets = path.join(engine_dir, "share", "tau", "engine-assets-src")
        info.shader_include = path.join(engine_dir, "include")
        info.engine_cooked = path.join(engine_dir, "share", "tau", "engine-assets", "engine")
        info.engine_guid_map = path.join(engine_dir, "share", "tau", "engine-assets", "guid_map.json")
        info.version_file = path.join(engine_dir, "share", "tau", "VERSION")
    end

    info.cooker = path.join(info.bindir, exe("tau-cooker"))
    info.runtime_main = path.join(engine_dir, "src", "tau-runtime", "main.cpp")
    info.volk_source = path.join(engine_dir, "lib", "volk", "volk.c")

    return info
end

function link_name(static)
    return static and "tau-engine-static" or "tau-engine"
end

function library_file(info, target, static)
    local name = link_name(static)
    if target:is_plat("windows") then
        return path.join(info.libdir, name .. ".lib")
    elseif static then
        return path.join(info.libdir, "lib" .. name .. ".a")
    end
    return path.join(info.libdir, "lib" .. name .. ".so")
end

function missing_artifact_error(info, what, wanted)
    local how
    if info.source then
        local mode = config.get("mode") or "debug"
        how = "build it in the engine repo:\n" ..
            "  cd " .. info.dir .. "\n" ..
            "  xmake f -m " .. mode .. "\n" ..
            "  xmake"
        if wanted == "static" then
            how = how .. "\n  xmake build tau-engine-static\n\n" ..
                "the static engine is not part of a plain 'xmake', and it has to be built in\n" ..
                "the same mode (" .. mode .. ") as this game"
        end
    else
        how = "the SDK install at " .. info.dir .. " looks incomplete"
    end
    raise("tau-engine: missing " .. what .. "\n" .. how)
end

function apply_links(target, info, static)
    target:add("linkdirs", info.libdir)
    target:add("links", link_name(static))

    if target:is_plat("linux") then
        target:add("rpathdirs", info.libdir, "$ORIGIN")
    end

    if not static then
        return
    end

    target:add("linkdirs", path.join(info.dir, "lib", "ktx", vendor_platdir(target)))
    target:add("links", "ktx")
    target:add("linkdirs", path.join(info.dir, "lib", "SDL3", vendor_platdir(target)))
    target:add("links", "SDL3")

    if target:is_plat("linux") then
        target:add("syslinks", "pthread", "m", "dl")
    elseif target:is_plat("windows") then
        target:add("syslinks", "user32", "gdi32", "winmm", "imm32", "ole32", "oleaut32",
            "version", "uuid", "advapi32", "setupapi", "shell32",
            "cfgmgr32", "kernel32")
    end
end

function vendor_platdir(target)
    return target:is_plat("windows") and "windows" or "linux"
end
