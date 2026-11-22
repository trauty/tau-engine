#include "hierarchy.h"

#include "imgui.h"
#include "tau-editor/asset_editors.h"
#include "tau-editor/commands.h"
#include "tau-editor/history.h"
#include "tau-editor/keymap.h"
#include "tau-editor/panels/asset_browser.h"
#include "tau-editor/prefab_edit.h"
#include "tau-editor/prefab_stage.h"
#include "tau/asset.h"
#include "tau/assets/prefab.h"
#include "tau/components/prefab.h"
#include "tau/components/tag.h"
#include "tau/components/transform.h"
#include "tau/ecs_fwd.h"
#include "tau/entity.h"
#include "tau/guid.h"
#include "tau/serialization.h"
#include "tau/systems/guid.h"
#include "tau/systems/transform.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tau::editor::panels
{
    namespace
    {

        enum class drop_e : u8_t
        {
            BEFORE,
            INTO,
            AFTER,
        };

        enum class action_kind_e : u8_t
        {
            NONE,
            CREATE,
            REMOVE,
            DUPLICATE,
            RENAME,
        };

        struct action_t
        {
            action_kind_e kind = action_kind_e::NONE;
            ecs::entity_t entity = ecs::NULL_ENTITY;
        };

        struct drop_t
        {
            std::vector<ecs::entity_t> moved;
            ecs::entity_t parent = ecs::NULL_ENTITY;
            ecs::entity_t before = ecs::NULL_ENTITY;
        };

        struct state_t
        {
            std::vector<u64_t> rows;
            std::vector<u64_t> last_rows;
            u64_t anchor = 0;

            // press on a selected row selects only it on release, unless it became a drag
            u64_t pending_click = 0;
            bool dragged = false;

            u64_t renaming = 0;
            bool rename_focus = false;
            char rename_buf[256] = {};

            std::unordered_set<u64_t> reveal;

            // applied after drawing, the tree must not change mid loop
            action_t action;
            std::optional<drop_t> drop;
            std::optional<std::pair<std::string, ecs::entity_t>> asset_drop;
        };

        state_t g;

        std::string display_name(const world_t& world, ecs::entity_t entity)
        {
            const tag_t* tag = world.registry.try_get<tag_t>(entity);
            return tag ? tag->name : "Entity " + std::to_string(ecs::get_entity_id(entity));
        }

        ecs::entity_t parent_of(const world_t& world, ecs::entity_t entity)
        {
            const transform_t* transform = world.registry.try_get<transform_t>(entity);
            return transform ? transform->get_parent() : ecs::NULL_ENTITY;
        }

        bool is_within(const world_t& world, ecs::entity_t entity, ecs::entity_t ancestor)
        {
            for (ecs::entity_t e = entity; e != ecs::NULL_ENTITY; e = parent_of(world, e))
            {
                if (e == ancestor) { return true; }
            }
            return false;
        }

        void append_subtree(const world_t& world, ecs::entity_t root, std::vector<ecs::entity_t>& out)
        {
            out.push_back(root);
            const transform_t* transform = world.registry.try_get<transform_t>(root);
            if (!transform) { return; }

            for (ecs::entity_t child = transform->get_first_child(); child != ecs::NULL_ENTITY;
                 child = world.registry.get<transform_t>(child).get_next_sibling())
            {
                append_subtree(world, child, out);
            }
        }

        std::vector<ecs::entity_t> tree_order(const world_t& world)
        {
            std::vector<ecs::entity_t> order;
            for (ecs::entity_t root = transform_system::first_root(world.registry); root != ecs::NULL_ENTITY;
                 root = world.registry.get<transform_t>(root).get_next_sibling())
            {
                append_subtree(world, root, order);
            }
            return order;
        }

        std::vector<ecs::entity_t> selected_roots(world_t& world, const editor_context_t& ctx)
        {
            std::vector<ecs::entity_t> roots;
            auto add = [&](ecs::entity_t entity)
            {
                if (!ctx.selection.contains(guid_system::of(world.registry, entity))) { return; }
                for (ecs::entity_t p = parent_of(world, entity); p != ecs::NULL_ENTITY; p = parent_of(world, p))
                {
                    if (ctx.selection.contains(guid_system::of(world.registry, p))) { return; }
                }
                roots.push_back(entity);
            };

            for (const ecs::entity_t entity : tree_order(world)) { add(entity); }
            for (const ecs::entity_t entity : world.registry.view<ecs::entity_t>())
            {
                if (!world.registry.all_of<transform_t>(entity)) { add(entity); }
            }

            std::erase(roots, prefab_stage::root(world));
            return roots;
        }

        std::string count_label(const char* verb, const world_t& world, const std::vector<ecs::entity_t>& entities)
        {
            if (entities.size() == 1) { return std::string(verb) + " " + display_name(world, entities.front()); }
            return std::string(verb) + " " + std::to_string(entities.size()) + " Entities";
        }

        void create_entity(world_t& world, editor_context_t& ctx, ecs::entity_t parent)
        {
            if (parent == ecs::NULL_ENTITY) { parent = prefab_stage::root(world); }

            const u64_t guid = guid::generate();
            const history::target_t target = history::entity_target(guid);
            history::begin(world, "Create Entity", {&target, 1});

            const entity_ref_t entity = entity::create(world, "New Entity", guid);
            if (entity.other(parent).has<transform_t>())
            {
                entity.set_parent(entity.other(parent));
                g.reveal.insert(guid_system::of(world.registry, parent));
            }
            ctx.selection.set(guid);
            g.anchor = guid;

            history::end(world);
        }

        // children go along, referrers are in the step for undo
        void delete_selection(world_t& world, editor_context_t& ctx)
        {
            const std::vector<ecs::entity_t> roots = selected_roots(world, ctx);
            if (roots.empty()) { return; }

            std::vector<ecs::entity_t> doomed;
            for (const ecs::entity_t root : roots) { append_subtree(world, root, doomed); }

            std::vector<history::target_t> targets;
            for (const ecs::entity_t entity : doomed)
            {
                targets.push_back(history::entity_target(guid_system::ensure(world.registry, entity)));
            }
            for (const ecs::entity_t entity : serialization::find_referrers(world, doomed))
            {
                targets.push_back(history::entity_target(guid_system::ensure(world.registry, entity)));
            }

            history::begin(world, count_label("Delete", world, roots), targets);

            // children first, a parent takes them only a frame later
            for (auto it = doomed.rbegin(); it != doomed.rend(); ++it) { world.registry.destroy(*it); }
            ctx.selection.clear();

            history::end(world);
        }

        void duplicate_selection(world_t& world, editor_context_t& ctx)
        {
            const std::vector<ecs::entity_t> roots = selected_roots(world, ctx);
            if (roots.empty()) { return; }

            std::vector<ecs::entity_t> entities;
            for (const ecs::entity_t root : roots) { append_subtree(world, root, entities); }

            std::unordered_map<u64_t, u64_t> new_guids;
            std::vector<history::target_t> targets;
            for (const ecs::entity_t entity : entities)
            {
                const u64_t copy = guid::generate();
                new_guids.emplace(guid_system::ensure(world.registry, entity), copy);
                targets.push_back(history::entity_target(copy));
            }

            history::begin(world, count_label("Duplicate", world, roots), targets);
            serialization::duplicate_entities(world, entities, new_guids);

            ctx.selection.clear();
            for (const ecs::entity_t root : roots)
            {
                ctx.selection.guids.push_back(new_guids.at(guid_system::of(world.registry, root)));
            }
            g.anchor = ctx.selection.primary();

            history::end(world);
        }

        void start_rename(world_t& world, ecs::entity_t entity)
        {
            const std::string name = display_name(world, entity);
            std::snprintf(g.rename_buf, sizeof(g.rename_buf), "%s", name.c_str());

            g.renaming = guid_system::ensure(world.registry, entity);
            g.rename_focus = true;
            for (ecs::entity_t p = parent_of(world, entity); p != ecs::NULL_ENTITY; p = parent_of(world, p))
            {
                g.reveal.insert(guid_system::of(world.registry, p));
            }
        }

        void finish_rename(world_t& world, bool apply)
        {
            const ecs::entity_t entity = guid_system::find(world.registry, g.renaming);
            g.renaming = 0;
            if (!apply || entity == ecs::NULL_ENTITY || display_name(world, entity) == g.rename_buf) { return; }

            const history::target_t target = history::entity_target(guid_system::of(world.registry, entity));
            history::begin(world, "Rename", {&target, 1});
            world.registry.emplace_or_replace<tag_t>(entity).name = g.rename_buf;
            history::end(world);
        }

        std::vector<ecs::entity_t> dragged_entities(world_t& world, const editor_context_t& ctx, u64_t guid)
        {
            std::vector<ecs::entity_t> moved;
            if (ctx.selection.contains(guid)) { moved = selected_roots(world, ctx); }
            else if (const ecs::entity_t entity = guid_system::find(world.registry, guid); entity != ecs::NULL_ENTITY)
            {
                moved.push_back(entity);
            }

            // only an instance root moves, parts follow the prefab
            std::erase_if(moved,
                          [&](ecs::entity_t e)
                          {
                              const ecs::entity_t root = prefab_edit::instance_root(world, e);
                              return !world.registry.all_of<transform_t>(e) || (root != ecs::NULL_ENTITY && root != e);
                          });
            std::erase(moved, prefab_stage::root(world));
            return moved;
        }

        bool can_move_under(const world_t& world, const std::vector<ecs::entity_t>& moved, ecs::entity_t parent)
        {
            if (moved.empty()) { return false; }
            if (parent == ecs::NULL_ENTITY && prefab_stage::is_open()) { return false; }
            return std::none_of(moved.begin(), moved.end(), [&](ecs::entity_t m)
                                { return parent != ecs::NULL_ENTITY && is_within(world, parent, m); });
        }

        void move_entities(world_t& world, const drop_t& drop)
        {
            std::vector<history::target_t> targets;
            for (const ecs::entity_t entity : drop.moved)
            {
                targets.push_back(history::entity_target(guid_system::of(world.registry, entity)));
            }

            const bool same_parent = std::all_of(drop.moved.begin(), drop.moved.end(),
                                                 [&](ecs::entity_t e) { return parent_of(world, e) == drop.parent; });
            history::begin(world, count_label(same_parent ? "Reorder" : "Reparent", world, drop.moved), targets);
            for (const ecs::entity_t entity : drop.moved)
            {
                transform_system::set_parent_keep_world(world.registry, entity, drop.parent, drop.before);
            }
            history::end(world);

            if (drop.parent != ecs::NULL_ENTITY) { g.reveal.insert(guid_system::of(world.registry, drop.parent)); }
        }

        void on_press(editor_context_t& ctx, u64_t guid, bool selected)
        {
            const ImGuiIO& io = ImGui::GetIO();
            g.dragged = false;

            if (io.KeyShift && g.anchor != 0)
            {
                const auto a = std::find(g.last_rows.begin(), g.last_rows.end(), g.anchor);
                const auto b = std::find(g.last_rows.begin(), g.last_rows.end(), guid);
                if (a != g.last_rows.end() && b != g.last_rows.end())
                {
                    ctx.selection.clear();
                    for (auto it = std::min(a, b); it <= std::max(a, b); ++it)
                    {
                        if (*it != guid) { ctx.selection.guids.push_back(*it); }
                    }
                    ctx.selection.guids.push_back(guid);
                    return;
                }
            }

            if (io.KeyCtrl) { ctx.selection.toggle(guid); }
            else if (!selected) { ctx.selection.set(guid); }
            else
            {
                g.pending_click = guid;
            }
            g.anchor = guid;
        }

        void accept_asset(ecs::entity_t parent)
        {
            const ImGuiPayload* peek = ImGui::GetDragDropPayload();
            if (!peek || !peek->IsDataType(ASSET_PAYLOAD)) { return; }

            const asset_editors::asset_editor_t* editor = asset_editors::of_path(static_cast<const char*>(peek->Data));
            if (!editor || !editor->drop_on_entity) { return; }

            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(ASSET_PAYLOAD))
            {
                g.asset_drop = std::make_pair(std::string(static_cast<const char*>(payload->Data)), parent);
            }
        }

        void draw_drop_target(world_t& world, const editor_context_t& ctx, ecs::entity_t entity)
        {
            if (!ImGui::BeginDragDropTarget()) { return; }
            accept_asset(entity);

            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            const f32 edge = (max.y - min.y) * 0.25f;
            const f32 mouse_y = ImGui::GetMousePos().y;
            const drop_e where = mouse_y < min.y + edge   ? drop_e::BEFORE
                                 : mouse_y > max.y - edge ? drop_e::AFTER
                                                          : drop_e::INTO;

            constexpr ImGuiDragDropFlags flags =
                ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(ENTITY_PAYLOAD, flags))
            {
                drop_t drop;
                drop.moved = dragged_entities(world, ctx, *static_cast<const u64_t*>(payload->Data));
                drop.parent = where == drop_e::INTO ? entity : parent_of(world, entity);
                if (where == drop_e::BEFORE) { drop.before = entity; }
                else if (where == drop_e::AFTER)
                {
                    drop.before = world.registry.get<transform_t>(entity).get_next_sibling();
                    while (drop.before != ecs::NULL_ENTITY &&
                           std::find(drop.moved.begin(), drop.moved.end(), drop.before) != drop.moved.end())
                    {
                        drop.before = world.registry.get<transform_t>(drop.before).get_next_sibling();
                    }
                }

                const bool onto_moved = std::any_of(drop.moved.begin(), drop.moved.end(),
                                                    [&](ecs::entity_t m) { return is_within(world, entity, m); });
                if (!onto_moved && can_move_under(world, drop.moved, drop.parent))
                {
                    const ImU32 color = ImGui::GetColorU32(ImGuiCol_DragDropTarget);
                    ImDrawList* draw = ImGui::GetWindowDrawList();
                    if (where == drop_e::INTO) { draw->AddRect(min, max, color, 0.0f, 0, 2.0f); }
                    else
                    {
                        const f32 y = where == drop_e::BEFORE ? min.y : max.y;
                        draw->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), color, 2.0f);
                    }

                    if (payload->IsDelivery()) { g.drop = std::move(drop); }
                }
            }
            ImGui::EndDragDropTarget();
        }

        void draw_node(world_t& world, editor_context_t& ctx, ecs::entity_t entity)
        {
            const transform_t* transform = world.registry.try_get<transform_t>(entity);
            const ecs::entity_t first_child = transform ? transform->get_first_child() : ecs::NULL_ENTITY;
            const u64_t guid = guid_system::ensure(world.registry, entity);
            const bool selected = ctx.selection.contains(guid);
            const bool renaming = g.renaming == guid;
            g.rows.push_back(guid);

            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (first_child == ecs::NULL_ENTITY) { flags |= ImGuiTreeNodeFlags_Leaf; }
            if (selected) { flags |= ImGuiTreeNodeFlags_Selected; }
            if (renaming) { flags |= ImGuiTreeNodeFlags_AllowOverlap; }
            if (g.reveal.contains(guid)) { ImGui::SetNextItemOpen(true); }

            const std::string name = display_name(world, entity);
            const bool part = world.registry.all_of<prefab_part_t>(entity);
            if (part) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.72f, 1.0f, 1.0f)); }
            const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<intptr_t>(guid)), flags, "%s",
                                                renaming ? "" : name.c_str());
            if (part) { ImGui::PopStyleColor(); }

            if (ctx.scroll_to_selection && guid == ctx.selection.primary()) { ImGui::SetScrollHereY(0.5f); }

            if (!renaming)
            {
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
                {
                    on_press(ctx, guid, selected);
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !selected)
                {
                    ctx.selection.set(guid);
                    g.anchor = guid;
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    g.action = {action_kind_e::RENAME, entity};
                }

                if (transform && ImGui::BeginDragDropSource())
                {
                    g.dragged = true;
                    ImGui::SetDragDropPayload(ENTITY_PAYLOAD, &guid, sizeof(guid));
                    const std::size_t count = selected ? ctx.selection.guids.size() : 1;
                    if (count > 1) { ImGui::Text("%zu entities", count); }
                    else
                    {
                        ImGui::TextUnformatted(name.c_str());
                    }
                    ImGui::EndDragDropSource();
                }
                if (transform) { draw_drop_target(world, ctx, entity); }

                if (ImGui::BeginPopupContextItem())
                {
                    if (ImGui::MenuItem("Create Child", nullptr, false, transform != nullptr))
                    {
                        g.action = {action_kind_e::CREATE, entity};
                    }
                    if (ImGui::MenuItem("Duplicate", keymap::label(keymap::action_e::DUPLICATE).c_str()))
                    {
                        g.action = {action_kind_e::DUPLICATE, entity};
                    }
                    if (ImGui::MenuItem("Rename", keymap::label(keymap::action_e::RENAME).c_str()))
                    {
                        g.action = {action_kind_e::RENAME, entity};
                    }
                    if (ImGui::MenuItem("Delete", keymap::label(keymap::action_e::DELETE_SELECTION).c_str()))
                    {
                        g.action = {action_kind_e::REMOVE, entity};
                    }
                    if (const ecs::entity_t root = prefab_edit::instance_root(world, entity); root != ecs::NULL_ENTITY)
                    {
                        ImGui::Separator();
                        if (ImGui::MenuItem("Open Prefab", nullptr, false,
                                            commands::enabled(ctx, commands::command_e::OPEN_PREFAB)))
                        {
                            commands::push(commands::command_e::OPEN_PREFAB, prefab_edit::prefab_path(world, root),
                                           world.registry.get<prefab_part_t>(entity).source);
                        }
                    }
                    ImGui::EndPopup();
                }
            }
            else
            {
                ImGui::SameLine();
                ImGui::SetCursorScreenPos(
                    ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(), ImGui::GetItemRectMin().y));
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (g.rename_focus)
                {
                    ImGui::SetKeyboardFocusHere();
                    g.rename_focus = false;
                }

                constexpr ImGuiInputTextFlags input_flags =
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll;
                if (ImGui::InputText("##rename", g.rename_buf, sizeof(g.rename_buf), input_flags))
                {
                    finish_rename(world, true);
                }
                else if (ImGui::IsItemDeactivated())
                {
                    finish_rename(world, !ImGui::IsKeyPressed(ImGuiKey_Escape, false));
                }
            }

            if (open)
            {
                for (ecs::entity_t child = first_child; child != ecs::NULL_ENTITY;)
                {
                    // next one first, safe if the tree changes
                    const ecs::entity_t next = world.registry.get<transform_t>(child).get_next_sibling();
                    draw_node(world, ctx, child);
                    child = next;
                }
                ImGui::TreePop();
            }
        }

        void draw_empty_space(world_t& world, editor_context_t& ctx)
        {
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            ImGui::InvisibleButton("##hierarchy_empty", ImVec2(std::max(avail.x, 1.0f), std::max(avail.y, 24.0f)));

            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) { ctx.selection.clear(); }

            if (ImGui::BeginDragDropTarget())
            {
                accept_asset(ecs::NULL_ENTITY);

                constexpr ImGuiDragDropFlags flags =
                    ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(ENTITY_PAYLOAD, flags))
                {
                    drop_t drop;
                    drop.moved = dragged_entities(world, ctx, *static_cast<const u64_t*>(payload->Data));
                    if (can_move_under(world, drop.moved, ecs::NULL_ENTITY))
                    {
                        const ImVec2 min = ImGui::GetItemRectMin();
                        ImGui::GetWindowDrawList()->AddLine(min, ImVec2(ImGui::GetItemRectMax().x, min.y),
                                                            ImGui::GetColorU32(ImGuiCol_DragDropTarget), 2.0f);
                        if (payload->IsDelivery()) { g.drop = std::move(drop); }
                    }
                }
                ImGui::EndDragDropTarget();
            }

            if (ImGui::BeginPopupContextItem("##hierarchy_empty_menu"))
            {
                if (ImGui::MenuItem("Create Entity")) { g.action = {action_kind_e::CREATE, ecs::NULL_ENTITY}; }
                ImGui::EndPopup();
            }
        }

        void run_action(world_t& world, editor_context_t& ctx)
        {
            const action_t action = g.action;
            g.action = {};

            switch (action.kind)
            {
            case action_kind_e::NONE: break;
            case action_kind_e::CREATE: create_entity(world, ctx, action.entity); break;
            case action_kind_e::REMOVE: delete_selection(world, ctx); break;
            case action_kind_e::DUPLICATE: duplicate_selection(world, ctx); break;
            case action_kind_e::RENAME:
                if (world.registry.valid(action.entity)) { start_rename(world, action.entity); }
                break;
            }
        }
    } // namespace

    void draw_hierarchy(world_t& world, editor_context_t& ctx)
    {
        if (ImGui::Begin("Hierarchy"))
        {
            if (ImGui::Button("+ Create Entity")) { g.action = {action_kind_e::CREATE, ecs::NULL_ENTITY}; }

            ImGui::Separator();

            if (ctx.scroll_to_selection)
            {
                const ecs::entity_t primary = guid_system::find(world.registry, ctx.selection.primary());
                for (ecs::entity_t p = parent_of(world, primary); p != ecs::NULL_ENTITY; p = parent_of(world, p))
                {
                    g.reveal.insert(guid_system::of(world.registry, p));
                }
            }

            g.rows.clear();
            for (ecs::entity_t root = transform_system::first_root(world.registry); root != ecs::NULL_ENTITY;)
            {
                const ecs::entity_t next = world.registry.get<transform_t>(root).get_next_sibling();
                draw_node(world, ctx, root);
                root = next;
            }
            for (const ecs::entity_t entity : world.registry.view<ecs::entity_t>())
            {
                if (!world.registry.all_of<transform_t>(entity)) { draw_node(world, ctx, entity); }
            }
            draw_empty_space(world, ctx);

            ctx.scroll_to_selection = false;
            g.reveal.clear();
            g.last_rows.swap(g.rows);

            if (g.pending_click != 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                if (!g.dragged) { ctx.selection.set(g.pending_click); }
                g.pending_click = 0;
            }

            // keys also work from the viewport, focus decides
            const bool in_scope =
                ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || ctx.is_viewport_focused;
            if (in_scope && g.renaming == 0 && !ctx.handle_dragging)
            {
                const ecs::entity_t primary = guid_system::find(world.registry, ctx.selection.primary());
                if (keymap::pressed(keymap::action_e::DELETE_SELECTION)) { g.action = {action_kind_e::REMOVE}; }
                if (keymap::pressed(keymap::action_e::DUPLICATE)) { g.action = {action_kind_e::DUPLICATE}; }
                if (keymap::pressed(keymap::action_e::RENAME) && primary != ecs::NULL_ENTITY)
                {
                    g.action = {action_kind_e::RENAME, primary};
                    ImGui::SetWindowFocus();
                }
            }

            if (g.asset_drop)
            {
                if (const asset_editors::asset_editor_t* editor = asset_editors::of_path(g.asset_drop->first))
                {
                    editor->drop_on_entity(world, ctx, g.asset_drop->first, g.asset_drop->second);
                }
                g.asset_drop.reset();
            }
            if (g.drop)
            {
                move_entities(world, *g.drop);
                g.drop.reset();
            }
            run_action(world, ctx);
        }

        ImGui::End();
    }
} // namespace tau::editor::panels
