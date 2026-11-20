#include "model_cooker.h"

#include "tau-cooker/gltf_reader.h"
#include "tau-cooker/material_cooker.h"
#include "tau-cooker/mesh_cooker.h"
#include "tau-cooker/texture_cooker.h"
#include "tau/asset_sources.h"
#include "tau/assets/animation_format.h"
#include "tau/assets/bytes.h"
#include "tau/assets/clip.h"
#include "tau/assets/import_settings.h"
#include "tau/assets/material.h"
#include "tau/assets/mesh.h"
#include "tau/assets/model.h"
#include "tau/assets/model_format.h"
#include "tau/assets/prefab.h"
#include "tau/assets/scene_format.h"
#include "tau/assets/skeleton.h"
#include "tau/assets/texture.h"
#include "tau/guid.h"
#include "tau/hash.h"
#include "tau/log.h"
#include "tau/math.h"

#include <algorithm>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace tau::cooker::model
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr const char* SUB_ASSETS_KEY = "sub_assets";
        constexpr const char* PBR_SHADER = "engine://assets/shaders/pbr.slang";
        constexpr const char* WHITE_TEXTURE = "engine://assets/textures/default_white.png";

        struct plan_t
        {
            model_import_t settings;
            // [mesh][0] whole, or [mesh][primitive] with submeshes_as_entities
            std::vector<std::vector<std::string>> meshes;
            std::vector<std::string> materials;
            // embedded images only, external ones stay their own assets
            std::vector<std::string> images;
            std::vector<std::string> skeletons;

            std::vector<std::string> clips;
            std::string prefab;
        };

        std::string sanitized(std::string_view name)
        {
            std::string out;
            for (const char c : name)
            {
                const bool bad = static_cast<unsigned char>(c) < 32 ||
                                 std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos;
                out.push_back(bad ? '_' : c);
            }
            while (!out.empty() && (out.back() == '.' || out.back() == ' ')) { out.pop_back(); }
            return out;
        }

        struct namer_t
        {
            std::unordered_set<std::string> taken;

            std::string take(std::string_view name, std::string_view fallback, std::string_view extension)
            {
                std::string base = sanitized(name);
                if (base.empty()) { base = fallback; }

                std::string candidate = base + std::string(extension);
                for (u32_t n = 1; taken.contains(candidate); n++)
                {
                    candidate = base + "_" + std::to_string(n) + std::string(extension);
                }
                taken.insert(candidate);
                return candidate;
            }
        };

        model_import_t settings_of(const std::string& source, const context_t& ctx, const nlohmann::json& meta)
        {
            static const nlohmann::json none = nlohmann::json::object();
            return import_settings::resolve_as<model_import_t>(
                get_type_id<model_t>(), ctx.project_defaults ? *ctx.project_defaults : none, meta, source);
        }

        plan_t plan_of(const model_desc_t& desc, const model_import_t& settings, const std::string& source)
        {
            plan_t plan;
            plan.settings = settings;
            namer_t namer;

            plan.prefab = namer.take(fs::path(source).stem().string(), "model", ".prefab");

            for (std::size_t m = 0; m < desc.meshes.size(); m++)
            {
                const mesh_desc_t& mesh = desc.meshes[m];
                const std::string fallback = "mesh_" + std::to_string(m);
                std::vector<std::string>& names = plan.meshes.emplace_back();
                if (!settings.submeshes_as_entities || mesh.primitives.size() <= 1)
                {
                    names.push_back(namer.take(mesh.name, fallback, ".mesh"));
                    continue;
                }
                for (std::size_t p = 0; p < mesh.primitives.size(); p++)
                {
                    const std::string suffix = "_" + std::to_string(p);
                    names.push_back(
                        namer.take(mesh.name.empty() ? "" : mesh.name + suffix, fallback + suffix, ".mesh"));
                }
            }

            for (std::size_t i = 0; i < desc.materials.size(); i++)
            {
                plan.materials.push_back(namer.take(desc.materials[i].name, "material_" + std::to_string(i), ".mat"));
            }

            for (std::size_t i = 0; i < desc.images.size(); i++)
            {
                const image_desc_t& image = desc.images[i];
                plan.images.push_back(image.path.empty() && !image.extension.empty()
                                          ? namer.take(image.name, "image_" + std::to_string(i), image.extension)
                                          : std::string{});
            }

            for (std::size_t i = 0; i < desc.skins.size(); i++)
            {
                plan.skeletons.push_back(namer.take(desc.skins[i].name, "skeleton_" + std::to_string(i), ".skeleton"));
            }

            // named even when skipped, so skipping one renames no other, skipped by that name
            for (std::size_t i = 0; i < desc.clips.size(); i++)
            {
                const std::string name = namer.take(desc.clips[i].name, "clip_" + std::to_string(i), ".clip");
                const std::string stem = fs::path(name).stem().string();
                const bool skipped = !settings.import_animations ||
                                     std::find(settings.skipped_clips.begin(), settings.skipped_clips.end(), stem) !=
                                         settings.skipped_clips.end();
                plan.clips.push_back(skipped ? std::string{} : name);
            }
            return plan;
        }

        // the outputs in a fixed order: prefab, meshes, materials, images, skeletons, clips
        std::vector<output_t> outputs_of(const plan_t& plan)
        {
            std::vector<output_t> out;
            out.push_back({plan.prefab, get_type_id<prefab_t>(), 0});
            for (const std::vector<std::string>& names : plan.meshes)
            {
                for (const std::string& name : names) { out.push_back({name, get_type_id<mesh_t>(), 0}); }
            }
            for (const std::string& name : plan.materials) { out.push_back({name, get_type_id<material_t>(), 0}); }
            for (const std::string& name : plan.images)
            {
                if (!name.empty()) { out.push_back({name, get_type_id<texture_t>(), 0}); }
            }
            for (const std::string& name : plan.skeletons) { out.push_back({name, get_type_id<skeleton_t>(), 0}); }
            for (const std::string& name : plan.clips)
            {
                if (!name.empty()) { out.push_back({name, get_type_id<clip_t>(), 0}); }
            }
            return out;
        }

        std::unordered_map<std::string, asset_guid_t> guids_of(const nlohmann::json& meta)
        {
            std::unordered_map<std::string, asset_guid_t> out;
            const auto it = meta.find(SUB_ASSETS_KEY);
            if (it == meta.end() || !it->is_object()) { return out; }
            for (const auto& [name, guid] : it->items())
            {
                if (guid.is_string()) { out[name] = assets::parse_guid(guid.get<std::string>()); }
            }
            return out;
        }

        // a unit conversion, so everything that holds a length scales
        void apply_scale(model_desc_t& desc, f32 scale)
        {
            if (scale == 1.0f) { return; }

            for (mesh_desc_t& mesh : desc.meshes)
            {
                for (primitive_desc_t& p : mesh.primitives)
                {
                    for (f32& v : p.positions) { v *= scale; }
                    for (morph_target_desc_t& t : p.targets)
                    {
                        for (f32& v : t.positions) { v *= scale; }
                    }
                }
            }
            for (node_desc_t& node : desc.nodes)
            {
                for (f32& v : node.translation) { v *= scale; }
            }
            for (skin_desc_t& skin : desc.skins)
            {
                for (std::size_t j = 0; j + 15 < skin.inverse_binds.size(); j += 16)
                {
                    for (std::size_t c = 12; c < 15; c++) { skin.inverse_binds[j + c] *= scale; }
                }
            }
            for (clip_desc_t& clip : desc.clips)
            {
                for (channel_desc_t& channel : clip.channels)
                {
                    if (channel.path != channel_path_e::TRANSLATION) { continue; }
                    for (f32& v : channel.values) { v *= scale; }
                }
            }
        }

        std::string node_name(const model_desc_t& desc, i32 node)
        {
            const std::string& name = desc.nodes[static_cast<std::size_t>(node)].name;
            return name.empty() ? "node_" + std::to_string(node) : name;
        }

        texture_kind_e kind_for_use(const material_desc_t& m, i32 image)
        {
            if (m.normal_tex.image == image) { return texture_kind_e::NORMAL; }
            if (m.metallic_roughness_tex.image == image || m.occlusion_tex.image == image ||
                m.transmission_tex.image == image)
            {
                return texture_kind_e::DATA;
            }
            return texture_kind_e::COLOR;
        }

        // first use wins, unused images are color
        std::vector<texture_kind_e> image_kinds(const model_desc_t& desc)
        {
            std::vector<texture_kind_e> kinds(desc.images.size(), texture_kind_e::COLOR);
            std::vector<bool> seen(desc.images.size(), false);
            for (const material_desc_t& m : desc.materials)
            {
                for (const texture_ref_t* ref : {&m.base_color_tex, &m.metallic_roughness_tex, &m.normal_tex,
                                                 &m.occlusion_tex, &m.emissive_tex, &m.transmission_tex})
                {
                    if (ref->image < 0 || static_cast<std::size_t>(ref->image) >= kinds.size() || seen[ref->image])
                    {
                        continue;
                    }
                    kinds[ref->image] = kind_for_use(m, ref->image);
                    seen[ref->image] = true;
                }
            }
            return kinds;
        }

        nlohmann::json tagged(scene_value_type_e type, nlohmann::json value)
        {
            return {
                {"ty", static_cast<u8_t>(type)},
                {"v",  std::move(value)       }
            };
        }

        nlohmann::json vec3_json(const f32* v) { return nlohmann::json::array({v[0], v[1], v[2]}); }

        struct prefab_builder_t
        {
            const model_desc_t& desc;
            const plan_t& plan;
            const std::unordered_map<std::string, asset_guid_t>& guids;
            std::string model_vfs;
            std::string id_seed;
            std::string root_name;

            std::unordered_set<i32> joints;
            nlohmann::json entities = nlohmann::json::array();

            u64_t id_of(const std::string& path) const
            {
                const u64_t id = hash_string64(id_seed + path);
                return id != 0 ? id : 1;
            }

            nlohmann::json mesh_ref(const std::string& name) const
            {
                const auto it = guids.find(name);
                return {
                    {"g", assets::guid_text(it != guids.end() ? it->second : 0)},
                    {"p", model_vfs + "/" + name                               },
                    {"t", "mesh"                                               }
                };
            }

            nlohmann::json components(const std::string& name, const f32* t, const f32* r, const f32* s,
                                      const std::string* mesh) const
            {
                const vec3_t euler = quat_t::to_euler(quat_t(r));
                const f32 e[3] = {euler.x, euler.y, euler.z};

                nlohmann::json out = {
                    {"tag_t",       {{"name", tagged(scene_value_type_e::STRING, name)}}},
                    {"transform_t",
                     {{"local_position", tagged(scene_value_type_e::VEC3, vec3_json(t))},
                      {"local_euler", tagged(scene_value_type_e::VEC3, vec3_json(e))},
                      {"local_scale", tagged(scene_value_type_e::VEC3, vec3_json(s))}}  },
                };
                if (mesh)
                {
                    out["mesh_renderer_t"] = {
                        {"mesh",         tagged(scene_value_type_e::ASSET_REF,      mesh_ref(*mesh))},
                        {"materials",    tagged(scene_value_type_e::ASSET_REF_LIST,
                         {{"t", "material"}, {"e", nlohmann::json::array()}})                       },
                        {"active",       tagged(scene_value_type_e::BOOL,           true)           },
                        {"casts_shadow", tagged(scene_value_type_e::BOOL,           true)           },
                    };
                }
                return out;
            }

            std::size_t add(u64_t id, u64_t parent, nlohmann::json components_json)
            {
                entities.push_back({
                    {"id",         guid::to_string(id)                                                     },
                    {"parent",     parent != 0 ? nlohmann::json(guid::to_string(parent)) : nlohmann::json()},
                    {"next",       nullptr                                                                 },
                    {"components", std::move(components_json)                                              },
                });
                return entities.size() - 1;
            }

            void link(std::unordered_map<u64_t, std::size_t>& last_child, u64_t parent, std::size_t entry)
            {
                const auto it = last_child.find(parent);
                if (it != last_child.end()) { entities[it->second]["next"] = entities[entry]["id"]; }
                last_child[parent] = entry;
            }

            void add_node(i32 node, u64_t parent, const std::string& parent_path,
                          std::unordered_map<u64_t, std::size_t>& last_child,
                          std::unordered_map<std::string, u32_t>& path_uses)
            {
                const node_desc_t& n = desc.nodes[static_cast<std::size_t>(node)];
                const std::string name = node_name(desc, node);

                std::string path = parent_path + "/" + name;
                if (const u32_t uses = path_uses[path]++; uses > 0) { path += "~" + std::to_string(uses); }

                if (joints.contains(node))
                {
                    for (const i32 child : n.children) { add_node(child, parent, path, last_child, path_uses); }
                    return;
                }

                const std::vector<std::string>* mesh_names =
                    n.mesh >= 0 && static_cast<std::size_t>(n.mesh) < plan.meshes.size() ? &plan.meshes[n.mesh]
                                                                                         : nullptr;
                const bool split = mesh_names && mesh_names->size() > 1;

                const std::string shown = parent == 0 && n.name.empty() ? root_name : name;

                const u64_t id = id_of(path);
                const std::size_t entry = add(id, parent,
                                              components(shown, n.translation, n.rotation, n.scale,
                                                         mesh_names && !split ? &mesh_names->front() : nullptr));
                link(last_child, parent, entry);

                if (split)
                {
                    const f32 zero[3] = {0.0f, 0.0f, 0.0f};
                    const f32 identity[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                    const f32 one[3] = {1.0f, 1.0f, 1.0f};
                    for (std::size_t p = 0; p < mesh_names->size(); p++)
                    {
                        const std::string& mesh = (*mesh_names)[p];
                        const std::size_t part =
                            add(id_of(path + "/#" + std::to_string(p)), id,
                                components(fs::path(mesh).stem().string(), zero, identity, one, &mesh));
                        link(last_child, id, part);
                    }
                }

                for (const i32 child : n.children) { add_node(child, id, path, last_child, path_uses); }
            }

            nlohmann::json build(const std::string& stem, asset_guid_t prefab_guid)
            {
                for (const skin_desc_t& skin : desc.skins) { joints.insert(skin.joints.begin(), skin.joints.end()); }

                std::vector<i32> top;
                for (const i32 root : desc.roots)
                {
                    if (root >= 0 && static_cast<std::size_t>(root) < desc.nodes.size()) { top.push_back(root); }
                }

                std::unordered_map<u64_t, std::size_t> last_child;
                std::unordered_map<std::string, u32_t> path_uses;

                const bool single = top.size() == 1 && !joints.contains(top.front());
                if (single) { add_node(top.front(), 0, {}, last_child, path_uses); }
                else
                {
                    const f32 zero[3] = {0.0f, 0.0f, 0.0f};
                    const f32 identity[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                    const f32 one[3] = {1.0f, 1.0f, 1.0f};
                    const u64_t root = id_of("/");
                    add(root, 0, components(stem, zero, identity, one, nullptr));
                    for (const i32 node : top) { add_node(node, root, {}, last_child, path_uses); }
                }

                return {
                    {"guid",     guid::to_string(prefab_guid)},
                    {"entities", std::move(entities)         },
                };
            }
        };

        nlohmann::json build_prefab(const model_desc_t& desc, const plan_t& plan,
                                    const std::unordered_map<std::string, asset_guid_t>& guids, const context_t& ctx,
                                    const std::string& source)
        {
            const auto it = guids.find(plan.prefab);
            const asset_guid_t prefab_guid = it != guids.end() ? it->second : 0;
            const std::string stem = fs::path(source).stem().string();
            prefab_builder_t builder{desc, plan, guids, ctx.model_vfs, assets::guid_text(prefab_guid), stem, {}, {}};
            return builder.build(stem, prefab_guid);
        }

        bool write_file(const fs::path& path, const std::vector<u8_t>& bytes)
        {
            fs::create_directories(path.parent_path());
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!file) { TAU_LOG_ERROR("MODEL_COOKER", "Could not write {}", path.generic_string()); }
            return static_cast<bool>(file);
        }

        fs::path cooked(const fs::path& dir, const std::string& name, u64_t type_id)
        {
            const asset_type_t* type = assets::type_by_id(type_id);
            fs::path out = dir / name;
            out.replace_extension(std::string(type ? type->cooked_extension : ""));
            return out;
        }

        bool write_skeleton(const model_desc_t& desc, const skin_desc_t& skin, const fs::path& path)
        {
            std::unordered_map<i32, i32> index_of;
            for (std::size_t j = 0; j < skin.joints.size(); j++) { index_of[skin.joints[j]] = static_cast<i32>(j); }

            byte_writer_t out;
            out.write(skeleton_header_t{.joint_count = static_cast<u32_t>(skin.joints.size())});
            for (std::size_t j = 0; j < skin.joints.size(); j++)
            {
                const i32 node = skin.joints[j];
                const node_desc_t& n = desc.nodes[static_cast<std::size_t>(node)];

                joint_record_t record;
                for (i32 up = n.parent; up >= 0; up = desc.nodes[static_cast<std::size_t>(up)].parent)
                {
                    if (const auto it = index_of.find(up); it != index_of.end())
                    {
                        record.parent = it->second;
                        break;
                    }
                }
                std::memcpy(record.translation, n.translation, sizeof(record.translation));
                std::memcpy(record.rotation, n.rotation, sizeof(record.rotation));
                std::memcpy(record.scale, n.scale, sizeof(record.scale));
                std::memcpy(record.inverse_bind, &skin.inverse_binds[j * 16], sizeof(record.inverse_bind));

                const std::string name = node_name(desc, node);
                record.name_length = static_cast<u32_t>(name.size());
                out.write(record);
                out.string(name);
            }
            return write_file(path, out.data);
        }

        bool write_clip(const model_desc_t& desc, const clip_desc_t& clip, const fs::path& path)
        {
            f32 duration = 0.0f;
            for (const channel_desc_t& channel : clip.channels)
            {
                if (!channel.times.empty()) { duration = std::max(duration, channel.times.back()); }
            }

            byte_writer_t out;
            out.write(clip_header_t{.duration = duration, .channel_count = static_cast<u32_t>(clip.channels.size())});
            for (const channel_desc_t& channel : clip.channels)
            {
                const std::string target = node_name(desc, channel.node);
                out.write(clip_channel_record_t{
                    .path = static_cast<u8_t>(channel.path),
                    .interpolation = static_cast<u8_t>(channel.interpolation),
                    .key_count = static_cast<u32_t>(channel.times.size()),
                    .value_count = static_cast<u32_t>(channel.values.size()),
                    .target_length = static_cast<u32_t>(target.size()),
                });
                out.string(target);
                out.array(channel.times);
                out.array(channel.values);
            }
            return write_file(path, out.data);
        }

        nlohmann::json material_json(const material_desc_t& m, const std::string& albedo_path, asset_guid_t albedo_guid)
        {
            nlohmann::json out = {
                {"shader",     PBR_SHADER                   },
                {"properties",
                 {{"base_color", {m.base_color[0], m.base_color[1], m.base_color[2], m.base_color[3]}},
                  {"roughness", m.roughness},
                  {"metallic", m.metallic}}                 },
                {"textures",   {{"albedo_tex", albedo_path}}},
            };
            if (albedo_guid != 0)
            {
                out["texture_guids"] = {
                    {"albedo_tex", assets::guid_text(albedo_guid)}
                };
            }
            return out;
        }
    } // namespace

    std::vector<output_t> list_outputs(const std::string& source, const context_t& ctx)
    {
        model_desc_t desc;
        if (!gltf::read(source, read_e::STRUCTURE, desc)) { return {}; }

        nlohmann::json meta = asset_sources::read_meta(source);
        const plan_t plan = plan_of(desc, settings_of(source, ctx, meta), source);
        std::vector<output_t> outputs = outputs_of(plan);

        bool changed = false;
        if (!meta.contains(SUB_ASSETS_KEY))
        {
            meta[SUB_ASSETS_KEY] = nlohmann::json::object();
            changed = true;

            const auto first_mesh = std::find_if(outputs.begin(), outputs.end(),
                                                 [](const output_t& o) { return o.type_id == get_type_id<mesh_t>(); });
            const asset_guid_t old = assets::parse_guid(meta.value("guid", std::string{}));
            if (old != 0 && first_mesh != outputs.end())
            {
                meta[SUB_ASSETS_KEY][first_mesh->name] = assets::guid_text(old);
                meta["guid"] = guid::to_string(guid::generate());
                TAU_LOG_INFO("MODEL_COOKER", "{}: the mesh guid {} moved to {}, model got a new one", source,
                             assets::guid_text(old), first_mesh->name);
            }
        }

        nlohmann::json& subs = meta[SUB_ASSETS_KEY];
        for (output_t& output : outputs)
        {
            output.guid = subs.contains(output.name) ? assets::parse_guid(subs[output.name].get<std::string>()) : 0;
            if (output.guid == 0)
            {
                output.guid = guid::generate();
                subs[output.name] = guid::to_string(output.guid);
                changed = true;
            }
        }

        if (changed) { asset_sources::write_meta(source, meta); }
        return outputs;
    }

    nlohmann::json prepare_meta(const std::string& source, const context_t& ctx)
    {
        model_desc_t desc;
        if (!gltf::read(source, read_e::STRUCTURE, desc)) { return asset_sources::read_meta(source); }

        static const nlohmann::json none = nlohmann::json::object();
        const nlohmann::json& defaults = ctx.project_defaults ? *ctx.project_defaults : none;

        const std::vector<texture_kind_e> kinds = image_kinds(desc);
        u32_t set = 0;
        for (std::size_t i = 0; i < desc.images.size(); i++)
        {
            const std::string& path = desc.images[i].path;
            if (path.empty() || !fs::exists(path)) { continue; }

            nlohmann::json meta = asset_sources::read_meta(path);
            if (meta.contains(import_settings::META_KEY)) { continue; }

            const reflection::any_t base = import_settings::base(get_type_id<texture_t>(), defaults);
            reflection::any_t value = base;
            texture_import_t& settings = value.cast<texture_import_t&>();
            settings.kind = kinds[i];
            if (settings.kind != texture_kind_e::COLOR) { settings.srgb = false; }

            meta[import_settings::META_KEY] = import_settings::meta_object(value, base);
            asset_sources::write_meta(path, meta);
            set++;
        }
        if (set > 0) { TAU_LOG_INFO("MODEL_COOKER", "{}: set the kind of {} image(s) from their use", source, set); }
        return asset_sources::read_meta(source);
    }

    bool cook(const std::string& source, const std::string& manifest_path, const context_t& ctx,
              std::vector<fs::path>& outputs)
    {
        TAU_LOG_INFO("MODEL_COOKER", "Cooking model: {} -> {}", source, manifest_path);

        model_desc_t desc;
        if (!gltf::read(source, read_e::FULL, desc)) { return false; }

        const nlohmann::json meta = asset_sources::read_meta(source);
        const plan_t plan = plan_of(desc, settings_of(source, ctx, meta), source);
        apply_scale(desc, plan.settings.scale);
        const std::unordered_map<std::string, asset_guid_t> guids = guids_of(meta);

        const auto guid_of_name = [&](const std::string& name)
        {
            const auto it = guids.find(name);
            return it != guids.end() ? it->second : asset_guid_t{0};
        };
        const auto sub_path = [&](const std::string& name) { return ctx.model_vfs + "/" + name; };

        const fs::path dir = fs::path(manifest_path).parent_path() / fs::path(source).filename();
        bool ok = true;

        static const nlohmann::json none = nlohmann::json::object();
        const nlohmann::json& defaults = ctx.project_defaults ? *ctx.project_defaults : none;

        // images: embedded ones cook here, external ones are referenced
        const std::vector<texture_kind_e> kinds = image_kinds(desc);
        std::vector<std::string> image_paths(desc.images.size());
        std::vector<asset_guid_t> image_guids(desc.images.size(), 0);
        for (std::size_t i = 0; i < desc.images.size(); i++)
        {
            const image_desc_t& image = desc.images[i];
            if (!plan.images[i].empty())
            {
                image_paths[i] = sub_path(plan.images[i]);
                image_guids[i] = guid_of_name(plan.images[i]);

                texture_import_t settings =
                    import_settings::resolve_as<texture_import_t>(get_type_id<texture_t>(), defaults, none, {});
                settings.kind = kinds[i];
                if (settings.kind != texture_kind_e::COLOR) { settings.srgb = false; }

                const fs::path out = cooked(dir, plan.images[i], get_type_id<texture_t>());
                if (texture::cook_texture_bytes(image.bytes, image.extension, out.generic_string(), settings,
                                                source + "/" + plan.images[i]))
                {
                    outputs.push_back(out);
                }
                else
                {
                    ok = false;
                }
            }
            else if (!image.path.empty())
            {
                image_paths[i] = ctx.vfs_of ? ctx.vfs_of(image.path) : std::string{};
                image_guids[i] = assets::guid_of(image_paths[i]);
                if (image_paths[i].empty())
                {
                    TAU_LOG_WARN("MODEL_COOKER", "{}: image {} is outside the asset folders, not used", source,
                                 image.path);
                }
            }
        }

        // materials
        for (std::size_t i = 0; i < desc.materials.size(); i++)
        {
            const material_desc_t& m = desc.materials[i];
            const i32 image = m.base_color_tex.image;
            const bool textured =
                image >= 0 && static_cast<std::size_t>(image) < image_paths.size() && !image_paths[image].empty();

            const fs::path out = cooked(dir, plan.materials[i], get_type_id<material_t>());
            if (material::write_material(
                    material_json(m, textured ? image_paths[image] : WHITE_TEXTURE, textured ? image_guids[image] : 0),
                    out.generic_string(), source + "/" + plan.materials[i]))
            {
                outputs.push_back(out);
            }
            else
            {
                ok = false;
            }
        }

        // what an empty slot of a submesh draws: remap, else the imported material
        const auto submesh_of = [&](const primitive_desc_t& p)
        {
            mesh::submesh_source_t s;
            s.primitive = &p;
            if (p.material < 0 || static_cast<std::size_t>(p.material) >= desc.materials.size()) { return s; }

            s.material_name = fs::path(plan.materials[p.material]).stem().string();
            for (const material_remap_t& remap : plan.settings.material_remaps)
            {
                if (remap.name != s.material_name || !remap.material.is_valid()) { continue; }
                s.material_guid = remap.material.guid();
                s.material_path = assets::path_of(remap.material);
                return s;
            }
            s.material_guid = guid_of_name(plan.materials[p.material]);
            s.material_path = sub_path(plan.materials[p.material]);
            return s;
        };

        // meshes
        for (std::size_t m = 0; m < desc.meshes.size(); m++)
        {
            const mesh_desc_t& mesh = desc.meshes[m];
            const std::vector<std::string>& names = plan.meshes[m];

            std::vector<std::vector<mesh::submesh_source_t>> parts;
            if (names.size() == 1)
            {
                std::vector<mesh::submesh_source_t>& all = parts.emplace_back();
                for (const primitive_desc_t& p : mesh.primitives) { all.push_back(submesh_of(p)); }
            }
            else
            {
                for (const primitive_desc_t& p : mesh.primitives) { parts.push_back({submesh_of(p)}); }
            }

            for (std::size_t i = 0; i < names.size() && i < parts.size(); i++)
            {
                const fs::path out = cooked(dir, names[i], get_type_id<mesh_t>());
                if (mesh::write_mesh(mesh, parts[i], out.generic_string())) { outputs.push_back(out); }
                else
                {
                    ok = false;
                }
            }
        }

        for (std::size_t i = 0; i < desc.skins.size(); i++)
        {
            const fs::path out = cooked(dir, plan.skeletons[i], get_type_id<skeleton_t>());
            if (write_skeleton(desc, desc.skins[i], out)) { outputs.push_back(out); }
            else
            {
                ok = false;
            }
        }

        for (std::size_t i = 0; i < desc.clips.size(); i++)
        {
            if (plan.clips[i].empty()) { continue; }
            const fs::path out = cooked(dir, plan.clips[i], get_type_id<clip_t>());
            if (write_clip(desc, desc.clips[i], out)) { outputs.push_back(out); }
            else
            {
                ok = false;
            }
        }

        {
            const fs::path out = cooked(dir, plan.prefab, get_type_id<prefab_t>());
            if (write_file(out, nlohmann::json::to_cbor(build_prefab(desc, plan, guids, ctx, source))))
            {
                outputs.push_back(out);
            }
            else
            {
                ok = false;
            }
        }

        // the manifest last
        byte_writer_t manifest;
        const std::vector<output_t> listed = outputs_of(plan);
        manifest.write(model_header_t{.sub_asset_count = static_cast<u32_t>(listed.size())});
        for (const output_t& output : listed)
        {
            manifest.write(model_sub_asset_record_t{output.type_id, guid_of_name(output.name),
                                                    static_cast<u32_t>(output.name.size()), 0});
            manifest.string(output.name);
        }
        if (ok && write_file(manifest_path, manifest.data)) { outputs.push_back(manifest_path); }
        else
        {
            ok = false;
        }
        return ok;
    }

    nlohmann::json prefab_data(const std::string& source, const std::string& name, const context_t& ctx)
    {
        model_desc_t desc;
        if (!gltf::read(source, read_e::STRUCTURE, desc)) { return nullptr; }

        const nlohmann::json meta = asset_sources::read_meta(source);
        const plan_t plan = plan_of(desc, settings_of(source, ctx, meta), source);
        if (plan.prefab != name) { return nullptr; }

        apply_scale(desc, plan.settings.scale);
        return build_prefab(desc, plan, guids_of(meta), ctx, source);
    }
} // namespace tau::cooker::model
