#include "material_edit.h"

#include "tau-editor/editor_context.h"
#include "tau-editor/history.h"
#include "tau-editor/project_manager.h"
#include "tau/asset.h"
#include "tau/asset_sources.h"
#include "tau/assets/material.h"
#include "tau/assets/shader.h"
#include "tau/assets/texture.h"
#include "tau/hash.h"
#include "tau/log.h"
#include "tau/rendering/renderer_constants.h"
#include "tau/vfs.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <json/json.hpp>
#include <string>

namespace tau::editor::material_edit
{
    namespace
    {
        editor_context_t* g_ctx = nullptr;

        std::filesystem::path engine_source_root()
        {
            const std::filesystem::path engine = project_manager::engine_dir();
            if (engine.empty()) { return {}; }

            const std::filesystem::path installed = engine / "share" / "tau" / "engine-assets-src";
            return std::filesystem::is_directory(installed) ? installed : engine / "assets";
        }

        std::filesystem::path resolve_source_path(const std::string& virtual_path)
        {
            const vfs::path_parts_t parts = vfs::split_protocol(virtual_path);
            if (parts.protocol.empty()) { return {}; }

            const std::string protocol(parts.protocol.substr(0, parts.protocol.size() - 3));
            std::string rel(parts.rest);

            constexpr std::string_view assets_prefix = "assets/";
            if (rel.rfind(assets_prefix, 0) == 0) { rel.erase(0, assets_prefix.size()); }

            std::filesystem::path root;
            if (protocol == "engine") { root = engine_source_root(); }
            else if (protocol == "game" && g_ctx && !g_ctx->project_assets_dir.empty())
            {
                root = g_ctx->project_assets_dir;
            }
            if (root.empty()) { return {}; }

            return root / rel;
        }

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

            for (const auto& [name_hash, member] : shader->module.members)
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
                {"members",  std::move(members)      },
                {"textures", std::move(textures)     },
                {"authored", mat->authored_properties},
            };
            return nlohmann::json::to_cbor(state);
        }

        bool apply(asset_handle_t handle, std::span<const u8_t> bytes)
        {
            material_t* mat = material_of(handle);
            const shader_t* shader = mat ? assets::get(mat->shader) : nullptr;
            if (!shader) { return false; }

            const nlohmann::json state = nlohmann::json::from_cbor(bytes, true, false);
            if (state.is_discarded()) { return false; }

            const nlohmann::json& members = state["members"];
            const nlohmann::json& textures = state["textures"];

            for (const auto& [name_hash, member] : shader->module.members)
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
            const material_t* mat = material_of(handle);
            const shader_t* shader = mat ? assets::get(mat->shader) : nullptr;
            if (!mat || !shader)
            {
                TAU_LOG_ERROR("EDITOR", "Cannot save a material whose shader is not loaded");
                return false;
            }

            const std::string virtual_path = assets::path_of(handle);
            const std::filesystem::path target = resolve_source_path(virtual_path);
            if (target.empty())
            {
                TAU_LOG_ERROR("EDITOR", "Cannot map '{}' to a source file", virtual_path);
                return false;
            }

            nlohmann::json out;
            out["guid"] = assets::guid_text(asset_sources::guid_for_writing(target.string()));

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
                TAU_LOG_ERROR("EDITOR", "Cannot open {}", target.string());
                return false;
            }

            file << out.dump(4) << '\n';
            file.close();

            TAU_LOG_INFO("EDITOR", "Material saved to {}", target.string());
            out_source = target;
            return true;
        }
    } // namespace

    void init(editor_context_t& ctx)
    {
        g_ctx = &ctx;
        history::reg_editable(get_type_id<material_t>(), &capture, &apply, &write);
    }
} // namespace tau::editor::material_edit
