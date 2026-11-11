#pragma once

#include "tau/assets/shader.h"
#include "tau/assets/shader_format.h"
#include "tau/defines.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace tau::cooker::shader
{
    // false if Slang cannot run the way the cooker relies on, then no shader cooks
    bool init();

    // fingerprints the shared struct layouts for the cook cache, see verify_core_layouts
    u64_t core_layout_stamp();

    struct slang_compilation_res_t
    {
        std::vector<u32> vert_spirv;
        std::vector<u32> frag_spirv;
        std::vector<u32> compute_spirv;
        std::vector<u32> gbuffer_spirv;

        std::vector<shader_module_info_t> shader_types;
        std::vector<VkFormat> target_formats;
        std::vector<VkFormat> gbuffer_target_formats;
        std::vector<shader_descriptor_binding_t> descriptor_bindings;

        bool is_compute = false;
        bool has_gbuffer_entry = false;
        bool success = false;

        // a .slang others import that draws nothing, not an error
        bool no_entry_point = false;

        // every file Slang read for this module, itself included: the real dependency set
        std::vector<std::filesystem::path> dependencies;
    };

    slang_compilation_res_t compile_slang_to_spirv(const std::string& module_name, const std::string& file_path,
                                                   const std::string& source_code,
                                                   const std::vector<std::string>& input_dirs);
    void write_taushader(const std::string& output_path, const slang_compilation_res_t& res);

    enum class cook_status_e
    {
        COOKED,
        SKIPPED, // import only module, no pipeline
        FAILED,
    };

    cook_status_e cook_shader(const std::string& input_path, const std::string& output_path,
                              const std::vector<std::string>& input_dirs,
                              std::vector<std::filesystem::path>& out_dependencies);
} // namespace tau::cooker::shader