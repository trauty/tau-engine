#include "widgets.h"

#include "imgui.h"
#include "tau-editor/asset_editors.h"
#include "tau-editor/history.h"
#include "tau-editor/panels/asset_browser.h"
#include "tau/asset.h"
#include "tau/vfs.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

namespace tau::editor::widgets
{
    using panels::ASSET_PAYLOAD;

    std::string_view asset_file_name(std::string_view path)
    {
        const std::size_t slash = path.find_last_of('/');
        return (slash == std::string_view::npos) ? path : path.substr(slash + 1);
    }

    bool asset_unsaved(asset_handle_t handle)
    { return handle.is_valid() && history::is_dirty({history::doc_kind_e::ASSET, handle.guid}); }

    namespace
    {
        std::string asset_folder_label(std::string_view path)
        {
            const std::optional<vfs::asset_path_parts_t> parts = vfs::split_asset_path(path);
            const std::string_view rest = parts ? parts->rel : path;

            const std::size_t slash = rest.find_last_of('/');
            const std::string_view folder =
                (slash == std::string_view::npos) ? std::string_view{} : rest.substr(0, slash);

            if (!parts) { return std::string(folder); }
            if (folder.empty()) { return std::string(parts->ns); }
            return std::string(parts->ns) + ": " + std::string(folder);
        }

        struct asset_picker_t
        {
            std::string open_id;
            char search[128] = {0};
            std::vector<std::string> candidates;
        };

        asset_picker_t asset_picker;

        void open_asset_picker(const std::string& popup_id, u64_t asset_type_hash)
        {
            asset_picker.open_id = popup_id;
            asset_picker.search[0] = '\0';
            asset_picker.candidates.clear();

            for (auto& [path, guid] : assets::locations())
            {
                if (assets::path_is(path, asset_type_hash)) { asset_picker.candidates.push_back(std::move(path)); }
            }
        }

        bool contains_ci(std::string_view haystack, std::string_view needle)
        {
            if (needle.empty()) { return true; }
            if (needle.size() > haystack.size()) { return false; }

            for (std::size_t i = 0; i + needle.size() <= haystack.size(); i++)
            {
                std::size_t j = 0;
                while (j < needle.size() && std::tolower(static_cast<unsigned char>(haystack[i + j])) ==
                                                std::tolower(static_cast<unsigned char>(needle[j])))
                {
                    j++;
                }
                if (j == needle.size()) { return true; }
            }
            return false;
        }

        constexpr f32 PICKER_WIDTH = 340.0f;
        constexpr f32 FOLDER_COLUMN = 190.0f;

        bool draw_asset_picker(u64_t asset_type_hash, std::string& out_path)
        {
            const asset_type_t* type = assets::type_by_id(asset_type_hash);
            ImGui::TextDisabled("%s", type ? std::string(type->display_name).c_str() : "Unknown");

            ImGui::SetNextItemWidth(PICKER_WIDTH);
            if (ImGui::IsWindowAppearing()) { ImGui::SetKeyboardFocusHere(); }
            ImGui::InputTextWithHint("##search", "search", asset_picker.search, sizeof(asset_picker.search));

            if (ImGui::Button("Clear"))
            {
                out_path.clear();
                return true;
            }

            ImGui::Separator();

            if (asset_picker.candidates.empty())
            {
                ImGui::TextDisabled("no cooked assets of this type");
                return false;
            }

            bool chose = false;
            ImGui::BeginChild("##asset_list", ImVec2(PICKER_WIDTH, 220.0f), ImGuiChildFlags_None);

            for (const std::string& path : asset_picker.candidates)
            {
                if (!contains_ci(path, asset_picker.search)) { continue; }

                const std::string_view name = asset_file_name(path);
                const std::string row_label = std::string(name) + "##" + path;

                if (ImGui::Selectable(row_label.c_str()))
                {
                    out_path = path;
                    chose = true;
                }

                if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", path.c_str()); }

                const std::string folder = asset_folder_label(path);
                if (!folder.empty())
                {
                    ImGui::SameLine(FOLDER_COLUMN);
                    ImGui::TextDisabled("%s", folder.c_str());
                }
            }

            ImGui::EndChild();
            return chose;
        }

    } // namespace

    bool asset_slot(world_t& world, const char* label, u64_t asset_type_hash, asset_handle_t handle,
                    asset_handle_t& out_handle, const std::string& popup_id, bool* out_remove, const char* empty_text)
    {
        bool modified = false;
        const std::string cur_path = assets::path_of(handle);

        ImGui::Text("%s", label);
        ImGui::SameLine();

        const f32 remove_width = out_remove ? (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x) : 0.0f;
        const f32 field_width = std::max(ImGui::GetContentRegionAvail().x - remove_width, 60.0f);

        std::string name = cur_path.empty() ? empty_text : std::string(asset_file_name(cur_path));
        if (asset_unsaved(handle)) { name += "*"; }

        if (ImGui::Button((name + popup_id).c_str(), ImVec2(field_width, 0.0f)))
        {
            open_asset_picker(popup_id, asset_type_hash);
            ImGui::OpenPopup(popup_id.c_str());
        }

        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", cur_path.empty() ? "nothing assigned, click to pick" : cur_path.c_str());
        }

        if (ImGui::BeginDragDropTarget())
        {
            const ImGuiPayload* peek = ImGui::GetDragDropPayload();
            if (peek && peek->IsDataType(ASSET_PAYLOAD) &&
                assets::path_is(static_cast<const char*>(peek->Data), asset_type_hash))
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(ASSET_PAYLOAD))
                {
                    out_handle = assets::load(asset_type_hash, static_cast<const char*>(payload->Data));
                    modified = true;
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (out_remove)
        {
            ImGui::SameLine();
            if (ImGui::Button(("x" + popup_id).c_str(), ImVec2(ImGui::GetFrameHeight(), 0.0f))) { *out_remove = true; }
        }

        if (ImGui::BeginPopup(popup_id.c_str()))
        {
            std::string picked;
            if (draw_asset_picker(asset_type_hash, picked))
            {
                out_handle = picked.empty() ? asset_handle_t{} : assets::load(asset_type_hash, picked);
                modified = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        const asset_editors::asset_editor_t* editor = asset_editors::of(asset_type_hash);
        if (editor && editor->draw_inline && handle.is_valid())
        {
            ImGui::PushID(popup_id.c_str());
            ImGui::Indent();
            if (ImGui::TreeNode("Properties"))
            {
                editor->draw_inline(world, handle);
                ImGui::TreePop();
            }
            ImGui::Unindent();
            ImGui::PopID();
        }

        return modified;
    }

} // namespace tau::editor::widgets
