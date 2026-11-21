#include "mesh_cooker.h"

#include "tau/assets/bytes.h"
#include "tau/assets/mesh.h"
#include "tau/assets/mesh_format.h"
#include "tau/log.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <meshoptimizer.h>
#include <vector>

namespace tau::cooker::mesh
{
    namespace
    {
        struct stream_t
        {
            mesh_stream_e kind;
            u32_t element_size;
            std::string name;
            std::vector<u8_t> data;
            bool present = false;
        };

        template <typename T>
        void append(stream_t& stream, const std::vector<T>& values, u32_t vertex_count)
        {
            const std::size_t bytes = static_cast<std::size_t>(vertex_count) * stream.element_size;
            const std::size_t at = stream.data.size();
            stream.data.resize(at + bytes, 0);
            if (values.size() * sizeof(T) >= bytes && !values.empty())
            {
                std::memcpy(stream.data.data() + at, values.data(), bytes);
                stream.present = true;
            }
        }

        void sphere_of(const std::vector<vertex_t>& vertices, std::span<const u32_t> indices, vec3& center, f32& radius)
        {
            vec3 lo = {FLT_MAX, FLT_MAX, FLT_MAX};
            vec3 hi = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
            for (const u32_t index : indices)
            {
                const f32* p = vertices[index].position;
                lo = {std::min(lo.x, p[0]), std::min(lo.y, p[1]), std::min(lo.z, p[2])};
                hi = {std::max(hi.x, p[0]), std::max(hi.y, p[1]), std::max(hi.z, p[2])};
            }
            if (indices.empty())
            {
                center = {};
                radius = 0.0f;
                return;
            }

            center = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
            f32 max_sq = 0.0f;
            for (const u32_t index : indices)
            {
                const f32* p = vertices[index].position;
                const f32 dx = p[0] - center.x;
                const f32 dy = p[1] - center.y;
                const f32 dz = p[2] - center.z;
                max_sq = std::max(max_sq, dx * dx + dy * dy + dz * dz);
            }
            radius = std::sqrt(max_sq);
        }
    } // namespace

    bool write_mesh(const model::mesh_desc_t& mesh, std::span<const submesh_source_t> submeshes,
                    const std::string& output_path)
    {
        std::vector<vertex_t> vertices;
        std::vector<u32_t> indices;
        std::vector<mesh_submesh_record_t> records;

        std::vector<stream_t> streams = {
            {mesh_stream_e::TANGENT, 16, {}, {}},
            {mesh_stream_e::UV1,     8,  {}, {}},
            {mesh_stream_e::COLOR,   16, {}, {}},
            {mesh_stream_e::JOINTS,  8,  {}, {}},
            {mesh_stream_e::WEIGHTS, 16, {}, {}},
        };

        // morph targets only when every submesh has the mesh's target count
        const std::size_t target_count = mesh.target_names.size() > 0 ? mesh.target_names.size()
                                         : submeshes.empty()          ? 0
                                                                      : submeshes[0].primitive->targets.size();
        const bool morphs =
            target_count > 0 && std::all_of(submeshes.begin(), submeshes.end(), [&](const submesh_source_t& s)
                                            { return s.primitive->targets.size() == target_count; });
        const std::size_t first_morph = streams.size();
        if (morphs)
        {
            for (std::size_t t = 0; t < target_count; t++)
            {
                const std::string name = t < mesh.target_names.size() ? mesh.target_names[t] : std::to_string(t);
                streams.push_back({mesh_stream_e::MORPH_POSITION, 12, name, {}});
                streams.push_back({mesh_stream_e::MORPH_NORMAL, 12, name, {}});
            }
        }

        for (const submesh_source_t& source : submeshes)
        {
            const model::primitive_desc_t& p = *source.primitive;
            const u32_t base = static_cast<u32_t>(vertices.size());
            const u32_t count = p.vertex_count();

            vertices.resize(base + count);
            for (u32_t v = 0; v < count; v++)
            {
                vertex_t& out = vertices[base + v];
                std::memcpy(out.position, &p.positions[v * 3], sizeof(out.position));
                if (p.normals.size() >= (v + 1) * 3) { std::memcpy(out.normal, &p.normals[v * 3], sizeof(out.normal)); }
                if (p.uv0.size() >= (v + 1) * 2) { std::memcpy(out.uv, &p.uv0[v * 2], sizeof(out.uv)); }
            }

            append(streams[0], p.tangents, count);
            append(streams[1], p.uv1, count);
            append(streams[2], p.colors, count);
            append(streams[3], p.joints, count);
            append(streams[4], p.weights, count);
            if (morphs)
            {
                for (std::size_t t = 0; t < target_count; t++)
                {
                    append(streams[first_morph + t * 2], p.targets[t].positions, count);
                    append(streams[first_morph + t * 2 + 1], p.targets[t].normals, count);
                }
            }

            mesh_submesh_record_t& record = records.emplace_back();
            record.first_index = static_cast<u32_t>(indices.size());
            record.index_count = static_cast<u32_t>(p.indices.size());
            for (const u32_t index : p.indices) { indices.push_back(index < count ? base + index : base); }
        }

        for (stream_t& stream : streams)
        {
            if (stream.kind == mesh_stream_e::MORPH_POSITION) { stream.present = true; }
        }

        const std::size_t vertex_count = vertices.size();
        for (const mesh_submesh_record_t& record : records)
        {
            u32_t* range = indices.data() + record.first_index;
            meshopt_optimizeVertexCache(range, range, record.index_count, vertex_count);
            meshopt_optimizeOverdraw(range, range, record.index_count, &vertices[0].position[0], vertex_count,
                                     sizeof(vertex_t), 1.05f);
        }

        // one fetch order for the whole mesh
        std::vector<u32_t> remap(vertex_count);
        const std::size_t unique = vertex_count > 0 ? meshopt_optimizeVertexFetchRemap(remap.data(), indices.data(),
                                                                                       indices.size(), vertex_count)
                                                    : 0;
        meshopt_remapIndexBuffer(indices.data(), indices.data(), indices.size(), remap.data());
        {
            std::vector<vertex_t> remapped(unique);
            meshopt_remapVertexBuffer(remapped.data(), vertices.data(), vertex_count, sizeof(vertex_t), remap.data());
            vertices = std::move(remapped);
        }
        for (stream_t& stream : streams)
        {
            if (!stream.present) { continue; }
            std::vector<u8_t> remapped(unique * stream.element_size);
            meshopt_remapVertexBuffer(remapped.data(), stream.data.data(), vertex_count, stream.element_size,
                                      remap.data());
            stream.data = std::move(remapped);
        }

        mesh_header_t header;
        header.vertex_count = static_cast<u32_t>(vertices.size());
        header.index_count = static_cast<u32_t>(indices.size());
        header.submesh_count = static_cast<u32_t>(records.size());
        sphere_of(vertices, indices, header.local_center, header.local_radius);

        byte_writer_t out;
        out.write(header);
        for (std::size_t i = 0; i < records.size(); i++)
        {
            mesh_submesh_record_t& record = records[i];
            sphere_of(vertices, std::span(indices).subspan(record.first_index, record.index_count), record.local_center,
                      record.local_radius);
            record.material_guid = submeshes[i].material_guid;
            record.material_path_length = static_cast<u32_t>(submeshes[i].material_path.size());
            record.material_name_length = static_cast<u32_t>(submeshes[i].material_name.size());
            out.write(record);
            out.string(submeshes[i].material_path);
            out.string(submeshes[i].material_name);
        }
        out.array(vertices);
        out.array(indices);

        u32_t stream_count = 0;
        const auto write_stream = [&](mesh_stream_e kind, u32_t element_size, u32_t count, const std::string& name,
                                      const void* data, std::size_t size)
        {
            out.write(mesh_stream_record_t{kind, element_size, count, static_cast<u32_t>(name.size())});
            out.string(name);
            out.put(data, size);
            stream_count++;
        };
        for (const stream_t& stream : streams)
        {
            if (stream.present)
            {
                write_stream(stream.kind, stream.element_size, header.vertex_count, stream.name, stream.data.data(),
                             stream.data.size());
            }
        }
        if (morphs && !mesh.default_weights.empty())
        {
            write_stream(mesh_stream_e::MORPH_WEIGHTS, sizeof(f32), static_cast<u32_t>(mesh.default_weights.size()), {},
                         mesh.default_weights.data(), mesh.default_weights.size() * sizeof(f32));
        }

        header.stream_count = stream_count;
        std::memcpy(out.data.data(), &header, sizeof(header));

        std::filesystem::create_directories(std::filesystem::path(output_path).parent_path());
        std::ofstream file(output_path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(out.data.data()), static_cast<std::streamsize>(out.data.size()));
        if (!file)
        {
            TAU_LOG_ERROR("MESH_COOKER", "Could not write {}", output_path);
            return false;
        }
        return true;
    }
} // namespace tau::cooker::mesh
