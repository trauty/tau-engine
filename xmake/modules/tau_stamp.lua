-- identifies an engine build: version, engine headers and build mode
-- a game library built against another one disagrees with it on struct layouts
import("core.project.config")

function build_mode()
    return config.get("mode") or "release"
end

function read_version(version_file)
    return os.isfile(version_file) and io.readfile(version_file):trim() or "0.0.0"
end

function compute(header_root, version_file, mode)
    local entries = {}
    for _, file in ipairs(os.files(path.join(header_root, "tau", "**.h"))) do
        local rel = path.relative(file, header_root):gsub("\\", "/")
        table.insert(entries, rel .. "=" .. hash.xxhash64(file))
    end
    table.sort(entries)

    return read_version(version_file) .. "+" .. hash.strhash64(table.concat(entries, "\n")) .. "-" .. mode
end

-- rewritten only when the stamp changes, so an unchanged engine compiles nothing
function write_source(file, content)
    if not os.isfile(file) or io.readfile(file) ~= content then
        io.writefile(file, content)
    end
end

function write_engine_source(file, header_root, version_file, mode)
    local stamp = compute(header_root, version_file, mode)
    write_source(file, '#include "tau/engine.h"\n\nconst char* tau::engine::build_stamp() { return "' .. stamp ..
                           '"; }\nconst char* tau::engine::version() { return "' .. read_version(version_file) .. '"; }\n')
end

function write_game_source(file, header_root, version_file, mode)
    local stamp = compute(header_root, version_file, mode)
    write_source(file, '#include "tau/defines.h"\n\nextern "C" TAU_GAME_API const char* tau_game_engine_stamp() { return "' ..
                           stamp .. '"; }\n')
end
