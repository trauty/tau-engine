#include "gltf_reader.h"

#include "tau/log.h"
#include "tau/math.h"

#include <cgltf/cgltf.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <meshoptimizer.h>
#include <string_view>

namespace tau::cooker::gltf
{
    namespace
    {
        using namespace model;

        struct reader_t
        {
            const cgltf_data* data = nullptr;
            std::string path;
            read_e what = read_e::FULL;
        };

        template <typename T>
        i32 index_of(const T* item, const T* first)
        { return item ? static_cast<i32>(item - first) : -1; }

        std::string name_of(const char* name) { return name ? std::string(name) : std::string{}; }

        std::vector<f32> floats(const cgltf_accessor* accessor)
        {
            if (!accessor) { return {}; }
            std::vector<f32> out(accessor->count * cgltf_num_components(accessor->type));
            cgltf_accessor_unpack_floats(accessor, out.data(), out.size());
            return out;
        }

        bool decode_meshopt(cgltf_data* data, const std::string& path)
        {
            for (cgltf_size i = 0; i < data->buffer_views_count; i++)
            {
                cgltf_buffer_view& view = data->buffer_views[i];
                if (!view.has_meshopt_compression) { continue; }

                const cgltf_meshopt_compression& mc = view.meshopt_compression;
                if (!mc.buffer || !mc.buffer->data)
                {
                    TAU_LOG_ERROR("GLTF", "{}: a meshopt compressed view has no data", path);
                    return false;
                }

                const u8_t* source = static_cast<const u8_t*>(mc.buffer->data) + mc.offset;
                void* decoded = std::malloc(mc.count * mc.stride);
                if (!decoded) { return false; }

                i32 result = -1;
                switch (mc.mode)
                {
                case cgltf_meshopt_compression_mode_attributes:
                    result = meshopt_decodeVertexBuffer(decoded, mc.count, mc.stride, source, mc.size);
                    break;
                case cgltf_meshopt_compression_mode_triangles:
                    result = meshopt_decodeIndexBuffer(decoded, mc.count, mc.stride, source, mc.size);
                    break;
                case cgltf_meshopt_compression_mode_indices:
                    result = meshopt_decodeIndexSequence(decoded, mc.count, mc.stride, source, mc.size);
                    break;
                default: break;
                }
                if (result != 0)
                {
                    std::free(decoded);
                    TAU_LOG_ERROR("GLTF", "{}: meshopt decoding failed ({})", path, result);
                    return false;
                }

                switch (mc.filter)
                {
                case cgltf_meshopt_compression_filter_octahedral:
                    meshopt_decodeFilterOct(decoded, mc.count, mc.stride);
                    break;
                case cgltf_meshopt_compression_filter_quaternion:
                    meshopt_decodeFilterQuat(decoded, mc.count, mc.stride);
                    break;
                case cgltf_meshopt_compression_filter_exponential:
                    meshopt_decodeFilterExp(decoded, mc.count, mc.stride);
                    break;
                default: break;
                }

                view.data = decoded;
            }
            return true;
        }

        texture_ref_t texture_ref(const reader_t& r, const cgltf_texture_view& view)
        {
            texture_ref_t ref;
            const cgltf_texture* texture = view.texture;
            if (!texture) { return ref; }

            const cgltf_image* image =
                texture->has_basisu && texture->basisu_image ? texture->basisu_image : texture->image;
            ref.image = index_of(image, r.data->images);
            ref.uv_set = static_cast<u32_t>(view.texcoord);
            if (view.has_transform)
            {
                ref.offset[0] = view.transform.offset[0];
                ref.offset[1] = view.transform.offset[1];
                ref.scale[0] = view.transform.scale[0];
                ref.scale[1] = view.transform.scale[1];
                ref.rotation = view.transform.rotation;
                if (view.transform.has_texcoord) { ref.uv_set = static_cast<u32_t>(view.transform.texcoord); }
            }
            return ref;
        }

        material_desc_t read_material(const reader_t& r, const cgltf_material& in)
        {
            material_desc_t out;
            out.name = name_of(in.name);

            if (in.has_pbr_metallic_roughness)
            {
                const cgltf_pbr_metallic_roughness& pbr = in.pbr_metallic_roughness;
                std::memcpy(out.base_color, pbr.base_color_factor, sizeof(out.base_color));
                out.base_color_tex = texture_ref(r, pbr.base_color_texture);
                out.metallic = pbr.metallic_factor;
                out.roughness = pbr.roughness_factor;
                out.metallic_roughness_tex = texture_ref(r, pbr.metallic_roughness_texture);
            }

            out.normal_tex = texture_ref(r, in.normal_texture);
            out.normal_scale = in.normal_texture.scale;
            out.occlusion_tex = texture_ref(r, in.occlusion_texture);
            out.occlusion_strength = in.occlusion_texture.scale;
            std::memcpy(out.emissive, in.emissive_factor, sizeof(out.emissive));
            out.emissive_tex = texture_ref(r, in.emissive_texture);
            if (in.has_emissive_strength) { out.emissive_strength = in.emissive_strength.emissive_strength; }
            if (in.has_transmission)
            {
                out.transmission = in.transmission.transmission_factor;
                out.transmission_tex = texture_ref(r, in.transmission.transmission_texture);
            }

            switch (in.alpha_mode)
            {
            case cgltf_alpha_mode_mask: out.alpha_mode = alpha_mode_e::MASK; break;
            case cgltf_alpha_mode_blend: out.alpha_mode = alpha_mode_e::BLEND; break;
            default: out.alpha_mode = alpha_mode_e::OPAQUE; break;
            }
            out.alpha_cutoff = in.alpha_cutoff;
            out.double_sided = in.double_sided;
            return out;
        }

        std::string image_extension(const cgltf_image& image)
        {
            const std::string_view mime = image.mime_type ? image.mime_type : "";
            if (mime == "image/png") { return ".png"; }
            if (mime == "image/jpeg") { return ".jpg"; }
            if (mime == "image/ktx2") { return ".ktx2"; }

            const std::string_view uri = image.uri ? image.uri : "";
            if (uri.starts_with("data:image/png")) { return ".png"; }
            if (uri.starts_with("data:image/jpeg")) { return ".jpg"; }
            if (uri.starts_with("data:image/ktx2")) { return ".ktx2"; }
            return {};
        }

        bool read_image(const reader_t& r, const cgltf_image& in, image_desc_t& out)
        {
            out.name = name_of(in.name);
            const std::string_view uri = in.uri ? in.uri : "";

            if (!uri.empty() && !uri.starts_with("data:"))
            {
                std::string decoded(uri);
                decoded.resize(cgltf_decode_uri(decoded.data()));
                out.path = (std::filesystem::path(r.path).parent_path() / decoded).lexically_normal().generic_string();
                return true;
            }

            out.extension = image_extension(in);
            if (out.extension.empty())
            {
                TAU_LOG_WARN("GLTF", "{}: embedded image {} has an unknown type, skipped", r.path, out.name);
                return false;
            }
            if (r.what != read_e::FULL) { return true; }

            if (in.buffer_view)
            {
                const cgltf_buffer_view& view = *in.buffer_view;
                const u8_t* bytes = view.data ? static_cast<const u8_t*>(view.data)
                                    : view.buffer && view.buffer->data
                                        ? static_cast<const u8_t*>(view.buffer->data) + view.offset
                                        : nullptr;
                if (!bytes) { return false; }
                out.bytes.assign(bytes, bytes + view.size);
                return true;
            }

            const std::size_t comma = uri.find(',');
            if (comma == std::string_view::npos || uri.find(";base64") == std::string_view::npos) { return false; }

            const std::string base64(uri.substr(comma + 1));
            const std::size_t size = base64.size() / 4 * 3 - (base64.ends_with("==")  ? 2
                                                              : base64.ends_with('=') ? 1
                                                                                      : 0);
            cgltf_options options = {};
            void* decoded = nullptr;
            if (cgltf_load_buffer_base64(&options, size, base64.c_str(), &decoded) != cgltf_result_success)
            {
                return false;
            }
            out.bytes.assign(static_cast<u8_t*>(decoded), static_cast<u8_t*>(decoded) + size);
            std::free(decoded);
            return true;
        }

        void read_primitive(const reader_t& r, const cgltf_primitive& in, primitive_desc_t& out)
        {
            out.material = index_of(in.material, r.data->materials);
            if (r.what != read_e::FULL) { return; }

            for (cgltf_size i = 0; i < in.attributes_count; i++)
            {
                const cgltf_attribute& attribute = in.attributes[i];
                switch (attribute.type)
                {
                case cgltf_attribute_type_position: out.positions = floats(attribute.data); break;
                case cgltf_attribute_type_normal: out.normals = floats(attribute.data); break;
                case cgltf_attribute_type_tangent: out.tangents = floats(attribute.data); break;
                case cgltf_attribute_type_texcoord:
                    if (attribute.index == 0) { out.uv0 = floats(attribute.data); }
                    else if (attribute.index == 1) { out.uv1 = floats(attribute.data); }
                    break;
                case cgltf_attribute_type_color:
                    if (attribute.index == 0)
                    {
                        std::vector<f32> colors = floats(attribute.data);
                        // rgb sets get alpha 1
                        if (attribute.data->type == cgltf_type_vec3)
                        {
                            out.colors.reserve(colors.size() / 3 * 4);
                            for (std::size_t v = 0; v + 2 < colors.size(); v += 3)
                            {
                                out.colors.insert(out.colors.end(), {colors[v], colors[v + 1], colors[v + 2], 1.0f});
                            }
                        }
                        else
                        {
                            out.colors = std::move(colors);
                        }
                    }
                    break;
                case cgltf_attribute_type_joints:
                    if (attribute.index == 0)
                    {
                        const std::vector<f32> joints = floats(attribute.data);
                        out.joints.reserve(joints.size());
                        for (const f32 joint : joints) { out.joints.push_back(static_cast<u16_t>(joint)); }
                    }
                    break;
                case cgltf_attribute_type_weights:
                    if (attribute.index == 0) { out.weights = floats(attribute.data); }
                    break;
                default: break;
                }
            }

            const u32_t vertex_count = out.vertex_count();
            if (in.indices)
            {
                out.indices.resize(in.indices->count);
                cgltf_accessor_unpack_indices(in.indices, out.indices.data(), sizeof(u32_t), out.indices.size());
            }
            else
            {
                out.indices.resize(vertex_count);
                for (u32_t i = 0; i < vertex_count; i++) { out.indices[i] = i; }
            }

            out.targets.resize(in.targets_count);
            for (cgltf_size t = 0; t < in.targets_count; t++)
            {
                for (cgltf_size a = 0; a < in.targets[t].attributes_count; a++)
                {
                    const cgltf_attribute& attribute = in.targets[t].attributes[a];
                    if (attribute.type == cgltf_attribute_type_position)
                    {
                        out.targets[t].positions = floats(attribute.data);
                    }
                    if (attribute.type == cgltf_attribute_type_normal)
                    {
                        out.targets[t].normals = floats(attribute.data);
                    }
                }
            }
        }

        void read_mesh(const reader_t& r, const cgltf_mesh& in, mesh_desc_t& out)
        {
            out.name = name_of(in.name);
            for (cgltf_size i = 0; i < in.target_names_count; i++)
            {
                out.target_names.push_back(name_of(in.target_names[i]));
            }
            out.default_weights.assign(in.weights, in.weights + in.weights_count);

            for (cgltf_size i = 0; i < in.primitives_count; i++)
            {
                const cgltf_primitive& primitive = in.primitives[i];
                if (primitive.type != cgltf_primitive_type_triangles)
                {
                    TAU_LOG_WARN("GLTF", "{}: mesh '{}' has a primitive that is not triangles, skipped", r.path,
                                 out.name);
                    continue;
                }
                if (primitive.has_draco_mesh_compression)
                {
                    TAU_LOG_WARN("GLTF", "{}: Draco compressed primitive in '{}' skipped (not supported)", r.path,
                                 out.name);
                    continue;
                }

                read_primitive(r, primitive, out.primitives.emplace_back());
                if (r.what == read_e::FULL && out.primitives.back().positions.empty()) { out.primitives.pop_back(); }
            }
        }

        void read_node(const reader_t& r, const cgltf_node& in, node_desc_t& out)
        {
            out.name = name_of(in.name);
            out.parent = index_of(in.parent, r.data->nodes);
            for (cgltf_size i = 0; i < in.children_count; i++)
            {
                out.children.push_back(index_of(in.children[i], r.data->nodes));
            }
            out.mesh = index_of(in.mesh, r.data->meshes);
            out.skin = index_of(in.skin, r.data->skins);
            if (in.extras.data) { out.extras = in.extras.data; }

            if (in.has_matrix)
            {
                mat4_t matrix;
                std::memcpy(&matrix, in.matrix, sizeof(matrix));
                vec3_t translation, scale;
                quat_t rotation;
                decompose(matrix, translation, rotation, scale);
                std::memcpy(out.translation, &translation, sizeof(out.translation));
                std::memcpy(out.rotation, &rotation, sizeof(out.rotation));
                std::memcpy(out.scale, &scale, sizeof(out.scale));
                return;
            }
            if (in.has_translation) { std::memcpy(out.translation, in.translation, sizeof(out.translation)); }
            if (in.has_rotation) { std::memcpy(out.rotation, in.rotation, sizeof(out.rotation)); }
            if (in.has_scale) { std::memcpy(out.scale, in.scale, sizeof(out.scale)); }
        }

        void read_clip(const reader_t& r, const cgltf_animation& in, clip_desc_t& out)
        {
            out.name = name_of(in.name);
            for (cgltf_size i = 0; i < in.channels_count; i++)
            {
                const cgltf_animation_channel& channel = in.channels[i];
                if (!channel.target_node || !channel.sampler) { continue; }

                channel_desc_t& c = out.channels.emplace_back();
                c.node = index_of(channel.target_node, r.data->nodes);
                switch (channel.target_path)
                {
                case cgltf_animation_path_type_translation: c.path = channel_path_e::TRANSLATION; break;
                case cgltf_animation_path_type_rotation: c.path = channel_path_e::ROTATION; break;
                case cgltf_animation_path_type_scale: c.path = channel_path_e::SCALE; break;
                case cgltf_animation_path_type_weights: c.path = channel_path_e::WEIGHTS; break;
                default: out.channels.pop_back(); continue;
                }
                switch (channel.sampler->interpolation)
                {
                case cgltf_interpolation_type_step: c.interpolation = interpolation_e::STEP; break;
                case cgltf_interpolation_type_cubic_spline: c.interpolation = interpolation_e::CUBIC_SPLINE; break;
                default: c.interpolation = interpolation_e::LINEAR; break;
                }
                if (r.what == read_e::FULL)
                {
                    c.times = floats(channel.sampler->input);
                    c.values = floats(channel.sampler->output);
                }
            }
        }
    } // namespace

    bool read(const std::string& path, read_e what, model_desc_t& out)
    {
        cgltf_options options = {};
        cgltf_data* data = nullptr;
        if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success)
        {
            TAU_LOG_ERROR("GLTF", "Could not parse {}", path);
            return false;
        }

        bool ok = true;
        if (what == read_e::FULL)
        {
            if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success)
            {
                TAU_LOG_ERROR("GLTF", "Could not load the buffers of {}", path);
                ok = false;
            }
            else
            {
                ok = decode_meshopt(data, path);
            }
        }

        if (ok)
        {
            for (cgltf_size i = 0; i < data->extensions_required_count; i++)
            {
                const std::string_view ext = data->extensions_required[i];
                if (ext != "KHR_mesh_quantization" && ext != "EXT_meshopt_compression" && ext != "KHR_texture_basisu" &&
                    ext != "KHR_texture_transform" && ext != "KHR_materials_emissive_strength")
                {
                    TAU_LOG_WARN("GLTF", "{} requires {}, which the import ignores", path, ext);
                }
            }

            const reader_t r{data, path, what};

            out.images.resize(data->images_count);
            for (cgltf_size i = 0; i < data->images_count; i++) { read_image(r, data->images[i], out.images[i]); }

            out.materials.reserve(data->materials_count);
            for (cgltf_size i = 0; i < data->materials_count; i++)
            {
                out.materials.push_back(read_material(r, data->materials[i]));
            }

            out.meshes.resize(data->meshes_count);
            for (cgltf_size i = 0; i < data->meshes_count; i++) { read_mesh(r, data->meshes[i], out.meshes[i]); }

            out.nodes.resize(data->nodes_count);
            for (cgltf_size i = 0; i < data->nodes_count; i++) { read_node(r, data->nodes[i], out.nodes[i]); }

            const cgltf_scene* scene = data->scene ? data->scene : data->scenes_count > 0 ? &data->scenes[0] : nullptr;
            if (scene)
            {
                for (cgltf_size i = 0; i < scene->nodes_count; i++)
                {
                    out.roots.push_back(index_of(scene->nodes[i], data->nodes));
                }
            }
            else
            {
                for (cgltf_size i = 0; i < data->nodes_count; i++)
                {
                    if (!data->nodes[i].parent) { out.roots.push_back(static_cast<i32>(i)); }
                }
            }

            out.skins.resize(data->skins_count);
            for (cgltf_size i = 0; i < data->skins_count; i++)
            {
                const cgltf_skin& skin = data->skins[i];
                out.skins[i].name = name_of(skin.name);
                for (cgltf_size j = 0; j < skin.joints_count; j++)
                {
                    out.skins[i].joints.push_back(index_of(skin.joints[j], data->nodes));
                }
                if (what == read_e::FULL)
                {
                    out.skins[i].inverse_binds = floats(skin.inverse_bind_matrices);
                    if (out.skins[i].inverse_binds.size() != skin.joints_count * 16)
                    {
                        out.skins[i].inverse_binds.assign(skin.joints_count * 16, 0.0f);
                        for (cgltf_size j = 0; j < skin.joints_count; j++)
                        {
                            for (u32_t d = 0; d < 4; d++) { out.skins[i].inverse_binds[j * 16 + d * 5] = 1.0f; }
                        }
                    }
                }
            }

            out.clips.resize(data->animations_count);
            for (cgltf_size i = 0; i < data->animations_count; i++) { read_clip(r, data->animations[i], out.clips[i]); }

            if (what == read_e::FULL && (data->lights_count > 0 || data->cameras_count > 0))
            {
                TAU_LOG_INFO("GLTF", "{}: {} light(s) and {} camera(s) not imported", path, data->lights_count,
                             data->cameras_count);
            }
        }

        cgltf_free(data);
        return ok;
    }
} // namespace tau::cooker::gltf
