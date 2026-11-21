#pragma once

#include "tau/assets/shader_format.h"
#include "tau/containers/flat_map.h"
#include "tau/defines.h"
#include "tau/rendering/vulkan.h"

#include <string>

namespace tau { struct shader_file_t; }

// every pipeline, built from the cooked shaders before the first frame
namespace tau::renderer::pipelines
{
    struct shader_pipelines_t
    {
        u64_t content_hash = 0;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        flat_map_t<VkDescriptorSetLayout> set_layouts;
        VkPipeline variants[static_cast<u32_t>(pipeline_variant_e::COUNT)] = {};
    };

    // every cooked shader the guid maps know and the table lacks, on job threads through the pipeline cache
    // renderer init builds the engine's, project::mount the project's
    TAU_ENGINE_API void build_all();

    // from here on a pipeline built on demand is late: logged and counted
    TAU_ENGINE_API void freeze();
    TAU_ENGINE_API u32_t late_count();

    // the shader loader's way in: the pipelines of this file, built now when missing or when the file changed
    shader_pipelines_t acquire(const std::string& shader_path, const shader_file_t& file);

    void shutdown();
} // namespace tau::renderer::pipelines
