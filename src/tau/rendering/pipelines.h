#pragma once

#include "tau/assets/shader_format.h"
#include "tau/containers/flat_map.h"
#include "tau/defines.h"
#include "tau/rendering/vulkan.h"

#include <string>

namespace tau { struct shader_file_t; }

// every pipeline, built before the first frame
namespace tau::renderer::pipelines
{
    struct shader_pipelines_t
    {
        u64_t content_hash = 0;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        flat_map_t<VkDescriptorSetLayout> set_layouts;
        VkPipeline variants[static_cast<u32_t>(pipeline_variant_e::COUNT)] = {};
    };

    // cooked shaders not built yet, on job threads
    TAU_ENGINE_API void build_all();

    // later builds count as late
    TAU_ENGINE_API void freeze();
    TAU_ENGINE_API u32_t late_count();

    // builds if missing or changed
    shader_pipelines_t acquire(const std::string& shader_path, const shader_file_t& file);

    void shutdown();
} // namespace tau::renderer::pipelines
