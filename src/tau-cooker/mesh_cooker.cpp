#include "mesh_cooker.h"

#include "tau/assets/mesh.h"
#include "tau/assets/mesh_format.h"
#include "tau/log.h"

#include <filesystem>
#include <fstream>
#include <meshoptimizer.h>
#include <tinygltf/tiny_gltf.h>
#include <vector>

namespace tau::cooker::mesh
{
    bool cook_mesh(const std::string& input_path, const std::string& output_path, const mesh_import_t& settings)
    {
        TAU_LOG_INFO("MESH_COOKER", "Cooking Mesh: {} -> {}", input_path, output_path);

        tinygltf::TinyGLTF loader;
        tinygltf::Model model;
        std::string warn, err;

        bool is_glb = input_path.ends_with(".glb");
        bool loaded = is_glb ? loader.LoadBinaryFromFile(&model, &err, &warn, input_path)
                             : loader.LoadASCIIFromFile(&model, &err, &warn, input_path);

        if (!loaded || model.meshes.empty())
        {
            TAU_LOG_ERROR("MESH_COOKER", "GLTF load error: {}", err);
            return false;
        }

        const tinygltf::Mesh& mesh = model.meshes[0];
        const tinygltf::Primitive& primitive = mesh.primitives[0];
        const std::map<std::string, int>& attribs = primitive.attributes;

        const tinygltf::Accessor& pos_accessor = model.accessors[primitive.attributes.at("POSITION")];
        const tinygltf::BufferView& pos_view = model.bufferViews[pos_accessor.bufferView];
        const tinygltf::Buffer& pos_buffer = model.buffers[pos_view.buffer];
        const f32* positions =
            reinterpret_cast<const f32*>(&pos_buffer.data[pos_view.byteOffset + pos_accessor.byteOffset]);

        const f32* normals = nullptr;
        if (attribs.find("NORMAL") != attribs.end())
        {
            const tinygltf::Accessor& norm_accessor = model.accessors[attribs.at("NORMAL")];
            const tinygltf::BufferView& norm_view = model.bufferViews[norm_accessor.bufferView];
            const tinygltf::Buffer& norm_buffer = model.buffers[norm_view.buffer];
            normals = reinterpret_cast<const f32*>(&norm_buffer.data[norm_view.byteOffset + norm_accessor.byteOffset]);
        }

        const f32* uvs = nullptr;
        if (attribs.find("TEXCOORD_0") != attribs.end())
        {
            const tinygltf::Accessor& uv_accessor = model.accessors[attribs.at("TEXCOORD_0")];
            const tinygltf::BufferView& uv_view = model.bufferViews[uv_accessor.bufferView];
            const tinygltf::Buffer& uv_buffer = model.buffers[uv_view.buffer];
            uvs = reinterpret_cast<const f32*>(&uv_buffer.data[uv_view.byteOffset + uv_accessor.byteOffset]);
        }

        std::vector<vertex_t> vertex_data(pos_accessor.count);
        for (size_t i = 0; i < vertex_data.size(); ++i)
        {
            for (u32_t axis = 0; axis < 3; axis++)
            {
                vertex_data[i].position[axis] = positions[i * 3 + axis] * settings.scale;
            }
            if (normals) { std::memcpy(vertex_data[i].normal, &normals[i * 3], 3 * sizeof(f32)); }
            if (uvs) { std::memcpy(vertex_data[i].uv, &uvs[i * 2], 2 * sizeof(f32)); }
        }

        std::vector<u32_t> indices;
        if (primitive.indices >= 0)
        {
            const tinygltf::Accessor& idx_accessor = model.accessors[primitive.indices];
            const tinygltf::BufferView& idx_view = model.bufferViews[idx_accessor.bufferView];
            const tinygltf::Buffer& idx_buffer = model.buffers[idx_view.buffer];

            const void* raw_indices = &idx_buffer.data[idx_view.byteOffset + idx_accessor.byteOffset];
            indices.resize(idx_accessor.count);

            switch (idx_accessor.componentType)
            {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
            {
                const u16* src = reinterpret_cast<const u16*>(raw_indices);
                for (size_t i = 0; i < indices.size(); ++i) indices[i] = static_cast<u32>(src[i]);
                break;
            }
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
            {
                const u32* src = reinterpret_cast<const u32*>(raw_indices);
                std::copy(src, src + idx_accessor.count, indices.begin());
                break;
            }
            }
        }
        else
        {
            indices.resize(vertex_data.size());
            for (u32_t i = 0; i < indices.size(); i++) { indices[i] = i; }
        }

        std::vector<u32_t> optimized_indices(indices.size());
        meshopt_optimizeVertexCache(optimized_indices.data(), indices.data(), indices.size(), vertex_data.size());
        meshopt_optimizeOverdraw(optimized_indices.data(), optimized_indices.data(), indices.size(),
                                 &vertex_data[0].position[0], vertex_data.size(), sizeof(vertex_t), 1.05f);

        std::vector<vertex_t> optimized_vertices(vertex_data.size());
        meshopt_optimizeVertexFetch(optimized_vertices.data(), optimized_indices.data(), indices.size(),
                                    vertex_data.data(), vertex_data.size(), sizeof(vertex_t));

        vec3_t min_pos = {FLT_MAX, FLT_MAX, FLT_MAX};
        vec3_t max_pos = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

        for (const vertex_t& vertex : optimized_vertices)
        {
            min_pos.x = std::min(min_pos.x, vertex.position[0]);
            min_pos.y = std::min(min_pos.y, vertex.position[1]);
            min_pos.z = std::min(min_pos.z, vertex.position[2]);

            max_pos.x = std::max(max_pos.x, vertex.position[0]);
            max_pos.y = std::max(max_pos.y, vertex.position[1]);
            max_pos.z = std::max(max_pos.z, vertex.position[2]);
        }

        vec3_t center = {
            (min_pos.x + max_pos.x) * 0.5f,
            (min_pos.y + max_pos.y) * 0.5f,
            (min_pos.z + max_pos.z) * 0.5f,
        };

        f32 max_sq_dist = 0.0f;
        for (const vertex_t& vertex : optimized_vertices)
        {
            f32 dx = vertex.position[0] - center.x;
            f32 dy = vertex.position[1] - center.y;
            f32 dz = vertex.position[2] - center.z;

            f32 sq_dist = (dx * dx) + (dy * dy) + (dz * dz);
            if (sq_dist > max_sq_dist) { max_sq_dist = sq_dist; }
        }

        f32 local_radius = std::sqrt(max_sq_dist);

        std::filesystem::create_directories(std::filesystem::path(output_path).parent_path());
        std::ofstream out(output_path, std::ios::binary);

        mesh_header_t header = {
            .magic = TAU_MESH_MAGIC,
            .version = TAU_MESH_VERSION,
            .vertex_count = static_cast<u32_t>(optimized_vertices.size()),
            .index_count = static_cast<u32_t>(optimized_indices.size()),
            .local_center = center,
            .local_radius = local_radius,
        };

        out.write(reinterpret_cast<const char*>(&header), sizeof(mesh_header_t));
        out.write(reinterpret_cast<const char*>(optimized_vertices.data()),
                  optimized_vertices.size() * sizeof(vertex_t));
        out.write(reinterpret_cast<const char*>(optimized_indices.data()), optimized_indices.size() * sizeof(u32_t));
        return true;
    }
} // namespace tau::cooker::mesh