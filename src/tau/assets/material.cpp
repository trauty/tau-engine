#include "material.h"

#include "tau/asset.h"
#include "tau/assets/material_format.h"
#include "tau/assets/shader.h"
#include "tau/log.h"
#include "tau/rhi/material_heap.h"
#include "tau/rhi/renderer.h"
#include "tau/rhi/renderer_types.h"
#include "tau/vfs.h"

#include <cstring>
#include <optional>
#include <vector>

namespace tau
{
    material_t::material_t(asset_t<shader_t> target_shader) : shader(target_shader)
    {
        const shader_t* layout = assets::get(target_shader);

        for (u32_t i = 0; i < renderer::MAX_FRAMES_IN_FLIGHT; i++) { heap_offset[i] = renderer::BINDLESS_NULL_HANDLE; }

        if (!layout)
        {
            TAU_LOG_ERROR("MATERIAL", "Cannot create material with null shader");
            return;
        }

        if (layout->has_material_data) { data.resize(layout->module.size, 0); }
        else
        {
            TAU_LOG_WARN("SHADER", "Shader '{}' has no material info attached", layout->module.name);
        }
    }

    namespace
    {
        std::string join_member_names(const shader_t& shader)
        {
            std::string out;
            for (auto [hash, member] : shader.module.members)
            {
                if (!out.empty()) { out += ", "; }
                out += member.name.empty() ? "?" : member.name;
            }
            return out;
        }
    } // namespace

    bool material_t::has_property(u32_t name_hash) const
    {
        const shader_t* layout = assets::get(shader);
        return layout && layout->module.members.contains(name_hash);
    }

    void material_t::set_property_raw(u32_t name_hash, const void* value, u32_t size)
    {
        const shader_t* layout = assets::get(shader);
        if (!layout) { return; }

        const auto& members = layout->module.members;
        const shader_member_t* it = members.find(name_hash);
        if (it == nullptr)
        {
            TAU_LOG_WARN("MATERIAL", "Property {} not found in shader '{}', it has: {}", name_hash, layout->module.name,
                         join_member_names(*layout));
            return;
        }

        const shader_member_t& member = *it;
        if (size != member.size)
        {
            TAU_LOG_ERROR("MATERIAL", "Size mismatch for '{}': expected {} bytes, got {}", name_hash, member.size,
                          size);
            return;
        }

        std::memcpy(data.data() + member.offset, value, size);
        dirty_frames = renderer::MAX_FRAMES_IN_FLIGHT;
    }

    void material_t::set_texture(u32_t name_hash, asset_t<texture_t> texture)
    {
        if (!texture.is_valid())
        {
            clear_texture(name_hash);
            return;
        }

        textures[name_hash] = texture;
        refresh_texture(name_hash);
    }

    void material_t::clear_texture(u32_t name_hash)
    {
        if (!textures.find(name_hash)) { return; }

        textures.erase(name_hash);
        set_property_if_present<u32_t>(name_hash, renderer::BINDLESS_NULL_HANDLE);
    }

    void material_t::refresh_texture(u32_t name_hash)
    {
        const asset_t<texture_t>* bound = textures.find(name_hash);
        if (!bound) { return; }

        const texture_t* tex = assets::get(*bound);
        if (!tex) { tex = assets::get(assets::load<texture_t>(renderer::FALLBACK_TEXTURE_PATH)); }

        set_property<u32_t>(name_hash, tex ? tex->bindless_id : renderer::BINDLESS_NULL_HANDLE);
    }

    void material_t::sync()
    {
        if (dirty_frames == 0 || data.empty()) { return; }

        u32_t cur_frame = renderer::ctx.cur_frame;

        if (heap_offset[cur_frame] == renderer::BINDLESS_NULL_HANDLE)
        {
            heap_offset[cur_frame] = renderer::material_heap::allocate(static_cast<u32_t>(data.size()));
        }

        renderer::material_heap::write(heap_offset[cur_frame], data.data(), static_cast<u32_t>(data.size()));

        if (last_synced_frame != cur_frame)
        {
            dirty_frames--;
            last_synced_frame = cur_frame;
        }
    }

    bool material_t::set_shader(asset_t<shader_t> new_shader)
    {
        const shader_t* next = assets::get(new_shader);
        if (!next || !next->has_material_data)
        {
            TAU_LOG_ERROR("MATERIAL", "'{}' cannot drive a material, it did not load or has no material data",
                          assets::path_of(new_shader));
            return false;
        }
        if (new_shader == shader) { return true; }

        const shader_t* prev = assets::get(shader);
        const std::vector<u8> prev_data = data;

        release_heap();
        shader = new_shader;
        data.assign(next->module.size, 0);

        std::vector<u32_t> kept;
        for (const u32_t hash : authored_properties)
        {
            const shader_member_t* now = next->module.members.find(hash);
            const shader_member_t* before = prev ? prev->module.members.find(hash) : nullptr;
            if (!now || !before || now->type != before->type || now->edit != before->edit) { continue; }

            if (now->edit != shader_member_edit_e::TEXTURE)
            {
                if (before->offset + before->size > prev_data.size() || now->size != before->size) { continue; }
                std::memcpy(data.data() + now->offset, prev_data.data() + before->offset, now->size);
            }
            kept.push_back(hash);
        }
        authored_properties = std::move(kept);

        std::vector<u32_t> bound;
        for (auto [hash, texture] : textures) { bound.push_back(hash); }
        for (const u32_t hash : bound)
        {
            const shader_member_t* member = next->module.members.find(hash);
            if (member && member->edit == shader_member_edit_e::TEXTURE) { refresh_texture(hash); }
            else
            {
                textures.erase(hash);
            }
        }

        dirty_frames = renderer::MAX_FRAMES_IN_FLIGHT;
        return true;
    }

    void material_t::release_heap()
    {
        for (u32_t i = 0; i < renderer::MAX_FRAMES_IN_FLIGHT; i++)
        {
            if (heap_offset[i] != renderer::BINDLESS_NULL_HANDLE)
            {
                renderer::material_heap::free(heap_offset[i], static_cast<u32_t>(data.size()));
                heap_offset[i] = renderer::BINDLESS_NULL_HANDLE;
            }
        }

        data.clear();
    }

    std::optional<material_t> asset_loader_t<material_t>::load(const std::string& path)
    {
        std::string cooked_path = assets::cooked_path(get_type_id<material_t>(), path);
        std::vector<u8_t> bytes = tau::vfs::read_bytes(cooked_path);
        if (bytes.empty())
        {
            TAU_LOG_WARN("MATERIAL", "Material file not found: {}", cooked_path);
            return std::nullopt;
        }

        if (bytes.size() < sizeof(material_header_t))
        {
            TAU_LOG_ERROR("MATERIAL", "Truncated material file: {}", cooked_path);
            return std::nullopt;
        }

        material_header_t header_data;
        std::memcpy(&header_data, bytes.data(), sizeof(header_data));
        const material_header_t* header = &header_data;
        if (header->magic != TAU_MATERIAL_MAGIC)
        {
            TAU_LOG_ERROR("MATERIAL", "Invalid material magic in: {}", cooked_path);
            return std::nullopt;
        }

        if (header->version != TAU_MATERIAL_VERSION)
        {
            TAU_LOG_ERROR("MATERIAL", "Unsupported material version {} (engine expects {}), recook: {}",
                          header->version, TAU_MATERIAL_VERSION, cooked_path);
            return std::nullopt;
        }

        u32_t offset = sizeof(material_header_t);

        if (offset + header->shader_path_length > bytes.size())
        {
            TAU_LOG_ERROR("MATERIAL", "Truncated shader path in: {}", cooked_path);
            return std::nullopt;
        }
        std::string shader_path(reinterpret_cast<char*>(bytes.data() + offset), header->shader_path_length);
        offset += header->shader_path_length;

        shader_path = assets::resolve(header->shader_guid, shader_path);

        // failed shader still has a slot
        const asset_t<shader_t> shader = assets::load<shader_t>(shader_path);
        if (!assets::get(shader))
        {
            TAU_LOG_ERROR("MATERIAL", "Shader '{}' did not load, so this material cannot be built", shader_path);
            return std::nullopt;
        }

        material_t mat(shader);

        for (u32_t i = 0; i < header->texture_count; i++)
        {
            if (offset + sizeof(cooked_texture_bind_t) > bytes.size())
            {
                TAU_LOG_ERROR("MATERIAL", "Truncated texture bind in: {}", cooked_path);
                return std::nullopt;
            }
            cooked_texture_bind_t bind;
            std::memcpy(&bind, bytes.data() + offset, sizeof(bind));
            const cooked_texture_bind_t* tex_bind = &bind;
            offset += sizeof(cooked_texture_bind_t);

            if (offset + tex_bind->path_length > bytes.size())
            {
                TAU_LOG_ERROR("MATERIAL", "Truncated texture path in: {}", cooked_path);
                return std::nullopt;
            }
            std::string tex_path(reinterpret_cast<char*>(bytes.data() + offset), tex_bind->path_length);
            offset += tex_bind->path_length;

            tex_path = assets::resolve(tex_bind->guid, tex_path);

            const asset_t<texture_t> tex = assets::load<texture_t>(tex_path);
            if (!assets::get(tex))
            {
                TAU_LOG_WARN("MATERIAL", "Texture '{}' did not load, drawing the fallback", tex_path);
            }
            mat.set_texture(tex_bind->name_hash, tex);

            // the file named it, so it is authored: save_material writes only authored entries
            mat.authored_properties.push_back(tex_bind->name_hash);
        }

        for (u32_t i = 0; i < header->sampler_count; i++)
        {
            if (offset + sizeof(cooked_sampler_bind_t) > bytes.size())
            {
                TAU_LOG_ERROR("MATERIAL", "Truncated sampler bind in: {}", cooked_path);
                return std::nullopt;
            }
            cooked_sampler_bind_t* smp_bind = reinterpret_cast<cooked_sampler_bind_t*>(bytes.data() + offset);
            offset += sizeof(cooked_sampler_bind_t);

            mat.set_sampler(smp_bind->name_hash, static_cast<sampler_type_e>(smp_bind->sampler_value));
            mat.authored_properties.push_back(smp_bind->name_hash);
        }

        for (u32_t i = 0; i < header->property_count; i++)
        {
            if (offset + sizeof(cooked_property_t) > bytes.size())
            {
                TAU_LOG_ERROR("MATERIAL", "Truncated property in: {}", cooked_path);
                return std::nullopt;
            }
            cooked_property_t* prop = reinterpret_cast<cooked_property_t*>(bytes.data() + offset);
            offset += sizeof(cooked_property_t);

            if (offset + prop->data_size > bytes.size())
            {
                TAU_LOG_ERROR("MATERIAL", "Truncated property data in: {}", cooked_path);
                return std::nullopt;
            }
            mat.set_property_raw(prop->name_hash, bytes.data() + offset, prop->data_size);
            mat.authored_properties.push_back(prop->name_hash);
            offset += prop->data_size;
        }

        return mat;
    }

    void asset_loader_t<material_t>::unload(material_t& mat)
    {
        mat.release_heap();
        mat.textures.clear();
    }

    void asset_loader_t<material_t>::deps(const material_t& mat, std::vector<asset_handle_t>& out)
    {
        out.push_back(mat.shader);
        for (const auto [name_hash, texture] : mat.textures) { out.push_back(texture); }
    }

    asset_dep_e asset_loader_t<material_t>::dep_reloaded(material_t& mat, asset_handle_t dep)
    {
        // data blob follows shader layout, may move on shader change
        if (dep == mat.shader) { return asset_dep_e::RELOAD; }

        std::vector<u32_t> slots;
        for (const auto [name_hash, texture] : mat.textures)
        {
            if (texture == dep) { slots.push_back(name_hash); }
        }
        for (const u32_t name_hash : slots) { mat.refresh_texture(name_hash); }

        return asset_dep_e::KEEP;
    }
} // namespace tau
