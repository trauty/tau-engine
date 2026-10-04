set_project("tau-engine")
set_version("0.1.0")

includes("xmake/rules/*.lua")
includes("xmake/tasks/*.lua")
add_moduledirs("xmake/modules")

set_languages("cxx20")
add_rules("mode.debug", "mode.release", "mode.releasedbg")

add_rules("plugin.compile_commands.autoupdate", {outputdir = "$(projectdir)"})

option("profiling")
    set_default(false)
    set_showmenu(true)
    set_description("Enable Tracy Profiling")
    add_defines("TAU_ENABLE_PROFILING")
    add_defines("TRACY_ENABLE")
option_end()

local function vendor_platdir()
    return is_plat("windows") and "windows" or "linux"
end

target("tau-interface")
    set_kind("headeronly")

    add_includedirs("include", {public = true})
    add_includedirs("src", {public = true})

    add_defines("CGLM_FORCE_LEFT_HANDED", "VK_NO_PROTOTYPES", "CGLM_FORCE_DEPTH_ZERO_TO_ONE", {public = true})

    add_includedirs("lib/JoltPhysics", {public = true})
    add_defines("JPH_OBJECT_LAYER_BITS=16", "JPH_PROFILE_ENABLED",
                "JPH_DEBUG_RENDERER", "JPH_OBJECT_STREAM", {public = true})
    if not is_mode("debug") then
        add_defines("JPH_NO_DEBUG", {public = true})
    end
target_end()

for _, variant in ipairs({"shared", "static"}) do
local is_static = (variant == "static")

target(is_static and "tau-engine-static" or "tau-engine")
    set_kind(is_static and "static" or "shared")

    if is_static then
        set_default(false)
        set_optimize("fastest")
        set_strip("all")
        set_policy("build.optimization.lto", true)
        add_defines("TAU_STATIC_LINK", {public = true})
    else
        if is_plat("linux") then
            add_shflags("-Wl,-Bsymbolic")
            add_rpathdirs("$ORIGIN")
        end
    end

    add_rules("tau.common", "tau.engine_stamp")
    if not is_static then
        add_rules("tau.reexports")
    end
    add_options("profiling", {public = true})
    add_defines("TAU_ENGINE_EXPORT", "CGLM_FORCE_LEFT_HANDED")
    add_deps("tau-interface")
    add_undefines("JPH_FLOATING_POINT_EXCEPTIONS_ENABLED")

    if has_config("profiling") then
        add_includedirs("lib/tracy", {public = true})
        add_files("lib/tracy/TracyClient.cpp", {warnings = "none"})
    end

    if not is_plat("windows") then
        add_syslinks("dl")
    end

    -- only TAU_ENGINE_API leaves the library, as on windows, so a missing export fails on linux too
    if not is_static and is_plat("linux") then
        add_files("src/tau/**.cpp", {cxflags = {"-fvisibility=hidden", "-fvisibility-inlines-hidden"}})
    else
        add_files("src/tau/**.cpp")
    end
    add_files("lib/vma/vk_mem_alloc.cpp", {warnings = "none"})
    add_files("lib/volk/volk.c", {warnings = "none"})
    add_files("lib/fmt/format.cc", "lib/fmt/os.cc", {warnings = "none"})
    add_files("lib/JoltPhysics/Jolt/**.cpp", {warnings = "none"})

    add_linkdirs(path.join("lib", "ktx", vendor_platdir()), {public = true})
    add_links("ktx", {public = true})
    add_linkdirs(path.join("lib", "SDL3", vendor_platdir()))
    if not is_static then
        add_links("SDL3")
    end
    if is_plat("linux") then
        add_syslinks("pthread", "m", {public = true})
    elseif is_plat("windows") then
        add_defines("KHRONOS_STATIC", {public = true})
        add_syslinks("user32", "gdi32", "winmm", "imm32", "ole32", "oleaut32",
                     "version", "uuid", "advapi32", "setupapi", "shell32",
                     "cfgmgr32", "kernel32", {public = true})
    end

    add_includedirs("include", {public = true})
    add_includedirs("include/imgui", {public = true})
    add_includedirs("src", {public = true})

    if not is_static then
        add_headerfiles("src/(tau/**.h)")
        add_headerfiles("include/(**)")
        add_headerfiles("lib/JoltPhysics/(Jolt/**.h)")
        add_headerfiles("lib/JoltPhysics/(Jolt/**.inl)")

        add_installfiles("xmake/tau.lua", {prefixdir = "xmake"})
        add_installfiles("xmake/rules/*.lua", {prefixdir = "xmake/rules"})
        add_installfiles("xmake/modules/*.lua", {prefixdir = "xmake/modules"})
        add_installfiles("xmake/tasks/*.lua", {prefixdir = "xmake/tasks"})
        add_installfiles("template/(**)", {prefixdir = "share/tau/template"})
        add_installfiles("VERSION", {prefixdir = "share/tau"})
        add_installfiles("assets/(**)", {prefixdir = "share/tau/engine-assets-src"})
        add_installfiles("lib/volk/volk.c", {prefixdir = "lib/volk"})

        if is_plat("linux") then
            add_installfiles("scripts/tau", {prefixdir = "bin"})
        end

        after_install(function (target)
            import("tau_project")

            if target:is_plat("linux") then
                local launcher = path.join(target:installdir(), "bin", "tau")
                if os.isfile(launcher) then os.vrunv("chmod", {"+x", launcher}) end
            end

            local proj = tau_project.load(os.projectdir())
            local cooked = proj and proj.cooked_assets_dir or path.join(os.projectdir(), ".tau")
            local dest = path.join(target:installdir(), "share", "tau", "engine-assets")

            if os.isdir(path.join(cooked, "engine")) then
                os.mkdir(dest)
                os.cp(path.join(cooked, "engine"), dest)
                os.trycp(path.join(cooked, "guid_map.json"), dest)
                os.tryrm(path.join(dest, "engine", "cooker_cache.json"))
            end
        end)
    end
target_end()
end

target("tau-runtime")
    set_kind("binary")
    add_rules("tau.common")
    add_deps("tau-engine", "tau-assets")

    if is_plat("windows") then
        if is_mode("release") then
            add_ldflags("/subsystem:windows", "/entry:mainCRTStartup", {force = true})
        end
    elseif is_plat("linux") then
        add_ldflags("-rdynamic", {force = true})
    end

    add_files("src/tau-runtime/main.cpp")
    add_files("src/tau-runtime/empty_game.cpp")

target_end()

target("tau-cooker")
    set_kind("binary")
    add_rules("tau.common", {march = false})
    add_deps("tau-engine")

    add_files("src/tau-cooker/**.cpp")

    -- a debug cooker writes debuggable shaders: see COOK_SHADER_DEBUG in cache_manager.h
    if is_mode("debug") then
        add_defines("TAU_COOK_SHADER_DEBUG")
    end
    add_files("lib/tinygltf/tiny_gltf.cpp", {warnings = "none"})
    add_files("lib/stb/*.cpp", {warnings = "none"})
    add_files("lib/meshoptimizer/*.cpp", {warnings = "none"})

    add_includedirs("include/slang")

    local slang_libdir = path.join(os.scriptdir(), "lib", "slang", vendor_platdir())
    add_linkdirs(slang_libdir)
    if is_plat("windows") then
        add_links("slang-compiler")
    else
        local so = os.files(path.join(slang_libdir, "libslang-compiler.so*"))[1]
        if so then add_links(":" .. path.filename(so)) end
    end

    -- Slang's runtime is two libraries: the compiler, and slang-glslang, which the compiler
    -- loads from beside itself to validate and optimise the SPIR-V it emits. Without it Slang
    -- still compiles but silently skips both -- or picks up whichever copy is on PATH, such as
    -- the Vulkan SDK's, of some other Slang version. The cooker refuses to run without it.
    local slang_runtime = path.join(slang_libdir, is_plat("windows") and "*.dll" or "libslang-*.so*")

    after_build(function (target)
        os.trycp(slang_runtime, target:targetdir())
    end)

    after_install(function (target)
        os.trycp(slang_runtime, path.join(target:installdir(), "bin"))
    end)
target_end()

target("tau-assets")
    set_kind("phony")
    add_rules("tau.assets")
    add_deps("tau-cooker", {inherit = false})
target_end()

target("tau-editor")
    set_kind("binary")
    add_rules("tau.common")
    add_deps("tau-engine", "tau-assets")

    add_files("lib/volk/volk.c", {warnings = "none"})

    if is_plat("windows") and is_mode("release") then
        add_ldflags("/subsystem:windows", "/entry:mainCRTStartup", {force = true})
    end

    add_files("src/tau-editor/**.cpp")

    if is_mode("debug") then
        add_defines("TAU_EDITOR_MODE=\"debug\"")
    elseif is_mode("releasedbg") then
        add_defines("TAU_EDITOR_MODE=\"releasedbg\"")
    else
        add_defines("TAU_EDITOR_MODE=\"release\"")
    end
    
    add_files("lib/imgui/**.cpp", {warnings = "none"})
target_end()
