#include "tau-cooker/cache_manager.h"
#include "tau-cooker/cookers.h"
#include "tau-cooker/shader_cooker.h"
#include "tau/asset.h"
#include "tau/asset_sources.h"
#include "tau/asset_types.h"
#include "tau/assets/import_settings.h"
#include "tau/log.h"
#include "tau/project.h"
#include "tau/reflection.h"
#include "tau/vfs.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__SANITIZE_ADDRESS__) || defined(__has_feature)
    #if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
extern "C" const char* __asan_default_options() { return "alloc_dealloc_mismatch=0"; }
    #endif
#endif

namespace fs = std::filesystem;

namespace
{
    struct source_t
    {
        fs::path path;
        std::string rel_path;
    };

    struct cook_run_t
    {
        tau::cooker::cook_env_t env;
        std::string output_dir = ".tau";
        // beside the output dir: guid_map.json and logs/
        std::string root_dir;
        // cook exactly these sources instead of walking the input dirs
        // references resolve through the guid map a previous full cook left on disk
        // repeatable, so a batch pays the ~240 ms process start once
        std::vector<std::string> single_files;
        // where to write the sources this run cooked, dependents included, for the editor to reload
        std::string report_path;

        std::string project_file;

        std::vector<source_t> sources;
        std::vector<fs::path> changed_includes;

        std::vector<std::pair<std::string, tau::asset_guid_t>> guid_map;
        bool guid_map_dirty = false;

        std::unordered_set<std::string> expected_outputs;
        std::unordered_set<std::string> live_cache_keys;

        std::vector<std::string> cooked_sources;
        bool any_failed = false;

        bool full() const { return single_files.empty(); }
        std::string vfs_prefix() const { return tau::vfs::asset_prefix(env.name_space); }
    };

    void parse_args(i32 argc, char** argv, cook_run_t& run)
    {
        for (i32 i = 1; i < argc; i++)
        {
            const std::string arg = argv[i];
            if (arg == "-i" && i + 1 < argc) { run.env.input_dirs.push_back(argv[++i]); }
            else if (arg == "-I" && i + 1 < argc) { run.env.include_dirs.push_back(argv[++i]); }
            else if (arg == "-o" && i + 1 < argc) { run.output_dir = argv[++i]; }
            else if ((arg == "-n" || arg == "--namespace") && i + 1 < argc) { run.env.name_space = argv[++i]; }
            else if (arg == "--file" && i + 1 < argc) { run.single_files.emplace_back(argv[++i]); }
            else if (arg == "--report" && i + 1 < argc) { run.report_path = argv[++i]; }
            else if (arg == "--project" && i + 1 < argc) { run.project_file = argv[++i]; }
        }
    }

    // every dir in one spelling (absolute, canonical, forward slashes), as paths built from them are cache keys
    // xmake and the editor spell the same dir differently, which split one file into two keys
    void canonicalize_dirs(cook_run_t& run)
    {
        auto canonical = [](std::string& dir)
        {
            std::error_code ec;
            const fs::path resolved = fs::weakly_canonical(fs::absolute(dir, ec), ec);
            if (!ec) { dir = resolved.generic_string(); }
        };
        for (std::string& dir : run.env.input_dirs) { canonical(dir); }
        for (std::string& dir : run.env.include_dirs) { canonical(dir); }
        canonical(run.output_dir);

        const std::string parent = fs::path(run.output_dir).parent_path().generic_string();
        run.root_dir = parent.empty() ? "." : parent;
    }

    // same path building as the walk, so guid map key, output path and cache entry agree
    std::optional<source_t> place(const cook_run_t& run, const std::string& named)
    {
        std::error_code ec;
        const fs::path file = fs::weakly_canonical(named, ec);

        for (const std::string& dir : run.env.input_dirs)
        {
            const fs::path root = fs::weakly_canonical(dir, ec);
            const std::string rel = fs::relative(file, root, ec).generic_string();

            if (ec || rel.empty() || rel == "." || rel.rfind("..", 0) == 0) { continue; }

            return source_t{fs::path(dir) / rel, rel};
        }

        return std::nullopt;
    }

    bool in_include_dir(const cook_run_t& run, const std::string& named)
    {
        for (const std::string& dir : run.env.include_dirs)
        {
            std::error_code ec;
            const std::string rel =
                fs::relative(fs::weakly_canonical(named, ec), fs::weakly_canonical(dir, ec), ec).generic_string();
            if (!ec && !rel.empty() && rel.rfind("..", 0) != 0) { return true; }
        }
        return false;
    }

    bool collect_named(cook_run_t& run)
    {
        for (const std::string& named : run.single_files)
        {
            std::optional<source_t> placed = place(run, named);

            if (!placed)
            {
                if (in_include_dir(run, named))
                {
                    run.changed_includes.emplace_back(named);
                    continue;
                }

                TAU_LOG_ERROR("ASSET_COOKER", "--file {} is not inside any -i or -I directory", named);
                return false;
            }

            if (tau::assets::type_by_path(placed->rel_path) == nullptr)
            {
                TAU_LOG_ERROR("ASSET_COOKER", "--file {} is not a cookable asset type", named);
                return false;
            }

            run.sources.push_back(*placed);
        }

        if (!run.sources.empty())
        {
            TAU_LOG_INFO("ASSET_COOKER", "Cooking {} named asset(s), first: {}", run.sources.size(),
                         run.sources[0].rel_path);
        }
        return true;
    }

    // a named file brings what was built from it, found through the sources the cache recorded
    void collect_dependents(cook_run_t& run, const tau::cooker::asset_cache_t& cache)
    {
        std::vector<fs::path> changed = run.changed_includes;
        for (const source_t& source : run.sources) { changed.push_back(source.path); }

        std::vector<std::string> known;
        for (const source_t& source : run.sources)
        {
            std::error_code ec;
            known.push_back(fs::weakly_canonical(source.path, ec).generic_string());
        }

        for (const std::string& dependent : cache.sources_depending_on(changed))
        {
            std::error_code ec;
            const std::string canonical = fs::weakly_canonical(dependent, ec).generic_string();
            if (std::find(known.begin(), known.end(), canonical) != known.end()) { continue; }

            std::optional<source_t> placed = place(run, dependent);
            if (!placed) { continue; }

            TAU_LOG_INFO("ASSET_COOKER", "Also checking {}: it was built from a named file", placed->rel_path);
            known.push_back(canonical);
            run.sources.push_back(*placed);
        }
    }

    void collect_walk(cook_run_t& run)
    {
        for (const std::string& dir : run.env.input_dirs)
        {
            if (!fs::exists(dir)) { continue; }

            TAU_LOG_INFO("ASSET_COOKER", "Scanning for assets: {}", dir);

            for (const auto& entry : fs::recursive_directory_iterator(dir))
            {
                if (!entry.is_regular_file()) { continue; }

                // only what the cooker can build is an asset, other files get no guid or stray .meta
                const std::string rel = fs::relative(entry.path(), dir).generic_string();
                if (tau::assets::type_by_path(rel) == nullptr) { continue; }

                run.sources.push_back({entry.path(), rel});
            }
        }
    }

    void assign_guids(cook_run_t& run)
    {
        // engine assets a game references live under their own prefix, load what a previous cook recorded first
        nlohmann::json existing_map;
        const std::string map_path = run.root_dir + "/guid_map.json";
        if (fs::exists(map_path))
        {
            std::ifstream map_file(map_path);
            const std::string text((std::istreambuf_iterator<char>(map_file)), std::istreambuf_iterator<char>());
            existing_map = nlohmann::json::parse(text, nullptr, false);
            tau::assets::merge_guid_map(text);
        }

        // a full walk always rewrites the map, only it knows which entries lost their source
        // a single file run rewrites only for a new asset, as the cost grows with the project
        run.guid_map_dirty = run.full();

        const auto record = [&](const std::string& key, tau::asset_guid_t guid)
        {
            run.guid_map.emplace_back(key, guid);
            tau::assets::set_location(key, guid);

            // the old spelling of the same guid rewrites the map too
            if (!existing_map.is_object() || existing_map.value(key, std::string{}) != tau::assets::guid_text(guid))
            {
                run.guid_map_dirty = true;
            }
        };

        const std::string prefix = run.vfs_prefix();
        for (const source_t& source : run.sources)
        {
            const std::string key = prefix + source.rel_path;

            // first: listing may hand the source's old guid to a sub-asset
            const tau::asset_type_t* type = tau::assets::type_by_path(source.rel_path);
            const tau::cooker::cooker_t cooker = type ? tau::cooker::cooker_for(type->key) : tau::cooker::cooker_t{};
            if (cooker.list)
            {
                for (const auto& [name, guid] : cooker.list(run.env, source.path.generic_string()))
                {
                    record(key + "/" + name, guid);
                }
            }

            record(key, tau::asset_sources::find_or_create_guid(source.path.generic_string()));
        }
    }

    // models first: they set the kind of the images they use before those guess it from the name
    std::unordered_map<std::string, nlohmann::json> prepare_all(const cook_run_t& run)
    {
        std::vector<const source_t*> order;
        for (const source_t& source : run.sources) { order.push_back(&source); }
        std::stable_sort(order.begin(), order.end(),
                         [](const source_t* a, const source_t* b)
                         {
                             const tau::asset_type_t* ta = tau::assets::type_by_path(a->rel_path);
                             const tau::asset_type_t* tb = tau::assets::type_by_path(b->rel_path);
                             return (ta && ta->has_sub_assets) > (tb && tb->has_sub_assets);
                         });

        std::unordered_map<std::string, nlohmann::json> metas;
        for (const source_t* source : order)
        {
            const tau::asset_type_t* type = tau::assets::type_by_path(source->rel_path);
            const tau::cooker::cooker_t cooker = type ? tau::cooker::cooker_for(type->key) : tau::cooker::cooker_t{};
            if (cooker.prepare)
            {
                metas[source->path.generic_string()] = cooker.prepare(run.env, source->path.generic_string());
            }
        }
        return metas;
    }

    // pass two: cook, every reference resolves to a guid now
    void cook_all(cook_run_t& run, tau::cooker::asset_cache_t& cache)
    {
        fs::create_directories(run.output_dir + "/shaders");

        const std::unordered_map<std::string, nlohmann::json> prepared = prepare_all(run);

        for (const source_t& source : run.sources)
        {
            const std::string path = source.path.generic_string();

            const tau::asset_type_t* type = tau::assets::type_by_path(source.rel_path);
            if (type == nullptr) { continue; }

            const tau::cooker::cooker_t cooker = tau::cooker::cooker_for(type->key);
            if (cooker.cook == nullptr) { continue; }

            // last cook produced nothing and nothing it reads changed
            if (cache.is_known_non_output(path))
            {
                run.live_cache_keys.insert(path);
                continue;
            }

            fs::path out_path = fs::path(run.output_dir) / source.rel_path;
            out_path.replace_extension(std::string(type->cooked_extension));
            const std::string out = out_path.generic_string();

            const auto prepared_meta = prepared.find(path);
            const nlohmann::json meta = prepared_meta != prepared.end() ? prepared_meta->second
                                        : type->guid_in_source          ? nlohmann::json::object()
                                                                        : tau::asset_sources::read_meta(path);
            const std::string settings =
                tau::import_settings::fingerprint(type->id, run.env.import_defaults, meta, path) +
                (cooker.other_settings ? cooker.other_settings(run.env) : std::string{});

            if (!cache.needs_cooking(out, {fs::path(path)}, settings))
            {
                // entries from before sources were recorded get them here, so dependents are found without a recook
                cache.remember_source(out, path);
                run.expected_outputs.insert(out);
                for (const std::string& extra : cache.outputs_of(out)) { run.expected_outputs.insert(extra); }
                continue;
            }

            tau::cooker::cook_result_t result = cooker.cook(run.env, path, out);

            if (!type->guid_in_source)
            {
                const fs::path meta_dep = tau::asset_sources::meta_path(path);
                if (std::find(result.deps.begin(), result.deps.end(), meta_dep) == result.deps.end())
                {
                    result.deps.push_back(meta_dep);
                }
            }

            for (const fs::path& extra : result.outputs) { run.expected_outputs.insert(extra.generic_string()); }

            if (!result.ok)
            {
                run.any_failed = true;
                run.expected_outputs.insert(out);
                continue;
            }

            if (!result.produced_output)
            {
                cache.mark_non_output(path, result.deps);
                run.live_cache_keys.insert(path);
                continue;
            }

            run.expected_outputs.insert(out);
            cache.update_cache(out, result.deps, path, settings, result.outputs);
            run.cooked_sources.push_back(path);
        }
    }

    // full runs only: a single file run's expected_outputs is partial, sweeping on it would delete the cooked tree
    void sweep_orphans(cook_run_t& run, tau::cooker::asset_cache_t& cache)
    {
        if (run.any_failed)
        {
            TAU_LOG_WARN("ASSET_COOKER", "Skipping orphan sweep because something failed to cook");
            return;
        }

        std::vector<fs::path> orphans;
        for (const auto& entry : fs::recursive_directory_iterator(run.output_dir))
        {
            if (!entry.is_regular_file()) { continue; }

            const std::string filename = entry.path().filename().string();
            if (filename == "cooker_cache.json" || filename == "guid_map.json") { continue; }
            if (run.expected_outputs.contains(entry.path().generic_string())) { continue; }

            orphans.push_back(entry.path());
        }

        for (const fs::path& orphan : orphans)
        {
            TAU_LOG_INFO("ASSET_COOKER", "Removing orphaned output (its source is gone): {}", orphan.generic_string());
            cache.forget(orphan.generic_string());

            std::error_code ec;
            fs::remove(orphan, ec);
            if (ec) { TAU_LOG_WARN("ASSET_COOKER", "Could not remove {}: {}", orphan.generic_string(), ec.message()); }
        }

        // same for the cache: drop entries whose source is gone or that use a path spelling no longer produced
        run.live_cache_keys.insert(run.expected_outputs.begin(), run.expected_outputs.end());
        const size_t dropped = cache.retain_only(run.live_cache_keys);
        if (dropped > 0) { TAU_LOG_INFO("ASSET_COOKER", "Dropped {} stale cook cache entries", dropped); }
    }

    void write_guid_map(const cook_run_t& run)
    {
        if (!run.guid_map_dirty) { return; }

        // the prefix retires entries whose source is gone, only a full walk knows that
        // a single file run merges its one entry and leaves the rest
        tau::asset_sources::write_guid_map(run.root_dir + "/guid_map.json", run.guid_map,
                                           run.full() ? run.vfs_prefix() : std::string{});
    }

    // written even on failure, what did cook should still reload
    void write_report(const cook_run_t& run)
    {
        if (run.report_path.empty()) { return; }

        nlohmann::json report;
        report["cooked"] = run.cooked_sources;

        std::ofstream report_file(run.report_path);
        report_file << report.dump(4);
    }

    i32 finish(i32 code)
    {
        tau::log::shutdown();
        return code;
    }
} // namespace

int main(i32 argc, char** argv)
{
    cook_run_t run;
    parse_args(argc, argv, run);

    // logging needs init() first: TAU_LOG_* queues onto the worker it starts
    tau::log::init();
    tau::assets::register_engine_types();
    tau::reflection::register_types();

    // the namespace is the vfs scheme cooked assets are keyed under
    if (run.env.input_dirs.empty() || run.env.name_space.empty())
    {
        TAU_LOG_ERROR("ASSET_COOKER", "usage: tau-cooker -i <cook_dir> [-I <include_dir>] -o <out_dir> "
                                      "-n <namespace> [--project <file>] [--file <source> ...] [--report <json>]");
        return finish(1);
    }

    if (!run.project_file.empty())
    {
        tau::project_t project;
        if (!tau::project_t::load(run.project_file, project)) { return finish(1); }
        run.env.import_defaults = std::move(project.import_defaults);
    }

    canonicalize_dirs(run);
    tau::log::to_file((fs::path(run.root_dir) / "logs" / "cooker.log").string());

    // a shared struct moving in C++ changes no shader source, so fingerprint it for the cache
    tau::cooker::set_layout_stamp(tau::cooker::shader::core_layout_stamp());

    if (run.full()) { TAU_LOG_INFO("ASSET_COOKER", "Cooking assets..."); }
    else if (!collect_named(run)) { return finish(1); }

    tau::cooker::asset_cache_t cache(run.output_dir + "/cooker_cache.json");
    cache.load();

    if (run.full()) { collect_walk(run); }
    else
    {
        collect_dependents(run, cache);
    }

    assign_guids(run);
    cook_all(run, cache);
    if (run.full()) { sweep_orphans(run, cache); }
    write_guid_map(run);

    cache.save();
    write_report(run);

    if (run.any_failed)
    {
        TAU_LOG_ERROR("ASSET_COOKER", "Cooking finished with errors");
        return finish(1);
    }

    TAU_LOG_INFO("ASSET_COOKER", "Cooking finished");
    return finish(0);
}
