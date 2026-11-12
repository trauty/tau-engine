#include "tau-cooker/cache_manager.h"
#include "tau-cooker/material_cooker.h"
#include "tau-cooker/mesh_cooker.h"
#include "tau-cooker/scene_cooker.h"
#include "tau-cooker/shader_cooker.h"
#include "tau-cooker/texture_cooker.h"
#include "tau/asset.h"
#include "tau/asset_sources.h"
#include "tau/asset_types.h"
#include "tau/log.h"
#include "tau/vfs.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(__SANITIZE_ADDRESS__) || defined(__has_feature)
    #if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
extern "C" const char* __asan_default_options() { return "alloc_dealloc_mismatch=0"; }
    #endif
#endif

std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

int main(i32 argc, char** argv)
{
    std::vector<std::string> input_dirs;
    std::vector<std::string> include_dirs;
    std::string output_dir = ".tau";
    std::string name_space;
    // cook exactly these sources instead of walking the input dirs
    // references resolve through the guid map a previous full cook left on disk
    // repeatable, so a batch pays the ~240 ms process start once
    std::vector<std::string> single_files;
    // where to write the sources this run cooked, dependents included, for the editor to reload
    std::string report_path;

    for (i32 i = 1; i < argc; i++)
    {
        std::string arg = argv[i];
        if (arg == "-i" && i + 1 < argc) { input_dirs.push_back(argv[++i]); }
        else if (arg == "-I" && i + 1 < argc) { include_dirs.push_back(argv[++i]); }
        else if (arg == "-o" && i + 1 < argc) { output_dir = argv[++i]; }
        else if ((arg == "-n" || arg == "--namespace") && i + 1 < argc) { name_space = argv[++i]; }
        else if (arg == "--file" && i + 1 < argc) { single_files.emplace_back(argv[++i]); }
        else if (arg == "--report" && i + 1 < argc) { report_path = argv[++i]; }
    }

    // logging needs init() first: TAU_LOG_* queues onto the worker it starts
    tau::log::init();
    tau::assets::register_engine_types();

    if (input_dirs.empty() || name_space.empty())
    {
        TAU_LOG_ERROR("ASSET_COOKER", "usage: tau-cooker -i <cook_dir> [-I <include_dir>] -o <out_dir> "
                                      "-n <namespace> [--file <source> ...] [--report <json>]");
        // the namespace is the vfs scheme cooked assets are keyed under
        tau::log::shutdown();
        return 1;
    }

    // every dir in one spelling (absolute, canonical, forward slashes), as paths built from them are cache keys
    // xmake and the editor spell the same dir differently, which split one file into two keys
    auto canonical_dir = [](std::string& dir)
    {
        std::error_code ec;
        const std::filesystem::path resolved =
            std::filesystem::weakly_canonical(std::filesystem::absolute(dir, ec), ec);
        if (!ec) { dir = resolved.generic_string(); }
    };
    for (std::string& dir : input_dirs) { canonical_dir(dir); }
    for (std::string& dir : include_dirs) { canonical_dir(dir); }
    canonical_dir(output_dir);

    std::vector<std::string> all_shader_dirs = input_dirs;
    all_shader_dirs.insert(all_shader_dirs.end(), include_dirs.begin(), include_dirs.end());

    {
        const std::string parent = std::filesystem::path(output_dir).parent_path().generic_string();
        const std::filesystem::path log_path =
            std::filesystem::path(parent.empty() ? "." : parent) / "logs" / "cooker.log";
        tau::log::to_file(log_path.string());
    }

    // a shared struct moving in C++ changes no shader source, so fingerprint it for the cache
    tau::cooker::set_layout_stamp(tau::cooker::shader::core_layout_stamp());

    // a source this run cooks, with its path relative to the input dir
    struct source_t
    {
        std::filesystem::path path;
        std::string rel_path;
    };

    std::vector<source_t> sources;

    // same path building as the scan below, so guid map key, output path and cache entry agree
    auto place = [&input_dirs](const std::string& named) -> std::optional<source_t>
    {
        std::error_code ec;
        const std::filesystem::path file = std::filesystem::weakly_canonical(named, ec);

        for (const std::string& dir : input_dirs)
        {
            const std::filesystem::path root = std::filesystem::weakly_canonical(dir, ec);
            const std::string rel = std::filesystem::relative(file, root, ec).generic_string();

            if (ec || rel.empty() || rel == "." || rel.rfind("..", 0) == 0) { continue; }

            return source_t{std::filesystem::path(dir) / rel, rel};
        }

        return std::nullopt;
    };

    // named files in an include dir: nothing is cooked of them here, but what was built from them is stale
    std::vector<std::filesystem::path> changed_includes;

    if (single_files.empty()) { TAU_LOG_INFO("ASSET_COOKER", "Cooking assets..."); }
    else
    {
        for (const std::string& named : single_files)
        {
            std::optional<source_t> placed = place(named);

            if (!placed)
            {
                bool in_include_dir = false;
                for (const std::string& dir : include_dirs)
                {
                    std::error_code ec;
                    const std::string rel = std::filesystem::relative(std::filesystem::weakly_canonical(named, ec),
                                                                      std::filesystem::weakly_canonical(dir, ec), ec)
                                                .generic_string();
                    if (!ec && !rel.empty() && rel.rfind("..", 0) != 0) { in_include_dir = true; }
                }

                if (in_include_dir)
                {
                    changed_includes.emplace_back(named);
                    continue;
                }

                TAU_LOG_ERROR("ASSET_COOKER", "--file {} is not inside any -i or -I directory", named);
                tau::log::shutdown();
                return 1;
            }

            if (tau::assets::type_by_path(placed->rel_path) == nullptr)
            {
                TAU_LOG_ERROR("ASSET_COOKER", "--file {} is not a cookable asset type", named);
                tau::log::shutdown();
                return 1;
            }

            sources.push_back(*placed);
        }

        if (!sources.empty())
        {
            TAU_LOG_INFO("ASSET_COOKER", "Cooking {} named asset(s), first: {}", sources.size(), sources[0].rel_path);
        }
    }

    tau::cooker::asset_cache_t cache(output_dir + "/cooker_cache.json");
    cache.load();

    // a named file brings what was built from it, found through the sources the cache recorded
    if (!single_files.empty())
    {
        std::vector<std::filesystem::path> changed = changed_includes;
        for (const source_t& source : sources) { changed.push_back(source.path); }

        std::vector<std::string> known;
        for (const source_t& source : sources)
        {
            std::error_code ec;
            known.push_back(std::filesystem::weakly_canonical(source.path, ec).generic_string());
        }

        for (const std::string& dependent : cache.sources_depending_on(changed))
        {
            std::error_code ec;
            const std::string canonical = std::filesystem::weakly_canonical(dependent, ec).generic_string();
            if (std::find(known.begin(), known.end(), canonical) != known.end()) { continue; }

            std::optional<source_t> placed = place(dependent);
            if (!placed) { continue; }

            TAU_LOG_INFO("ASSET_COOKER", "Also checking {}: it was built from a named file", placed->rel_path);
            known.push_back(canonical);
            sources.push_back(*placed);
        }
    }

    // what this run produced, for --report
    std::vector<std::string> cooked_sources;

    // cache keys of live sources: expected_outputs plus import only modules, keyed by source
    std::unordered_set<std::string> live_cache_keys;

    std::filesystem::create_directories(output_dir + "/shaders");

    std::string vfs_prefix = tau::vfs::asset_prefix(name_space);
    std::vector<std::pair<std::string, tau::asset_guid_t>> guid_map;
    bool any_failed = false;
    std::unordered_set<std::string> expected_outputs;

    // pass one: every asset gets its guid first, materials record their shader and texture guids
    for (const std::string& dir : input_dirs)
    {
        if (!single_files.empty()) { break; }
        if (!std::filesystem::exists(dir)) { continue; }

        TAU_LOG_INFO("ASSET_COOKER", "Scanning for assets: {}", dir);

        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir))
        {
            if (!entry.is_regular_file()) { continue; }

            // only what the cooker can build is an asset, other files get no guid or stray .meta
            const std::string rel = std::filesystem::relative(entry.path(), dir).generic_string();
            if (tau::assets::type_by_path(rel) == nullptr) { continue; }

            sources.push_back({entry.path(), rel});
        }
    }

    // engine assets a game references live under their own prefix, load what a previous cook recorded first
    nlohmann::json existing_map;
    {
        const std::string parent = std::filesystem::path(output_dir).parent_path().generic_string();
        const std::string map_path = (parent.empty() ? std::string(".") : parent) + "/guid_map.json";
        if (std::filesystem::exists(map_path))
        {
            std::ifstream map_file(map_path);
            std::string text((std::istreambuf_iterator<char>(map_file)), std::istreambuf_iterator<char>());
            existing_map = nlohmann::json::parse(text, nullptr, false);
            tau::assets::merge_guid_map(text);
        }
    }

    // a full walk always rewrites the map, only it knows which entries lost their source
    // a single file run rewrites only for a new asset, as the cost grows with the project
    bool guid_map_dirty = single_files.empty();

    for (const source_t& source : sources)
    {
        const std::string key = vfs_prefix + source.rel_path;
        const tau::asset_guid_t guid = tau::asset_sources::find_or_create_guid(source.path.generic_string());

        guid_map.emplace_back(key, guid);
        tau::assets::set_location(key, guid);

        // the old spelling of the same guid rewrites the map too
        if (!existing_map.is_object() || existing_map.value(key, std::string{}) != tau::assets::guid_text(guid))
        {
            guid_map_dirty = true;
        }
    }

    // how the cooker builds each type, a cook reports its output and the files it read
    struct cook_result_t
    {
        bool ok = false;
        bool produced_output = true;
        std::vector<std::filesystem::path> deps;
    };

    using cook_fn_t = std::function<cook_result_t(const std::string&, const std::string&)>;

    auto simple = [](bool (*fn)(const std::string&, const std::string&)) -> cook_fn_t
    {
        return [fn](const std::string& in, const std::string& out) -> cook_result_t
        { return {fn(in, out), true, {std::filesystem::path(in)}}; };
    };

    // "ns://assets/rel" to the source: this run's own namespace in the input dirs, others in the include dirs
    const auto resolve_source = [&](const std::string& vfs_path) -> std::filesystem::path
    {
        const std::optional<tau::vfs::asset_path_parts_t> parts = tau::vfs::split_asset_path(vfs_path);
        if (!parts) { return {}; }

        const std::string rel(parts->rel);
        const std::vector<std::string>& dirs = parts->ns == name_space ? input_dirs : include_dirs;
        for (const std::string& dir : dirs)
        {
            const std::filesystem::path candidate = std::filesystem::path(dir) / rel;
            if (std::filesystem::exists(candidate)) { return candidate; }
        }
        return {};
    };

    const std::unordered_map<std::string_view, cook_fn_t> cookers = {
        {"shader",
         cook_fn_t{[&all_shader_dirs](const std::string& in, const std::string& out) -> cook_result_t
                   {
                       std::vector<std::filesystem::path> deps;
                       const auto status = tau::cooker::shader::cook_shader(in, out, all_shader_dirs, deps);
                       if (deps.empty()) { deps.emplace_back(in); }

                       return {status != tau::cooker::shader::cook_status_e::FAILED,
                               status == tau::cooker::shader::cook_status_e::COOKED, std::move(deps)};
                   }}},
        {"mesh", simple(&tau::cooker::mesh::cook_mesh)},
        {"material", simple(&tau::cooker::material::cook_material)},
        {"scene",
         cook_fn_t{[&](const std::string& in, const std::string& out) -> cook_result_t
                   {
                       std::vector<std::filesystem::path> deps = {std::filesystem::path(in)};
                       const bool ok = tau::cooker::scene::cook_scene(in, out, resolve_source, deps);
                       return {ok, true, std::move(deps)};
                   }}},
        {"prefab", simple(&tau::cooker::scene::cook_prefab)},
        {"texture",
         cook_fn_t{[](const std::string& in, const std::string& out) -> cook_result_t
                   {
                       // the texture cooker may create the .meta, so track it either way
                       // a missing file hashes to 0 and its appearance forces exactly one more cook
                       return {tau::cooker::texture::cook_texture(in, out),
                               true,
                               {std::filesystem::path(in), std::filesystem::path(tau::asset_sources::meta_path(in))}};
                   }}},
    };

    // pass two: cook, every reference resolves to a guid now
    for (const source_t& source : sources)
    {
        const std::string path = source.path.generic_string();
        std::filesystem::path out_path = std::filesystem::path(output_dir) / source.rel_path;

        const tau::asset_type_t* type = tau::assets::type_by_path(source.rel_path);
        if (type == nullptr) { continue; }

        auto cooker_it = cookers.find(type->key);
        if (cooker_it == cookers.end()) { continue; }

        // last cook produced nothing and nothing it reads changed
        if (cache.is_known_non_output(path))
        {
            live_cache_keys.insert(path);
            continue;
        }

        out_path.replace_extension(std::string(type->cooked_extension));
        const std::string out = out_path.generic_string();

        if (!cache.needs_cooking(out, {std::filesystem::path(path)}))
        {
            // entries from before sources were recorded get them here, so dependents are found without a recook
            cache.remember_source(out, path);
            expected_outputs.insert(out);
            continue;
        }

        const cook_result_t result = cooker_it->second(path, out);

        if (!result.ok)
        {
            any_failed = true;
            expected_outputs.insert(out);
            continue;
        }

        if (!result.produced_output)
        {
            cache.mark_non_output(path, result.deps);
            live_cache_keys.insert(path);
            continue;
        }

        expected_outputs.insert(out);
        cache.update_cache(out, result.deps, path);
        cooked_sources.push_back(path);
    }

    // a single file run's expected_outputs is partial, sweeping on it would delete the cooked tree
    if (!any_failed && single_files.empty())
    {
        std::vector<std::filesystem::path> orphans;

        for (const auto& entry : std::filesystem::recursive_directory_iterator(output_dir))
        {
            if (!entry.is_regular_file()) { continue; }

            const std::string generic = entry.path().generic_string();
            const std::string filename = entry.path().filename().string();

            if (filename == "cooker_cache.json" || filename == "guid_map.json") { continue; }
            if (expected_outputs.count(generic) > 0) { continue; }

            orphans.push_back(entry.path());
        }

        for (const std::filesystem::path& orphan : orphans)
        {
            TAU_LOG_INFO("ASSET_COOKER", "Removing orphaned output (its source is gone): {}", orphan.generic_string());
            cache.forget(orphan.generic_string());

            std::error_code ec;
            std::filesystem::remove(orphan, ec);
            if (ec) { TAU_LOG_WARN("ASSET_COOKER", "Could not remove {}: {}", orphan.generic_string(), ec.message()); }
        }

        // same for the cache: drop entries whose source is gone or that use a path spelling no longer produced
        live_cache_keys.insert(expected_outputs.begin(), expected_outputs.end());
        const size_t dropped = cache.retain_only(live_cache_keys);
        if (dropped > 0) { TAU_LOG_INFO("ASSET_COOKER", "Dropped {} stale cook cache entries", dropped); }
    }
    else if (any_failed && single_files.empty())
    {
        TAU_LOG_WARN("ASSET_COOKER", "Skipping orphan sweep because something failed to cook");
    }

    if (guid_map_dirty)
    {
        std::string parent_out = std::filesystem::path(output_dir).parent_path().generic_string();
        if (parent_out.empty()) { parent_out = "."; }

        // the prefix retires entries whose source is gone, only a full walk knows that
        // a single file run merges its one entry and leaves the rest
        tau::asset_sources::write_guid_map(parent_out + "/guid_map.json", guid_map,
                                           single_files.empty() ? vfs_prefix : std::string{});
    }

    cache.save();

    // written even on failure, what did cook should still reload
    if (!report_path.empty())
    {
        nlohmann::json report;
        report["cooked"] = cooked_sources;

        std::ofstream report_file(report_path);
        report_file << report.dump(4);
    }

    if (any_failed)
    {
        TAU_LOG_ERROR("ASSET_COOKER", "Cooking finished with errors");
        tau::log::shutdown();
        return 1;
    }

    TAU_LOG_INFO("ASSET_COOKER", "Cooking finished");

    tau::log::shutdown();

    return 0;
}