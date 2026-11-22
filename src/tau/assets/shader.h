#pragma once

#include "tau/asset.h"
#include "tau/assets/shader_format.h"
#include "tau/containers/flat_map.h"
#include "tau/defines.h"
#include "tau/rhi/vulkan.h"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tau
{
    struct TAU_ENGINE_API shader_member_t
    {
        std::string name;
        u32_t offset = 0;
        u32_t size = 0;
        shader_member_type_e type = shader_member_type_e::UNKNOWN;

        shader_member_edit_e edit = shader_member_edit_e::DEFAULT;
        f32 range_min = 0.0f;
        f32 range_max = 0.0f;
        std::string enum_names;
        std::string tooltip;

        bool has_range() const { return range_max > range_min; }
    };

    struct TAU_ENGINE_API shader_module_info_t
    {
        std::string name;
        u32_t size;
        flat_map_t<shader_member_t> members;

        shader_domain_e domain = shader_domain_e::SURFACE;
        blend_mode_e blend_mode = blend_mode_e::SOLID;
        bool depth_write = true;
        bool depth_test = true;
        bool casts_shadow = true;
    };

    struct shader_file_t
    {
        shader_header_t header = {};
        std::vector<VkFormat> target_formats;
        std::vector<VkFormat> gbuffer_target_formats;
        shader_module_info_t module;
        std::vector<shader_descriptor_binding_t> descriptor_bindings;
        std::vector<shader_pipeline_desc_t> pipelines;
        std::vector<u32_t> vert_spirv;
        std::vector<u32_t> frag_spirv;
        std::vector<u32_t> gbuffer_spirv;
        std::vector<u32_t> comp_spirv;
        u64_t content_hash = 0;
    };

    bool read_shader_file(const std::string& cooked_path, shader_file_t& out);
    // the file's bytes already read; `cooked_path` names it in errors
    bool parse_shader_file(const std::string& cooked_path, std::span<const u8_t> bytes, shader_file_t& out);

    struct TAU_ENGINE_API shader_t
    {
        static constexpr std::string_view type_key = "shader";

        // owned by renderer::pipelines
        flat_map_t<VkPipeline> pipelines;
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;

        bool has_material_data = false;
        shader_module_info_t module;

        flat_map_t<VkDescriptorSetLayout> custom_layouts;
        std::vector<shader_descriptor_binding_t> descriptor_bindings;

        bool is_compute = false;
        std::vector<VkFormat> target_formats;
        std::vector<VkFormat> gbuffer_target_formats;

        bool is_deferred_capable() const { return !gbuffer_target_formats.empty(); }

        VkPipeline get_pipeline(pipeline_variant_e variant) const
        {
            const VkPipeline* p = pipelines.find(static_cast<u32_t>(variant));
            return p ? *p : VK_NULL_HANDLE;
        }

        bool ready() const { return pipeline_layout != VK_NULL_HANDLE; }
    };

    template <>
    struct TAU_ENGINE_API asset_loader_t<shader_t>
    {
        static std::optional<shader_t> load(const std::string& path, std::span<const u8_t> bytes);
        static void unload(shader_t& shader);
    };
} // namespace tau
