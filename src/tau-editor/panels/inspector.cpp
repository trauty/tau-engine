#include "inspector.h"

#include "imgui.h"
#include "tau-editor/asset_cook.h"
#include "tau-editor/asset_watch.h"
#include "tau-editor/project_manager.h"
#include "tau/asset.h"
#include "tau/asset_meta.h"
#include "tau/asset_serde.h"
#include "tau/assets/material.h"
#include "tau/assets/shader.h"
#include "tau/components/tag.h"
#include "tau/ecs_fwd.h"
#include "tau/engine.h"
#include "tau/hash.h"
#include "tau/log.h"
#include "tau/reflection.h"
#include "tau/vfs.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <json/json.hpp>
#include <numeric>
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

        struct source_roots_t
        {
            std::filesystem::path engine;
            std::filesystem::path game;
        };

        source_roots_t source_roots;

        std::filesystem::path resolve_source_path(const std::string& virtual_path)
        {
            const tau::vfs::path_parts_t parts = tau::vfs::split_protocol(virtual_path);
            if (parts.protocol.empty()) { return {}; }

            // the protocol includes its "://", the roots are keyed by the bare name
            const std::string protocol(parts.protocol.substr(0, parts.protocol.size() - 3));
            std::string rel(parts.rest);

            constexpr std::string_view assets_prefix = "assets/";
            if (rel.rfind(assets_prefix, 0) == 0) { rel.erase(0, assets_prefix.size()); }

            const std::filesystem::path& root = (protocol == "engine") ? source_roots.engine : source_roots.game;
            if (protocol != "engine" && protocol != "game") { return {}; }
            if (root.empty()) { return {}; }

            return root / rel;
        }

        bool save_material(asset_handle_t handle, std::string& out_err)
        {
            tau::asset_registry_t& assets = tau::engine::get_asset_registry();

            material_t* mat = assets.get<material_t>(handle);
            shader_t* shader = mat ? assets.get<shader_t>(mat->shader_handle) : nullptr;
            if (!mat || !shader)
            {
                out_err = "material or its shader is not loaded";
                return false;
            }

            const std::string virtual_path = assets.get_path(handle);
            const std::filesystem::path target = resolve_source_path(virtual_path);
            if (target.empty())
            {
                out_err = "cannot map '" + virtual_path + "' to a source file";
                return false;
            }

            nlohmann::json out;
            // the material's identity, kept inside it, scenes reference the material by it
            out["guid"] = tau::asset_meta::guid_for_writing(target.string());

            const std::string shader_path = assets.get_path(mat->shader_handle);
            out["shader"] = shader_path;

            // record the guid next to the path so renaming the shader does not orphan this material
            // the path stays for readability and as the fallback
            const std::string_view shader_guid = tau::asset_meta::get_guid(shader_path);
            if (!shader_guid.empty()) { out["shader_guid"] = std::string(shader_guid); }

            nlohmann::json properties = nlohmann::json::object();
            nlohmann::json textures = nlohmann::json::object();
            nlohmann::json texture_guids = nlohmann::json::object();
            nlohmann::json samplers = nlohmann::json::object();

            for (u32_t name_hash : mat->authored_properties)
            {
                const shader_member_t* it = shader->module.members.find(name_hash);
                if (it == nullptr) { continue; }

                const shader_member_t& member = *it;
                if (member.edit == shader_member_edit_e::HIDDEN) { continue; }
                if (member.offset + member.size > mat->data.size()) { continue; }

                const void* slot = mat->data.data() + member.offset;

                if (member.edit == shader_member_edit_e::TEXTURE)
                {
                    // keep what the file named, not the fallback drawn in its place
                    std::string tex_path;
                    if (const std::string* missing = mat->missing_textures.find(name_hash)) { tex_path = *missing; }
                    else if (const asset_handle_t* bound = mat->bound_textures.find(name_hash);
                             bound != nullptr && bound->is_valid())
                    {
                        tex_path = assets.get_path(*bound);
                    }

                    if (!tex_path.empty())
                    {
                        textures[member.name] = tex_path;

                        const std::string_view tex_guid = tau::asset_meta::get_guid(tex_path);
                        if (!tex_guid.empty()) { texture_guids[member.name] = std::string(tex_guid); }
                    }
                    continue;
                }

                if (member.edit == shader_member_edit_e::SAMPLER)
                {
                    samplers[member.name] = *static_cast<const u32_t*>(slot);
                    continue;
                }

                switch (member.type)
                {
                case shader_member_type_e::FLOAT: properties[member.name] = *static_cast<const f32*>(slot); break;
                case shader_member_type_e::FLOAT2:
                case shader_member_type_e::FLOAT3:
                case shader_member_type_e::FLOAT4:
                {
                    const u32_t count = member.size / sizeof(f32);
                    const f32* values = static_cast<const f32*>(slot);
                    properties[member.name] = std::vector<f32>(values, values + count);
                    break;
                }
                case shader_member_type_e::INT: properties[member.name] = *static_cast<const i32*>(slot); break;
                case shader_member_type_e::UINT: properties[member.name] = *static_cast<const u32_t*>(slot); break;
                case shader_member_type_e::BOOL:
                    properties[member.name] = (*static_cast<const u32_t*>(slot) != 0u);
                    break;
                default: break;
                }
            }

            if (!properties.empty()) { out["properties"] = std::move(properties); }
            if (!textures.empty()) { out["textures"] = std::move(textures); }
            if (!texture_guids.empty()) { out["texture_guids"] = std::move(texture_guids); }
            if (!samplers.empty()) { out["samplers"] = std::move(samplers); }

            std::error_code ec;
            std::filesystem::create_directories(target.parent_path(), ec);

            std::ofstream file(target);
            if (!file.is_open())
            {
                out_err = "cannot open " + target.string();
                return false;
            }

            file << out.dump(4) << '\n';
            file.close();

            TAU_LOG_INFO("EDITOR", "Material saved to {}", target.string());

            // our own write, the watcher must not take it for an outside edit and cook again
            asset_watch::ignore_own_write(target);

            std::string cook_err;
            if (!asset_cook::cook_asset(target, cook_err))
            {
                out_err = "saved " + target.string() + " but could not reimport it: " + cook_err;
                return false;
            }

            // the cooked .taumat is fresh, update the registry copy too since everything draws from it
            if (!assets.reload<material_t>(virtual_path))
            {
                out_err = "saved and reimported " + target.string() + " but could not reload it";
                return false;
            }

            return true;
        }

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

        bool draw_asset_slot(const char* label, u64_t asset_type_hash, asset_handle_t handle,
                             asset_handle_t& out_handle, const std::string& popup_id, bool* out_remove = nullptr);

        bool draw_material_properties(asset_handle_t handle)
        {
            tau::asset_registry_t& assets = tau::engine::get_asset_registry();

            material_t* mat = assets.get<material_t>(handle);
            if (!mat) { return false; }

            shader_t* shader = assets.get<shader_t>(mat->shader_handle);
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
                    const asset_handle_t* bound = mat->bound_textures.find(name_hash);
                    const asset_handle_t cur = bound ? *bound : asset_handle_t{};

                    asset_handle_t picked = cur;
                    if (draw_asset_slot(label, tau::get_type_id<tau::texture_t>(), cur, picked,
                                        "##mattex_" + std::to_string(member->offset)))
                    {
                        // the picker took a reference, set_texture takes it over and returns the previous one
                        if (picked.is_valid()) { mat->set_texture(name_hash, picked); }
                        else
                        {
                            mat->clear_texture(name_hash);
                        }
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

            for (std::string& path : tau::asset_meta::all_paths())
            {
                if (tau::asset_serde::path_matches_type(asset_type_hash, path))
                {
                    asset_picker.candidates.push_back(std::move(path));
                }
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
            ImGui::TextDisabled("%s", std::string(tau::asset_serde::display_name(asset_type_hash)).c_str());

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

        void draw_material_save_row(asset_handle_t handle)
        {
            material_t* mat = tau::engine::get_asset_registry().get<material_t>(handle);
            if (!mat) { return; }

            static std::string last_error;

            if (ImGui::SmallButton("Save to .mat"))
            {
                last_error.clear();
                if (!save_material(handle, last_error))
                {
                    TAU_LOG_ERROR("EDITOR", "Failed to save material: {}", last_error);
                }
            }

            ImGui::SameLine();
            ImGui::TextDisabled("%zu authored", mat->authored_properties.size());

            if (!last_error.empty()) { ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", last_error.c_str()); }
        }

        bool draw_asset_slot(const char* label, u64_t asset_type_hash, asset_handle_t handle,
                             asset_handle_t& out_handle, const std::string& popup_id, bool* out_remove)
        {
            bool modified = false;
            std::string cur_path = tau::engine::get_asset_registry().get_path(handle);

            ImGui::Text("%s", label);
            ImGui::SameLine();

            const f32 remove_width = out_remove ? (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x) : 0.0f;
            const f32 field_width = std::max(ImGui::GetContentRegionAvail().x - remove_width, 60.0f);

            const std::string name = cur_path.empty() ? "None" : std::string(asset_file_name(cur_path));

            if (ImGui::Button((name + popup_id).c_str(), ImVec2(field_width, 0.0f)))
            {
                open_asset_picker(popup_id, asset_type_hash);
                ImGui::OpenPopup(popup_id.c_str());
            }

            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", cur_path.empty() ? "nothing assigned, click to pick" : cur_path.c_str());
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
                    out_handle = picked.empty() ? asset_handle_t{}
                                                : tau::asset_serde::load(asset_type_hash,
                                                                         tau::engine::get_asset_registry(), picked);
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
                    draw_material_properties(handle);
                    draw_material_save_row(handle);
                    ImGui::TreePop();
                }
                ImGui::Unindent();
                ImGui::PopID();
            }

            return modified;
        }

        bool draw_asset_list(tau::reflection::data_t& data, tau::reflection::any_t& instance,
                             tau::reflection::any_t& field_value, const tau::reflection::editor_prop_t& prop, u32_t id)
        {
            std::vector<asset_handle_t> list = field_value.cast<std::vector<asset_handle_t>>();

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

                if (draw_asset_slot(row_label.c_str(), prop.asset_type_hash, list[i], new_handle, row_id, &remove))
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

        bool draw_meta_any(tau::world_t& world, tau::reflection::any_t& instance, tau::reflection::type_t type);

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

        bool draw_meta_any(tau::world_t& world, tau::reflection::any_t& instance, tau::reflection::type_t type)
        {
            bool was_modified = false;

            for (auto [id, data] : type.data())
            {
                tau::reflection::editor_prop_t* prop = data.custom();
                const char* label = prop ? prop->name : "Unknown";

                tau::reflection::any_t field_value = data.get(instance);

                if (prop && prop->asset_type_hash != 0 && prop->is_list)
                {
                    if (draw_asset_list(data, instance, field_value, *prop, id)) { was_modified = true; }
                    continue;
                }

                if (prop && prop->asset_type_hash != 0)
                {
                    asset_handle_t handle = field_value.cast<asset_handle_t>();
                    asset_handle_t new_handle = handle;

                    if (draw_asset_slot(label, prop->asset_type_hash, handle, new_handle,
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
    } // namespace

    void draw_inspector(world_t& world, editor_context_t& ctx)
    {
        if (source_roots.game.empty() && !ctx.project_dir.empty())
        {
            source_roots.game = std::filesystem::path(ctx.project_dir) / "assets";
        }

        if (source_roots.engine.empty())
        {
            const std::filesystem::path engine = project_manager::engine_dir();
            if (!engine.empty())
            {
                const std::filesystem::path installed = engine / "share" / "tau" / "engine-assets-src";
                source_roots.engine = std::filesystem::is_directory(installed) ? installed : engine / "assets";
            }
        }

        ecs::entity_t selected_entity = ctx.selected_entity;

        if (ImGui::Begin("Inspector"))
        {
            if (selected_entity != tau::ecs::NULL_ENTITY && world.registry.valid(selected_entity))
            {
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

                            bool header_open = ImGui::CollapsingHeader(
                                header_name, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

                            ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
                            if (ImGui::Button("X"))
                            {
                                if (tau::reflection::func_t remove_func = type.func("remove"_h); remove_func)
                                {
                                    remove_func.invoke({}, tau::reflection::forward_as_meta(world.registry),
                                                       selected_entity);
                                }
                            }

                            if (header_open)
                            {
                                bool changed = draw_meta_any(world, instance, type);

                                if (changed)
                                {
                                    if (tau::reflection::func_t on_changed = type.func("on_changed"_h); on_changed)
                                    {
                                        on_changed.invoke({}, tau::reflection::forward_as_meta(world.registry),
                                                          selected_entity);
                                    }
                                }
                            }

                            ImGui::PopID();
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
                                    ImGui::CloseCurrentPopup();
                                }
                            }
                        }
                    }

                    ImGui::EndPopup();
                }
            }
        }

        ImGui::End();
    }
} // namespace tau::editor::panels