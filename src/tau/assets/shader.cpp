#include "shader.h"

#include "tau/asset.h"
#include "tau/assets/shader_format.h"
#include "tau/hash.h"
#include "tau/log.h"
#include "tau/rhi/context.h"
#include "tau/rhi/pipelines.h"
#include "tau/rhi/swapchain.h"
#include "tau/vfs.h"

#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tau
{
    namespace
    {
        struct reader_t
        {
            std::span<const u8_t> bytes;
            size_t at = 0;
            bool ok = true;

            void read(void* dst, size_t size)
            {
                if (!ok || at + size > bytes.size())
                {
                    ok = false;
                    return;
                }
                if (size > 0) { std::memcpy(dst, bytes.data() + at, size); }
                at += size;
            }

            template <typename T>
            void read_array(std::vector<T>& out, u32_t count)
            {
                out.resize(count);
                read(out.data(), count * sizeof(T));
            }
        };
    } // namespace

    bool read_shader_file(const std::string& cooked_path, shader_file_t& out)
    {
        const std::vector<u8_t> bytes = vfs::read_bytes(cooked_path);
        if (bytes.empty())
        {
            TAU_LOG_ERROR("SHADER", "Shader not found: {}", cooked_path);
            return false;
        }
        return parse_shader_file(cooked_path, bytes, out);
    }

    bool parse_shader_file(const std::string& cooked_path, std::span<const u8_t> bytes, shader_file_t& out)
    {
        reader_t r{bytes};
        r.read(&out.header, sizeof(shader_header_t));
        if (!r.ok || out.header.magic != TAU_SHADER_MAGIC)
        {
            TAU_LOG_ERROR("SHADER", "Invalid .taushader file: {}", cooked_path);
            return false;
        }
        if (out.header.version != TAU_SHADER_VERSION)
        {
            TAU_LOG_ERROR("SHADER", "Unsupported .taushader version {} (engine expects {}), recook: {}",
                          out.header.version, TAU_SHADER_VERSION, cooked_path);
            return false;
        }

        const shader_header_t& h = out.header;
        r.read_array(out.target_formats, h.target_format_count);
        r.read_array(out.gbuffer_target_formats, h.gbuffer_target_format_count);

        if (h.has_material_data)
        {
            shader_module_header_t mod_header;
            r.read(&mod_header, sizeof(shader_module_header_t));

            out.module = {
                .name = mod_header.name,
                .size = mod_header.size,
                .domain = mod_header.domain,
                .blend_mode = mod_header.blend_mode,
                .depth_write = mod_header.depth_write,
                .depth_test = mod_header.depth_test,
                .casts_shadow = mod_header.casts_shadow,
            };

            for (u32_t j = 0; j < mod_header.member_count && r.ok; j++)
            {
                shader_member_header_t m;
                r.read(&m, sizeof(shader_member_header_t));
                out.module.members[m.name_hash] = {m.name,      m.offset,    m.size,       m.type,   m.edit,
                                                   m.range_min, m.range_max, m.enum_names, m.tooltip};
            }
        }

        r.read_array(out.descriptor_bindings, h.descriptor_binding_count);
        r.read_array(out.pipelines, h.pipeline_count);
        r.read_array(out.vert_spirv, h.vert_spirv_size);
        r.read_array(out.frag_spirv, h.frag_spirv_size);
        r.read_array(out.gbuffer_spirv, h.gbuffer_spirv_size);
        r.read_array(out.comp_spirv, h.comp_spirv_size);

        if (!r.ok)
        {
            TAU_LOG_ERROR("SHADER", "Truncated .taushader file: {}", cooked_path);
            return false;
        }

        out.content_hash = hash_string64(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        return true;
    }

    std::optional<shader_t> tau::asset_loader_t<shader_t>::load(const std::string& path, std::span<const u8_t> bytes)
    {
        const std::string cooked_path = assets::cooked_path(get_type_id<shader_t>(), path);

        shader_file_t file;
        if (!parse_shader_file(cooked_path, bytes, file)) { return std::nullopt; }

        // built at startup
        const renderer::pipelines::shader_pipelines_t built = renderer::pipelines::acquire(path, file);
        if (built.layout == VK_NULL_HANDLE)
        {
            TAU_LOG_ERROR("SHADER", "No pipelines for shader {}", path);
            return std::nullopt;
        }

        shader_t shader;
        shader.is_compute = file.header.is_compute;
        shader.has_material_data = file.header.has_material_data;
        shader.module = std::move(file.module);
        shader.descriptor_bindings = std::move(file.descriptor_bindings);
        shader.target_formats = std::move(file.target_formats);
        shader.gbuffer_target_formats = std::move(file.gbuffer_target_formats);
        for (VkFormat& format : shader.target_formats)
        {
            if (format == VK_FORMAT_UNDEFINED) { format = renderer::swapchain::state().format; }
        }

        shader.pipeline_layout = built.layout;
        shader.custom_layouts = built.set_layouts;
        for (u32_t v = 0; v < static_cast<u32_t>(pipeline_variant_e::COUNT); v++)
        {
            if (built.variants[v] != VK_NULL_HANDLE) { shader.pipelines[v] = built.variants[v]; }
        }
        return shader;
    }

    // pipelines stay in renderer::pipelines
    void asset_loader_t<shader_t>::unload(shader_t& shader)
    {
        shader.pipelines.clear();
        shader.custom_layouts.clear();
        shader.pipeline_layout = VK_NULL_HANDLE;
    }
} // namespace tau