#include "tau-editor/asset_watch.h"

#include "tau-editor/asset_cook.h"
#include "tau-editor/asset_editors.h"
#include "tau-editor/history.h"
#include "tau-editor/prefab_edit.h"
#include "tau-editor/prefab_stage.h"
#include "tau/asset.h"
#include "tau/assets/prefab.h"
#include "tau/engine.h"
#include "tau/hash.h"
#include "tau/log.h"
#include "tau/rendering/renderer.h"
#include "tau/vfs.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace tau::editor::asset_watch
{
    namespace
    {
        using clock_t = std::chrono::steady_clock;

        constexpr auto POLL_INTERVAL = std::chrono::milliseconds(250);

        // how long a file must hold still to count as written
        // tools save in stages, and cooking on the first movement cooks half a file
        constexpr auto SETTLE_TIME = std::chrono::milliseconds(250);

        struct stamp_t
        {
            i64 write_time = 0;
            u64_t size = 0;

            bool operator==(const stamp_t& other) const { return write_time == other.write_time && size == other.size; }
        };

        struct pending_t
        {
            stamp_t stamp;
            clock_t::time_point last_moved;
        };

        struct polling_backend_t final : backend_t
        {
            fs::path root;
            std::unordered_map<std::string, stamp_t> seen;
            bool primed = false;

            bool start(const fs::path& watch_root) override
            {
                root = watch_root;
                seen.clear();
                primed = false;

                std::error_code ec;
                return fs::is_directory(root, ec);
            }

            void collect(std::vector<fs::path>& out) override
            {
                std::error_code ec;
                std::unordered_set<std::string> present;

                for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec))
                {
                    if (ec || !it->is_regular_file(ec)) { continue; }

                    const std::string key = it->path().generic_string();
                    present.insert(key);

                    const stamp_t stamp = {it->last_write_time(ec).time_since_epoch().count(),
                                           static_cast<u64_t>(it->file_size(ec))};
                    if (ec) { continue; }

                    stamp_t& known = seen[key];
                    if (known == stamp) { continue; }

                    known = stamp;

                    // the first walk defines unchanged, it is not a set of edits
                    if (primed) { out.push_back(it->path()); }
                }

                for (auto it = seen.begin(); it != seen.end();)
                {
                    if (present.count(it->first) == 0) { it = seen.erase(it); }
                    else
                    {
                        it++;
                    }
                }

                primed = true;
            }
        };

        struct root_t
        {
            fs::path dir;
            std::string vfs_prefix;
            bool is_engine = false;
            std::unique_ptr<backend_t> backend;
        };

        // reloads include a partly failed batch, whatever cooked is newer than what is loaded
        struct cook_outcome_t
        {
            std::vector<std::string> reload; // vfs names
            std::unordered_set<std::string> saved;
            std::string error;
        };

        struct state_t
        {
            std::vector<root_t> roots;
            bool running = false;

            clock_t::time_point last_poll = clock_t::now();

            std::unordered_map<std::string, pending_t> settling;
            std::unordered_set<std::string> self_written;
            std::vector<fs::path> saved;
            std::vector<fs::path> edited;
            std::string open_scene; // as a vfs name

            std::future<cook_outcome_t> cook;
        };

        state_t g_state;

        std::vector<fs::path> g_candidates;

        bool is_inside(const fs::path& path, const fs::path& dir)
        {
            std::error_code ec;
            const std::string rel = fs::relative(path, dir, ec).generic_string();
            return !ec && !rel.empty() && rel.rfind("..", 0) != 0;
        }

        // the cooker takes disk paths, the registry vfs names: root scheme plus path inside the root
        std::string vfs_name_for(const fs::path& path, const fs::path& root, const std::string& prefix)
        {
            std::error_code ec;
            const std::string rel = fs::relative(path, root, ec).generic_string();
            if (ec || rel.empty() || rel.rfind("..", 0) == 0) { return {}; }

            return prefix + rel;
        }

        const root_t* root_for(const fs::path& path)
        {
            for (const root_t& root : g_state.roots)
            {
                if (is_inside(path, root.dir)) { return &root; }
            }
            return nullptr;
        }

        std::string normalise(const fs::path& path)
        {
            std::error_code ec;
            const fs::path canonical = fs::weakly_canonical(path, ec);
            return (ec ? path : canonical).generic_string();
        }

        bool is_cookable(const fs::path& path) { return tau::assets::type_by_path(path.generic_string()) != nullptr; }

        fs::path source_of_meta(const fs::path& path)
        {
            if (path.extension() != ".meta") { return {}; }

            fs::path source = path;
            source.replace_extension();
            return is_cookable(source) ? source : fs::path{};
        }

        void take_settled(std::vector<fs::path>& out)
        {
            const clock_t::time_point now = clock_t::now();

            for (auto it = g_state.settling.begin(); it != g_state.settling.end();)
            {
                if (now - it->second.last_moved < SETTLE_TIME)
                {
                    it++;
                    continue;
                }

                const auto own = g_state.self_written.find(it->first);
                if (own != g_state.self_written.end())
                {
                    g_state.self_written.erase(own);
                    it = g_state.settling.erase(it);
                    continue;
                }

                out.emplace_back(it->first);
                it = g_state.settling.erase(it);
            }
        }

        // runs off the main thread: engine edits first, into the editor's own engine tree
        // then the project, given the engine files too so only assets that read them are recooked
        cook_outcome_t cook_batch(std::vector<fs::path> game_files, std::vector<fs::path> engine_files,
                                  fs::path game_root, fs::path engine_root, std::unordered_set<std::string> saved)
        {
            cook_outcome_t outcome;
            outcome.saved = std::move(saved);

            auto note_error = [&](const std::string& err)
            {
                if (!outcome.error.empty()) { outcome.error += "; "; }
                outcome.error += err;
            };

            if (!engine_files.empty())
            {
                std::vector<fs::path> cooked;
                std::string err;
                if (!asset_cook::cook_engine_assets(engine_files, err, &cooked)) { note_error(err); }

                for (const fs::path& source : cooked)
                {
                    const std::string name =
                        vfs_name_for(source, engine_root, tau::vfs::asset_prefix(tau::vfs::ENGINE_NS));
                    if (!name.empty()) { outcome.reload.push_back(name); }
                }
            }

            std::vector<fs::path> project_batch = std::move(game_files);
            project_batch.insert(project_batch.end(), engine_files.begin(), engine_files.end());

            if (!project_batch.empty())
            {
                std::vector<fs::path> cooked;
                std::string err;
                if (!asset_cook::cook_assets(project_batch, err, &cooked)) { note_error(err); }

                for (const fs::path& source : cooked)
                {
                    const std::string name = vfs_name_for(source, game_root, tau::vfs::asset_prefix(tau::vfs::GAME_NS));
                    if (!name.empty()) { outcome.reload.push_back(name); }
                }
            }

            return outcome;
        }

        void apply_reloads(const cook_outcome_t& outcome)
        {
            // unsaved edits survive reloads of their deps
            tau::world_t& world = tau::engine::get_active_world();
            const std::vector<history::held_t> held = history::hold_unsaved_assets(world);

            for (const std::string& vfs_name : outcome.reload)
            {
                // would drop undo steps and only re read memory
                if (outcome.saved.contains(vfs_name)) { continue; }

                // the open scene in memory is the newer copy, reloading would discard unsaved work
                if (!g_state.open_scene.empty() && vfs_name == g_state.open_scene)
                {
                    TAU_LOG_INFO("ASSET_WATCH", "Recooked the open scene but left it alone: {}", vfs_name);
                    continue;
                }

                const history::doc_t doc = history::asset_doc(vfs_name);
                if (history::is_dirty(doc))
                {
                    TAU_LOG_WARN("ASSET_WATCH", "{} changed on disk, kept the unsaved edits made in the editor",
                                 vfs_name);
                    continue;
                }

                if (prefab_stage::on_disk_change(world, vfs_name)) { continue; }

                const asset_editors::asset_editor_t* editor = asset_editors::of_path(vfs_name);
                if (editor && editor->reload ? editor->reload(world, vfs_name) : tau::assets::reload(vfs_name))
                {
                    // renderer may warn again if it goes missing later
                    tau::renderer::forget_missing_asset_reports();
                    // stale undo steps
                    history::drop(doc);
                    TAU_LOG_INFO("ASSET_WATCH", "Reloaded {}", vfs_name);
                }
            }

            history::restore_unsaved_assets(world, held);
        }
    } // namespace

    std::unique_ptr<backend_t> make_polling_backend() { return std::make_unique<polling_backend_t>(); }

    void start(const fs::path& game_root, const fs::path& engine_root, backend_factory_t make_backend)
    {
        stop();

        auto add_root = [&](const fs::path& dir, const std::string& prefix, bool is_engine)
        {
            root_t root{dir, prefix, is_engine, make_backend()};
            if (!root.backend->start(dir))
            {
                TAU_LOG_WARN("ASSET_WATCH", "Not watching '{}': it is not a directory", dir.generic_string());
                return;
            }

            TAU_LOG_INFO("ASSET_WATCH", "Watching {}", dir.generic_string());
            g_state.roots.push_back(std::move(root));
        };

        add_root(game_root, tau::vfs::asset_prefix(tau::vfs::GAME_NS), false);
        if (!engine_root.empty()) { add_root(engine_root, tau::vfs::asset_prefix(tau::vfs::ENGINE_NS), true); }

        g_state.running = !g_state.roots.empty();
        g_state.last_poll = clock_t::now();
    }

    void stop()
    {
        if (g_state.cook.valid()) { g_state.cook.wait(); }

        g_state = {};
    }

    void ignore_own_write(const fs::path& path) { g_state.self_written.insert(normalise(path)); }

    void set_open_scene(const fs::path& path)
    {
        g_state.open_scene.clear();
        if (path.empty()) { return; }

        for (const root_t& root : g_state.roots)
        {
            if (root.is_engine) { continue; }
            g_state.open_scene = vfs_name_for(normalise(path), normalise(root.dir), root.vfs_prefix);
        }
    }

    namespace
    {
        void finish_cook()
        {
            const cook_outcome_t outcome = g_state.cook.get();
            g_state.cook = {};

            if (!outcome.error.empty()) { TAU_LOG_ERROR("ASSET_WATCH", "Could not cook: {}", outcome.error); }
            apply_reloads(outcome);
        }

        void start_cook(std::vector<fs::path> changed, bool wait)
        {
            std::vector<fs::path> game_files;
            std::vector<fs::path> engine_files;
            std::unordered_set<std::string> saved;
            fs::path game_root;
            fs::path engine_root;

            for (const root_t& root : g_state.roots) { (root.is_engine ? engine_root : game_root) = root.dir; }

            const auto add = [&](const fs::path& path)
            {
                const root_t* root = root_for(path);
                if (root == nullptr) { return false; }

                std::vector<fs::path>& files = root->is_engine ? engine_files : game_files;
                if (std::find(files.begin(), files.end(), path) == files.end()) { files.push_back(path); }
                return true;
            };

            if (!changed.empty())
            {
                TAU_LOG_INFO("ASSET_WATCH", "{} asset(s) changed on disk, reimporting", changed.size());
            }
            for (const fs::path& path : changed)
            {
                const fs::path source = source_of_meta(path);
                add(source.empty() ? path : fs::path(normalise(source)));
            }
            for (const fs::path& path : g_state.edited) { add(path); }
            g_state.edited.clear();

            for (const fs::path& path : g_state.saved)
            {
                const root_t* root = root_for(path);
                if (root && add(path)) { saved.insert(vfs_name_for(path, root->dir, root->vfs_prefix)); }
            }
            g_state.saved.clear();

            if (game_files.empty() && engine_files.empty()) { return; }

            g_state.cook = std::async(std::launch::async, &cook_batch, std::move(game_files), std::move(engine_files),
                                      std::move(game_root), std::move(engine_root), std::move(saved));
            if (wait) { finish_cook(); }
        }
    } // namespace

    bool is_cooking() { return g_state.cook.valid() || !g_state.saved.empty() || !g_state.edited.empty(); }

    void cook_saved(std::span<const fs::path> sources)
    {
        if (!g_state.running)
        {
            std::vector<fs::path> files(sources.begin(), sources.end());
            std::string err;
            if (!asset_cook::cook_assets(files, err)) { TAU_LOG_ERROR("ASSET_WATCH", "Could not cook: {}", err); }
            return;
        }

        for (const fs::path& source : sources)
        {
            const std::string key = normalise(source);
            g_state.self_written.insert(key);
            if (std::find(g_state.saved.begin(), g_state.saved.end(), fs::path(key)) == g_state.saved.end())
            {
                g_state.saved.emplace_back(key);
            }
        }
    }

    void cook_edited(std::span<const fs::path> sources)
    {
        if (!g_state.running)
        {
            std::vector<fs::path> files(sources.begin(), sources.end());
            std::string err;
            if (!asset_cook::cook_assets(files, err)) { TAU_LOG_ERROR("ASSET_WATCH", "Could not cook: {}", err); }
            return;
        }

        for (const fs::path& source : sources)
        {
            const fs::path key = normalise(source);
            if (std::find(g_state.edited.begin(), g_state.edited.end(), key) == g_state.edited.end())
            {
                g_state.edited.push_back(key);
            }
        }
    }

    void flush()
    {
        if (g_state.cook.valid()) { finish_cook(); }
        if (!g_state.saved.empty() || !g_state.edited.empty()) { start_cook({}, true); }
    }

    void tick()
    {
        if (!g_state.running) { return; }

        // a finished cook first, so its reloads land before a new one starts
        if (g_state.cook.valid() && g_state.cook.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            finish_cook();
        }

        const clock_t::time_point now = clock_t::now();
        if (now - g_state.last_poll < POLL_INTERVAL) { return; }
        g_state.last_poll = now;

        g_candidates.clear();
        for (root_t& root : g_state.roots) { root.backend->collect(g_candidates); }

        // the backend only nominates, the stat decides, which keeps coalesced or duplicate events harmless
        for (const fs::path& candidate : g_candidates)
        {
            if (!is_cookable(candidate) && source_of_meta(candidate).empty()) { continue; }

            std::error_code ec;
            const stamp_t stamp = {fs::last_write_time(candidate, ec).time_since_epoch().count(),
                                   static_cast<u64_t>(fs::file_size(candidate, ec))};
            if (ec) { continue; }

            pending_t& pending = g_state.settling[normalise(candidate)];
            if (!(pending.stamp == stamp))
            {
                pending.stamp = stamp;
                pending.last_moved = now;
            }
        }

        if (g_state.cook.valid()) { return; } // one cook at a time; the rest keeps settling

        std::vector<fs::path> ready;
        take_settled(ready);

        start_cook(std::move(ready), false);
    }
} // namespace tau::editor::asset_watch
