#include "asset_browser.h"

#include "imgui.h"
#include "tau-editor/asset_cook.h"
#include "tau-editor/asset_files.h"
#include "tau-editor/history.h"
#include "tau-editor/keymap.h"
#include "tau-editor/panels/hierarchy.h"
#include "tau-editor/panels/main_menu_bar.h"
#include "tau-editor/prefab_edit.h"
#include "tau-editor/prefab_stage.h"
#include "tau/asset.h"
#include "tau/assets/texture.h"
#include "tau/engine.h"
#include "tau/systems/guid.h"

#include <SDL3/SDL_misc.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace tau::editor::panels
{
    namespace
    {
        enum class root_e : u8_t
        {
            GAME,
            ENGINE,
        };

        struct item_t
        {
            std::string name;
            std::string rel;
            bool is_dir = false;
            const asset_type_t* type = nullptr;
        };

        struct folder_t
        {
            std::string name;
            std::string rel;
            std::vector<folder_t> children;
        };

        enum class action_kind_e : u8_t
        {
            NONE,
            NEW_FOLDER,
            NEW_MATERIAL,
            NEW_SCENE,
            RENAME,
            DUPLICATE,
            REMOVE,
            MOVE,
            MAKE_PREFAB,
        };

        struct action_t
        {
            action_kind_e kind = action_kind_e::NONE;
            std::string rel;
            std::vector<std::string> rels;
            u64_t entity = 0;
        };

        struct state_t
        {
            root_e root = root_e::GAME;
            std::string dir;

            std::vector<item_t> items;
            folder_t game_tree;
            folder_t engine_tree;
            f64 listed_at = -1.0;
            bool stale = true;
            std::string listed_for;

            std::vector<std::string> selection;
            std::string anchor;
            std::string pending_click;
            bool dragged = false;
            // inspector switches on release, a press may become a drag
            std::string inspect_on_release;

            std::string renaming;
            bool rename_focus = false;
            char rename_buf[256] = {};

            char search[128] = {};
            f32 tile = 76.0f;

            std::unordered_map<std::string, asset_t<texture_t>> thumbs;

            action_t action;
        };

        state_t g;

        constexpr f64 RELIST_SECONDS = 1.0;

        fs::path root_dir(const editor_context_t& ctx, root_e root)
        { return root == root_e::GAME ? fs::path(ctx.project_assets_dir) : asset_cook::engine_assets_dir(); }

        std::string vfs_of(root_e root, const std::string& rel)
        { return (root == root_e::GAME ? "game://assets/" : "engine://assets/") + rel; }

        std::string join(const std::string& dir, const std::string& name)
        { return dir.empty() ? name : dir + "/" + name; }

        bool editable(const editor_context_t& ctx)
        { return g.root == root_e::GAME && ctx.cur_mode == editor_mode_e::EDIT && !ctx.project_assets_dir.empty(); }

        bool contains_ci(std::string_view haystack, std::string_view needle)
        {
            auto lower = [](unsigned char c) { return std::tolower(c); };
            return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                               [&](char a, char b) { return lower(a) == lower(b); }) != haystack.end();
        }

        bool listed(const fs::path& path)
        {
            const std::string name = path.filename().string();
            return !name.empty() && name.front() != '.' && path.extension() != ".meta";
        }

        void build_tree(const fs::path& root, folder_t& folder)
        {
            folder.children.clear();
            std::error_code ec;
            for (fs::directory_iterator it(root / folder.rel, ec), end; it != end; it.increment(ec))
            {
                if (ec || !it->is_directory(ec) || !listed(it->path())) { continue; }

                folder_t child;
                child.name = it->path().filename().string();
                child.rel = join(folder.rel, child.name);
                build_tree(root, child);
                folder.children.push_back(std::move(child));
            }
            std::sort(folder.children.begin(), folder.children.end(),
                      [](const folder_t& a, const folder_t& b) { return a.name < b.name; });
        }

        void release_thumbs()
        {
            if (g.thumbs.empty()) { return; }

            for (auto& [path, texture] : g.thumbs) { assets::unpin(texture); }
            g.thumbs.clear();
            assets::collect();
        }

        void relist(const editor_context_t& ctx)
        {
            const fs::path root = root_dir(ctx, g.root);
            const std::string key = std::to_string(static_cast<int>(g.root)) + "|" + g.dir + "|" + g.search;
            if (key != g.listed_for) { release_thumbs(); }
            g.listed_for = key;

            g.items.clear();
            auto add = [&](const fs::directory_entry& entry)
            {
                std::error_code ec;
                item_t item;
                item.name = entry.path().filename().string();
                item.rel = fs::relative(entry.path(), root, ec).generic_string();
                item.is_dir = entry.is_directory(ec);
                if (!item.is_dir) { item.type = assets::type_by_path(item.name); }
                g.items.push_back(std::move(item));
            };

            std::error_code ec;
            if (g.search[0] != '\0')
            {
                for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec))
                {
                    if (ec || !listed(it->path()) || it->is_directory(ec)) { continue; }
                    if (contains_ci(it->path().filename().string(), g.search)) { add(*it); }
                }
            }
            else
            {
                for (fs::directory_iterator it(root / g.dir, ec), end; it != end; it.increment(ec))
                {
                    if (!ec && listed(it->path())) { add(*it); }
                }
            }

            std::sort(g.items.begin(), g.items.end(),
                      [](const item_t& a, const item_t& b)
                      {
                          if (a.is_dir != b.is_dir) { return a.is_dir; }
                          return std::lexicographical_compare(a.name.begin(), a.name.end(), b.name.begin(),
                                                              b.name.end(), [](unsigned char x, unsigned char y)
                                                              { return std::tolower(x) < std::tolower(y); });
                      });

            std::erase_if(g.selection,
                          [](const std::string& rel)
                          {
                              return std::none_of(g.items.begin(), g.items.end(),
                                                  [&](const item_t& item) { return item.rel == rel; });
                          });

            g.game_tree.name = "Assets";
            build_tree(root_dir(ctx, root_e::GAME), g.game_tree);
            g.engine_tree.name = "Engine";
            if (!asset_cook::engine_assets_dir().empty())
            {
                build_tree(asset_cook::engine_assets_dir(), g.engine_tree);
            }

            g.listed_at = ImGui::GetTime();
            g.stale = false;
        }

        void navigate(root_e root, const std::string& dir)
        {
            g.root = root;
            g.dir = dir;
            g.search[0] = '\0';
            g.selection.clear();
            g.renaming.clear();
            g.stale = true;
        }

        const item_t* find_item(const std::string& rel)
        {
            const auto it = std::find_if(g.items.begin(), g.items.end(), [&](const item_t& i) { return i.rel == rel; });
            return it != g.items.end() ? &*it : nullptr;
        }

        void inspect(editor_context_t& ctx, const std::string& rel)
        {
            const item_t* item = find_item(rel);
            if (!item || item->is_dir || !item->type) { return; }

            ctx.inspected_asset = vfs_of(g.root, rel);
            ctx.selection.clear();
        }

        void on_press(editor_context_t& ctx, const std::string& rel, bool selected)
        {
            const ImGuiIO& io = ImGui::GetIO();
            g.dragged = false;

            if (io.KeyShift && !g.anchor.empty())
            {
                const auto a =
                    std::find_if(g.items.begin(), g.items.end(), [](const item_t& i) { return i.rel == g.anchor; });
                const auto b =
                    std::find_if(g.items.begin(), g.items.end(), [&](const item_t& i) { return i.rel == rel; });
                if (a != g.items.end() && b != g.items.end())
                {
                    g.selection.clear();
                    for (auto it = std::min(a, b); it <= std::max(a, b); ++it)
                    {
                        if (it->rel != rel) { g.selection.push_back(it->rel); }
                    }
                    g.selection.push_back(rel);
                    g.inspect_on_release = rel;
                    return;
                }
            }

            if (io.KeyCtrl)
            {
                const auto it = std::find(g.selection.begin(), g.selection.end(), rel);
                if (it != g.selection.end()) { g.selection.erase(it); }
                else
                {
                    g.selection.push_back(rel);
                }
            }
            else if (!selected) { g.selection = {rel}; }
            else
            {
                g.pending_click = rel;
            }
            g.anchor = rel;
            g.inspect_on_release = rel;
        }

        void start_rename(const std::string& rel)
        {
            const item_t* item = find_item(rel);
            if (!item) { return; }

            const std::string name = item->is_dir ? item->name : fs::path(item->name).stem().string();
            std::snprintf(g.rename_buf, sizeof(g.rename_buf), "%s", name.c_str());
            g.renaming = rel;
            g.rename_focus = true;
        }

        std::vector<std::string> dragged_rels(const std::string& vfs)
        {
            const std::string prefix = "game://assets/";
            if (!vfs.starts_with(prefix)) { return {}; }

            const std::string rel = vfs.substr(prefix.size());
            if (std::find(g.selection.begin(), g.selection.end(), rel) != g.selection.end()) { return g.selection; }
            return {rel};
        }

        void folder_drop_target(const editor_context_t& ctx, const std::string& dir)
        {
            if (!editable(ctx) || !ImGui::BeginDragDropTarget()) { return; }

            const ImGuiPayload* peek = ImGui::GetDragDropPayload();
            if (peek && peek->IsDataType(ASSET_PAYLOAD) &&
                std::string_view(static_cast<const char*>(peek->Data)).starts_with("game://assets/"))
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(ASSET_PAYLOAD))
                {
                    g.action = {action_kind_e::MOVE, dir, dragged_rels(static_cast<const char*>(payload->Data))};
                }
            }

            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(ENTITY_PAYLOAD))
            {
                g.action = {action_kind_e::MAKE_PREFAB, dir, {}, *static_cast<const u64_t*>(payload->Data)};
            }
            ImGui::EndDragDropTarget();
        }

        void draw_folder(const editor_context_t& ctx, root_e root, const folder_t& folder)
        {
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (folder.children.empty()) { flags |= ImGuiTreeNodeFlags_Leaf; }
            if (g.root == root && g.dir == folder.rel && g.search[0] == '\0') { flags |= ImGuiTreeNodeFlags_Selected; }
            if (folder.rel.empty() || (g.root == root && g.dir.starts_with(folder.rel + "/")))
            {
                ImGui::SetNextItemOpen(true, folder.rel.empty() ? ImGuiCond_Once : ImGuiCond_Appearing);
            }

            const bool open =
                ImGui::TreeNodeEx(folder.rel.empty() ? folder.name.c_str() : (folder.rel + "##dir").c_str(), flags,
                                  "%s", folder.name.c_str());
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) { navigate(root, folder.rel); }
            if (root == root_e::GAME) { folder_drop_target(ctx, folder.rel); }

            if (open)
            {
                for (const folder_t& child : folder.children) { draw_folder(ctx, root, child); }
                ImGui::TreePop();
            }
        }

        void draw_breadcrumbs(const editor_context_t& ctx)
        {
            const char* root_name = g.root == root_e::GAME ? "Assets" : "Engine";
            if (ImGui::SmallButton(root_name)) { navigate(g.root, {}); }
            if (g.root == root_e::GAME) { folder_drop_target(ctx, {}); }

            std::string path;
            std::size_t start = 0;
            while (!g.dir.empty() && start <= g.dir.size())
            {
                const std::size_t slash = g.dir.find('/', start);
                const std::string part =
                    g.dir.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
                path = join(path, part);

                ImGui::SameLine(0.0f, 2.0f);
                ImGui::TextDisabled("/");
                ImGui::SameLine(0.0f, 2.0f);
                if (ImGui::SmallButton((part + "##crumb" + path).c_str())) { navigate(g.root, path); }
                if (g.root == root_e::GAME) { folder_drop_target(ctx, path); }

                if (slash == std::string::npos) { break; }
                start = slash + 1;
            }
        }

        void new_menu_items(const editor_context_t& ctx)
        {
            const bool can = editable(ctx);
            if (ImGui::MenuItem("Folder", nullptr, false, can)) { g.action = {action_kind_e::NEW_FOLDER}; }
            if (ImGui::MenuItem("Material", nullptr, false, can)) { g.action = {action_kind_e::NEW_MATERIAL}; }
            if (ImGui::MenuItem("Scene", nullptr, false, can)) { g.action = {action_kind_e::NEW_SCENE}; }
        }

        ImU32 type_color(const item_t& item)
        {
            if (item.is_dir) { return IM_COL32(200, 170, 90, 255); }
            if (!item.type) { return IM_COL32(110, 110, 110, 255); }

            const std::string_view key = item.type->key;
            if (key == "texture") { return IM_COL32(90, 150, 210, 255); }
            if (key == "mesh") { return IM_COL32(120, 190, 110, 255); }
            if (key == "material") { return IM_COL32(210, 120, 80, 255); }
            if (key == "scene") { return IM_COL32(170, 110, 200, 255); }
            if (key == "shader") { return IM_COL32(200, 200, 90, 255); }
            if (key == "prefab") { return IM_COL32(90, 180, 230, 255); }
            return IM_COL32(150, 150, 150, 255);
        }

        std::string type_label(const item_t& item)
        {
            if (item.is_dir) { return {}; }
            if (!item.type) { return "FILE"; }

            std::string label(item.type->key);
            std::transform(label.begin(), label.end(), label.begin(), [](unsigned char c) { return std::toupper(c); });
            return label;
        }

        std::string fitted(const std::string& text, f32 width)
        {
            if (ImGui::CalcTextSize(text.c_str()).x <= width) { return text; }

            std::string cut = text;
            while (!cut.empty() && ImGui::CalcTextSize((cut + "...").c_str()).x > width) { cut.pop_back(); }
            return cut + "...";
        }

        void draw_icon(ImDrawList* draw, const item_t& item, ImVec2 min, ImVec2 max)
        {
            if (item.type && item.type->key == "texture")
            {
                const std::string vfs = vfs_of(g.root, item.rel);
                auto [it, inserted] = g.thumbs.try_emplace(vfs);
                if (inserted) { it->second = assets::pin(assets::load_async<texture_t>(vfs)); }

                if (const texture_t* tex = assets::get(it->second); tex && tex->width > 0)
                {
                    draw->AddImage((ImTextureID)(intptr_t)tex->bindless_id, min, max);
                    return;
                }
            }

            const ImU32 color = type_color(item);
            if (item.is_dir)
            {
                const f32 w = max.x - min.x;
                const f32 h = max.y - min.y;
                draw->AddRectFilled(ImVec2(min.x, min.y + h * 0.12f), ImVec2(min.x + w * 0.42f, min.y + h * 0.3f),
                                    color, 3.0f);
                draw->AddRectFilled(ImVec2(min.x, min.y + h * 0.24f), max, color, 4.0f);
                return;
            }

            draw->AddRectFilled(min, max, color & IM_COL32(255, 255, 255, 90), 4.0f);
            draw->AddRect(min, max, color, 4.0f, 0, 2.0f);

            const std::string label = type_label(item);
            const ImVec2 size = ImGui::CalcTextSize(label.c_str());
            draw->AddText(ImVec2((min.x + max.x - size.x) * 0.5f, (min.y + max.y - size.y) * 0.5f),
                          ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
        }

        bool unsaved(const item_t& item)
        {
            if (item.is_dir || !item.type) { return false; }
            const std::string vfs = vfs_of(g.root, item.rel);
            return history::is_dirty(history::asset_doc(vfs)) ||
                   (prefab_stage::is_dirty() && assets::find(vfs) == prefab_stage::prefab().handle);
        }

        void item_context_menu(const editor_context_t& ctx, const item_t& item)
        {
            if (!ImGui::BeginPopupContextItem()) { return; }

            const bool editing = ctx.cur_mode == editor_mode_e::EDIT;
            const history::doc_t doc = history::asset_doc(vfs_of(g.root, item.rel));
            if (ImGui::MenuItem("Save", nullptr, false, editing && unsaved(item))) { history::save_asset(doc); }
            if (ImGui::MenuItem("Revert", nullptr, false, editing && unsaved(item))) { history::revert_asset(doc); }
            ImGui::Separator();

            const bool can = editable(ctx);
            if (ImGui::MenuItem("Rename", keymap::label(keymap::action_e::RENAME).c_str(), false, can))
            {
                g.action = {action_kind_e::RENAME, item.rel};
            }
            if (ImGui::MenuItem("Duplicate", keymap::label(keymap::action_e::DUPLICATE).c_str(), false,
                                can && !item.is_dir))
            {
                g.action = {action_kind_e::DUPLICATE};
            }
            if (ImGui::MenuItem("Delete", keymap::label(keymap::action_e::DELETE_SELECTION).c_str(), false, can))
            {
                g.action = {action_kind_e::REMOVE};
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Show in File Manager"))
            {
                const fs::path at = root_dir(ctx, g.root) / item.rel;
                const fs::path dir = item.is_dir ? at : at.parent_path();
                SDL_OpenURL(("file://" + dir.generic_string()).c_str());
            }
            ImGui::EndPopup();
        }

        void draw_tile(world_t& world, editor_context_t& ctx, const item_t& item)
        {
            const f32 text_h = ImGui::GetTextLineHeightWithSpacing();
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const bool selected = std::find(g.selection.begin(), g.selection.end(), item.rel) != g.selection.end();
            const bool renaming = g.renaming == item.rel;

            ImGui::PushID(item.rel.c_str());
            if (renaming) { ImGui::SetNextItemAllowOverlap(); }
            ImGui::InvisibleButton("##tile", ImVec2(g.tile, g.tile + text_h));
            const bool hovered = ImGui::IsItemHovered();

            ImDrawList* draw = ImGui::GetWindowDrawList();
            if (selected || hovered)
            {
                draw->AddRectFilled(pos, ImVec2(pos.x + g.tile, pos.y + g.tile + text_h),
                                    ImGui::GetColorU32(selected ? ImGuiCol_Header : ImGuiCol_HeaderHovered), 4.0f);
            }

            const f32 pad = g.tile * 0.12f;
            draw_icon(draw, item, ImVec2(pos.x + pad, pos.y + pad), ImVec2(pos.x + g.tile - pad, pos.y + g.tile - pad));

            if (!renaming)
            {
                const std::string mark = unsaved(item) ? "*" : "";
                const std::string name = fitted(item.is_dir ? item.name : fs::path(item.name).stem().string(),
                                                g.tile - 4.0f - ImGui::CalcTextSize(mark.c_str()).x) +
                                         mark;
                const f32 w = ImGui::CalcTextSize(name.c_str()).x;
                draw->AddText(ImVec2(pos.x + (g.tile - w) * 0.5f, pos.y + g.tile), ImGui::GetColorU32(ImGuiCol_Text),
                              name.c_str());
            }

            if (hovered && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                ImGui::SetTooltip("%s\n%s", item.rel.c_str(),
                                  item.is_dir ? "Folder"
                                  : item.type ? item.type->display_name.data()
                                              : "Not an asset");
            }

            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) { on_press(ctx, item.rel, selected); }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !selected)
            {
                g.selection = {item.rel};
                g.anchor = item.rel;
                inspect(ctx, item.rel);
            }
            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                if (item.is_dir) { navigate(g.root, item.rel); }
                else if (item.type && item.type->key == "scene" && g.root == root_e::GAME)
                {
                    ctx.unsaved_action_scene = asset_files::source_path(item.rel).string();
                    request_action(world, ctx, unsaved_action_e::LOAD_SCENE);
                }
                else if (item.type && item.type->key == "prefab")
                {
                    prefab_stage::open(world, ctx, vfs_of(g.root, item.rel));
                }
            }

            if (ImGui::BeginDragDropSource())
            {
                g.dragged = true;
                const std::string vfs = vfs_of(g.root, item.rel);
                ImGui::SetDragDropPayload(ASSET_PAYLOAD, vfs.c_str(), vfs.size() + 1);
                const std::size_t count = selected ? g.selection.size() : 1;
                if (count > 1) { ImGui::Text("%zu items", count); }
                else
                {
                    ImGui::TextUnformatted(item.name.c_str());
                }
                ImGui::EndDragDropSource();
            }
            if (item.is_dir && g.root == root_e::GAME) { folder_drop_target(ctx, item.rel); }

            item_context_menu(ctx, item);

            if (renaming)
            {
                ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + g.tile));
                ImGui::SetNextItemWidth(g.tile);
                if (g.rename_focus)
                {
                    ImGui::SetKeyboardFocusHere();
                    g.rename_focus = false;
                }

                constexpr ImGuiInputTextFlags flags =
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll;
                const bool commit = ImGui::InputText("##rename", g.rename_buf, sizeof(g.rename_buf), flags);
                if (commit || ImGui::IsItemDeactivated())
                {
                    if (commit || !ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                    {
                        g.action = {action_kind_e::RENAME, item.rel, {g.rename_buf}};
                    }
                    g.renaming.clear();
                }

                ImGui::SetCursorScreenPos(pos);
                ImGui::Dummy(ImVec2(g.tile, g.tile + text_h));
            }

            ImGui::PopID();
        }

        void draw_grid(world_t& world, editor_context_t& ctx)
        {
            const f32 spacing = ImGui::GetStyle().ItemSpacing.x;
            const f32 avail = ImGui::GetContentRegionAvail().x;
            const i32 columns = std::max(1, static_cast<i32>((avail + spacing) / (g.tile + spacing)));

            for (std::size_t i = 0; i < g.items.size(); i++)
            {
                if (i % static_cast<std::size_t>(columns) != 0) { ImGui::SameLine(); }
                draw_tile(world, ctx, g.items[i]);
            }

            if (g.items.empty()) { ImGui::TextDisabled(g.search[0] != '\0' ? "Nothing matches" : "Empty folder"); }

            const ImVec2 rest = ImGui::GetContentRegionAvail();
            ImGui::InvisibleButton("##empty", ImVec2(std::max(rest.x, 1.0f), std::max(rest.y, 24.0f)));
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) { g.selection.clear(); }
            if (g.root == root_e::GAME && g.search[0] == '\0') { folder_drop_target(ctx, g.dir); }
            if (ImGui::BeginPopupContextItem("##empty_menu"))
            {
                new_menu_items(ctx);
                ImGui::EndPopup();
            }
        }

        void run_action(world_t& world, editor_context_t& ctx)
        {
            const action_t action = std::move(g.action);
            g.action = {};

            std::string made;
            switch (action.kind)
            {
            case action_kind_e::NONE: return;
            case action_kind_e::NEW_FOLDER: made = asset_files::create_folder(world, g.dir); break;
            case action_kind_e::NEW_MATERIAL: made = asset_files::create_material(world, g.dir); break;
            case action_kind_e::NEW_SCENE: made = asset_files::create_scene(world, g.dir); break;
            case action_kind_e::RENAME:
                if (action.rels.empty()) { start_rename(action.rel); }
                else
                {
                    asset_files::rename(world, action.rel, action.rels.front());
                }
                break;
            case action_kind_e::DUPLICATE:
            {
                const std::vector<std::string> copies = asset_files::duplicate(world, g.selection);
                if (!copies.empty()) { g.selection = copies; }
                break;
            }
            case action_kind_e::REMOVE:
                for (const std::string& rel : g.selection)
                {
                    if (prefab_stage::is_open() && assets::find(vfs_of(g.root, rel)) == prefab_stage::prefab().handle)
                    {
                        prefab_stage::close(world, ctx);
                    }
                }
                asset_files::remove(world, g.selection);
                g.selection.clear();
                break;
            case action_kind_e::MOVE: asset_files::move(world, action.rels, action.rel); break;
            case action_kind_e::MAKE_PREFAB:
            {
                const ecs::entity_t entity = guid_system::find(world.registry, action.entity);
                if (entity != ecs::NULL_ENTITY)
                {
                    const std::string made_prefab = prefab_edit::create(world, ctx, entity, action.rel);
                    relist(ctx);
                    if (!made_prefab.empty()) { g.selection = {made_prefab}; }
                }
                return;
            }
            }

            relist(ctx);

            if (!made.empty() && find_item(made))
            {
                g.selection = {made};
                start_rename(made);
            }
        }
    } // namespace

    void draw_asset_browser(world_t& world, editor_context_t& ctx)
    {
        if (ImGui::Begin("Assets"))
        {
            const f64 now = ImGui::GetTime();
            if (g.stale || now - g.listed_at > RELIST_SECONDS) { relist(ctx); }

            ImGui::BeginDisabled(!editable(ctx));
            if (ImGui::Button("+ New")) { ImGui::OpenPopup("##new_asset"); }
            ImGui::EndDisabled();
            if (ImGui::BeginPopup("##new_asset"))
            {
                new_menu_items(ctx);
                ImGui::EndPopup();
            }

            ImGui::SameLine();
            draw_breadcrumbs(ctx);

            constexpr f32 right = 260.0f;
            ImGui::SameLine();
            const f32 room = ImGui::GetContentRegionAvail().x;
            if (room > right) { ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room - right); }
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::InputTextWithHint("##search", "search", g.search, sizeof(g.search))) { g.stale = true; }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::SliderFloat("##tile", &g.tile, 48.0f, 160.0f, "%.0f px");

            ImGui::BeginChild("##folders", ImVec2(180.0f, 0.0f), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
            draw_folder(ctx, root_e::GAME, g.game_tree);
            if (!g.engine_tree.name.empty()) { draw_folder(ctx, root_e::ENGINE, g.engine_tree); }
            ImGui::EndChild();

            if (g.stale) { relist(ctx); }

            ImGui::SameLine();
            ImGui::BeginChild("##grid", ImVec2(0.0f, 0.0f));
            draw_grid(world, ctx);
            const bool grid_focused = ImGui::IsWindowFocused();
            ImGui::EndChild();

            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                if (!g.dragged && !g.pending_click.empty()) { g.selection = {g.pending_click}; }
                if (!g.dragged && !g.inspect_on_release.empty()) { inspect(ctx, g.inspect_on_release); }
                g.pending_click.clear();
                g.inspect_on_release.clear();
            }

            const bool focused = grid_focused || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            if (focused && editable(ctx) && g.renaming.empty() && !g.selection.empty())
            {
                if (keymap::pressed(keymap::action_e::RENAME))
                {
                    g.action = {action_kind_e::RENAME, g.selection.back()};
                }
                if (keymap::pressed(keymap::action_e::DUPLICATE)) { g.action = {action_kind_e::DUPLICATE}; }
                if (keymap::pressed(keymap::action_e::DELETE_SELECTION)) { g.action = {action_kind_e::REMOVE}; }
            }

            run_action(world, ctx);
        }

        ImGui::End();
    }
} // namespace tau::editor::panels
