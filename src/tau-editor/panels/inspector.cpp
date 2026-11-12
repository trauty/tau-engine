#include "inspector.h"

#include "imgui.h"
#include "tau-editor/asset_files.h"
#include "tau-editor/history.h"
#include "tau-editor/panels/asset_browser.h"
#include "tau-editor/panels/main_menu_bar.h"
#include "tau-editor/prefab_edit.h"
#include "tau-editor/prefab_stage.h"
#include "tau/asset.h"
#include "tau/assets/material.h"
#include "tau/assets/shader.h"
#include "tau/assets/texture.h"
#include "tau/components/prefab.h"
#include "tau/components/tag.h"
#include "tau/components/unresolved.h"
#include "tau/ecs_fwd.h"
#include "tau/engine.h"
#include "tau/hash.h"
#include "tau/log.h"
#include "tau/reflection.h"
#include "tau/systems/guid.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <numeric>
#include <string>
#include <unordered_set>
#include <vector>

namespace tau::editor::panels
{
    namespace
    {
        char asset_path_input[1024] = {0};

        // radians to quat to radians is not stable
        struct degree_edit_t
        {
            ImGuiID widget = 0;
            tau::vec3_t degrees{};
        };

        degree_edit_t active_degree_edit;

        std::vector<std::string> split_enum_labels(const std::string& csv)
        {
            std::vector<std::string> labels;
            std::size_t start = 0;

            while (start <= csv.size())
            {
                const std::size_t comma = csv.find(',', start);
                const std::size_t end = (comma == std::string::npos) ? csv.size() : comma;

                std::string label = csv.substr(start, end - start);
                const std::size_t first = label.find_first_not_of(" \t");
                const std::size_t last = label.find_last_not_of(" \t");
                if (first != std::string::npos) { labels.push_back(label.substr(first, last - first + 1)); }

                if (comma == std::string::npos) { break; }
                start = comma + 1;
            }

            return labels;
        }

        constexpr const char* SAMPLER_LABELS[] = {"Linear Repeat", "Linear Clamp", "Nearest Repeat", "Nearest Clamp"};

        void draw_member_tooltip(const shader_member_t& member)
        {
            if (member.tooltip.empty() || !ImGui::IsItemHovered()) { return; }
            ImGui::SetTooltip("%s", member.tooltip.c_str());
        }

        bool draw_asset_slot(tau::world_t& world, const char* label, u64_t asset_type_hash, asset_handle_t handle,
                             asset_handle_t& out_handle, const std::string& popup_id, bool* out_remove = nullptr);

        bool draw_material_properties(tau::world_t& world, asset_handle_t handle)
        {
            material_t* mat = assets::get(asset_t<material_t>{handle});
            if (!mat) { return false; }

            const shader_t* shader = assets::get(mat->shader);
            if (!shader || mat->data.empty()) { return false; }

            std::vector<const shader_member_t*> ordered;
            ordered.reserve(shader->module.members.size());
            for (const auto& [hash, member] : shader->module.members) { ordered.push_back(&member); }
            std::sort(ordered.begin(), ordered.end(),
                      [](const shader_member_t* a, const shader_member_t* b) { return a->offset < b->offset; });

            bool changed = false;
            u32_t hidden_count = 0;

            for (const shader_member_t* member : ordered)
            {
                if (member->offset + member->size > mat->data.size()) { continue; }

                if (member->edit == shader_member_edit_e::HIDDEN)
                {
                    hidden_count++;
                    continue;
                }

                const u32_t name_hash = tau::hash_string(member->name);
                const char* label = member->name.empty() ? "(unnamed)" : member->name.c_str();
                void* slot = mat->data.data() + member->offset;
                bool member_changed = false;

                ImGui::PushID(static_cast<i32>(member->offset));

                switch (member->edit)
                {
                case shader_member_edit_e::TEXTURE:
                {
                    const asset_t<texture_t>* bound = mat->textures.find(name_hash);
                    const asset_handle_t cur = bound ? bound->handle : asset_handle_t{};

                    asset_handle_t picked = cur;
                    if (draw_asset_slot(world, label, tau::get_type_id<tau::texture_t>(), cur, picked,
                                        "##mattex_" + std::to_string(member->offset)))
                    {
                        mat->set_texture(name_hash, asset_t<texture_t>{picked});
                        member_changed = true;
                    }
                    break;
                }
                case shader_member_edit_e::SAMPLER:
                {
                    i32 cur = static_cast<i32>(*static_cast<u32_t*>(slot));
                    if (cur < 0 || cur >= IM_ARRAYSIZE(SAMPLER_LABELS)) { cur = 0; }

                    if (ImGui::Combo(label, &cur, SAMPLER_LABELS, IM_ARRAYSIZE(SAMPLER_LABELS)))
                    {
                        *static_cast<u32_t*>(slot) = static_cast<u32_t>(cur);
                        member_changed = true;
                    }
                    draw_member_tooltip(*member);
                    break;
                }
                case shader_member_edit_e::ENUM:
                {
                    const std::vector<std::string> labels = split_enum_labels(member->enum_names);

                    std::vector<const char*> items;
                    items.reserve(labels.size());
                    for (const std::string& l : labels) { items.push_back(l.c_str()); }

                    i32 cur = (member->type == shader_member_type_e::INT)
                                  ? *static_cast<i32*>(slot)
                                  : static_cast<i32>(*static_cast<u32_t*>(slot));

                    if (items.empty() || cur < 0 || cur >= static_cast<i32>(items.size()))
                    {
                        ImGui::Text("%s", label);
                        ImGui::SameLine();
                        ImGui::TextDisabled("%d (no label)", cur);
                        break;
                    }

                    if (ImGui::Combo(label, &cur, items.data(), static_cast<i32>(items.size())))
                    {
                        if (member->type == shader_member_type_e::INT) { *static_cast<i32*>(slot) = cur; }
                        else
                        {
                            *static_cast<u32_t*>(slot) = static_cast<u32_t>(cur);
                        }
                        member_changed = true;
                    }
                    draw_member_tooltip(*member);
                    break;
                }
                case shader_member_edit_e::COLOR:
                {
                    if (member->type == shader_member_type_e::FLOAT4)
                    {
                        member_changed = ImGui::ColorEdit4(label, static_cast<f32*>(slot));
                    }
                    else
                    {
                        member_changed = ImGui::ColorEdit3(label, static_cast<f32*>(slot));
                    }
                    draw_member_tooltip(*member);
                    break;
                }
                default:
                {
                    const bool ranged = member->has_range();

                    switch (member->type)
                    {
                    case shader_member_type_e::FLOAT:
                        member_changed = ranged ? ImGui::SliderFloat(label, static_cast<f32*>(slot), member->range_min,
                                                                     member->range_max)
                                                : ImGui::DragFloat(label, static_cast<f32*>(slot), 0.01f);
                        break;
                    case shader_member_type_e::FLOAT2:
                        member_changed = ranged ? ImGui::SliderFloat2(label, static_cast<f32*>(slot), member->range_min,
                                                                      member->range_max)
                                                : ImGui::DragFloat2(label, static_cast<f32*>(slot), 0.01f);
                        break;
                    case shader_member_type_e::FLOAT3:
                        member_changed = ranged ? ImGui::SliderFloat3(label, static_cast<f32*>(slot), member->range_min,
                                                                      member->range_max)
                                                : ImGui::DragFloat3(label, static_cast<f32*>(slot), 0.01f);
                        break;
                    case shader_member_type_e::FLOAT4:
                        member_changed = ranged ? ImGui::SliderFloat4(label, static_cast<f32*>(slot), member->range_min,
                                                                      member->range_max)
                                                : ImGui::DragFloat4(label, static_cast<f32*>(slot), 0.01f);
                        break;
                    case shader_member_type_e::INT:
                        member_changed = ranged ? ImGui::SliderInt(label, static_cast<i32*>(slot),
                                                                   static_cast<i32>(member->range_min),
                                                                   static_cast<i32>(member->range_max))
                                                : ImGui::DragInt(label, static_cast<i32*>(slot));
                        break;
                    case shader_member_type_e::UINT:
                    {
                        i32 as_int = static_cast<i32>(*static_cast<u32_t*>(slot));
                        const i32 lo = ranged ? static_cast<i32>(member->range_min) : 0;
                        const i32 hi = ranged ? static_cast<i32>(member->range_max) : INT_MAX;

                        if (ranged ? ImGui::SliderInt(label, &as_int, lo, hi)
                                   : ImGui::DragInt(label, &as_int, 1.0f, lo, hi))
                        {
                            *static_cast<u32_t*>(slot) = static_cast<u32_t>(as_int < 0 ? 0 : as_int);
                            member_changed = true;
                        }
                        break;
                    }
                    case shader_member_type_e::BOOL:
                    {
                        bool as_bool = *static_cast<u32_t*>(slot) != 0u;
                        if (ImGui::Checkbox(label, &as_bool))
                        {
                            *static_cast<u32_t*>(slot) = as_bool ? 1u : 0u;
                            member_changed = true;
                        }
                        break;
                    }
                    default: ImGui::TextDisabled("%s (%u bytes, unsupported type)", label, member->size); break;
                    }

                    draw_member_tooltip(*member);
                    break;
                }
                }

                ImGui::PopID();

                if (member_changed)
                {
                    changed = true;

                    if (std::find(mat->authored_properties.begin(), mat->authored_properties.end(), name_hash) ==
                        mat->authored_properties.end())
                    {
                        mat->authored_properties.push_back(name_hash);
                    }
                }
            }

            if (hidden_count > 0)
            {
                ImGui::TextDisabled("%u engine-bound %s hidden", hidden_count,
                                    hidden_count == 1 ? "property" : "properties");
            }

            if (changed) { mat->dirty_frames = tau::renderer::MAX_FRAMES_IN_FLIGHT; }

            return changed;
        }

        i32 string_resize_cb(ImGuiInputTextCallbackData* data)
        {
            if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
            {
                std::string* str = static_cast<std::string*>(data->UserData);
                str->resize(data->BufTextLen);
                data->Buf = str->data();
            }

            return 0;
        }

        bool enum_to_i32(const tau::reflection::any_t& value, i32& out)
        {
            const tau::reflection::any_t converted = value.allow_cast<i32>();
            if (!converted) { return false; }

            out = converted.cast<i32>();
            return true;
        }

        std::string_view asset_file_name(std::string_view path)
        {
            const std::size_t slash = path.find_last_of('/');
            return (slash == std::string_view::npos) ? path : path.substr(slash + 1);
        }

        std::string asset_folder_label(std::string_view path)
        {
            std::string_view rest = path;
            std::string protocol;

            const std::size_t proto = rest.find("://");
            if (proto != std::string_view::npos)
            {
                protocol = std::string(rest.substr(0, proto));
                rest = rest.substr(proto + 3);
            }

            constexpr std::string_view assets_prefix = "assets/";
            if (rest.substr(0, assets_prefix.size()) == assets_prefix) { rest = rest.substr(assets_prefix.size()); }

            const std::size_t slash = rest.find_last_of('/');
            const std::string_view folder =
                (slash == std::string_view::npos) ? std::string_view{} : rest.substr(0, slash);

            if (protocol.empty()) { return std::string(folder); }
            if (folder.empty()) { return protocol; }
            return protocol + ": " + std::string(folder);
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

        bool asset_unsaved(asset_handle_t handle)
        { return handle.is_valid() && history::is_dirty({history::doc_kind_e::ASSET, handle.guid}); }

        bool draw_asset_slot(tau::world_t& world, const char* label, u64_t asset_type_hash, asset_handle_t handle,
                             asset_handle_t& out_handle, const std::string& popup_id, bool* out_remove)
        {
            bool modified = false;
            const std::string cur_path = assets::path_of(handle);

            ImGui::Text("%s", label);
            ImGui::SameLine();

            const f32 remove_width = out_remove ? (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x) : 0.0f;
            const f32 field_width = std::max(ImGui::GetContentRegionAvail().x - remove_width, 60.0f);

            std::string name = cur_path.empty() ? "None" : std::string(asset_file_name(cur_path));
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
                if (ImGui::Button(("x" + popup_id).c_str(), ImVec2(ImGui::GetFrameHeight(), 0.0f)))
                {
                    *out_remove = true;
                }
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

            if (asset_type_hash == tau::get_type_id<tau::material_t>() && handle.is_valid())
            {
                ImGui::PushID(popup_id.c_str());
                ImGui::Indent();
                if (ImGui::TreeNode("Properties"))
                {
                    const history::target_t target = history::asset_target(asset_type_hash, handle);
                    history::watch(world, target);
                    if (draw_material_properties(world, handle)) { history::edited(world, target, "Edit Material"); }
                    ImGui::TreePop();
                }
                ImGui::Unindent();
                ImGui::PopID();
            }

            return modified;
        }

        bool draw_asset_list(tau::world_t& world, tau::reflection::data_t& data, tau::reflection::any_t& instance,
                             tau::reflection::any_t& field_value, const tau::reflection::editor_prop_t& prop, u32_t id)
        {
            std::vector<asset_handle_t> list = tau::reflection::assets_of(field_value);

            bool modified = false;
            const std::string base_id = "##list_" + std::to_string(id);

            ImGui::Text("%s", prop.name);
            ImGui::SameLine();
            if (ImGui::SmallButton(("+" + base_id).c_str()))
            {
                list.emplace_back();
                modified = true;
            }

            ImGui::Indent();

            i32 remove_index = -1;
            for (std::size_t i = 0; i < list.size(); i++)
            {
                const std::string row_label = std::to_string(i);
                const std::string row_id = base_id + "_" + std::to_string(i);

                bool remove = false;
                asset_handle_t new_handle = list[i];

                if (draw_asset_slot(world, row_label.c_str(), prop.asset_type_hash, list[i], new_handle, row_id,
                                    &remove))
                {
                    list[i] = new_handle;
                    modified = true;
                }

                if (remove) { remove_index = static_cast<i32>(i); }
            }

            if (remove_index >= 0)
            {
                list.erase(list.begin() + remove_index);
                modified = true;
            }

            if (list.empty()) { ImGui::TextDisabled("empty, slips to final pass"); }

            ImGui::Unindent();

            if (modified) { data.set(instance, list); }
            return modified;
        }

        // overridden: field keys differing from the prefab
        bool draw_meta_any(tau::world_t& world, tau::reflection::any_t& instance, tau::reflection::type_t type,
                           const std::unordered_set<std::string>* overridden = nullptr);

        std::string entity_name(tau::world_t& world, tau::ecs::entity_t entity)
        {
            if (entity == tau::ecs::NULL_ENTITY) { return "None"; }
            if (!world.registry.valid(entity)) { return "Missing"; }

            const tau::tag_t* tag = world.registry.try_get<tau::tag_t>(entity);
            return tag ? tag->name : "Entity " + std::to_string(tau::ecs::get_entity_id(entity));
        }

        bool draw_entity_ref(tau::world_t& world, const char* label, tau::ecs::entity_t& entity)
        {
            bool changed = false;

            if (ImGui::BeginCombo(label, entity_name(world, entity).c_str()))
            {
                if (ImGui::Selectable("None", entity == tau::ecs::NULL_ENTITY))
                {
                    entity = tau::ecs::NULL_ENTITY;
                    changed = true;
                }

                for (tau::ecs::entity_t candidate : world.registry.view<tau::ecs::entity_t>())
                {
                    ImGui::PushID(static_cast<i32>(tau::ecs::get_entity_id(candidate)));
                    if (ImGui::Selectable(entity_name(world, candidate).c_str(), candidate == entity))
                    {
                        entity = candidate;
                        changed = true;
                    }
                    ImGui::PopID();
                }

                ImGui::EndCombo();
            }

            return changed;
        }

        bool draw_vec3(const char* label, tau::vec3_t& vec, const tau::reflection::editor_prop_t* prop)
        {
            if (!prop || prop->unit != tau::reflection::unit_e::RADIANS)
            {
                return ImGui::DragFloat3(label, &vec.x, 0.1f);
            }

            constexpr f32 RAD2DEG = 57.2957795f;
            constexpr f32 DEG2RAD = 0.0174532925f;

            auto wrap_deg = [](f32 deg) { return std::fmod(deg, 360.0f); };

            const ImGuiID widget = ImGui::GetID(label);

            tau::vec3_t deg = {wrap_deg(vec.x * RAD2DEG), wrap_deg(vec.y * RAD2DEG), wrap_deg(vec.z * RAD2DEG)};
            if (active_degree_edit.widget == widget) { deg = active_degree_edit.degrees; }

            bool changed = false;
            if (ImGui::DragFloat3(label, &deg.x, 0.5f))
            {
                deg = {wrap_deg(deg.x), wrap_deg(deg.y), wrap_deg(deg.z)};
                vec = {deg.x * DEG2RAD, deg.y * DEG2RAD, deg.z * DEG2RAD};
                changed = true;
            }

            if (ImGui::IsItemActive())
            {
                active_degree_edit.widget = widget;
                active_degree_edit.degrees = deg;
            }
            else if (active_degree_edit.widget == widget) { active_degree_edit = degree_edit_t{}; }

            return changed;
        }

        bool draw_enum(tau::world_t& world, const char* label, tau::reflection::any_t& value,
                       tau::reflection::type_t type)
        {
            i32 cur_val = 0;
            if (!enum_to_i32(value, cur_val)) { return false; }

            const char* preview_name = "Unknown";
            for (auto [enum_data_id, enum_data] : type.data())
            {
                i32 candidate = 0;
                if (!enum_to_i32(enum_data.get({}), candidate)) { continue; }

                if (candidate == cur_val)
                {
                    const auto* enum_prop = static_cast<tau::reflection::editor_prop_t*>(enum_data.custom());
                    preview_name = enum_prop ? enum_prop->name : "Selected";
                }
            }

            bool changed = false;
            if (ImGui::BeginCombo(label, preview_name))
            {
                for (auto [enum_data_id, enum_data] : type.data())
                {
                    i32 enum_val = 0;
                    if (!enum_to_i32(enum_data.get({}), enum_val)) { continue; }

                    const tau::reflection::editor_prop_t* enum_prop =
                        static_cast<tau::reflection::editor_prop_t*>(enum_data.custom());
                    const char* enum_name = enum_prop ? enum_prop->name : "Option";

                    bool is_selected = (cur_val == enum_val);
                    if (ImGui::Selectable(enum_name, is_selected))
                    {
                        changed = value.assign(tau::reflection::any_t{*world.reflection_ctx, enum_val});
                    }
                    if (is_selected) { ImGui::SetItemDefaultFocus(); }
                }

                ImGui::EndCombo();
            }

            return changed;
        }

        // removes and moves rebuild the list, its iterators only step one at a time
        void rebuild_list(tau::reflection::any_t& value, const std::vector<std::size_t>& order)
        {
            tau::reflection::any_t original = value;
            auto source = original.as_sequence_container();
            auto target = value.as_sequence_container();

            target.clear();
            for (const std::size_t index : order) { target.insert(target.end(), source[index]); }
        }

        bool draw_value(tau::world_t& world, const char* label, tau::reflection::any_t& value,
                        tau::reflection::type_t type, const tau::reflection::editor_prop_t* prop);

        bool draw_list(tau::world_t& world, const char* label, tau::reflection::any_t& value)
        {
            auto list = value.as_sequence_container();
            const tau::reflection::type_t element_type = list.value_type();

            const bool open = ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_DefaultOpen,
                                                "%s (%zu)", label, list.size());

            bool changed = false;
            ImGui::SameLine();
            if (ImGui::SmallButton("+"))
            {
                // a value initialised entity_t is entity 0, not null
                if (element_type == tau::reflection::resolve<tau::ecs::entity_t>(*world.reflection_ctx))
                {
                    list.insert(list.end(), tau::reflection::any_t{*world.reflection_ctx, tau::ecs::NULL_ENTITY});
                }
                else
                {
                    list.insert(list.end(), element_type.construct());
                }
                changed = true;
            }

            if (!open) { return changed; }

            i64 remove_at = -1;
            i64 move_up = -1;

            i64 index = 0;
            for (tau::reflection::any_t element : list)
            {
                ImGui::PushID(static_cast<i32>(index));

                if (ImGui::SmallButton("x")) { remove_at = index; }
                ImGui::SameLine();
                ImGui::BeginDisabled(index == 0);
                if (ImGui::ArrowButton("up", ImGuiDir_Up)) { move_up = index; }
                ImGui::EndDisabled();
                ImGui::SameLine();

                const std::string element_label = "[" + std::to_string(index) + "]";
                if (draw_value(world, element_label.c_str(), element, element_type, nullptr)) { changed = true; }

                ImGui::PopID();
                index++;
            }

            if (remove_at >= 0 || move_up > 0)
            {
                std::vector<std::size_t> order(list.size());
                std::iota(order.begin(), order.end(), std::size_t{0});

                if (remove_at >= 0) { order.erase(order.begin() + remove_at); }
                else
                {
                    std::swap(order[static_cast<std::size_t>(move_up) - 1], order[static_cast<std::size_t>(move_up)]);
                }

                rebuild_list(value, order);
                changed = true;
            }

            ImGui::TreePop();
            return changed;
        }

        // edits value in place, it is a field copy or a reference into a list
        bool draw_value(tau::world_t& world, const char* label, tau::reflection::any_t& value,
                        tau::reflection::type_t type, const tau::reflection::editor_prop_t* prop)
        {
            tau::reflection::ctx_t& ctx = *world.reflection_ctx;

            if (type == tau::reflection::resolve<i32>(ctx)) { return ImGui::DragInt(label, &value.cast<i32&>()); }
            if (type == tau::reflection::resolve<u32>(ctx))
            {
                return ImGui::DragScalar(label, ImGuiDataType_U32, &value.cast<u32&>());
            }
            if (type == tau::reflection::resolve<f32>(ctx))
            {
                return ImGui::DragFloat(label, &value.cast<f32&>(), 0.1f);
            }
            if (type == tau::reflection::resolve<bool>(ctx)) { return ImGui::Checkbox(label, &value.cast<bool&>()); }
            if (type == tau::reflection::resolve<std::string>(ctx))
            {
                std::string& target = value.cast<std::string&>();
                std::string str = target;
                if (str.capacity() < 32) { str.reserve(32); }

                if (ImGui::InputText(label, str.data(), str.capacity() + 1, ImGuiInputTextFlags_CallbackResize,
                                     string_resize_cb, &str))
                {
                    target = std::string(str.data());
                    return true;
                }
                return false;
            }
            if (type == tau::reflection::resolve<tau::vec3_t>(ctx))
            {
                return draw_vec3(label, value.cast<tau::vec3_t&>(), prop);
            }
            // before the enum check, entities are an enum
            if (type == tau::reflection::resolve<tau::ecs::entity_t>(ctx))
            {
                return draw_entity_ref(world, label, value.cast<tau::ecs::entity_t&>());
            }
            if (type.is_enum()) { return draw_enum(world, label, value, type); }
            if (type.is_sequence_container()) { return draw_list(world, label, value); }
            if (type.data().begin() != type.data().end())
            {
                if (!ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen)) { return false; }

                const bool changed = draw_meta_any(world, value, type);
                ImGui::TreePop();
                return changed;
            }

            ImGui::TextDisabled("%s: unsupported type", label);
            return false;
        }

        std::string meta_key(const char* name, u32_t id) { return name ? std::string(name) : std::to_string(id); }

        constexpr ImVec4 PREFAB_COLOR = {0.45f, 0.72f, 1.0f, 1.0f};

        void mark_override()
        {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x - 6.0f, at.y + 1.0f),
                                                      ImVec2(at.x - 3.0f, at.y + ImGui::GetFrameHeight() - 1.0f),
                                                      ImGui::GetColorU32(PREFAB_COLOR));
        }

        bool draw_meta_any(tau::world_t& world, tau::reflection::any_t& instance, tau::reflection::type_t type,
                           const std::unordered_set<std::string>* overridden)
        {
            bool was_modified = false;

            for (auto [id, data] : type.data())
            {
                tau::reflection::editor_prop_t* prop = data.custom();
                const char* label = prop ? prop->name : "Unknown";
                if (overridden && overridden->contains(meta_key(data.name(), id))) { mark_override(); }

                tau::reflection::any_t field_value = data.get(instance);

                if (prop && prop->asset_type_hash != 0 && prop->is_list)
                {
                    if (draw_asset_list(world, data, instance, field_value, *prop, id)) { was_modified = true; }
                    continue;
                }

                if (prop && prop->asset_type_hash != 0)
                {
                    const asset_handle_t handle = tau::reflection::asset_of(field_value);
                    asset_handle_t new_handle = handle;

                    if (draw_asset_slot(world, label, prop->asset_type_hash, handle, new_handle,
                                        "##asset_" + std::to_string(id)))
                    {
                        data.set(instance, new_handle);
                        was_modified = true;
                    }
                    continue;
                }

                ImGui::PushID(static_cast<i32>(id));
                if (draw_value(world, label, field_value, data.type(), prop))
                {
                    data.set(instance, field_value);
                    was_modified = true;
                }
                ImGui::PopID();
            }

            return was_modified;
        }
        struct inspected_t
        {
            std::string path;
            asset_handle_t handle;
        };

        inspected_t inspected;

        void release_inspected()
        {
            assets::unpin(inspected.handle);
            inspected = {};
        }

        void draw_asset_inspector(tau::world_t& world, editor_context_t& ctx)
        {
            const std::string& path = ctx.inspected_asset;
            const asset_type_t* type = assets::type_by_path(path);
            if (!type) { return; }

            if (inspected.path != path)
            {
                release_inspected();
                inspected.path = path;
                if (type->id == tau::get_type_id<material_t>() || type->id == tau::get_type_id<texture_t>())
                {
                    inspected.handle = assets::pin(assets::load(type->id, path));
                }
            }

            const history::doc_t doc = {history::doc_kind_e::ASSET, inspected.handle.guid};
            const bool unsaved = inspected.handle.is_valid() && history::is_dirty(doc);
            ImGui::TextUnformatted((std::string(asset_file_name(path)) + (unsaved ? "*" : "")).c_str());
            if (ImGui::BeginPopupContextItem("##asset_menu"))
            {
                const bool editing = ctx.cur_mode == editor_mode_e::EDIT;
                if (ImGui::MenuItem("Save", nullptr, false, unsaved && editing)) { history::save_asset(doc); }
                if (ImGui::MenuItem("Revert", nullptr, false, unsaved && editing)) { history::revert_asset(doc); }
                ImGui::EndPopup();
            }
            ImGui::TextDisabled("%s, %s", std::string(type->display_name).c_str(), path.c_str());
            const asset_guid_t guid = assets::guid_of(path);
            ImGui::TextDisabled("GUID %s", guid != 0 ? assets::guid_text(guid).c_str() : "none yet");
            ImGui::Separator();

            if (type->id == tau::get_type_id<material_t>())
            {
                const history::target_t target = history::asset_target(type->id, inspected.handle);
                history::watch(world, target);
                if (draw_material_properties(world, inspected.handle))
                {
                    history::edited(world, target, "Edit Material");
                }
            }
            else if (const texture_t* tex = type->id == tau::get_type_id<texture_t>()
                                                ? assets::get(asset_t<texture_t>{inspected.handle})
                                                : nullptr)
            {
                ImGui::Text("%d x %d", tex->width, tex->height);
                const f32 width = std::min(ImGui::GetContentRegionAvail().x, 512.0f);
                if (tex->width > 0)
                {
                    ImGui::Image((ImTextureID)(intptr_t)tex->bindless_id,
                                 ImVec2(width, width * static_cast<f32>(tex->height) / static_cast<f32>(tex->width)));
                }
            }
            else if (type->key == "scene" && path.starts_with("game://assets/"))
            {
                if (ImGui::Button("Open Scene"))
                {
                    ctx.unsaved_action_scene =
                        asset_files::source_path(path.substr(std::strlen("game://assets/"))).string();
                    request_action(world, ctx, unsaved_action_e::LOAD_SCENE);
                }
            }
        }
        enum class prefab_action_e : u8_t
        {
            NONE,
            OPEN,
            REVERT,
            APPLY,
            UNPACK,
        };

        struct pending_prefab_t
        {
            prefab_action_e action = prefab_action_e::NONE;
            u64_t root = 0;
            u64_t source = 0;
        };

        pending_prefab_t pending_prefab;

        void draw_prefab_header(tau::world_t& world, editor_context_t& ctx, ecs::entity_t root, u64_t source)
        {
            const std::string path = prefab_edit::prefab_path(world, root);
            const prefab_instance_t& instance = world.registry.get<prefab_instance_t>(root);
            const bool unsaved = asset_unsaved(instance.prefab);

            ImGui::TextColored(PREFAB_COLOR, "Prefab");
            ImGui::SameLine();
            ImGui::TextUnformatted(path.empty() ? "missing"
                                                : (std::string(asset_file_name(path)) + (unsaved ? "*" : "")).c_str());
            if (ImGui::IsItemHovered() && !path.empty()) { ImGui::SetTooltip("%s", path.c_str()); }

            const u64_t root_guid = guid_system::of(world.registry, root);
            ImGui::BeginDisabled(ctx.cur_mode != editor_mode_e::EDIT || path.empty());
            if (ImGui::SmallButton("Select"))
            {
                ctx.inspected_asset = path;
                ctx.selection.clear();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Open")) { pending_prefab = {prefab_action_e::OPEN, root_guid, source}; }
            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Edit the prefab on its own, its instances follow"); }
            ImGui::SameLine();
            if (ImGui::SmallButton("Revert")) { pending_prefab = {prefab_action_e::REVERT, root_guid}; }
            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Back to the prefab, the root keeps its placement"); }
            ImGui::SameLine();
            ImGui::BeginDisabled(prefab_stage::is_open());
            if (ImGui::SmallButton("Apply")) { pending_prefab = {prefab_action_e::APPLY, root_guid}; }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip(prefab_stage::is_open()
                                      ? "Not inside an open prefab, Open edits this one"
                                      : "The prefab becomes this instance, its other instances follow");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Unpack")) { pending_prefab = {prefab_action_e::UNPACK, root_guid}; }
            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Plain entities, no longer linked to the prefab"); }
            ImGui::EndDisabled();
            ImGui::Separator();
        }

        void run_prefab_action(tau::world_t& world, editor_context_t& ctx)
        {
            const pending_prefab_t pending = pending_prefab;
            pending_prefab = {};

            const ecs::entity_t root = guid_system::find(world.registry, pending.root);
            if (root == ecs::NULL_ENTITY) { return; }

            switch (pending.action)
            {
            case prefab_action_e::NONE: break;
            case prefab_action_e::OPEN:
                prefab_stage::open(world, ctx, prefab_edit::prefab_path(world, root), pending.source);
                break;
            case prefab_action_e::REVERT: prefab_edit::revert(world, ctx, root); break;
            case prefab_action_e::APPLY: prefab_edit::apply(world, ctx, root); break;
            case prefab_action_e::UNPACK: prefab_edit::unpack(world, ctx, root); break;
            }
        }
    } // namespace

    void draw_inspector(world_t& world, editor_context_t& ctx)
    {
        const ecs::entity_t selected_entity = guid_system::find(world.registry, ctx.selection.primary());

        if (ImGui::Begin("Inspector"))
        {
            if (selected_entity != tau::ecs::NULL_ENTITY && world.registry.valid(selected_entity))
            {
                ctx.inspected_asset.clear();

                const history::target_t target = history::entity_target(ctx.selection.primary());
                history::watch(world, target);

                const ecs::entity_t prefab_root = prefab_edit::instance_root(world, selected_entity);
                prefab_edit::overrides_t overrides;
                if (prefab_root != ecs::NULL_ENTITY)
                {
                    overrides = prefab_edit::overrides_of(world, selected_entity);
                    draw_prefab_header(world, ctx, prefab_root,
                                       world.registry.get<prefab_part_t>(selected_entity).source);
                }

                for (auto [id, type] : tau::reflection::resolve(*world.reflection_ctx))
                {
                    if (tau::reflection::func_t get_func = type.func("get"_h); get_func)
                    {
                        tau::reflection::any_t instance =
                            get_func.invoke({}, tau::reflection::forward_as_meta(world.registry), selected_entity);

                        if (instance)
                        {
                            tau::reflection::editor_prop_t* type_prop = type.custom();
                            const char* header_name = type_prop ? type_prop->name : "UnknownComponent";

                            ImGui::PushID(static_cast<i32>(id));

                            // vs prefab: missing component or differing fields
                            const auto changed_fields =
                                overrides.find(meta_key(type.name(), static_cast<u32_t>(type.id())));
                            const bool added =
                                changed_fields != overrides.end() && changed_fields->second.contains("*");
                            if (changed_fields != overrides.end()) { mark_override(); }
                            const std::string header =
                                std::string(header_name) + (added ? "  (added)" : "") + "###" + header_name;

                            bool header_open = ImGui::CollapsingHeader(
                                header.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

                            ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
                            if (ImGui::Button("X"))
                            {
                                if (tau::reflection::func_t remove_func = type.func("remove"_h); remove_func)
                                {
                                    remove_func.invoke({}, tau::reflection::forward_as_meta(world.registry),
                                                       selected_entity);
                                    history::edited(world, target, std::string("Remove ") + header_name);
                                }
                            }

                            if (header_open)
                            {
                                bool changed = draw_meta_any(world, instance, type,
                                                             changed_fields != overrides.end() ? &changed_fields->second
                                                                                               : nullptr);

                                if (changed)
                                {
                                    if (tau::reflection::func_t on_changed = type.func("on_changed"_h); on_changed)
                                    {
                                        on_changed.invoke({}, tau::reflection::forward_as_meta(world.registry),
                                                          selected_entity);
                                    }
                                    history::edited(world, target, std::string("Edit ") + header_name);
                                }
                            }

                            ImGui::PopID();
                        }
                    }
                }

                if (const unresolved_components_t* kept = world.registry.try_get<unresolved_components_t>(selected_entity))
                {
                    const nlohmann::json saved = nlohmann::json::parse(kept->json, nullptr, false);
                    if (saved.is_object())
                    {
                        for (const auto& [key, fields] : saved.items())
                        {
                            ImGui::TextDisabled("%s: type not registered, kept as saved", key.c_str());
                        }
                    }
                }

                ImGui::Separator();

                if (ImGui::Button("Add Component")) { ImGui::OpenPopup("AddComponentPopup"); }

                if (ImGui::BeginPopup("AddComponentPopup"))
                {
                    for (auto [id, type] : tau::reflection::resolve(*world.reflection_ctx))
                    {
                        tau::reflection::func_t add_func = type.func("add"_h);
                        tau::reflection::func_t get_func = type.func("get"_h);

                        if (add_func && get_func)
                        {
                            tau::reflection::any_t instance =
                                get_func.invoke({}, tau::reflection::forward_as_meta(world.registry), selected_entity);

                            if (!instance)
                            {
                                tau::reflection::editor_prop_t* type_prop = type.custom();
                                const char* menu_name = type_prop ? type_prop->name : "UnknownComponent";

                                if (ImGui::MenuItem(menu_name))
                                {
                                    add_func.invoke({}, tau::reflection::forward_as_meta(world.registry),
                                                    selected_entity);
                                    history::edited(world, target, std::string("Add ") + menu_name);
                                    ImGui::CloseCurrentPopup();
                                }
                            }
                        }
                    }

                    ImGui::EndPopup();
                }
            }
            else if (!ctx.inspected_asset.empty()) { draw_asset_inspector(world, ctx); }
        }

        if (ctx.inspected_asset.empty() && !inspected.path.empty()) { release_inspected(); }

        ImGui::End();
        run_prefab_action(world, ctx);
    }
} // namespace tau::editor::panels