#include "pipelines.h"

#include "tau/asset.h"
#include "tau/assets/shader.h"
#include "tau/jobs.h"
#include "tau/log.h"
#include "tau/rhi/device.h"
#include "tau/rhi/gpu_structs.h"
#include "tau/rhi/renderer.h"
#include "tau/rhi/renderer_resources.h"
#include "tau/rhi/renderer_types.h"
#include "tau/vfs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tau::renderer::pipelines
{
    namespace
    {
        using steady_t = std::chrono::steady_clock;

        std::mutex g_mutex;
        // keyed by the shader's source path
        std::unordered_map<std::string, shader_pipelines_t> g_table;
        bool g_frozen = false;
        std::atomic<u32_t> g_late = 0;

        f64 ms_since(steady_t::time_point start)
        { return std::chrono::duration<f64, std::milli>(steady_t::now() - start).count(); }

        VkShaderModule create_module(const std::vector<u32_t>& code)
        {
            if (code.empty()) { return VK_NULL_HANDLE; }

            VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = code.size() * sizeof(u32_t);
            info.pCode = code.data();

            VkShaderModule module = VK_NULL_HANDLE;
            if (vkCreateShaderModule(ctx.device, &info, nullptr, &module) != VK_SUCCESS)
            {
                TAU_LOG_ERROR("PIPELINES", "Failed to create a shader module");
                return VK_NULL_HANDLE;
            }
            return module;
        }

        // set 0 bindless, 1 frame globals, 2+ the shader's
        bool create_layout(const shader_file_t& file, shader_pipelines_t& out)
        {
            flat_map_t<std::vector<VkDescriptorSetLayoutBinding>> set_bindings;
            for (const shader_descriptor_binding_t& b : file.descriptor_bindings)
            {
                set_bindings[b.set].push_back({
                    .binding = b.binding,
                    .descriptorType = vulkan::map_descriptor_type(b.type),
                    .descriptorCount = b.count,
                    .stageFlags = VK_SHADER_STAGE_ALL,
                });
            }

            for (auto [set, bindings] : set_bindings)
            {
                const VkDescriptorSetLayoutCreateInfo info = {
                    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                    .bindingCount = static_cast<u32_t>(bindings.size()),
                    .pBindings = bindings.data(),
                };
                VkDescriptorSetLayout layout = VK_NULL_HANDLE;
                vkCreateDescriptorSetLayout(ctx.device, &info, nullptr, &layout);
                out.set_layouts[set] = layout;
            }

            u32_t max_set = 1;
            for (auto [set, layout] : out.set_layouts) { max_set = std::max(max_set, set); }

            std::vector<VkDescriptorSetLayout> sets(max_set + 1, VK_NULL_HANDLE);
            sets[0] = res_system.global_layout;
            sets[1] = res_system.frame_layout;
            for (u32_t i = 2; i <= max_set; i++)
            {
                if (const VkDescriptorSetLayout* layout = out.set_layouts.find(i)) { sets[i] = *layout; }
            }

            const VkPushConstantRange push_constant = {
                .stageFlags = static_cast<VkShaderStageFlags>(
                    file.header.is_compute ? VK_SHADER_STAGE_COMPUTE_BIT
                                           : (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)),
                .offset = 0,
                .size = sizeof(gpu::PushConstants),
            };
            const VkPipelineLayoutCreateInfo info = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .setLayoutCount = static_cast<u32_t>(sets.size()),
                .pSetLayouts = sets.data(),
                .pushConstantRangeCount = 1,
                .pPushConstantRanges = &push_constant,
            };
            return vkCreatePipelineLayout(ctx.device, &info, nullptr, &out.layout) == VK_SUCCESS;
        }

        struct modules_t
        {
            VkShaderModule vert = VK_NULL_HANDLE;
            VkShaderModule frag = VK_NULL_HANDLE;
            VkShaderModule gbuffer = VK_NULL_HANDLE;
            VkShaderModule comp = VK_NULL_HANDLE;

            void create(const shader_file_t& file)
            {
                vert = create_module(file.vert_spirv);
                frag = create_module(file.frag_spirv);
                gbuffer = create_module(file.gbuffer_spirv);
                comp = create_module(file.comp_spirv);
            }

            void destroy()
            {
                for (VkShaderModule m : {vert, frag, gbuffer, comp})
                {
                    if (m != VK_NULL_HANDLE) { vkDestroyShaderModule(ctx.device, m, nullptr); }
                }
            }
        };

        VkBlendFactor src_factor(blend_mode_e blend, bool alpha)
        {
            switch (blend)
            {
            case blend_mode_e::TRANSPARENT_ALPHA: return alpha ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
            case blend_mode_e::TRANSPARENT_ADD: return VK_BLEND_FACTOR_ONE;
            case blend_mode_e::TRANSPARENT_MULT: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
            case blend_mode_e::SOLID:
            case blend_mode_e::CUTOUT: break;
            }
            return VK_BLEND_FACTOR_ONE;
        }

        VkBlendFactor dst_factor(blend_mode_e blend)
        {
            switch (blend)
            {
            case blend_mode_e::TRANSPARENT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case blend_mode_e::TRANSPARENT_ADD: return VK_BLEND_FACTOR_ONE;
            case blend_mode_e::TRANSPARENT_MULT:
            case blend_mode_e::SOLID:
            case blend_mode_e::CUTOUT: break;
            }
            return VK_BLEND_FACTOR_ZERO;
        }

        // translates the cooked description
        VkPipeline create_pipeline(const shader_pipeline_desc_t& d, const modules_t& m, VkPipelineLayout layout,
                                   const std::string& shader_path)
        {
            VkPipeline pipeline = VK_NULL_HANDLE;
            VkResult result = VK_ERROR_UNKNOWN;
            const steady_t::time_point start = steady_t::now();

            if (m.comp != VK_NULL_HANDLE)
            {
                const VkComputePipelineCreateInfo info = {
                    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                    .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                              .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                              .module = m.comp,
                              .pName = SHADER_ENTRY_COMPUTE},
                    .layout = layout,
                };
                result = vkCreateComputePipelines(ctx.device, ctx.pipeline_cache, 1, &info, nullptr, &pipeline);
            }
            else
            {
                VkPipelineShaderStageCreateInfo stages[2] = {
                    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                     .stage = VK_SHADER_STAGE_VERTEX_BIT,
                     .module = m.vert,
                     .pName = SHADER_ENTRY_VERTEX                                                                 },
                    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                     .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                     .module = d.fragment == pipeline_fragment_e::GBUFFER ? m.gbuffer : m.frag,
                     .pName =
                         d.fragment == pipeline_fragment_e::GBUFFER ? SHADER_ENTRY_GBUFFER : SHADER_ENTRY_FRAGMENT},
                };
                const u32_t stage_count = d.fragment == pipeline_fragment_e::NONE ? 1u : 2u;
                if (m.vert == VK_NULL_HANDLE || (stage_count == 2 && stages[1].module == VK_NULL_HANDLE))
                {
                    TAU_LOG_ERROR("PIPELINES", "{} lacks the code for one of its pipelines", shader_path);
                    return VK_NULL_HANDLE;
                }

                VkFormat color_formats[PIPELINE_MAX_COLOR_TARGETS];
                for (u32_t i = 0; i < d.color_count; i++)
                {
                    const VkFormat f = static_cast<VkFormat>(d.color_formats[i]);
                    color_formats[i] = f == VK_FORMAT_UNDEFINED ? ctx.swapchain.format : f;
                }
                const VkPipelineRenderingCreateInfo rendering = {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                    .colorAttachmentCount = d.color_count,
                    .pColorAttachmentFormats = d.color_count > 0 ? color_formats : nullptr,
                    .depthAttachmentFormat = d.depth_attachment ? ctx.swapchain.depth_format : VK_FORMAT_UNDEFINED,
                };

                const VkPipelineVertexInputStateCreateInfo vertex_input = {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
                const VkPipelineInputAssemblyStateCreateInfo input_assembly = {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                };
                const VkPipelineViewportStateCreateInfo viewport = {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                    .viewportCount = 1,
                    .scissorCount = 1,
                };
                const VkPipelineRasterizationStateCreateInfo raster = {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                    .polygonMode = VK_POLYGON_MODE_FILL,
                    .cullMode = d.cull_mode,
                    .frontFace = VK_FRONT_FACE_CLOCKWISE,
                    .depthBiasEnable = d.depth_bias ? VK_TRUE : VK_FALSE,
                    .depthBiasConstantFactor = d.depth_bias_constant,
                    .depthBiasSlopeFactor = d.depth_bias_slope,
                    .lineWidth = 1.0f,
                };
                const VkPipelineMultisampleStateCreateInfo multisample = {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
                };
                const VkPipelineDepthStencilStateCreateInfo depth = {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                    .depthTestEnable = d.depth_test ? VK_TRUE : VK_FALSE,
                    .depthWriteEnable = d.depth_write ? VK_TRUE : VK_FALSE,
                    .depthCompareOp = static_cast<VkCompareOp>(d.depth_compare),
                };

                const bool blending = d.blend != blend_mode_e::SOLID && d.blend != blend_mode_e::CUTOUT;
                const VkPipelineColorBlendAttachmentState attachment = {
                    .blendEnable = blending ? VK_TRUE : VK_FALSE,
                    .srcColorBlendFactor = src_factor(d.blend, false),
                    .dstColorBlendFactor = dst_factor(d.blend),
                    .colorBlendOp = VK_BLEND_OP_ADD,
                    .srcAlphaBlendFactor = src_factor(d.blend, true),
                    .dstAlphaBlendFactor = dst_factor(d.blend),
                    .alphaBlendOp = VK_BLEND_OP_ADD,
                    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                                      VK_COLOR_COMPONENT_A_BIT,
                };
                VkPipelineColorBlendAttachmentState attachments[PIPELINE_MAX_COLOR_TARGETS];
                for (u32_t i = 0; i < d.color_count; i++) { attachments[i] = attachment; }
                const VkPipelineColorBlendStateCreateInfo blend = {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                    .attachmentCount = d.color_count,
                    .pAttachments = d.color_count > 0 ? attachments : nullptr,
                };

                const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
                const VkPipelineDynamicStateCreateInfo dynamic = {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                    .dynamicStateCount = 2,
                    .pDynamicStates = dynamic_states,
                };

                const VkGraphicsPipelineCreateInfo info = {
                    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                    .pNext = &rendering,
                    .stageCount = stage_count,
                    .pStages = stages,
                    .pVertexInputState = &vertex_input,
                    .pInputAssemblyState = &input_assembly,
                    .pViewportState = &viewport,
                    .pRasterizationState = &raster,
                    .pMultisampleState = &multisample,
                    .pDepthStencilState = &depth,
                    .pColorBlendState = &blend,
                    .pDynamicState = &dynamic,
                    .layout = layout,
                };
                result = vkCreateGraphicsPipelines(ctx.device, ctx.pipeline_cache, 1, &info, nullptr, &pipeline);
            }

            note_pipeline_created(ms_since(start));
            if (result == VK_SUCCESS)
            {
                static constexpr const char* VARIANT_NAMES[] = {"forward", "shadow", "gbuffer"};
                const std::string label = shader_path + " " + VARIANT_NAMES[static_cast<u32_t>(d.variant)];
                device::name(pipeline, VK_OBJECT_TYPE_PIPELINE, label.c_str());
            }
            if (result != VK_SUCCESS)
            {
                TAU_LOG_ERROR("PIPELINES", "Failed to create pipeline {} of {} (VkResult {})",
                              static_cast<u32_t>(d.variant), shader_path, static_cast<i32>(result));
                return VK_NULL_HANDLE;
            }
            return pipeline;
        }

        void retire(const shader_pipelines_t& old)
        {
            std::scoped_lock lock(res_system.deletion_mutex);
            bool layout_attached = false;
            for (VkPipeline p : old.variants)
            {
                if (p == VK_NULL_HANDLE) { continue; }
                res_system.deletion_queue.push_back({
                    .type = resource_type_e::PIPELINE,
                    .handle = {.pipeline = {p, layout_attached ? VK_NULL_HANDLE : old.layout}},
                    .bindless_id = BINDLESS_NULL_HANDLE,
                    .gpu_timeline_value = deletion_timeline_value(),
                });
                layout_attached = true;
            }
            if (!layout_attached && old.layout != VK_NULL_HANDLE)
            {
                res_system.deletion_queue.push_back({
                    .type = resource_type_e::PIPELINE,
                    .handle = {.pipeline = {VK_NULL_HANDLE, old.layout}},
                    .bindless_id = BINDLESS_NULL_HANDLE,
                    .gpu_timeline_value = deletion_timeline_value(),
                });
            }
            for (auto [set, layout] : old.set_layouts)
            {
                res_system.deletion_queue.push_back({
                    .type = resource_type_e::DESCRIPTOR_SET_LAYOUT,
                    .handle = {.descriptor_set_layout = layout},
                    .bindless_id = BINDLESS_NULL_HANDLE,
                    .gpu_timeline_value = deletion_timeline_value(),
                });
            }
        }

        // one shader on the calling thread
        shader_pipelines_t build_one(const std::string& path, const shader_file_t& file)
        {
            shader_pipelines_t built;
            built.content_hash = file.content_hash;
            if (!create_layout(file, built)) { return {}; }
            device::name(built.layout, VK_OBJECT_TYPE_PIPELINE_LAYOUT, path.c_str());

            modules_t modules;
            modules.create(file);
            for (const shader_pipeline_desc_t& d : file.pipelines)
            {
                built.variants[static_cast<u32_t>(d.variant)] = create_pipeline(d, modules, built.layout, path);
            }
            modules.destroy();
            return built;
        }

        struct pending_t
        {
            std::string path;
            shader_file_t file;
            shader_pipelines_t built;
            modules_t modules;
        };

        struct work_t
        {
            pending_t* shader;
            const shader_pipeline_desc_t* desc;
        };
    } // namespace

    void build_all()
    {
        std::scoped_lock lock(g_mutex);
        const steady_t::time_point start = steady_t::now();

        std::vector<std::unique_ptr<pending_t>> pending;
        for (const auto& [path, guid] : assets::locations())
        {
            if (!assets::path_is<shader_t>(path)) { continue; }

            const std::string cooked = assets::cooked_path(get_type_id<shader_t>(), path);
            if (!vfs::exists(cooked)) { continue; }

            if (g_table.contains(path)) { continue; }

            auto p = std::make_unique<pending_t>();
            p->path = path;
            if (!read_shader_file(cooked, p->file)) { continue; }

            p->built.content_hash = p->file.content_hash;
            if (!create_layout(p->file, p->built)) { continue; }
            device::name(p->built.layout, VK_OBJECT_TYPE_PIPELINE_LAYOUT, path.c_str());
            p->modules.create(p->file);
            pending.push_back(std::move(p));
        }

        std::vector<work_t> work;
        for (const std::unique_ptr<pending_t>& p : pending)
        {
            for (const shader_pipeline_desc_t& d : p->file.pipelines) { work.push_back({p.get(), &d}); }
        }

        // cache + device internally synced, one slot per job
        jobs::counter_t counter;
        work_t* items = work.data();
        jobs::dispatch(
            static_cast<u32_t>(work.size()), 1,
            [items](u32_t begin, u32_t end)
            {
                for (u32_t i = begin; i < end; i++)
                {
                    pending_t& s = *items[i].shader;
                    s.built.variants[static_cast<u32_t>(items[i].desc->variant)] =
                        create_pipeline(*items[i].desc, s.modules, s.built.layout, s.path);
                }
            },
            &counter);
        jobs::wait(&counter);

        for (const std::unique_ptr<pending_t>& p : pending)
        {
            p->modules.destroy();
            g_table[p->path] = p->built;
        }

        if (!work.empty())
        {
            TAU_LOG_INFO("PIPELINES", "Built {} pipelines of {} shaders in {:.1f} ms", work.size(), pending.size(),
                         ms_since(start));
        }
    }

    void freeze()
    {
        std::scoped_lock lock(g_mutex);
        g_frozen = true;
    }

    u32_t late_count() { return g_late.load(std::memory_order_relaxed); }

    shader_pipelines_t acquire(const std::string& shader_path, const shader_file_t& file)
    {
        std::scoped_lock lock(g_mutex);

        const auto it = g_table.find(shader_path);
        if (it != g_table.end() && it->second.content_hash == file.content_hash) { return it->second; }

        if (g_frozen)
        {
            g_late.fetch_add(1, std::memory_order_relaxed);
            TAU_LOG_WARN("PIPELINES", "Building pipelines for {} after startup", shader_path);
        }

        shader_pipelines_t built = build_one(shader_path, file);
        if (built.layout == VK_NULL_HANDLE) { return {}; }

        if (it != g_table.end())
        {
            retire(it->second);
            TAU_LOG_INFO("PIPELINES", "Rebuilt the pipelines of {}", shader_path);
        }
        g_table[shader_path] = built;
        return built;
    }

    void shutdown()
    {
        std::scoped_lock lock(g_mutex);
        for (auto& [path, entry] : g_table)
        {
            for (VkPipeline p : entry.variants)
            {
                if (p != VK_NULL_HANDLE) { vkDestroyPipeline(ctx.device, p, nullptr); }
            }
            if (entry.layout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(ctx.device, entry.layout, nullptr); }
            for (auto [set, layout] : entry.set_layouts) { vkDestroyDescriptorSetLayout(ctx.device, layout, nullptr); }
        }
        g_table.clear();
        g_frozen = false;
        g_late = 0;
    }
} // namespace tau::renderer::pipelines
