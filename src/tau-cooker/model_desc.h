#pragma once

#include "tau/defines.h"

#include <string>
#include <vector>

// a model file as the sub-asset writers see it, whatever format it came from
// indices are into the vectors of the same description, -1 for none
namespace tau::cooker::model
{
    struct texture_ref_t
    {
        i32 image = -1;
        u32_t uv_set = 0;
        // KHR_texture_transform
        f32 offset[2] = {0.0f, 0.0f};
        f32 scale[2] = {1.0f, 1.0f};
        f32 rotation = 0.0f;
    };

    enum class alpha_mode_e : u8_t
    {
        OPAQUE,
        MASK,
        BLEND,
    };

    struct material_desc_t
    {
        std::string name;

        f32 base_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        texture_ref_t base_color_tex;
        f32 metallic = 1.0f;
        f32 roughness = 1.0f;
        texture_ref_t metallic_roughness_tex;
        f32 normal_scale = 1.0f;
        texture_ref_t normal_tex;
        f32 occlusion_strength = 1.0f;
        texture_ref_t occlusion_tex;
        f32 emissive[3] = {0.0f, 0.0f, 0.0f};
        f32 emissive_strength = 1.0f;
        texture_ref_t emissive_tex;
        f32 transmission = 0.0f;
        texture_ref_t transmission_tex;

        alpha_mode_e alpha_mode = alpha_mode_e::OPAQUE;
        f32 alpha_cutoff = 0.5f;
        bool double_sided = false;
    };

    struct image_desc_t
    {
        std::string name;
        // external: the file on disk
        std::string path;
        // embedded: the encoded file and its extension
        std::vector<u8_t> bytes;
        std::string extension;
    };

    struct morph_target_desc_t
    {
        // deltas, xyz per vertex, empty when the target has none
        std::vector<f32> positions;
        std::vector<f32> normals;
    };

    // per vertex arrays, empty when the file has none
    struct primitive_desc_t
    {
        std::vector<f32> positions; // xyz
        std::vector<f32> normals;   // xyz
        std::vector<f32> tangents;  // xyzw
        std::vector<f32> uv0;       // xy
        std::vector<f32> uv1;       // xy
        std::vector<f32> colors;    // rgba
        std::vector<u16_t> joints;  // 4
        std::vector<f32> weights;   // 4
        std::vector<u32_t> indices;
        std::vector<morph_target_desc_t> targets;
        i32 material = -1;

        u32_t vertex_count() const { return static_cast<u32_t>(positions.size() / 3); }
    };

    struct mesh_desc_t
    {
        std::string name;
        std::vector<primitive_desc_t> primitives;
        std::vector<std::string> target_names;
        std::vector<f32> default_weights;
    };

    struct node_desc_t
    {
        std::string name;
        i32 parent = -1;
        std::vector<i32> children;
        f32 translation[3] = {0.0f, 0.0f, 0.0f};
        f32 rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        f32 scale[3] = {1.0f, 1.0f, 1.0f};
        i32 mesh = -1;
        i32 skin = -1;
        // custom properties as raw JSON, mapped to components later
        std::string extras;
    };

    struct skin_desc_t
    {
        std::string name;
        std::vector<i32> joints;
        // 16 floats per joint, column major
        std::vector<f32> inverse_binds;
    };

    enum class channel_path_e : u8_t
    {
        TRANSLATION,
        ROTATION,
        SCALE,
        WEIGHTS,
    };

    enum class interpolation_e : u8_t
    {
        STEP,
        LINEAR,
        CUBIC_SPLINE,
    };

    struct channel_desc_t
    {
        i32 node = -1;
        channel_path_e path = channel_path_e::TRANSLATION;
        interpolation_e interpolation = interpolation_e::LINEAR;
        std::vector<f32> times;
        std::vector<f32> values;
    };

    struct clip_desc_t
    {
        std::string name;
        std::vector<channel_desc_t> channels;
    };

    struct model_desc_t
    {
        std::vector<node_desc_t> nodes;
        // the scene's root nodes in order
        std::vector<i32> roots;
        std::vector<mesh_desc_t> meshes;
        std::vector<material_desc_t> materials;
        std::vector<image_desc_t> images;
        std::vector<skin_desc_t> skins;
        std::vector<clip_desc_t> clips;
    };

    enum class read_e : u8_t
    {
        // names, nodes, materials, skins, clips without keys: guid listing and prefabs
        STRUCTURE,
        // geometry, image bytes and keys too
        FULL,
    };
} // namespace tau::cooker::model
