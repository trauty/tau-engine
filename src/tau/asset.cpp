#include "tau/asset.h"

#include "tau/asset_types.h"
#include "tau/guid.h"
#include "tau/jobs.h"
#include "tau/log.h"
#include "tau/profiling.h"
#include "tau/vfs.h"

#include "json/json.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace tau::assets
{
    namespace
    {
        constexpr u32_t CHUNK_SLOTS = 256;
        constexpr u32_t MAX_CHUNKS = 4096;
        constexpr u32_t MAX_TYPES = 64;
        constexpr u32_t NO_SLOT = UINT32_MAX;

        struct slot_t
        {
            std::atomic<asset_guid_t> guid = 0;
            std::atomic<asset_state_e> state = asset_state_e::NONE;
            const asset_type_t* type = nullptr;
            void* data = nullptr;
            std::string path;
            u32_t pins = 0;
            bool marked = false;
            // READ: the file, waiting for a loader; `charged` against the read-ahead
            std::vector<u8_t> bytes;
            u64_t charged = 0;
        };

        struct chunk_t
        { slot_t slots[CHUNK_SLOTS]; };

        // chunks never move: get() reads without the lock
        std::atomic<chunk_t*> g_chunks[MAX_CHUNKS] = {};

        std::mutex g_mutex;
        u32_t g_slot_count = 0;
        std::vector<u32_t> g_free;
        std::unordered_map<asset_guid_t, u32_t> g_by_guid;
        std::unordered_map<std::string, asset_guid_t> g_guid_by_path;
        std::unordered_map<asset_guid_t, std::string> g_path_by_guid;
        std::unordered_set<std::string> g_warned;
        std::vector<roots_fn_t> g_roots;

        asset_type_t g_types[MAX_TYPES];
        u32_t g_type_count = 0;

        std::atomic<u32_t> g_in_flight = 0;

        // file bytes read and not yet consumed by a loader; the IO thread waits above it
        constexpr u64_t READ_AHEAD_LIMIT = 256ull << 20;
        std::mutex g_read_mutex;
        std::condition_variable g_read_cv;
        u64_t g_read_ahead = 0;

        bool in_flight_state(asset_state_e state)
        {
            return state == asset_state_e::QUEUED || state == asset_state_e::LOADING ||
                   state == asset_state_e::READING || state == asset_state_e::READ;
        }

        void release_read_ahead(u64_t bytes)
        {
            if (bytes == 0) { return; }
            {
                std::scoped_lock lock(g_read_mutex);
                g_read_ahead -= bytes;
            }
            g_read_cv.notify_all();
        }

        std::vector<u8_t> read_cooked(const asset_type_t* type, const std::string& path)
        {
            ZoneScopedN("asset read");
            ZoneText(path.data(), path.size());
            return vfs::read_bytes(cooked_path(type->id, path));
        }

        // loader deps pinned until its asset is placed
        struct load_scope_t
        { std::vector<asset_handle_t> pinned; };

        thread_local load_scope_t* t_scope = nullptr;

        std::string extension_of(std::string_view path)
        {
            const std::size_t dot = path.find_last_of('.');
            if (dot == std::string_view::npos) { return {}; }

            const std::size_t slash = path.find_last_of("/\\");
            if (slash != std::string_view::npos && dot < slash) { return {}; }

            std::string ext(path.substr(dot));
            for (char& c : ext) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
            return ext;
        }

        slot_t* slot_at(u32_t index)
        {
            if (index / CHUNK_SLOTS >= MAX_CHUNKS) { return nullptr; }

            chunk_t* chunk = g_chunks[index / CHUNK_SLOTS].load(std::memory_order_acquire);
            return chunk ? &chunk->slots[index % CHUNK_SLOTS] : nullptr;
        }

        u32_t index_of(asset_handle_t handle)
        {
            if (!handle.is_valid()) { return NO_SLOT; }

            const slot_t* slot = slot_at(handle.slot);
            if (slot && slot->guid.load(std::memory_order_relaxed) == handle.guid) { return handle.slot; }

            const auto it = g_by_guid.find(handle.guid);
            return it != g_by_guid.end() ? it->second : NO_SLOT;
        }

        u32_t take_slot()
        {
            if (!g_free.empty())
            {
                const u32_t index = g_free.back();
                g_free.pop_back();
                return index;
            }

            const u32_t index = g_slot_count;
            const u32_t chunk = index / CHUNK_SLOTS;
            if (chunk >= MAX_CHUNKS) { return NO_SLOT; }

            if (!g_chunks[chunk].load(std::memory_order_relaxed))
            {
                g_chunks[chunk].store(new chunk_t(), std::memory_order_release);
            }

            g_slot_count++;
            return index;
        }

        asset_guid_t mapped_guid(std::string_view path)
        {
            const auto it = g_guid_by_path.find(std::string(path));
            return it != g_guid_by_path.end() ? it->second : 0;
        }

        asset_guid_t identity(std::string_view path, bool warn)
        {
            if (const asset_guid_t guid = mapped_guid(path)) { return guid; }

            if (warn && g_warned.insert(std::string(path)).second)
            {
                TAU_LOG_ERROR("ASSET",
                              "No guid for '{}', its path stands in: references to it break if it moves. Cook the "
                              "project so it lands in guid_map.json.",
                              path);
            }
            return hash_string64(path);
        }

        void set_location_locked(const std::string& path, asset_guid_t guid)
        {
            if (const auto old = g_guid_by_path.find(path); old != g_guid_by_path.end())
            {
                if (const auto rev = g_path_by_guid.find(old->second);
                    rev != g_path_by_guid.end() && rev->second == path)
                {
                    g_path_by_guid.erase(rev);
                }
                g_guid_by_path.erase(old);
            }
            if (const auto old = g_path_by_guid.find(guid); old != g_path_by_guid.end())
            {
                g_guid_by_path.erase(old->second);
            }

            g_guid_by_path[path] = guid;
            g_path_by_guid[guid] = path;
        }

        void unpin_all(const std::vector<asset_handle_t>& handles)
        {
            if (handles.empty()) { return; }

            std::scoped_lock lock(g_mutex);
            for (const asset_handle_t& handle : handles)
            {
                const u32_t index = index_of(handle);
                if (index == NO_SLOT) { continue; }

                slot_t* slot = slot_at(index);
                if (slot->pins > 0) { slot->pins--; }
            }
        }

        bool claim(slot_t* slot, asset_state_e from, asset_state_e to)
        { return slot->state.compare_exchange_strong(from, to, std::memory_order_acq_rel); }

        std::string path_of_slot(const slot_t* slot)
        {
            std::scoped_lock lock(g_mutex);
            return slot->path;
        }

        // claimed (LOADING); `bytes` empty: read here
        void run_loader(slot_t* slot, std::vector<u8_t> bytes)
        {
            const std::string path = path_of_slot(slot);
            if (bytes.empty()) { bytes = read_cooked(slot->type, path); }

            bool ok = false;
            load_scope_t scope;
            if (bytes.empty()) { TAU_LOG_ERROR("ASSET", "{} not found: {}", path, cooked_path(slot->type->id, path)); }
            else
            {
                ZoneScopedN("asset load");
                ZoneText(path.data(), path.size());

                load_scope_t* outer = std::exchange(t_scope, &scope);
                ok = slot->type->load(slot->data, path, bytes);
                t_scope = outer;
            }

            if (!ok) { TAU_LOG_ERROR("ASSET", "Failed to load {}", path); }

            slot->state.store(ok ? asset_state_e::READY : asset_state_e::FAILED, std::memory_order_release);
            slot->state.notify_all();
            g_in_flight.fetch_sub(1, std::memory_order_release);

            // after READY: collect() reaches the deps through it from now on
            unpin_all(scope.pinned);
        }

        // READ -> LOADING: the bytes the IO thread left
        bool load_read(slot_t* slot)
        {
            if (!claim(slot, asset_state_e::READ, asset_state_e::LOADING)) { return false; }

            std::vector<u8_t> bytes = std::move(slot->bytes);
            const u64_t charged = std::exchange(slot->charged, 0);
            run_loader(slot, std::move(bytes));
            release_read_ahead(charged);
            return true;
        }

        // the IO thread: waits for read-ahead room before claiming, so a blocking load can still take the slot
        void read_stage(slot_t* slot)
        {
            if (slot->state.load(std::memory_order_acquire) != asset_state_e::QUEUED) { return; }

            const std::string path = path_of_slot(slot);
            const u64_t size = vfs::file_size(cooked_path(slot->type->id, path));
            {
                std::unique_lock lock(g_read_mutex);
                g_read_cv.wait(lock,
                               [slot, size]
                               {
                                   return g_read_ahead == 0 || g_read_ahead + size <= READ_AHEAD_LIMIT ||
                                          slot->state.load(std::memory_order_acquire) != asset_state_e::QUEUED;
                               });
                g_read_ahead += size;
            }

            if (!claim(slot, asset_state_e::QUEUED, asset_state_e::READING))
            {
                release_read_ahead(size);
                return;
            }

            slot->bytes = read_cooked(slot->type, path);
            slot->charged = size;
            slot->state.store(asset_state_e::READ, std::memory_order_release);
            slot->state.notify_all();

            jobs::kick([slot]() { load_read(slot); }, nullptr, jobs::priority_e::LOW);
        }

        // runs the load, or waits for whoever does; never for a job still queued
        void finish(slot_t* slot)
        {
            for (;;)
            {
                const asset_state_e state = slot->state.load(std::memory_order_acquire);
                if (state == asset_state_e::QUEUED)
                {
                    if (claim(slot, asset_state_e::QUEUED, asset_state_e::LOADING))
                    {
                        run_loader(slot, {});
                        return;
                    }
                    continue;
                }
                if (state == asset_state_e::READ)
                {
                    if (load_read(slot)) { return; }
                    continue;
                }
                if (state != asset_state_e::LOADING && state != asset_state_e::READING) { return; }

                slot->state.wait(state, std::memory_order_acquire);
            }
        }

        struct freed_t
        {
            const asset_type_t* type = nullptr;
            void* data = nullptr;
            bool ready = false;
        };

        // locked: caller unloads outside it, unload may load assets
        freed_t free_slot(u32_t index)
        {
            slot_t* slot = slot_at(index);
            const freed_t freed = {slot->type, slot->data,
                                   slot->state.load(std::memory_order_acquire) == asset_state_e::READY};

            g_by_guid.erase(slot->guid.load(std::memory_order_relaxed));
            slot->guid.store(0, std::memory_order_release);
            slot->state.store(asset_state_e::NONE, std::memory_order_release);
            slot->type = nullptr;
            slot->data = nullptr;
            slot->path.clear();
            slot->pins = 0;
            slot->bytes = {};
            release_read_ahead(std::exchange(slot->charged, 0));
            g_free.push_back(index);

            return freed;
        }

        void destroy(const freed_t& freed)
        {
            if (freed.ready && freed.type->unload) { freed.type->unload(freed.data); }
            freed.type->destroy(freed.data);
        }

        bool reload_slot(asset_handle_t handle, u32_t depth)
        {
            // deeper means a cycle of assets using each other
            if (depth > 16) { return false; }

            slot_t* slot = nullptr;
            std::string path;
            asset_state_e before = asset_state_e::NONE;
            {
                std::scoped_lock lock(g_mutex);
                const u32_t index = index_of(handle);
                if (index == NO_SLOT) { return false; }

                slot = slot_at(index);
                before = slot->state.load(std::memory_order_acquire);
                path = slot->path;
                handle.slot = index;
            }

            if (in_flight_state(before))
            {
                TAU_LOG_WARN("ASSET", "{} is still loading, not reloaded", path);
                return false;
            }

            const asset_type_t* type = slot->type;
            void* fresh = type->create();

            const std::vector<u8_t> bytes = read_cooked(type, path);
            load_scope_t scope;
            load_scope_t* outer = std::exchange(t_scope, &scope);
            const bool ok = !bytes.empty() && type->load(fresh, path, bytes);
            t_scope = outer;

            if (!ok)
            {
                TAU_LOG_ERROR("ASSET", "Failed to reload {}, {}", path,
                              before == asset_state_e::READY ? "kept what was loaded" : "it stays missing");
                type->destroy(fresh);
                unpin_all(scope.pinned);
                return false;
            }

            if (before == asset_state_e::READY && type->unload) { type->unload(slot->data); }
            type->move(slot->data, fresh);
            type->destroy(fresh);
            slot->state.store(asset_state_e::READY, std::memory_order_release);
            unpin_all(scope.pinned);

            TAU_LOG_INFO("ASSET", "Reloaded {}", path);

            // users: each type says if refresh in place is enough
            std::vector<std::pair<asset_handle_t, slot_t*>> users;
            {
                std::scoped_lock lock(g_mutex);
                std::vector<asset_handle_t> deps;
                for (u32_t i = 0; i < g_slot_count; i++)
                {
                    slot_t* user = slot_at(i);
                    const asset_guid_t guid = user->guid.load(std::memory_order_relaxed);
                    if (guid == 0 || user == slot || !user->type->deps || !user->type->dep_reloaded ||
                        user->state.load(std::memory_order_acquire) != asset_state_e::READY)
                    {
                        continue;
                    }

                    deps.clear();
                    user->type->deps(user->data, deps);
                    if (std::find(deps.begin(), deps.end(), handle) != deps.end())
                    {
                        users.push_back({
                            {guid, i},
                            user
                        });
                    }
                }
            }

            u32_t refreshed = 0;
            for (const auto& [user_handle, user] : users)
            {
                if (user->type->dep_reloaded(user->data, handle) == asset_dep_e::RELOAD)
                {
                    reload_slot(user_handle, depth + 1);
                }
                else
                {
                    refreshed++;
                }
            }
            if (refreshed > 0) { TAU_LOG_INFO("ASSET", "Refreshed {} asset(s) using {}", refreshed, path); }

            return true;
        }
    } // namespace

    namespace detail
    {
        asset_handle_t load(u64_t type_id, std::string_view path, bool async)
        {
            const asset_type_t* type = type_by_id(type_id);
            if (!type)
            {
                TAU_LOG_ERROR("ASSET", "No asset type {} is registered, cannot load {}", type_id, path);
                return {};
            }
            if (path.empty()) { return {}; }

            slot_t* slot = nullptr;
            asset_handle_t handle;
            bool created = false;
            {
                std::scoped_lock lock(g_mutex);
                const asset_guid_t guid = identity(path, true);

                if (const auto it = g_by_guid.find(guid); it != g_by_guid.end())
                {
                    slot = slot_at(it->second);
                    if (slot->type != type)
                    {
                        TAU_LOG_ERROR("ASSET", "{} is loaded as a {}, not a {}", path, slot->type->display_name,
                                      type->display_name);
                        return {};
                    }
                    handle = {guid, it->second};
                }
                else
                {
                    const u32_t index = take_slot();
                    if (index == NO_SLOT)
                    {
                        TAU_LOG_ERROR("ASSET", "Out of asset slots, cannot load {}", path);
                        return {};
                    }

                    slot = slot_at(index);
                    slot->type = type;
                    slot->data = type->create();
                    slot->path = std::string(path);
                    slot->pins = 0;
                    slot->state.store(asset_state_e::QUEUED, std::memory_order_relaxed);
                    slot->guid.store(guid, std::memory_order_release);

                    g_by_guid.emplace(guid, index);
                    g_in_flight.fetch_add(1, std::memory_order_relaxed);
                    handle = {guid, index};
                    created = true;
                }

                if (t_scope)
                {
                    slot->pins++;
                    t_scope->pinned.push_back(handle);
                }
            }

            if (async)
            {
                if (created)
                {
                    jobs::kick([slot]() { read_stage(slot); }, nullptr, jobs::priority_e::IO);
                }
                return handle;
            }

            finish(slot);
            return handle;
        }

        void* get(u64_t type_id, asset_handle_t handle)
        {
            if (!handle.is_valid()) { return nullptr; }

            slot_t* slot = slot_at(handle.slot);
            if (!slot || slot->guid.load(std::memory_order_acquire) != handle.guid)
            {
                std::scoped_lock lock(g_mutex);
                const auto it = g_by_guid.find(handle.guid);
                if (it == g_by_guid.end()) { return nullptr; }
                slot = slot_at(it->second);
            }

            if (slot->state.load(std::memory_order_acquire) != asset_state_e::READY) { return nullptr; }
            return slot->type->id == type_id ? slot->data : nullptr;
        }
    } // namespace detail

    asset_handle_t load(u64_t type_id, std::string_view path) { return detail::load(type_id, path, false); }

    u32_t in_flight() { return g_in_flight.load(std::memory_order_acquire); }

    asset_state_e state(asset_handle_t handle)
    {
        std::scoped_lock lock(g_mutex);
        const u32_t index = index_of(handle);
        if (index == NO_SLOT) { return asset_state_e::NONE; }

        const asset_state_e state = slot_at(index)->state.load(std::memory_order_acquire);
        return in_flight_state(state) ? (state == asset_state_e::QUEUED ? state : asset_state_e::LOADING) : state;
    }

    u64_t type_of(asset_handle_t handle)
    {
        std::scoped_lock lock(g_mutex);
        const u32_t index = index_of(handle);
        return index != NO_SLOT ? slot_at(index)->type->id : 0;
    }

    std::string path_of(asset_handle_t handle)
    {
        if (!handle.is_valid()) { return {}; }

        std::scoped_lock lock(g_mutex);
        if (const u32_t index = index_of(handle); index != NO_SLOT) { return slot_at(index)->path; }

        const auto it = g_path_by_guid.find(handle.guid);
        return it != g_path_by_guid.end() ? it->second : std::string{};
    }

    asset_handle_t find(asset_guid_t guid)
    {
        std::scoped_lock lock(g_mutex);
        const auto it = g_by_guid.find(guid);
        return it != g_by_guid.end() ? asset_handle_t{guid, it->second} : asset_handle_t{};
    }

    asset_handle_t find(std::string_view path)
    {
        std::scoped_lock lock(g_mutex);

        // loaded before getting a guid: known by path (late textures)
        for (const asset_guid_t guid : {mapped_guid(path), hash_string64(path)})
        {
            if (const auto it = g_by_guid.find(guid); guid != 0 && it != g_by_guid.end()) { return {guid, it->second}; }
        }
        return {};
    }

    asset_handle_t pin(asset_handle_t handle)
    {
        std::scoped_lock lock(g_mutex);
        const u32_t index = index_of(handle);
        if (index == NO_SLOT) { return handle; }

        slot_at(index)->pins++;
        return {handle.guid, index};
    }

    void unpin(asset_handle_t handle)
    {
        std::scoped_lock lock(g_mutex);
        const u32_t index = index_of(handle);
        if (index == NO_SLOT) { return; }

        slot_t* slot = slot_at(index);
        if (slot->pins == 0)
        {
            TAU_LOG_WARN("ASSET", "{} was unpinned more often than pinned", slot->path);
            return;
        }
        slot->pins--;
    }

    void collect()
    {
        std::vector<roots_fn_t> root_fns;
        {
            std::scoped_lock lock(g_mutex);
            root_fns = g_roots;
        }

        std::vector<asset_handle_t> roots;
        for (roots_fn_t fn : root_fns) { fn(roots); }

        std::vector<freed_t> freed;
        {
            std::scoped_lock lock(g_mutex);

            std::vector<u32_t> work;
            const auto mark = [&work](u32_t index)
            {
                if (index == NO_SLOT) { return; }

                slot_t* slot = slot_at(index);
                if (slot->marked) { return; }

                slot->marked = true;
                work.push_back(index);
            };

            for (u32_t i = 0; i < g_slot_count; i++) { slot_at(i)->marked = false; }

            for (u32_t i = 0; i < g_slot_count; i++)
            {
                const slot_t* slot = slot_at(i);
                if (slot->guid.load(std::memory_order_relaxed) == 0) { continue; }

                const asset_state_e state = slot->state.load(std::memory_order_acquire);
                if (slot->pins > 0 || in_flight_state(state)) { mark(i); }
            }

            for (const asset_handle_t& root : roots) { mark(index_of(root)); }

            std::vector<asset_handle_t> deps;
            while (!work.empty())
            {
                const slot_t* slot = slot_at(work.back());
                work.pop_back();

                if (!slot->type->deps || slot->state.load(std::memory_order_acquire) != asset_state_e::READY)
                {
                    continue;
                }

                deps.clear();
                slot->type->deps(slot->data, deps);
                for (const asset_handle_t& dep : deps) { mark(index_of(dep)); }
            }

            for (u32_t i = 0; i < g_slot_count; i++)
            {
                const slot_t* slot = slot_at(i);
                if (slot->guid.load(std::memory_order_relaxed) == 0 || slot->marked) { continue; }

                freed.push_back(free_slot(i));
            }
        }

        for (const freed_t& f : freed) { destroy(f); }

        if (!freed.empty()) { TAU_LOG_INFO("ASSET", "Unloaded {} unused asset(s)", freed.size()); }
    }

    void add_roots(roots_fn_t fn)
    {
        std::scoped_lock lock(g_mutex);
        if (std::find(g_roots.begin(), g_roots.end(), fn) == g_roots.end()) { g_roots.push_back(fn); }
    }

    bool reload(std::string_view path)
    {
        const asset_handle_t handle = find(path);
        return handle.is_valid() && reload_slot(handle, 0);
    }

    bool reload(asset_handle_t handle) { return reload_slot(handle, 0); }

    void shutdown()
    {
        if (g_in_flight.load(std::memory_order_acquire) > 0) { jobs::wait_idle(5000); }

        std::vector<freed_t> freed;
        {
            std::scoped_lock lock(g_mutex);
            for (u32_t i = 0; i < g_slot_count; i++)
            {
                if (slot_at(i)->guid.load(std::memory_order_relaxed) != 0) { freed.push_back(free_slot(i)); }
            }
        }

        for (const freed_t& f : freed) { destroy(f); }

        std::scoped_lock lock(g_mutex);
        for (std::atomic<chunk_t*>& chunk : g_chunks) { delete chunk.exchange(nullptr); }
        g_slot_count = 0;
        g_free.clear();
        g_by_guid.clear();
        g_guid_by_path.clear();
        g_path_by_guid.clear();
        g_warned.clear();
    }

    void read_guid_map(const std::string& path)
    {
        const std::string text = vfs::read_text(path);
        if (text.empty())
        {
            TAU_LOG_INFO("ASSET", "No guid map at {}", path);
            return;
        }

        merge_guid_map(text);
        TAU_LOG_INFO("ASSET", "Read the guid map {}", path);
    }

    void merge_guid_map(const std::string& json_text)
    {
        const nlohmann::json data = nlohmann::json::parse(json_text, nullptr, false);
        if (data.is_discarded() || !data.is_object())
        {
            TAU_LOG_ERROR("ASSET", "The guid map is not a JSON object");
            return;
        }

        std::scoped_lock lock(g_mutex);
        for (const auto& [path, value] : data.items())
        {
            const asset_guid_t guid = value.is_string() ? parse_guid(value.get<std::string>()) : 0;
            if (guid != 0) { set_location_locked(path, guid); }
        }
    }

    asset_guid_t guid_of(std::string_view path)
    {
        std::scoped_lock lock(g_mutex);
        return mapped_guid(path);
    }

    std::string resolve(asset_guid_t guid, std::string_view recorded_path)
    {
        if (guid == 0) { return std::string(recorded_path); }

        std::scoped_lock lock(g_mutex);
        if (const auto it = g_path_by_guid.find(guid); it != g_path_by_guid.end()) { return it->second; }

        // deleted or stale map: only the recorded path
        if (g_warned.insert(guid_text(guid)).second)
        {
            TAU_LOG_WARN("ASSET", "Guid {} is not in the map, trying its recorded path '{}'", guid_text(guid),
                         recorded_path);
        }
        return std::string(recorded_path);
    }

    void set_location(std::string_view path, asset_guid_t guid)
    {
        if (guid == 0) { return; }

        std::scoped_lock lock(g_mutex);
        set_location_locked(std::string(path), guid);

        // moved asset stays loaded, reload reads the new path
        if (const auto it = g_by_guid.find(guid); it != g_by_guid.end()) { slot_at(it->second)->path = path; }
    }

    void remove_location(std::string_view path)
    {
        std::scoped_lock lock(g_mutex);
        const auto it = g_guid_by_path.find(std::string(path));
        if (it == g_guid_by_path.end()) { return; }

        if (const auto rev = g_path_by_guid.find(it->second); rev != g_path_by_guid.end() && rev->second == path)
        {
            g_path_by_guid.erase(rev);
        }
        g_guid_by_path.erase(it);
    }

    std::vector<std::pair<std::string, asset_guid_t>> locations(std::string_view prefix)
    {
        std::vector<std::pair<std::string, asset_guid_t>> out;
        {
            std::scoped_lock lock(g_mutex);
            for (const auto& [path, guid] : g_guid_by_path)
            {
                if (std::string_view(path).starts_with(prefix)) { out.emplace_back(path, guid); }
            }
        }

        std::sort(out.begin(), out.end());
        return out;
    }

    asset_guid_t parse_guid(std::string_view text)
    {
        if (const u64_t guid = guid::from_string(text)) { return guid; }

        // legacy uuid text hashed at runtime (this is a horrible fallback, will remove later): same asset
        if (text.size() == 36 && text[8] == '-' && text[13] == '-' && text[18] == '-' && text[23] == '-')
        {
            return hash_string64(text);
        }
        return 0;
    }

    std::string guid_text(asset_guid_t guid) { return guid != 0 ? guid::to_string(guid) : std::string{}; }

    void add_type(const asset_type_t& type)
    {
        for (u32_t i = 0; i < g_type_count; i++)
        {
            if (g_types[i].id == type.id)
            {
                g_types[i] = type;
                return;
            }
        }

        if (g_type_count == MAX_TYPES)
        {
            TAU_LOG_ERROR("ASSET", "Too many asset types, '{}' is not registered", type.key);
            return;
        }

        g_types[g_type_count++] = type;
    }

    std::span<const asset_type_t> types() { return {g_types, g_type_count}; }

    const asset_type_t* type_by_id(u64_t id)
    {
        for (u32_t i = 0; i < g_type_count; i++)
        {
            if (g_types[i].id == id) { return &g_types[i]; }
        }
        return nullptr;
    }

    const asset_type_t* type_by_key(std::string_view key)
    {
        for (u32_t i = 0; i < g_type_count; i++)
        {
            if (g_types[i].key == key) { return &g_types[i]; }
        }
        return nullptr;
    }

    const asset_type_t* type_by_path(std::string_view path_or_ext)
    {
        const std::string ext = extension_of(path_or_ext);
        if (ext.empty()) { return nullptr; }

        for (u32_t i = 0; i < g_type_count; i++)
        {
            const std::vector<std::string_view>& sources = g_types[i].source_extensions;
            if (std::find(sources.begin(), sources.end(), ext) != sources.end()) { return &g_types[i]; }
        }
        for (u32_t i = 0; i < g_type_count; i++)
        {
            if (!g_types[i].sub_extension.empty() && g_types[i].sub_extension == ext) { return &g_types[i]; }
        }
        return nullptr;
    }

    bool path_is(std::string_view path, u64_t type_id)
    {
        const asset_type_t* type = type_by_path(path);
        return type && type->id == type_id;
    }

    std::string cooked_path(u64_t type_id, std::string_view source_path)
    {
        const asset_type_t* type = type_by_id(type_id);
        if (!type) { return std::string(source_path); }

        const vfs::path_parts_t parts = vfs::split_protocol(source_path);

        std::filesystem::path cooked(parts.rest);
        cooked.replace_extension(std::string(type->cooked_extension));

        return std::string(parts.protocol) + cooked.generic_string();
    }

    std::optional<sub_asset_path_t> split_sub_asset(std::string_view path)
    {
        for (std::size_t slash = path.find('/'); slash != std::string_view::npos; slash = path.find('/', slash + 1))
        {
            const std::string_view head = path.substr(0, slash);
            const std::string_view name = path.substr(slash + 1);
            if (name.empty() || name.find('/') != std::string_view::npos) { continue; }

            const asset_type_t* type = type_by_path(head);
            if (type && type->has_sub_assets) { return sub_asset_path_t{head, name}; }
        }
        return std::nullopt;
    }
} // namespace tau::assets
