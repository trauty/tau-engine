#include "history.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "tau-editor/asset_files.h"
#include "tau-editor/asset_watch.h"
#include "tau-editor/editor_context.h"
#include "tau-editor/keymap.h"
#include "tau/asset.h"
#include "tau/hash.h"
#include "tau/log.h"
#include "tau/serialization.h"
#include "tau/systems/guid.h"
#include "tau/world.h"

#include <algorithm>
#include <json/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>

namespace tau::editor::history
{
    namespace
    {
        constexpr std::size_t MAX_STEPS = 256;

        struct editable_t
        {
            capture_fn_t capture = nullptr;
            apply_fn_t apply = nullptr;
            write_fn_t write = nullptr;
        };

        struct change_t
        {
            target_t target;
            std::vector<u8_t> before;
            std::vector<u8_t> after;
            u64_t prev_state = 0;
        };

        struct step_t
        {
            std::string label;
            u64_t serial = 0;
            std::vector<change_t> changes;
            selection_t selection_before;
            selection_t selection_after;
        };

        struct doc_entry_t
        {
            doc_t doc;
            u64_t asset_type = 0;
            u64_t current = 0;
            u64_t saved = 0;
            // asset with steps stays loaded
            bool pinned = false;
        };

        struct pending_t
        {
            target_t target;
            std::vector<u8_t> state;
        };

        editor_context_t* g_ctx = nullptr;
        std::unordered_map<u64_t, editable_t> editables;
        std::vector<doc_entry_t> docs;

        std::vector<step_t> steps;
        std::size_t cursor = 0;
        u64_t next_serial = 1;

        doc_t g_entity_doc = OPEN_SCENE;
        std::optional<std::size_t> g_floor;

        std::optional<step_t> open;
        bool open_in_place = false;
        std::vector<pending_t> pending;

        bool enabled() { return g_ctx && g_ctx->cur_mode == editor_mode_e::EDIT; }

        doc_entry_t* find_doc(doc_t doc)
        {
            const auto it =
                std::find_if(docs.begin(), docs.end(), [doc](const doc_entry_t& e) { return e.doc == doc; });
            return it != docs.end() ? &*it : nullptr;
        }

        doc_entry_t& doc_entry(doc_t doc)
        {
            if (doc_entry_t* entry = find_doc(doc)) { return *entry; }
            return docs.emplace_back(doc_entry_t{.doc = doc});
        }

        asset_handle_t live_handle(u64_t type_id, asset_guid_t guid)
        {
            const asset_handle_t handle = assets::find(guid);
            return handle.is_valid() && assets::type_of(handle) == type_id ? handle : asset_handle_t{};
        }

        void hold(doc_entry_t& entry)
        {
            if (entry.doc.kind != doc_kind_e::ASSET || entry.pinned) { return; }

            assets::pin(asset_handle_t{entry.doc.id});
            entry.pinned = true;
        }

        void let_go(doc_entry_t& entry)
        {
            if (!entry.pinned) { return; }

            assets::unpin(asset_handle_t{entry.doc.id});
            entry.pinned = false;
        }

        const editable_t* editable_of(doc_t doc, asset_handle_t& out_handle)
        {
            const doc_entry_t* entry = find_doc(doc);
            if (!entry) { return nullptr; }

            const auto it = editables.find(entry->asset_type);
            if (it == editables.end()) { return nullptr; }

            out_handle = live_handle(entry->asset_type, doc.id);
            return out_handle.is_valid() ? &it->second : nullptr;
        }

        std::vector<u8_t> capture(world_t& world, target_t target)
        {
            switch (target.doc.kind)
            {
            case doc_kind_e::SCENE:
            case doc_kind_e::STAGE:
            {
                const ecs::entity_t entity = guid_system::find(world.registry, target.key);
                if (entity == ecs::NULL_ENTITY) { return {}; }
                return nlohmann::json::to_cbor(serialization::serialize_entity(world, entity));
            }
            case doc_kind_e::ASSET:
            {
                asset_handle_t handle;
                const editable_t* editable = editable_of(target.doc, handle);
                return editable ? editable->capture(handle) : std::vector<u8_t>{};
            }
            case doc_kind_e::FILES: return asset_files::capture(target.key);
            }
            return {};
        }

        void apply(world_t& world, const std::vector<change_t>& changes, bool use_before)
        {
            std::vector<nlohmann::json> parsed;
            parsed.reserve(changes.size());
            std::vector<serialization::entity_state_t> states;

            for (const change_t& c : changes)
            {
                const std::vector<u8_t>& state = use_before ? c.before : c.after;

                switch (c.target.doc.kind)
                {
                case doc_kind_e::SCENE:
                case doc_kind_e::STAGE:
                    if (state.empty()) { states.push_back({c.target.key, nullptr}); }
                    else
                    {
                        parsed.push_back(nlohmann::json::from_cbor(state));
                        states.push_back({c.target.key, &parsed.back()});
                    }
                    break;
                case doc_kind_e::ASSET:
                {
                    asset_handle_t handle;
                    const editable_t* editable = editable_of(c.target.doc, handle);
                    if (!editable || state.empty() || !editable->apply(handle, state))
                    {
                        TAU_LOG_WARN("HISTORY", "Could not put back an asset, it is no longer loaded");
                    }
                    break;
                }
                case doc_kind_e::FILES: asset_files::apply(world, c.target.key, state); break;
                }
            }

            if (!states.empty()) { serialization::apply_entities(world, states); }
        }

        void set_state(doc_t doc, u64_t state)
        {
            doc_entry_t& entry = doc_entry(doc);
            entry.current = state;
            if (doc.kind == doc_kind_e::FILES) { entry.saved = state; }
        }

        bool contains(const step_t& step, target_t target)
        {
            return std::any_of(step.changes.begin(), step.changes.end(),
                               [target](const change_t& c) { return c.target == target; });
        }

        void commit(world_t& world)
        {
            step_t step = std::move(*open);
            open.reset();
            open_in_place = false;

            for (change_t& c : step.changes) { c.after = capture(world, c.target); }
            std::erase_if(step.changes, [](const change_t& c) { return c.before == c.after; });
            if (step.changes.empty()) { return; }

            step.serial = next_serial++;
            step.selection_after = g_ctx->selection;

            for (change_t& c : step.changes) { c.prev_state = doc_entry(c.target.doc).current; }
            for (change_t& c : step.changes)
            {
                set_state(c.target.doc, step.serial);
                hold(doc_entry(c.target.doc));
            }

            steps.resize(cursor);
            steps.push_back(std::move(step));
            if (steps.size() > MAX_STEPS)
            {
                steps.erase(steps.begin());
                if (g_floor && *g_floor > 0) { --*g_floor; }
            }
            cursor = steps.size();
        }

        bool input_this_frame()
        {
            const ImGuiIO& io = ImGui::GetIO();
            for (i32 button = 0; button < ImGuiMouseButton_COUNT; button++)
            {
                if (io.MouseClicked[button] || io.MouseReleased[button]) { return true; }
            }
            if (io.InputQueueCharacters.Size > 0) { return true; }

            for (i32 key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key++)
            {
                if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), true)) { return true; }
            }
            return false;
        }

    } // namespace

    target_t entity_target(u64_t guid) { return {g_entity_doc, guid}; }

    void set_entity_doc(doc_t doc) { g_entity_doc = doc; }
    doc_t entity_doc() { return g_entity_doc; }

    void set_floor(bool enabled)
    {
        if (enabled) { g_floor = cursor; }
        else
        {
            g_floor.reset();
        }
    }

    std::size_t floor() { return g_floor.value_or(0); }

    u64_t doc_state(doc_t doc)
    {
        const doc_entry_t* entry = find_doc(doc);
        return entry ? entry->current : 0;
    }

    void reg_editable(u64_t type_id, capture_fn_t capture, apply_fn_t apply, write_fn_t write)
    { editables[type_id] = {capture, apply, write}; }

    doc_t asset_doc(const std::string& vfs_path) { return {doc_kind_e::ASSET, assets::guid_of(vfs_path)}; }

    target_t asset_target(u64_t type_id, asset_handle_t handle)
    {
        const doc_t doc = {doc_kind_e::ASSET, handle.guid};
        doc_entry(doc).asset_type = type_id;
        return {doc, 0};
    }

    void init(editor_context_t& ctx) { g_ctx = &ctx; }

    void clear()
    {
        steps.clear();
        cursor = 0;
        g_entity_doc = OPEN_SCENE;
        g_floor.reset();
        for (doc_entry_t& entry : docs) { let_go(entry); }
        docs.clear();
        open.reset();
        open_in_place = false;
        pending.clear();
    }

    void begin(world_t& world, std::string_view label, std::span<const target_t> targets)
    {
        if (!enabled()) { return; }
        if (open) { commit(world); }

        open = step_t{.label = std::string(label), .selection_before = g_ctx->selection};
        open_in_place = false;

        for (const target_t& target : targets)
        {
            if (!contains(*open, target)) { open->changes.push_back({target, capture(world, target)}); }
        }
    }

    void end(world_t& world)
    {
        if (open && !open_in_place) { commit(world); }
    }

    void cancel(world_t& world)
    {
        if (!open) { return; }

        apply(world, open->changes, true);
        g_ctx->selection = open->selection_before;
        open.reset();
        open_in_place = false;
    }

    void watch(world_t& world, target_t target)
    {
        if (!enabled() || (open && contains(*open, target)) || !input_this_frame()) { return; }

        std::vector<u8_t> state = capture(world, target);
        const auto it =
            std::find_if(pending.begin(), pending.end(), [target](const pending_t& p) { return p.target == target; });
        if (it != pending.end()) { it->state = std::move(state); }
        else
        {
            pending.push_back({target, std::move(state)});
        }
    }

    void edited(world_t&, target_t target, std::string_view label)
    {
        if (!enabled() || (open && !open_in_place)) { return; }

        if (!open)
        {
            open = step_t{.label = std::string(label), .selection_before = g_ctx->selection};
            open_in_place = true;
        }
        if (contains(*open, target)) { return; }

        const auto it =
            std::find_if(pending.begin(), pending.end(), [target](const pending_t& p) { return p.target == target; });
        if (it == pending.end())
        {
            TAU_LOG_WARN("HISTORY", "'{}' changed without a watch before it, it cannot be undone", label);
            return;
        }

        open->changes.push_back({target, std::move(it->state)});
        pending.erase(it);
    }

    void update(world_t& world, editor_context_t& ctx)
    {
        if (!enabled())
        {
            open.reset();
            open_in_place = false;
            pending.clear();
            return;
        }

        const bool interacting = ImGui::IsAnyItemActive();
        if (open && open_in_place && !interacting) { commit(world); }
        if (!interacting) { pending.clear(); }

        if (ImGui::GetIO().WantTextInput || open || ctx.handle_dragging) { return; }

        if (keymap::pressed(keymap::action_e::UNDO, true)) { undo(world, ctx); }
        else if (keymap::pressed(keymap::action_e::REDO, true)) { redo(world, ctx); }
    }

    namespace
    {
        // saved even if the cook fails
        bool save_docs(std::span<const doc_t> docs)
        {
            bool ok = true;
            std::vector<std::filesystem::path> sources;

            for (const doc_t doc : docs)
            {
                asset_handle_t handle;
                const editable_t* editable = editable_of(doc, handle);
                std::filesystem::path source;
                if (!editable || !editable->write || !editable->write(handle, source))
                {
                    ok = false;
                    continue;
                }

                sources.push_back(source);
                mark_saved(doc);
            }

            asset_watch::cook_saved(sources);
            return ok;
        }
    } // namespace

    bool can_undo() { return cursor > floor(); }
    bool can_redo() { return cursor < steps.size(); }
    std::string_view undo_label()
    { return can_undo() ? std::string_view(steps[cursor - 1].label) : std::string_view{}; }
    std::string_view redo_label() { return can_redo() ? std::string_view(steps[cursor].label) : std::string_view{}; }

    void undo(world_t& world, editor_context_t& ctx)
    {
        if (!enabled() || open || !can_undo()) { return; }

        const step_t& step = steps[--cursor];
        apply(world, step.changes, true);
        for (const change_t& c : step.changes) { set_state(c.target.doc, c.prev_state); }
        ctx.selection = step.selection_before;
    }

    void redo(world_t& world, editor_context_t& ctx)
    {
        if (!enabled() || open || !can_redo()) { return; }

        const step_t& step = steps[cursor++];
        apply(world, step.changes, false);
        for (const change_t& c : step.changes) { set_state(c.target.doc, step.serial); }
        ctx.selection = step.selection_after;
    }

    std::size_t step_count() { return steps.size(); }
    std::size_t applied_count() { return cursor; }
    std::string_view step_label(std::size_t index)
    {
        if (index >= steps.size()) { return {}; }
        return steps[index].label;
    }

    void go_to(world_t& world, editor_context_t& ctx, std::size_t applied)
    {
        // stops when a step refuses (for example mid drag)
        while (cursor > applied)
        {
            const std::size_t before = cursor;
            undo(world, ctx);
            if (cursor == before) { return; }
        }
        while (cursor < applied && cursor < steps.size())
        {
            const std::size_t before = cursor;
            redo(world, ctx);
            if (cursor == before) { return; }
        }
    }

    bool is_dirty(doc_t doc)
    {
        const doc_entry_t* entry = find_doc(doc);
        return entry && entry->current != entry->saved;
    }

    bool any_dirty()
    {
        return std::any_of(docs.begin(), docs.end(), [](const doc_entry_t& e) { return e.current != e.saved; });
    }

    std::vector<doc_t> dirty_docs()
    {
        std::vector<doc_t> out;
        for (const doc_entry_t& e : docs)
        {
            if (e.current != e.saved) { out.push_back(e.doc); }
        }
        return out;
    }

    std::string asset_path(doc_t doc)
    {
        const doc_entry_t* entry = find_doc(doc);
        if (!entry || doc.kind != doc_kind_e::ASSET) { return {}; }
        return assets::path_of(asset_handle_t{doc.id});
    }

    void mark_saved(doc_t doc)
    {
        doc_entry_t& entry = doc_entry(doc);
        entry.saved = entry.current;
    }

    bool save_asset(doc_t doc) { return save_docs({&doc, 1}); }

    bool save_assets()
    {
        std::vector<doc_t> docs = dirty_docs();
        std::erase_if(docs, [](doc_t doc) { return doc.kind != doc_kind_e::ASSET; });
        return save_docs(docs);
    }

    bool revert_asset(doc_t doc)
    {
        const doc_entry_t* entry = find_doc(doc);
        asset_handle_t handle;
        if (!entry || !editable_of(doc, handle)) { return false; }

        asset_watch::flush();
        const std::string path = assets::path_of(handle);
        if (!assets::reload(handle))
        {
            TAU_LOG_ERROR("HISTORY", "Could not revert {}", path);
            return false;
        }

        drop(doc);
        TAU_LOG_INFO("HISTORY", "Reverted {}", path);
        return true;
    }

    void drop(doc_t doc)
    {
        std::size_t removed_before_cursor = 0;
        std::size_t removed_before_floor = 0;
        for (std::size_t i = 0; i < steps.size(); i++)
        {
            std::erase_if(steps[i].changes, [doc](const change_t& c) { return c.target.doc == doc; });
            if (steps[i].changes.empty() && i < cursor) { removed_before_cursor++; }
            if (steps[i].changes.empty() && g_floor && i < *g_floor) { removed_before_floor++; }
        }
        std::erase_if(steps, [](const step_t& s) { return s.changes.empty(); });
        cursor -= removed_before_cursor;
        if (g_floor) { *g_floor -= removed_before_floor; }

        if (open)
        {
            std::erase_if(open->changes, [doc](const change_t& c) { return c.target.doc == doc; });
        }
        std::erase_if(pending, [doc](const pending_t& p) { return p.target.doc == doc; });

        if (doc_entry_t* entry = find_doc(doc))
        {
            entry->current = entry->saved = next_serial++;
            let_go(*entry);
        }
    }

    std::vector<held_t> hold_unsaved_assets(world_t& world)
    {
        std::vector<held_t> held;
        for (const doc_t doc : dirty_docs())
        {
            if (doc.kind != doc_kind_e::ASSET) { continue; }

            const target_t target = {doc, 0};
            held.push_back({target, capture(world, target)});
        }
        return held;
    }

    void restore_unsaved_assets(world_t& world, const std::vector<held_t>& held)
    {
        for (const held_t& h : held)
        {
            if (h.state.empty()) { continue; }

            apply(world,
                  {
                      change_t{.target = h.target, .after = h.state}
            },
                  false);
        }
    }
} // namespace tau::editor::history
