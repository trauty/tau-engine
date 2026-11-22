#include "material_edit.h"

#include "imgui.h"
#include "tau-editor/asset_editors.h"
#include "tau-editor/asset_files.h"
#include "tau-editor/editor_context.h"
#include "tau-editor/history.h"
#include "tau-editor/widgets.h"
#include "tau/asset.h"
#include "tau/asset_sources.h"
#include "tau/assets/material.h"
#include "tau/assets/shader.h"
#include "tau/assets/texture.h"
#include "tau/hash.h"
#include "tau/log.h"
#include "tau/rhi/renderer.h"
#include "tau/rhi/renderer_constants.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <json/json.hpp>
#include <string>
#include <vector>

namespace tau::editor::material_edit
{
    namespace
    {
        // float as double gains digits (0.9f -> 0.8999999761581421), churns diffs
        f64 tidy(f32 value) { return std::round(static_cast<f64>(value) * 1e6) / 1e6; }

        std::string named_texture(const material_t& mat, u32_t name_hash)
        {
            const asset_t<texture_t>* bound = mat.textures.find(name_hash);
            return bound && bound->is_valid() ? assets::path_of(*bound) : std::string{};
        }

        material_t* material_of(asset_handle_t handle) { return assets::get(asset_t<material_t>{handle}); }

        // by member name, so a state survives the shader's layout changing under it
        std::vector<u8_t> capture(asset_handle_t handle)
        {
            const material_t* mat = material_of(handle);
            const shader_t* shader = mat ? assets::get(mat->shader) : nullptr;
            if (!shader) { return {}; }

            nlohmann::json members = nlohmann::json::object();
            nlohmann::json textures = nlohmann::json::object();

            for (auto [name_hash, member] : shader->module.members)
            {
                if (member.edit == shader_member_edit_e::HIDDEN) { continue; }

                const std::string key = std::to_string(name_hash);
                if (member.edit == shader_member_edit_e::TEXTURE)
                {
                    const std::string path = named_texture(*mat, name_hash);
                    if (!path.empty()) { textures[key] = path; }
                    continue;
                }

                if (member.offset + member.size > mat->data.size()) { continue; }
                const u8_t* bytes = mat->data.data() + member.offset;
                members[key] = nlohmann::json::binary_t(std::vector<u8_t>(bytes, bytes + member.size));
            }

            const nlohmann::json state = {
                {"shader",   assets::path_of(mat->shader)},
                {"members",  std::move(members)          },
                {"textures", std::move(textures)         },
                {"authored", mat->authored_properties    },
            };
            return nlohmann::json::to_cbor(state);
        }

        bool apply(asset_handle_t handle, std::span<const u8_t> bytes)
        {
            material_t* mat = material_of(handle);
            const shader_t* shader = mat ? assets::get(mat->shader) : nullptr;
            if (!shader) { return false; }

            nlohmann::json state = nlohmann::json::from_cbor(bytes, true, false);
            if (state.is_discarded()) { return false; }

            const std::string shader_path = state.value("shader", std::string{});
            if (!shader_path.empty() && shader_path != assets::path_of(mat->shader))
            {
                if (!mat->set_shader(assets::load<shader_t>(shader_path))) { return false; }
                shader = assets::get(mat->shader);
            }

            const nlohmann::json& members = state["members"];
            const nlohmann::json& textures = state["textures"];

            for (auto [name_hash, member] : shader->module.members)
            {
                if (member.edit == shader_member_edit_e::HIDDEN) { continue; }

                const std::string key = std::to_string(name_hash);
                if (member.edit == shader_member_edit_e::TEXTURE)
                {
                    const std::string path = textures.value(key, std::string{});
                    if (path == named_texture(*mat, name_hash)) { continue; }

                    if (path.empty()) { mat->clear_texture(name_hash); }
                    else
                    {
                        mat->set_texture(name_hash, assets::load<texture_t>(path));
                    }
                    continue;
                }

                if (!members.contains(key)) { continue; }
                const nlohmann::json::binary_t& value = members[key].get_binary();
                if (value.size() == member.size) { mat->set_property_raw(name_hash, value.data(), member.size); }
            }

            mat->authored_properties = state["authored"].get<std::vector<u32_t>>();
            mat->dirty_frames = renderer::MAX_FRAMES_IN_FLIGHT;
            return true;
        }

        bool write(asset_handle_t handle, std::filesystem::path& out_source)
        {
            const std::string virtual_path = assets::path_of(handle);
            const std::filesystem::path target = asset_files::source_of(virtual_path);
            if (target.empty())
            {
                TAU_LOG_ERROR("EDITOR", "Cannot map '{}' to a source file", virtual_path);
                return false;
            }

            nlohmann::json out = to_json(handle);
            if (out.is_null())
            {
                TAU_LOG_ERROR("EDITOR", "Cannot save a material whose shader is not loaded");
                return false;
            }
            out["guid"] = assets::guid_text(asset_sources::guid_for_writing(target.string()));

            std::error_code ec;
            std::filesystem::create_directories(target.parent_path(), ec);

            std::ofstream file(target);
            if (!file.is_open())
            {
                TAU_LOG_ERROR("EDITOR", "Cannot open {}", target.string());
                return false;
            }

            file << out.dump(4) << '\n';
            file.close();

            TAU_LOG_INFO("EDITOR", "Material saved to {}", target.string());
            out_source = target;
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

        bool draw_properties(world_t& world, asset_handle_t handle)
        {
            material_t* mat = assets::get(asset_t<material_t>{handle});
            if (!mat) { return false; }

            asset_handle_t picked_shader = mat->shader;
            if (widgets::asset_slot(world, "Shader", get_type_id<shader_t>(), mat->shader, picked_shader, "##shader") &&
                picked_shader != mat->shader.handle && mat->set_shader(asset_t<shader_t>{picked_shader}))
            {
                return true;
            }

            const shader_t* shader = assets::get(mat->shader);
            if (!shader || mat->data.empty()) { return false; }

            std::vector<const shader_member_t*> ordered;
            ordered.reserve(shader->module.members.size());
            for (auto [hash, member] : shader->module.members) { ordered.push_back(&member); }
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
                    if (widgets::asset_slot(world, label, tau::get_type_id<tau::texture_t>(), cur, picked,
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

        bool edit_properties(world_t& world, asset_handle_t handle)
        {
            const history::target_t target = history::asset_target(get_type_id<material_t>(), handle);
            history::watch(world, target);
            const bool changed = draw_properties(world, handle);
            if (changed) { history::edited(world, target, "Edit Material"); }
            return changed;
        }

        void draw_inspector(world_t& world, editor_context_t&, const std::string&, asset_handle_t handle)
        { edit_properties(world, handle); }
    } // namespace

    nlohmann::json to_json(asset_handle_t handle)
    {
        const material_t* mat = material_of(handle);
        const shader_t* shader = mat ? assets::get(mat->shader) : nullptr;
        if (!mat || !shader) { return nullptr; }

        nlohmann::json out;
        const std::string shader_path = assets::path_of(mat->shader);
        out["shader"] = shader_path;

        // guid next to the path survives a shader rename
        if (const asset_guid_t shader_guid = assets::guid_of(shader_path))
        {
            out["shader_guid"] = assets::guid_text(shader_guid);
        }

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
                const std::string tex_path = named_texture(*mat, name_hash);
                if (!tex_path.empty())
                {
                    textures[member.name] = tex_path;

                    if (const asset_guid_t tex_guid = assets::guid_of(tex_path))
                    {
                        texture_guids[member.name] = assets::guid_text(tex_guid);
                    }
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
            case shader_member_type_e::FLOAT: properties[member.name] = tidy(*static_cast<const f32*>(slot)); break;
            case shader_member_type_e::FLOAT2:
            case shader_member_type_e::FLOAT3:
            case shader_member_type_e::FLOAT4:
            {
                const u32_t count = member.size / sizeof(f32);
                const f32* values = static_cast<const f32*>(slot);
                std::vector<f64> tidied;
                for (u32_t i = 0; i < count; i++) { tidied.push_back(tidy(values[i])); }
                properties[member.name] = std::move(tidied);
                break;
            }
            case shader_member_type_e::INT: properties[member.name] = *static_cast<const i32*>(slot); break;
            case shader_member_type_e::UINT: properties[member.name] = *static_cast<const u32_t*>(slot); break;
            case shader_member_type_e::BOOL: properties[member.name] = (*static_cast<const u32_t*>(slot) != 0u); break;
            default: break;
            }
        }

        if (!properties.empty()) { out["properties"] = std::move(properties); }
        if (!textures.empty()) { out["textures"] = std::move(textures); }
        if (!texture_guids.empty()) { out["texture_guids"] = std::move(texture_guids); }
        if (!samplers.empty()) { out["samplers"] = std::move(samplers); }

        return out;
    }

    void init(editor_context_t&)
    {
        history::reg_editable(get_type_id<material_t>(), &capture, &apply, &write);

        asset_editors::add({
            .type_id = get_type_id<material_t>(),
            .color = IM_COL32(210, 120, 80, 255),
            .create_label = "Material",
            .create = &asset_files::create_material,
            .load_when_inspected = true,
            .draw_inspector = &draw_inspector,
            .draw_inline = &edit_properties,
        });
    }
} // namespace tau::editor::material_edit
