#include "shader.h"

#include "tau/asset.h"
#include "tau/assets/shader_format.h"
#include "tau/containers/flat_map.h"
#include "tau/defines.h"
#include "tau/log.h"
#include "tau/rendering/renderer.h"
#include "tau/rendering/renderer_resources.h"
#include "tau/rendering/renderer_types.h"
#include "tau/rendering/vulkan.h"
#include "tau/vfs.h"
#include "vulkan/vulkan_core.h"

#include <SDL3/SDL_iostream.h>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace tau
{
    namespace
    {
        VkShaderModule create_shader_module(const std::vector<u32_t>& code)
        {
            VkShaderModuleCreateInfo shader_module_info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            shader_module_info.codeSize = code.size() * 4;
            shader_module_info.pCode = code.data();

            VkShaderModule module;
            if (vkCreateShaderModule(renderer::ctx.device, &shader_module_info, nullptr, &module) != VK_SUCCESS)
            {
                TAU_LOG_ERROR("VULKAN", "Failed to create shader module");
                return VK_NULL_HANDLE;
            }

            return module;
        }
    } // namespace

    std::optional<shader_t> tau::asset_loader_t<shader_t>::load(const std::string& path)
    {
        std::string cooked_path = tau::assets::cooked_path(tau::get_type_id<shader_t>(), path);

        SDL_IOStream* stream = tau::vfs::open_read(cooked_path);
        if (!stream)
        {
            TAU_LOG_ERROR("SHADER", "Shader not found: {}", cooked_path);
            return std::nullopt;
        }

        shader_header_t header;
        SDL_ReadIO(stream, &header, sizeof(shader_header_t));

        if (header.magic != TAU_SHADER_MAGIC)
        {
            TAU_LOG_ERROR("SHADER", "Invalid .taushader file: {}", cooked_path);
            SDL_CloseIO(stream);
            return std::nullopt;
        }

        if (header.version != TAU_SHADER_VERSION)
        {
            TAU_LOG_ERROR("SHADER", "Unsupported .taushader version {} (engine expects {}), recook: {}", header.version,
                          TAU_SHADER_VERSION, cooked_path);
            SDL_CloseIO(stream);
            return std::nullopt;
        }

        shader_t shader;
        shader.is_compute = header.is_compute;
        shader.has_material_data = header.has_material_data;

        shader.target_formats.resize(header.target_format_count);
        SDL_ReadIO(stream, shader.target_formats.data(), header.target_format_count * sizeof(VkFormat));

        shader.gbuffer_target_formats.resize(header.gbuffer_target_format_count);
        SDL_ReadIO(stream, shader.gbuffer_target_formats.data(), header.gbuffer_target_format_count * sizeof(VkFormat));

        for (VkFormat& format : shader.target_formats)
        {
            if (format == VK_FORMAT_UNDEFINED) { format = renderer::ctx.swapchain.format; }
        }

        if (shader.has_material_data)
        {
            shader_module_header_t mod_header;
            SDL_ReadIO(stream, &mod_header, sizeof(shader_module_header_t));

            shader.module = {
                .name = mod_header.name,
                .size = mod_header.size,
                .domain = mod_header.domain,
                .blend_mode = mod_header.blend_mode,
                .depth_write = mod_header.depth_write,
                .depth_test = mod_header.depth_test,
                .casts_shadow = mod_header.casts_shadow,
            };

            for (u32_t j = 0; j < mod_header.member_count; j++)
            {
                shader_member_header_t member_header;
                SDL_ReadIO(stream, &member_header, sizeof(shader_member_header_t));
                shader.module.members[member_header.name_hash] = {
                    member_header.name,      member_header.offset,     member_header.size,
                    member_header.type,      member_header.edit,       member_header.range_min,
                    member_header.range_max, member_header.enum_names, member_header.tooltip};
            }
        }

        if (header.descriptor_binding_count > 0)
        {
            shader.descriptor_bindings.resize(header.descriptor_binding_count);
            SDL_ReadIO(stream, shader.descriptor_bindings.data(),
                       header.descriptor_binding_count * sizeof(shader_descriptor_binding_t));
        }

        std::vector<u32_t> vert_spirv(header.vert_spirv_size);
        std::vector<u32_t> frag_spirv(header.frag_spirv_size);
        std::vector<u32_t> gbuffer_spirv(header.gbuffer_spirv_size);
        std::vector<u32_t> comp_spirv(header.comp_spirv_size);

        if (header.vert_spirv_size > 0) { SDL_ReadIO(stream, vert_spirv.data(), header.vert_spirv_size * 4); }

        if (header.frag_spirv_size > 0) { SDL_ReadIO(stream, frag_spirv.data(), header.frag_spirv_size * 4); }

        if (header.gbuffer_spirv_size > 0) { SDL_ReadIO(stream, gbuffer_spirv.data(), header.gbuffer_spirv_size * 4); }

        if (header.comp_spirv_size > 0) { SDL_ReadIO(stream, comp_spirv.data(), header.comp_spirv_size * 4); }

        SDL_CloseIO(stream);

        flat_map_t<std::vector<VkDescriptorSetLayoutBinding>> set_bindings;
        for (const shader_descriptor_binding_t& b : shader.descriptor_bindings)
        {
            VkDescriptorSetLayoutBinding layout_binding = {
                .binding = b.binding,
                .descriptorType = vulkan::map_descriptor_type(b.type),
                .descriptorCount = b.count,
                .stageFlags = VK_SHADER_STAGE_ALL,
            };

            set_bindings[b.set].push_back(layout_binding);
        }

        for (auto [set, bindings] : set_bindings)
        {
            VkDescriptorSetLayoutCreateInfo layout_info = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .bindingCount = static_cast<u32_t>(bindings.size()),
                .pBindings = bindings.data(),
            };

            VkDescriptorSetLayout layout;
            vkCreateDescriptorSetLayout(renderer::ctx.device, &layout_info, nullptr, &layout);
            shader.custom_layouts[set] = layout;
        }

        u32_t max_set = 1;
        for (auto [set, layout] : shader.custom_layouts) { max_set = std::max(max_set, set); }

        std::vector<VkDescriptorSetLayout> set_layouts(max_set + 1, VK_NULL_HANDLE);

        set_layouts[0] = renderer::res_system.global_layout;
        set_layouts[1] = renderer::res_system.frame_layout;

        for (u32_t i = 2; i <= max_set; i++)
        {
            // should also check for skipped sets, but this works for now
            if (shader.custom_layouts.contains(i)) { set_layouts[i] = shader.custom_layouts[i]; }
        }

        VkPipelineColorBlendAttachmentState base_blend = {
            .blendEnable = VK_FALSE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                              VK_COLOR_COMPONENT_A_BIT,
        };

        blend_mode_e blend_mode = blend_mode_e::SOLID;
        bool is_depth_write = true;
        bool is_depth_test = true;

        if (shader.has_material_data)
        {
            blend_mode = shader.module.blend_mode;
            is_depth_write = shader.module.depth_write;
            is_depth_test = shader.module.depth_test;
        }

        switch (blend_mode)
        {
        case blend_mode_e::TRANSPARENT_ALPHA:
            base_blend.blendEnable = VK_TRUE;
            base_blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            base_blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            base_blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            base_blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case blend_mode_e::TRANSPARENT_ADD:
            base_blend.blendEnable = VK_TRUE;
            base_blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            base_blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            base_blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            base_blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
        case blend_mode_e::TRANSPARENT_MULT:
            base_blend.blendEnable = VK_TRUE;
            base_blend.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
            base_blend.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            base_blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
            base_blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            break;
        case blend_mode_e::SOLID:
        case blend_mode_e::CUTOUT: break;
        }

        VkPipelineVertexInputStateCreateInfo vertex_input_info = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount = 0,
            .pVertexBindingDescriptions = nullptr,
            .vertexAttributeDescriptionCount = 0,
            .pVertexAttributeDescriptions = nullptr,
        };

        VkPipelineInputAssemblyStateCreateInfo input_assembly = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .primitiveRestartEnable = VK_FALSE,
        };

        VkPipelineViewportStateCreateInfo viewport_state = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount = 1,
        };

        VkPipelineRasterizationStateCreateInfo rasterizer_info = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .depthClampEnable = VK_FALSE,
            .rasterizerDiscardEnable = VK_FALSE,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = static_cast<VkCullModeFlags>(is_depth_test ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE),
            .frontFace = VK_FRONT_FACE_CLOCKWISE,
            .depthBiasEnable = VK_FALSE,
            .lineWidth = 1.0f,
        };

        VkPipelineMultisampleStateCreateInfo multisampling_info = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            .sampleShadingEnable = VK_FALSE,
        };

        VkPipelineDepthStencilStateCreateInfo depth_stencil_info = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = is_depth_test ? VK_TRUE : VK_FALSE,
            .depthWriteEnable = is_depth_write ? VK_TRUE : VK_FALSE,
            .depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL, // reverse z
            .depthBoundsTestEnable = VK_FALSE,
            .stencilTestEnable = VK_FALSE,
        };

        std::vector<VkPipelineColorBlendAttachmentState> blend_attachments(shader.target_formats.size(), base_blend);

        VkPipelineColorBlendStateCreateInfo color_blend_info = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .logicOpEnable = VK_FALSE,
            .attachmentCount = static_cast<u32_t>(blend_attachments.size()),
            .pAttachments = blend_attachments.empty() ? nullptr : blend_attachments.data(),
        };

        std::vector<VkDynamicState> dynamic_states = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
        };
        VkPipelineDynamicStateCreateInfo dynamic_state_info = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = static_cast<u32_t>(dynamic_states.size()),
            .pDynamicStates = dynamic_states.data(),
        };

        VkPushConstantRange push_constant = {
            .stageFlags = static_cast<VkShaderStageFlags>(
                shader.is_compute ? VK_SHADER_STAGE_COMPUTE_BIT
                                  : (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)),
            .offset = 0,
            .size = sizeof(renderer::push_constants_t),
        };

        VkPipelineLayoutCreateInfo pipeline_layout_info = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = static_cast<u32_t>(set_layouts.size()),
            .pSetLayouts = set_layouts.data(),
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push_constant,
        };

        if (vkCreatePipelineLayout(renderer::ctx.device, &pipeline_layout_info, nullptr, &shader.pipeline_layout) !=
            VK_SUCCESS)
        {
            TAU_LOG_ERROR("SHADER", "Failed to create pipeline layout for shader at path: {}", path);
            return std::nullopt;
        }

        if (shader.is_compute)
        {
            VkShaderModule comp_module = create_shader_module(comp_spirv);

            VkPipelineShaderStageCreateInfo comp_stage_info = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = comp_module,
                .pName = "main",
            };

            VkComputePipelineCreateInfo comp_pipeline_info = {
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage = comp_stage_info,
                .layout = shader.pipeline_layout,
            };

            VkPipeline compute_pipeline = VK_NULL_HANDLE;
            const auto comp_start = std::chrono::steady_clock::now();
            VkResult comp_result = vkCreateComputePipelines(renderer::ctx.device, renderer::ctx.pipeline_cache, 1,
                                                            &comp_pipeline_info, nullptr, &compute_pipeline);
            renderer::note_pipeline_created(
                std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - comp_start).count());
            if (comp_result != VK_SUCCESS)
            {
                TAU_LOG_ERROR("SHADER", "Failed to create compute pipeline for shader at path: {} (VkResult {})", path,
                              (int)comp_result);
            }
            else
            {
                shader.pipelines[static_cast<u32_t>(pipeline_variant_e::FORWARD)] = compute_pipeline;
            }

            vkDestroyShaderModule(renderer::ctx.device, comp_module, nullptr);
        }
        else
        {
            VkShaderModule vert_mod = create_shader_module(vert_spirv);
            VkShaderModule frag_mod = frag_spirv.empty() ? VK_NULL_HANDLE : create_shader_module(frag_spirv);
            VkShaderModule gbuffer_mod = gbuffer_spirv.empty() ? VK_NULL_HANDLE : create_shader_module(gbuffer_spirv);

            // a shader that always draws into the gbuffer ships without forward fragment code
            // the cooker leaves it out, since nothing could bind it
            const bool needs_forward = renderer::pass_for(shader) != renderer::mesh_pass_e::GBUFFER;

            if (!vert_mod || (needs_forward && !frag_mod))
            {
                if (vert_mod) { vkDestroyShaderModule(renderer::ctx.device, vert_mod, nullptr); }
                if (frag_mod) { vkDestroyShaderModule(renderer::ctx.device, frag_mod, nullptr); }
                TAU_LOG_ERROR("SHADER", "Could not create Vulkan shader module from file at: {}", path);
                return std::nullopt;
            }

            VkPipelineShaderStageCreateInfo vert_stage_info = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            vert_stage_info.stage = VK_SHADER_STAGE_VERTEX_BIT;
            vert_stage_info.module = vert_mod;
            vert_stage_info.pName = "main";

            VkPipelineShaderStageCreateInfo frag_stage_info = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            frag_stage_info.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            frag_stage_info.module = frag_mod;
            frag_stage_info.pName = "main";

            VkPipelineShaderStageCreateInfo shader_stages[] = {vert_stage_info, frag_stage_info};

            VkPipelineColorBlendAttachmentState gbuffer_blend = {
                .blendEnable = VK_FALSE,
                .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                                  VK_COLOR_COMPONENT_A_BIT,
            };
            std::vector<VkPipelineColorBlendAttachmentState> gbuffer_blend_attachments(
                shader.gbuffer_target_formats.size(), gbuffer_blend);

            VkPipelineRenderingCreateInfo pipeline_rendering_info = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                .colorAttachmentCount = static_cast<u32_t>(shader.target_formats.size()),
                .pColorAttachmentFormats = shader.target_formats.empty() ? nullptr : shader.target_formats.data(),
            };

            if (is_depth_test || is_depth_write)
            {
                pipeline_rendering_info.depthAttachmentFormat = renderer::ctx.swapchain.depth_format;
            }

            // forward is always built, shadow only when needed
            struct pipeline_variant_config_t
            {
                pipeline_variant_e variant;
                bool needs_fragment_stage;
                bool has_color_attachments;
                VkCullModeFlags cull_mode;
                bool depth_bias_enabled;
            };

            const pipeline_variant_config_t variant_configs[] = {
                {pipeline_variant_e::FORWARD, true,  true,  rasterizer_info.cullMode, false},
                {pipeline_variant_e::SHADOW,  false, false, VK_CULL_MODE_FRONT_BIT,   true },
                {pipeline_variant_e::GBUFFER, true,  true,  rasterizer_info.cullMode, false},
            };

            auto wants_variant = [&](pipeline_variant_e variant)
            {
                if (variant == pipeline_variant_e::FORWARD)
                {
                    // tauSurfaceFragment is the forward/transparent path, never bound for a deferred
                    // capable opaque surface shader (always routed to the gbuffer pass), so skip its compile
                    return needs_forward;
                }
                if (variant == pipeline_variant_e::SHADOW)
                {
                    return shader.has_material_data && shader.module.casts_shadow;
                }
                if (variant == pipeline_variant_e::GBUFFER)
                {
                    return gbuffer_mod != VK_NULL_HANDLE && !shader.gbuffer_target_formats.empty();
                }
                return false;
            };

            for (const pipeline_variant_config_t& cfg : variant_configs)
            {
                if (!wants_variant(cfg.variant)) { continue; }

                VkPipelineRenderingCreateInfo variant_rendering_info = pipeline_rendering_info;
                if (!cfg.has_color_attachments)
                {
                    variant_rendering_info.colorAttachmentCount = 0;
                    variant_rendering_info.pColorAttachmentFormats = nullptr;
                }
                if (cfg.variant == pipeline_variant_e::GBUFFER)
                {
                    variant_rendering_info.colorAttachmentCount =
                        static_cast<u32_t>(shader.gbuffer_target_formats.size());
                    variant_rendering_info.pColorAttachmentFormats = shader.gbuffer_target_formats.data();
                    variant_rendering_info.depthAttachmentFormat = renderer::ctx.swapchain.depth_format;
                }
                if (cfg.variant == pipeline_variant_e::SHADOW)
                {
                    // depth only variants always render into a depth attachment
                    variant_rendering_info.depthAttachmentFormat = renderer::ctx.swapchain.depth_format;
                }

                VkPipelineRasterizationStateCreateInfo variant_rasterizer = rasterizer_info;
                variant_rasterizer.cullMode = cfg.cull_mode;
                variant_rasterizer.depthBiasEnable = cfg.depth_bias_enabled ? VK_TRUE : VK_FALSE;
                if (cfg.depth_bias_enabled)
                {
                    variant_rasterizer.depthBiasConstantFactor = 1.25f;
                    variant_rasterizer.depthBiasSlopeFactor = 1.75f;
                }

                VkPipelineDepthStencilStateCreateInfo variant_depth_stencil = depth_stencil_info;
                if (cfg.variant == pipeline_variant_e::SHADOW)
                {
                    variant_depth_stencil.depthTestEnable = VK_TRUE;
                    variant_depth_stencil.depthWriteEnable = VK_TRUE;
                    variant_depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
                }

                VkPipelineColorBlendStateCreateInfo variant_color_blend = color_blend_info;
                if (!cfg.has_color_attachments)
                {
                    variant_color_blend.attachmentCount = 0;
                    variant_color_blend.pAttachments = nullptr;
                }
                if (cfg.variant == pipeline_variant_e::GBUFFER)
                {
                    variant_color_blend.attachmentCount = static_cast<u32_t>(gbuffer_blend_attachments.size());
                    variant_color_blend.pAttachments = gbuffer_blend_attachments.data();
                }

                // the gbuffer variant swaps in its own fragment stage
                VkPipelineShaderStageCreateInfo variant_stages[2] = {shader_stages[0], shader_stages[1]};
                if (cfg.variant == pipeline_variant_e::GBUFFER) { variant_stages[1].module = gbuffer_mod; }

                VkPipelineDepthStencilStateCreateInfo gbuffer_depth = variant_depth_stencil;
                if (cfg.variant == pipeline_variant_e::GBUFFER)
                {
                    gbuffer_depth.depthTestEnable = VK_TRUE;
                    gbuffer_depth.depthWriteEnable = VK_TRUE;
                    variant_depth_stencil = gbuffer_depth;
                }

                VkGraphicsPipelineCreateInfo pipeline_info = {
                    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                    .pNext = &variant_rendering_info,
                    .stageCount = cfg.needs_fragment_stage ? 2u : 1u,
                    .pStages = variant_stages,
                    .pVertexInputState = &vertex_input_info,
                    .pInputAssemblyState = &input_assembly,
                    .pViewportState = &viewport_state,
                    .pRasterizationState = &variant_rasterizer,
                    .pMultisampleState = &multisampling_info,
                    .pDepthStencilState = &variant_depth_stencil,
                    .pColorBlendState = &variant_color_blend,
                    .pDynamicState = &dynamic_state_info,
                    .layout = shader.pipeline_layout,
                    .renderPass = VK_NULL_HANDLE,
                    .subpass = 0,
                    .basePipelineHandle = VK_NULL_HANDLE,
                };

                VkPipeline variant_pipeline = VK_NULL_HANDLE;
                const auto gfx_start = std::chrono::steady_clock::now();
                VkResult gfx_result = vkCreateGraphicsPipelines(renderer::ctx.device, renderer::ctx.pipeline_cache, 1,
                                                                &pipeline_info, nullptr, &variant_pipeline);
                renderer::note_pipeline_created(
                    std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - gfx_start).count());
                if (gfx_result != VK_SUCCESS)
                {
                    const char* variant_name = cfg.variant == pipeline_variant_e::SHADOW    ? "shadow"
                                               : cfg.variant == pipeline_variant_e::GBUFFER ? "gbuffer"
                                                                                            : "forward";
                    TAU_LOG_ERROR("SHADER", "Failed to create {} pipeline for shader at path: {} (VkResult {})",
                                  variant_name, path, (int)gfx_result);
                    continue;
                }

                shader.pipelines[static_cast<u32_t>(cfg.variant)] = variant_pipeline;
            }

            vkDestroyShaderModule(renderer::ctx.device, vert_mod, nullptr);
            if (frag_mod) { vkDestroyShaderModule(renderer::ctx.device, frag_mod, nullptr); }
            if (gbuffer_mod) { vkDestroyShaderModule(renderer::ctx.device, gbuffer_mod, nullptr); }
        }

        return shader;
    } // namespace tau

    void asset_loader_t<shader_t>::unload(shader_t& shader)
    {
        if (shader.pipelines.empty()) { return; }

        std::scoped_lock lock(renderer::res_system.deletion_mutex);

        // pipeline layout is shared, so only one deletion
        bool layout_attached = false;
        for (auto [variant, pipeline] : shader.pipelines)
        {
            renderer::res_system.deletion_queue.push_back({
                .type = renderer::resource_type_e::PIPELINE,
                .handle = {.pipeline = {pipeline, layout_attached ? VK_NULL_HANDLE : shader.pipeline_layout}},
                .bindless_id = renderer::BINDLESS_NULL_HANDLE,
                .gpu_timeline_value = renderer::deletion_timeline_value(),
            });
            layout_attached = true;
        }

        for (auto [set, layout] : shader.custom_layouts)
        {
            renderer::res_system.deletion_queue.push_back({
                .type = renderer::resource_type_e::DESCRIPTOR_SET_LAYOUT,
                .handle = {.descriptor_set_layout = layout},
                .bindless_id = renderer::BINDLESS_NULL_HANDLE,
                .gpu_timeline_value = renderer::deletion_timeline_value(),
            });
        }

        shader.custom_layouts.clear();

        shader.pipelines.clear();
        shader.pipeline_layout = VK_NULL_HANDLE;
    }
} // namespace tau