#include "shader_cooker.h"

#include "tau-cooker/cache_manager.h"
#include "tau/assets/shader.h"
#include "tau/assets/shader_format.h"
#include "tau/hash.h"
#include "tau/log.h"
#include "tau/rendering/renderer_types.h"
#include "tau/rendering/shader_shared.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <slang-com-ptr.h>
#include <slang.h>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace tau::cooker::shader
{
    namespace
    {
        Slang::ComPtr<slang::IGlobalSession> global_session;

        // one compile session per run, so shared modules are parsed and checked once
        Slang::ComPtr<slang::ISession> run_session;
    } // namespace

    // validation and optimisation live in the glslang library Slang loads beside itself
    // without it Slang skips both quietly or loads a mismatched one from PATH, so the cook stops
    bool init()
    {
        // once per run, on the first shader: starting Slang is most of a run that cooks none
        static std::optional<bool> ready;
        if (ready) { return *ready; }
        ready = false;

        SlangGlobalSessionDesc global_desc = {};
        if (SLANG_FAILED(slang::createGlobalSession(&global_desc, global_session.writeRef())))
        {
            TAU_LOG_ERROR("SHADER_COOKER", "Could not create the Slang global session");
            return false;
        }

        if (SLANG_FAILED(global_session->checkPassThroughSupport(SLANG_PASS_THROUGH_GLSLANG)))
        {
            TAU_LOG_ERROR("SHADER_COOKER",
                          "Slang cannot load slang-glslang, which it validates and optimises SPIR-V with. It "
                          "belongs next to the cooker; rebuilding copies it there");
            return false;
        }

        TAU_LOG_INFO("SHADER_COOKER", "Slang {}", global_session->getBuildTagString());
        ready = true;
        return true;
    }

    // Swapchain maps to VK_FORMAT_UNDEFINED, so an unknown alias needs its own check
    bool map_alias_to_format(const std::string& alias, VkFormat& out)
    {
// the alias vocabulary lives in shader_shared.h
#define TAU_FORMAT_ALIAS(name, format)                                                                                 \
    if (alias == #name)                                                                                                \
    {                                                                                                                  \
        out = format;                                                                                                  \
        return true;                                                                                                   \
    }

        TAU_RENDER_TARGET_FORMATS(TAU_FORMAT_ALIAS)

#undef TAU_FORMAT_ALIAS

        return false;
    }

    bool parse_domain(const std::string& name, shader_domain_e& out)
    {
        if (name == "Surface")
        {
            out = shader_domain_e::SURFACE;
            return true;
        }
        if (name == "PostProcess")
        {
            out = shader_domain_e::POST_PROCESS;
            return true;
        }
        if (name == "Custom")
        {
            out = shader_domain_e::CUSTOM;
            return true;
        }
        return false;
    }

    bool parse_blend_mode(const std::string& name, blend_mode_e& out)
    {
        if (name == "Opaque")
        {
            out = blend_mode_e::SOLID;
            return true;
        }
        if (name == "Cutout")
        {
            out = blend_mode_e::CUTOUT;
            return true;
        }
        if (name == "TransparentAlpha")
        {
            out = blend_mode_e::TRANSPARENT_ALPHA;
            return true;
        }
        if (name == "TransparentAdd")
        {
            out = blend_mode_e::TRANSPARENT_ADD;
            return true;
        }
        if (name == "TransparentMult")
        {
            out = blend_mode_e::TRANSPARENT_MULT;
            return true;
        }
        return false;
    }

    bool validate_module(const shader_module_info_t& info)
    {
        bool ok = true;

        if (info.domain != shader_domain_e::SURFACE && info.casts_shadow)
        {
            TAU_LOG_ERROR("SHADER_COOKER", "'{}': only a Surface shader can cast a shadow", info.name);
            ok = false;
        }

        if (info.domain == shader_domain_e::POST_PROCESS && (info.depth_test || info.depth_write))
        {
            TAU_LOG_ERROR("SHADER_COOKER",
                          "'{}': a PostProcess shader draws a fullscreen triangle with no "
                          "depth attachment, so depth_test and depth_write must be false",
                          info.name);
            ok = false;
        }

        return ok;
    }

    tau::descriptor_type_e map_slang_type_to_descriptor(slang::TypeReflection* type)
    {
        slang::TypeReflection::Kind kind = type->getKind();
        if (kind == slang::TypeReflection::Kind::Resource)
        {
            SlangResourceShape shape = type->getResourceShape();
            SlangResourceAccess access = type->getResourceAccess();

            if (shape == SLANG_STRUCTURED_BUFFER || shape == SLANG_BYTE_ADDRESS_BUFFER)
            {
                return descriptor_type_e::STORAGE_BUFFER;
            }
            if (shape == SLANG_TEXTURE_2D)
            {
                return access == SLANG_RESOURCE_ACCESS_READ_WRITE ? descriptor_type_e::STORAGE_IMAGE
                                                                  : descriptor_type_e::SAMPLED_IMAGE;
            }
        }
        if (kind == slang::TypeReflection::Kind::SamplerState) { return descriptor_type_e::SAMPLER; }
        if (kind == slang::TypeReflection::Kind::ConstantBuffer) { return descriptor_type_e::UNIFORM_BUFFER; }

        return descriptor_type_e::STORAGE_BUFFER;
    }

    void reflect_descriptors(slang::ProgramLayout* layout, slang_compilation_res_t& res)
    {
        u32_t param_count = layout->getParameterCount();
        for (u32_t i = 0; i < param_count; i++)
        {
            slang::VariableLayoutReflection* param = layout->getParameterByIndex(i);
            if (!param) { continue; }

            u32_t set = param->getBindingSpace();

            if (set < 2) { continue; } // ignore bindless and globals sets

            shader_descriptor_binding_t binding;
            binding.name_hash = tau::hash_string(param->getName());
            binding.set = set;
            binding.binding = param->getBindingIndex();
            binding.count = 1;

            slang::TypeLayoutReflection* type_layout = param->getTypeLayout();
            binding.type = map_slang_type_to_descriptor(type_layout->getType());

            res.descriptor_bindings.push_back(binding);

            TAU_LOG_DEBUG("SHADER_COOKER", "Detected custom descriptor: Set {}; Binding {}; Type {};", set,
                          binding.binding, (u32_t)binding.type);
        }
    }

    shader_member_type_e classify_member(slang::TypeReflection* type)
    {
        using Kind = slang::TypeReflection::Kind;
        using Scalar = slang::TypeReflection::ScalarType;

        const Kind kind = type->getKind();

        auto from_scalar = [](Scalar scalar, u32_t components) -> shader_member_type_e
        {
            switch (scalar)
            {
            case Scalar::Float32:
                switch (components)
                {
                case 1: return shader_member_type_e::FLOAT;
                case 2: return shader_member_type_e::FLOAT2;
                case 3: return shader_member_type_e::FLOAT3;
                case 4: return shader_member_type_e::FLOAT4;
                default: return shader_member_type_e::UNKNOWN;
                }
            case Scalar::Int32: return components == 1 ? shader_member_type_e::INT : shader_member_type_e::UNKNOWN;
            case Scalar::UInt32: return components == 1 ? shader_member_type_e::UINT : shader_member_type_e::UNKNOWN;
            case Scalar::Bool: return components == 1 ? shader_member_type_e::BOOL : shader_member_type_e::UNKNOWN;
            default: return shader_member_type_e::UNKNOWN;
            }
        };

        if (kind == Kind::Scalar) { return from_scalar(type->getScalarType(), 1); }
        if (kind == Kind::Vector)
        {
            return from_scalar(type->getScalarType(), static_cast<u32_t>(type->getElementCount()));
        }

        return shader_member_type_e::UNKNOWN;
    }

    void read_member_attributes(slang::VariableReflection* var, shader_member_t& member, const std::string& full_name)
    {
        if (!var) { return; }

        const bool is_scalar_uint = (member.type == shader_member_type_e::UINT);
        const bool is_scalar_int = (member.type == shader_member_type_e::INT);
        const bool is_vec3_or_4 =
            (member.type == shader_member_type_e::FLOAT3 || member.type == shader_member_type_e::FLOAT4);

        auto set_edit = [&](shader_member_edit_e edit, const char* attr_name, bool allowed, const char* requires_what)
        {
            if (!allowed)
            {
                TAU_LOG_WARN("SHADER_COOKER", "Property '{}' is marked [{}] but is not {}, ignored", full_name,
                             attr_name, requires_what);
                return;
            }
            member.edit = edit;
        };

        for (u32 i = 0; i < var->getUserAttributeCount(); i++)
        {
            slang::UserAttribute* attr = var->getUserAttributeByIndex(i);
            if (!attr || !attr->getName()) { continue; }

            std::string name = attr->getName();
            if (name.size() > 9 && name.compare(name.size() - 9, 9, "Attribute") == 0) { name.erase(name.size() - 9); }

            if (name == "Hidden") { member.edit = shader_member_edit_e::HIDDEN; }
            else if (name == "Texture")
            {
                set_edit(shader_member_edit_e::TEXTURE, "Texture", is_scalar_uint, "a uint");
            }
            else if (name == "Sampler")
            {
                set_edit(shader_member_edit_e::SAMPLER, "Sampler", is_scalar_uint, "a uint");
            }
            else if (name == "Color")
            {
                set_edit(shader_member_edit_e::COLOR, "Color", is_vec3_or_4, "a float3 or float4");
            }
            else if (name == "Enum")
            {
                set_edit(shader_member_edit_e::ENUM, "Enum", is_scalar_uint || is_scalar_int, "an int or uint");

                size_t len = 0;
                if (const char* names = attr->getArgumentValueString(0, &len))
                {
                    member.enum_names.assign(names, len);
                    if (member.enum_names.size() >= SHADER_MEMBER_ENUM_NAMES_MAX)
                    {
                        TAU_LOG_WARN("SHADER_COOKER", "Enum labels for '{}' exceed {} chars -- truncated", full_name,
                                     SHADER_MEMBER_ENUM_NAMES_MAX - 1);
                        member.enum_names.resize(SHADER_MEMBER_ENUM_NAMES_MAX - 1);
                    }
                }
            }
            else if (name == "Range")
            {
                f32 lo = 0.0f;
                f32 hi = 0.0f;
                if (attr->getArgumentValueFloat(0, &lo) == SLANG_OK && attr->getArgumentValueFloat(1, &hi) == SLANG_OK)
                {
                    if (hi > lo)
                    {
                        member.range_min = lo;
                        member.range_max = hi;
                    }
                    else
                    {
                        TAU_LOG_WARN("SHADER_COOKER", "Range on '{}' is not increasing ({} -> {}), ignored", full_name,
                                     lo, hi);
                    }
                }
            }
            else if (name == "Tooltip")
            {
                size_t len = 0;
                if (const char* text = attr->getArgumentValueString(0, &len))
                {
                    member.tooltip.assign(text, len);
                    if (member.tooltip.size() >= SHADER_MEMBER_TOOLTIP_MAX)
                    {
                        member.tooltip.resize(SHADER_MEMBER_TOOLTIP_MAX - 1);
                    }
                }
            }
        }
    }

    void flatten_members(slang::TypeLayoutReflection* type_layout, const std::string& prefix, u32_t base_offset,
                         tau::flat_map_t<shader_member_t>& members)
    {
        u32_t field_count = type_layout->getFieldCount();
        for (u32_t i = 0; i < field_count; i++)
        {
            slang::VariableLayoutReflection* field = type_layout->getFieldByIndex(i);
            std::string field_name = field->getVariable()->getName();
            std::string full_name = prefix.empty() ? field_name : prefix + "_" + field_name;
            u32_t field_offset = base_offset + static_cast<u32_t>(field->getOffset());
            slang::TypeLayoutReflection* field_type_layout = field->getTypeLayout();

            if (field_type_layout->getType()->getKind() == slang::TypeReflection::Kind::Struct)
            {
                flatten_members(field_type_layout, full_name, field_offset, members);
            }
            else
            {
                shader_member_t member;
                member.name = full_name;
                member.offset = field_offset;
                member.size = static_cast<u32_t>(field_type_layout->getSize());
                member.type = classify_member(field_type_layout->getType());
                read_member_attributes(field->getVariable(), member, full_name);
                members[tau::hash_string(full_name)] = member;
            }
        }
    }

    // shared structs are declared in C++ and tau_globals.slang, so compare them
    // C++ numbers come from offsetof and sizeof, Slang reflects its own layout, a mismatch fails the cook
    namespace
    {
        struct layout_field_t
        {
            const char* name;
            u32_t offset;
            u32_t size;
        };

        // what must match depends on how a struct crosses the boundary
        // buffer address arrays need their stride, constant and push constant blocks their field offsets
        // Slang rounds a block up to 16 bytes, so its size may exceed the C++ one by less than that
        enum class layout_kind_e
        {
            BUFFER_ARRAY,
            UNIFORM_BLOCK,
        };

        struct layout_check_t
        {
            const char* slang_name;
            const char* cpp_name;
            layout_kind_e kind;
            u32_t cpp_size;
            std::vector<layout_field_t> fields;
        };

// Slang and C++ field names are identical, so one token gives both sides
#define TAU_FIELD(cpp_type, field)                                                                                     \
    layout_field_t                                                                                                     \
    { #field, static_cast<u32_t>(offsetof(cpp_type, field)), static_cast<u32_t>(sizeof(cpp_type::field)) }

        std::vector<layout_check_t> core_layout_checks()
        {
            using namespace tau::renderer;

            return {
                {"PushConstants",
                 "push_constants_t",  layout_kind_e::UNIFORM_BLOCK,
                 sizeof(push_constants_t),
                 {TAU_FIELD(push_constants_t, object_buffer), TAU_FIELD(push_constants_t, material_buffer),
                  TAU_FIELD(push_constants_t, custom_data), TAU_FIELD(push_constants_t, texture_id)}},

                {"ObjectData",
                 "object_data_t",     layout_kind_e::BUFFER_ARRAY,
                 sizeof(object_data_t),
                 {TAU_FIELD(object_data_t, model_matrix), TAU_FIELD(object_data_t, normal_matrix),
                  TAU_FIELD(object_data_t, vertex_buffer), TAU_FIELD(object_data_t, index_buffer),
                  TAU_FIELD(object_data_t, material_offset), TAU_FIELD(object_data_t, entity),
                  TAU_FIELD(object_data_t, _pad1), TAU_FIELD(object_data_t, _pad2)}                 },

                {"CullData",
                 "cull_data_t",       layout_kind_e::BUFFER_ARRAY,
                 sizeof(cull_data_t),
                 {TAU_FIELD(cull_data_t, bounding_sphere), TAU_FIELD(cull_data_t, index_count),
                  TAU_FIELD(cull_data_t, pipeline_index), TAU_FIELD(cull_data_t, flags),
                  TAU_FIELD(cull_data_t, draw_bin_offset)}                                          },

                {"ShadowTile",
                 "gpu_shadow_tile_t", layout_kind_e::BUFFER_ARRAY,
                 sizeof(gpu_shadow_tile_t),
                 {TAU_FIELD(gpu_shadow_tile_t, view_proj), TAU_FIELD(gpu_shadow_tile_t, uv_offset_scale),
                  TAU_FIELD(gpu_shadow_tile_t, texel_world_size), TAU_FIELD(gpu_shadow_tile_t, normal_bias),
                  TAU_FIELD(gpu_shadow_tile_t, depth_bias), TAU_FIELD(gpu_shadow_tile_t, split_far)}},

                {"GlobalData",
                 "global_data_t",     layout_kind_e::UNIFORM_BLOCK,
                 sizeof(global_data_t),
                 {TAU_FIELD(global_data_t, view),
                  TAU_FIELD(global_data_t, projection),
                  TAU_FIELD(global_data_t, view_proj),
                  TAU_FIELD(global_data_t, camera_pos),
                  TAU_FIELD(global_data_t, frustum_planes),
                  TAU_FIELD(global_data_t, inv_view_proj),
                  TAU_FIELD(global_data_t, dir_light_buffer),
                  TAU_FIELD(global_data_t, point_light_buffer),
                  TAU_FIELD(global_data_t, spot_light_buffer),
                  TAU_FIELD(global_data_t, cluster_counts_buffer),
                  TAU_FIELD(global_data_t, cluster_indices_buffer),
                  TAU_FIELD(global_data_t, shadow_cascade_buffer),
                  TAU_FIELD(global_data_t, time),
                  TAU_FIELD(global_data_t, dir_light_count),
                  TAU_FIELD(global_data_t, point_light_count),
                  TAU_FIELD(global_data_t, spot_light_count),
                  TAU_FIELD(global_data_t, object_count),
                  TAU_FIELD(global_data_t, active_pipeline_count),
                  TAU_FIELD(global_data_t, cull_flags),
                  TAU_FIELD(global_data_t, debug_view),
                  TAU_FIELD(global_data_t, shadow_atlas_id),
                  TAU_FIELD(global_data_t, shadow_cascade_count),
                  TAU_FIELD(global_data_t, shadow_fade_start),
                  TAU_FIELD(global_data_t, shadow_fade_end),
                  TAU_FIELD(global_data_t, cluster_z_near),
                  TAU_FIELD(global_data_t, cluster_z_far),
                  TAU_FIELD(global_data_t, cluster_tiles_x),
                  TAU_FIELD(global_data_t, cluster_tiles_y),
                  TAU_FIELD(global_data_t, punctual_shadow_buffer),
                  TAU_FIELD(global_data_t, punctual_shadow_atlas_id),
                  TAU_FIELD(global_data_t, punctual_shadow_count)}                                  },
            };
        }

#undef TAU_FIELD

        bool core_layouts_checked = false;
        bool core_layouts_ok = true;
    } // namespace

    u64_t core_layout_stamp()
    {
        std::string described;
        for (const layout_check_t& check : core_layout_checks())
        {
            described += check.slang_name;
            described += ":" + std::to_string(check.cpp_size);
            for (const layout_field_t& field : check.fields)
            {
                described += "," + std::string(field.name) + "@" + std::to_string(field.offset) + "+" +
                             std::to_string(field.size);
            }
            described += ";";
        }

        return XXH3_64bits(described.data(), described.size());
    }

    // returns false on any mismatch. runs once per process, the verdict sticks for every later shader
    bool verify_core_layouts(slang::ProgramLayout* layout)
    {
        if (layout == nullptr) { return true; }
        if (core_layouts_checked)
        {
            if (core_layouts_ok) { return true; }

            // a shader that sees none of the shared types is unaffected
            for (const layout_check_t& check : core_layout_checks())
            {
                if (layout->findTypeByName(check.slang_name) != nullptr)
                {
                    TAU_LOG_ERROR("SHADER_COOKER", "Not cooked: the shared struct layouts disagree (see above)");
                    return false;
                }
            }
            return true;
        }

        bool all_ok = true;
        bool saw_any = false;

        for (const layout_check_t& check : core_layout_checks())
        {
            slang::TypeReflection* type = layout->findTypeByName(check.slang_name);
            if (type == nullptr) { continue; }

            slang::TypeLayoutReflection* type_layout = layout->getTypeLayout(type);
            if (type_layout == nullptr) { continue; }

            saw_any = true;

            if (check.kind == layout_kind_e::BUFFER_ARRAY)
            {
                const u32_t slang_stride = static_cast<u32_t>(type_layout->getStride());
                if (slang_stride != check.cpp_size)
                {
                    TAU_LOG_ERROR("SHADER_COOKER",
                                  "{} strides {} bytes in Slang but {} is {} bytes in C++ -- every element "
                                  "after the first would be read from the wrong place",
                                  check.slang_name, slang_stride, check.cpp_name, check.cpp_size);
                    all_ok = false;
                }
            }
            else
            {
                const u32_t slang_size = static_cast<u32_t>(type_layout->getSize());
                if (slang_size < check.cpp_size || slang_size - check.cpp_size >= 16)
                {
                    TAU_LOG_ERROR("SHADER_COOKER",
                                  "{} is {} bytes in Slang but {} is {} bytes in C++ (a block may only round "
                                  "up to the next 16)",
                                  check.slang_name, slang_size, check.cpp_name, check.cpp_size);
                    all_ok = false;
                }
            }

            for (const layout_field_t& field : check.fields)
            {
                slang::VariableLayoutReflection* found = nullptr;
                for (u32_t i = 0; i < type_layout->getFieldCount(); i++)
                {
                    slang::VariableLayoutReflection* candidate = type_layout->getFieldByIndex(i);
                    const char* candidate_name = candidate->getVariable()->getName();
                    if (candidate_name != nullptr && std::strcmp(candidate_name, field.name) == 0)
                    {
                        found = candidate;
                        break;
                    }
                }

                if (found == nullptr)
                {
                    TAU_LOG_ERROR("SHADER_COOKER", "{}::{} exists in C++ ({}) but not in Slang's {}", check.cpp_name,
                                  field.name, check.cpp_name, check.slang_name);
                    all_ok = false;
                    continue;
                }

                const u32_t slang_offset = static_cast<u32_t>(found->getOffset());
                const u32_t field_size = static_cast<u32_t>(found->getTypeLayout()->getSize());

                if (slang_offset != field.offset || field_size != field.size)
                {
                    TAU_LOG_ERROR("SHADER_COOKER",
                                  "{}::{} disagrees -- Slang says offset {} size {}, C++ says offset {} size {}",
                                  check.slang_name, field.name, slang_offset, field_size, field.offset, field.size);
                    all_ok = false;
                }
            }

            if (static_cast<u32_t>(type_layout->getFieldCount()) != check.fields.size())
            {
                TAU_LOG_ERROR("SHADER_COOKER", "{} has {} fields in Slang but {} are checked against {}",
                              check.slang_name, static_cast<u32_t>(type_layout->getFieldCount()),
                              static_cast<u32_t>(check.fields.size()), check.cpp_name);
                all_ok = false;
            }
        }

        if (!saw_any) { return true; }

        core_layouts_checked = true;
        core_layouts_ok = all_ok;
        if (all_ok) { TAU_LOG_INFO("SHADER_COOKER", "Shared struct layouts agree between C++ and Slang"); }

        return all_ok;
    }

    // finds the struct carrying [ShaderConfig] by walking declarations
    // Slang lists only used parameters, and a material struct reached via getMaterial<T>() is not one
    std::vector<shader_module_info_t> reflect_slang_layout(slang::ProgramLayout* layout,
                                                           slang::DeclReflection* module_decl, bool& out_valid)
    {
        std::vector<shader_module_info_t> res;
        if (module_decl == nullptr) { return res; }

        const unsigned int child_count = module_decl->getChildrenCount();
        for (unsigned int i = 0; i < child_count; i++)
        {
            slang::DeclReflection* decl = module_decl->getChild(i);
            if (!decl || decl->getKind() != slang::DeclReflection::Kind::Struct) { continue; }

            slang::TypeReflection* target_type = decl->getType();
            if (!target_type) { continue; }

            bool is_material = false;
            shader_module_info_t shader_info;
            shader_info.name = target_type->getName() ? target_type->getName() : "Unknown";

            for (u32_t attr_idx = 0; attr_idx < target_type->getUserAttributeCount(); attr_idx++)
            {
                slang::UserAttribute* attr = target_type->getUserAttributeByIndex(attr_idx);
                std::string attr_name = attr->getName();

                if (attr_name == "ShaderConfig" || attr_name == "ShaderConfigAttribute")
                {
                    is_material = true;
                    size_t len = 0;
                    if (const char* domain_str = attr->getArgumentValueString(0, &len))
                    {
                        const std::string domain_name(domain_str, len);
                        if (!parse_domain(domain_name, shader_info.domain))
                        {
                            TAU_LOG_ERROR("SHADER_COOKER",
                                          "'{}': unknown shader domain '{}', expected Surface, PostProcess "
                                          "or Custom",
                                          shader_info.name, domain_name);
                            out_valid = false;
                        }
                    }

                    if (const char* blend_str = attr->getArgumentValueString(1, &len))
                    {
                        const std::string blend_name(blend_str, len);
                        if (!parse_blend_mode(blend_name, shader_info.blend_mode))
                        {
                            TAU_LOG_ERROR("SHADER_COOKER", "'{}': unknown blend mode '{}'", shader_info.name,
                                          blend_name);
                            out_valid = false;
                        }
                    }

                    i32 dw = 1, dt = 1, cs = 1;
                    attr->getArgumentValueInt(2, &dw);
                    shader_info.depth_write = dw != 0;
                    attr->getArgumentValueInt(3, &dt);
                    shader_info.depth_test = dt != 0;
                    attr->getArgumentValueInt(4, &cs);
                    shader_info.casts_shadow = cs != 0;

                    break;
                }
            }

            if (is_material)
            {
                slang::TypeLayoutReflection* struct_layout = layout->getTypeLayout(target_type);
                if (struct_layout == nullptr)
                {
                    TAU_LOG_ERROR("SHADER_COOKER", "'{}': could not lay out the shader config struct",
                                  shader_info.name);
                    out_valid = false;
                    continue;
                }
                shader_info.size = static_cast<u32_t>(struct_layout->getSize());

                flatten_members(struct_layout, "", 0, shader_info.members);

                if (!validate_module(shader_info)) { out_valid = false; }

                TAU_LOG_DEBUG("SHADER_COOKER", "Discovered shader module: {} (size: {} bytes)", shader_info.name,
                              shader_info.size);
                res.push_back(shader_info);
            }
        }

        // the format holds one material layout, a second would vanish
        if (res.size() > 1)
        {
            TAU_LOG_ERROR("SHADER_COOKER", "'{}' and '{}' are both [ShaderConfig] -- one per file", res[0].name,
                          res[1].name);
            out_valid = false;
        }

        return res;
    }

    // search paths: every input root plus every folder holding a .slang, so `import lighting;` resolves anywhere
    // module names are global, so a clash is reported here
    std::vector<std::string> collect_search_paths(const std::vector<std::string>& input_dirs)
    {
        std::vector<std::string> paths;
        std::unordered_map<std::string, std::string> first_by_name;

        auto add = [&](const std::string& dir)
        {
            if (std::find(paths.begin(), paths.end(), dir) == paths.end()) { paths.push_back(dir); }
        };

        for (const std::string& dir : input_dirs)
        {
            add(dir);

            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) { continue; }

            for (std::filesystem::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec))
            {
                if (ec || !it->is_regular_file(ec) || it->path().extension() != ".slang") { continue; }

                add(it->path().parent_path().generic_string());

                const std::string name = it->path().stem().string();
                const std::string where = it->path().generic_string();
                const auto [known, inserted] = first_by_name.emplace(name, where);
                if (!inserted)
                {
                    TAU_LOG_WARN("SHADER_COOKER",
                                 "Two shader modules are named '{}': {} and {}. `import {};` can only ever reach "
                                 "one of them -- rename one",
                                 name, known->second, where, name);
                }
            }
        }

        return paths;
    }

    slang::ISession* shared_session(const std::vector<std::string>& input_dirs)
    {
        if (run_session) { return run_session.get(); }

        const std::vector<std::string> search_paths = collect_search_paths(input_dirs);
        std::vector<const char*> search_path_ptrs;
        for (const std::string& dir : search_paths) { search_path_ptrs.push_back(dir.c_str()); }

        std::vector<slang::CompilerOptionEntry> compiler_options;

        auto add_int_option = [&](slang::CompilerOptionName name, int32_t val)
        {
            slang::CompilerOptionEntry entry = {};
            entry.name = name;
            entry.value.kind = slang::CompilerOptionValueKind::Int;
            entry.value.intValue0 = val;
            compiler_options.push_back(entry);
        };

        add_int_option(slang::CompilerOptionName::EmitSpirvDirectly, 1);

        // debug builds keep source level debug info for RenderDoc
        // skipping optimisation costs about 1% more machine code, the driver optimises anyway
        if constexpr (COOK_SHADER_DEBUG)
        {
            add_int_option(slang::CompilerOptionName::Optimization, SLANG_OPTIMIZATION_LEVEL_NONE);
            add_int_option(slang::CompilerOptionName::DebugInformation, SLANG_DEBUG_INFO_LEVEL_STANDARD);
        }
        else
        {
            add_int_option(slang::CompilerOptionName::Optimization, SLANG_OPTIMIZATION_LEVEL_MAXIMAL);
        }

        SlangCapabilityID spirv15_cap = global_session->findCapability("spirv_1_5");
        if (spirv15_cap != 0) { add_int_option(slang::CompilerOptionName::Capability, spirv15_cap); }

        slang::TargetDesc target_desc = {};
        target_desc.format = SLANG_SPIRV;
        target_desc.profile = global_session->findProfile("sm_6_6");
        target_desc.compilerOptionEntryCount = static_cast<u32>(compiler_options.size());
        target_desc.compilerOptionEntries = compiler_options.data();

        slang::SessionDesc session_desc = {};
        session_desc.searchPaths = search_path_ptrs.data();
        session_desc.searchPathCount = static_cast<SlangInt>(search_path_ptrs.size());
        session_desc.targets = &target_desc;
        session_desc.targetCount = 1;

        global_session->createSession(session_desc, run_session.writeRef());
        return run_session.get();
    }

    bool same_file(const char* a, const std::string& b)
    {
        if (a == nullptr) { return false; }

        std::error_code ec;
        return std::filesystem::weakly_canonical(a, ec) == std::filesystem::weakly_canonical(b, ec);
    }

    slang::IModule* find_loaded_module(slang::ISession* session, const std::string& file_path)
    {
        for (SlangInt i = 0; i < session->getLoadedModuleCount(); i++)
        {
            slang::IModule* loaded = session->getLoadedModule(i);
            if (loaded != nullptr && same_file(loaded->getFilePath(), file_path)) { return loaded; }
        }
        return nullptr;
    }

    slang::IModule* find_loaded_module_by_name(slang::ISession* session, const std::string& name)
    {
        for (SlangInt i = 0; i < session->getLoadedModuleCount(); i++)
        {
            slang::IModule* loaded = session->getLoadedModule(i);
            if (loaded != nullptr && loaded->getName() != nullptr && name == loaded->getName()) { return loaded; }
        }
        return nullptr;
    }

    slang_compilation_res_t compile_slang_to_spirv(const std::string& module_name, const std::string& file_path,
                                                   const std::string& source_code,
                                                   const std::vector<std::string>& input_dirs)
    {
        slang_compilation_res_t res;

        slang::ISession* session = shared_session(input_dirs);
        if (session == nullptr)
        {
            TAU_LOG_ERROR("SHADER_COOKER", "Could not create a Slang session");
            return res;
        }

        Slang::ComPtr<slang::IBlob> diag_blob;

        // already compiled and checked if an earlier shader imported it
        slang::IModule* module = find_loaded_module(session, file_path);
        if (module == nullptr)
        {
            // load under the name `import` uses so later importers reuse this copy
            // on a name clash (see collect_search_paths) use the path derived name
            const std::string import_name = std::filesystem::path(file_path).stem().string();
            const bool name_taken = find_loaded_module_by_name(session, import_name) != nullptr;
            const std::string& load_name = name_taken ? module_name : import_name;

            module = session->loadModuleFromSourceString(load_name.c_str(), file_path.c_str(), source_code.c_str(),
                                                         diag_blob.writeRef());
        }

        if (diag_blob && diag_blob->getBufferSize() > 0)
        {
            // loaded despite diagnostics, warnings only
            if (module) { TAU_LOG_WARN("SHADER_COOKER", "{}", (const char*)diag_blob->getBufferPointer()); }
            else
            {
                TAU_LOG_ERROR("SHADER_COOKER", "{}", (const char*)diag_blob->getBufferPointer());
            }
        }
        if (!module) { return res; }

        // files Slang read to build the module: the real import closure plus the file itself
        const SlangInt32 dep_count = module->getDependencyFileCount();
        for (SlangInt32 i = 0; i < dep_count; i++)
        {
            const char* dep = module->getDependencyFilePath(i);
            if (dep != nullptr) { res.dependencies.emplace_back(dep); }
        }

        std::vector<slang::IComponentType*> components;
        components.push_back(module);

        Slang::ComPtr<slang::IEntryPoint> comp_entry;
        module->findEntryPointByName("computeMain", comp_entry.writeRef());

        if (comp_entry)
        {
            res.is_compute = true;
            components.push_back(comp_entry);
        }
        else
        {
            Slang::ComPtr<slang::IEntryPoint> vert_entry;
            module->findEntryPointByName("vertexMain", vert_entry.writeRef());

            // no vertexMain or computeMain: an import only module
            if (!vert_entry)
            {
                res.no_entry_point = true;
                return res;
            }
            components.push_back(vert_entry);

            Slang::ComPtr<slang::IEntryPoint> frag_entry;
            module->findEntryPointByName("fragmentMain", frag_entry.writeRef());

            // entry points are read back by position, a gap would shift gbuffer code into the fragment slot
            if (!frag_entry)
            {
                TAU_LOG_ERROR("SHADER_COOKER", "'{}' has a vertexMain but no fragmentMain", file_path);
                return res;
            }
            components.push_back(frag_entry);

            Slang::ComPtr<slang::IEntryPoint> gbuffer_entry;
            module->findEntryPointByName("gbufferMain", gbuffer_entry.writeRef());
            if (gbuffer_entry)
            {
                res.has_gbuffer_entry = true;
                components.push_back(gbuffer_entry);
            }
        }

        Slang::ComPtr<slang::IComponentType> composed_program;
        session->createCompositeComponentType(components.data(), components.size(), composed_program.writeRef());

        Slang::ComPtr<slang::IComponentType> linked_program;
        {
            SlangResult link_res = composed_program->link(linked_program.writeRef(), diag_blob.writeRef());

            const bool linked = SLANG_SUCCEEDED(link_res) && linked_program;

            if (diag_blob && diag_blob->getBufferSize() > 0)
            {
                if (linked) { TAU_LOG_WARN("SHADER_COOKER", "{}", (const char*)diag_blob->getBufferPointer()); }
                else
                {
                    TAU_LOG_ERROR("SHADER_COOKER", "Linking of shader program failed: {}",
                                  (const char*)diag_blob->getBufferPointer());
                }
            }

            if (!linked) { return res; }
        }

        slang::ProgramLayout* layout = linked_program->getLayout();

        if (!verify_core_layouts(layout)) { return res; }

        bool modules_valid = true;
        res.shader_types = reflect_slang_layout(layout, module->getModuleReflection(), modules_valid);
        if (!modules_valid) { return res; }
        reflect_descriptors(layout, res);

        for (u32 ep_idx = 0; ep_idx < layout->getEntryPointCount(); ep_idx++)
        {
            slang::EntryPointReflection* entry_point = layout->getEntryPointByIndex(ep_idx);
            std::string ep_name = entry_point->getName() ? entry_point->getName() : "";

            const bool is_forward_ep = (ep_name == "fragmentMain");
            const bool is_gbuffer_ep = (ep_name == "gbufferMain");

            if (is_forward_ep || is_gbuffer_ep)
            {
                std::vector<VkFormat>& formats = is_gbuffer_ep ? res.gbuffer_target_formats : res.target_formats;

                slang::VariableLayoutReflection* result_var = entry_point->getResultVarLayout();
                if (!result_var) { continue; }

                slang::TypeReflection* result_type = result_var->getTypeLayout()->getType();

                for (u32 field_idx = 0; field_idx < result_type->getFieldCount(); field_idx++)
                {
                    slang::VariableReflection* field = result_type->getFieldByIndex(field_idx);

                    for (u32 attr_idx = 0; attr_idx < field->getUserAttributeCount(); attr_idx++)
                    {
                        slang::UserAttribute* attr_reflection = field->getUserAttributeByIndex(attr_idx);
                        std::string attr_name = attr_reflection->getName();

                        if (attr_name == "RenderTarget" || attr_name == "RenderTargetAttribute")
                        {
                            size_t len = 0;
                            const char* alias_str = attr_reflection->getArgumentValueString(0, &len);
                            const std::string alias = alias_str ? std::string(alias_str, len) : std::string{};

                            VkFormat format = VK_FORMAT_UNDEFINED;
                            if (!map_alias_to_format(alias, format))
                            {
                                TAU_LOG_ERROR("SHADER_COOKER",
                                              "'{}': {}.{} names render target format '{}', which is not one of "
                                              "TAU_RENDER_TARGET_FORMATS in shader_shared.h",
                                              file_path, result_type->getName(), field->getName(), alias);
                                return res;
                            }
                            formats.push_back(format);

                            TAU_LOG_DEBUG("SHADER_COOKER",
                                          "Found target format of target '{} - SV_Target{}' of shader '{}': {}",
                                          result_type->getName(), field_idx, file_path, alias);
                        }
                    }
                }
            }
        }

        // codegen can fail after a clean link and its diagnostics are the only explanation
        auto emit_spirv = [&](int entry_index, const char* stage, std::vector<u32>& out)
        {
            Slang::ComPtr<slang::IBlob> kernel_blob;
            Slang::ComPtr<slang::IBlob> code_diag;
            linked_program->getEntryPointCode(entry_index, 0, kernel_blob.writeRef(), code_diag.writeRef());

            if (code_diag && code_diag->getBufferSize() > 0)
            {
                if (kernel_blob)
                {
                    TAU_LOG_WARN("SHADER_COOKER", "{} ({}): {}", file_path, stage,
                                 (const char*)code_diag->getBufferPointer());
                }
                else
                {
                    TAU_LOG_ERROR("SHADER_COOKER", "{} ({}): {}", file_path, stage,
                                  (const char*)code_diag->getBufferPointer());
                }
            }
            if (!kernel_blob) { return; }

            // already validated by Slang, and optimised in a release cook, see init()
            const u32* words = static_cast<const u32*>(kernel_blob->getBufferPointer());
            out.assign(words, words + kernel_blob->getBufferSize() / 4);
        };

        if (res.is_compute)
        {
            emit_spirv(0, "compute", res.compute_spirv);
            res.success = !res.compute_spirv.empty();
        }
        else
        {
            // a gbufferMain only surface shader keeps fragmentMain for target formats but never generates its code
            const shader_module_info_t config = res.shader_types.empty() ? shader_module_info_t{} : res.shader_types[0];
            const bool forward_unused =
                res.has_gbuffer_entry &&
                draws_into_gbuffer(config.domain, config.blend_mode, !res.gbuffer_target_formats.empty());

            emit_spirv(0, "vertex", res.vert_spirv);
            if (!forward_unused) { emit_spirv(1, "fragment", res.frag_spirv); }
            if (res.has_gbuffer_entry) { emit_spirv(2, "gbuffer", res.gbuffer_spirv); }

            res.success = !res.vert_spirv.empty() && (forward_unused || !res.frag_spirv.empty()) &&
                          (!res.has_gbuffer_entry || !res.gbuffer_spirv.empty());
        }

        return res;
    }

    void write_taushader(const std::string& output_path, const slang_compilation_res_t& res)
    {
        std::filesystem::create_directories(std::filesystem::path(output_path).parent_path());

        std::ofstream out(output_path, std::ios::binary);
        if (!out.is_open())
        {
            TAU_LOG_ERROR("SHADER_COOKER", "Failed to open output file: {}", output_path);
            return;
        }

        // headers go to disk byte for byte, padding included
        // zero each struct first so two cooks of one shader write identical files
        shader_header_t header;
        std::memset(&header, 0, sizeof(header));
        header.magic = TAU_SHADER_MAGIC;
        header.version = TAU_SHADER_VERSION;
        header.is_compute = res.is_compute;
        header.has_material_data = !res.shader_types.empty();
        header.vert_spirv_size = static_cast<u32_t>(res.vert_spirv.size());
        header.frag_spirv_size = static_cast<u32_t>(res.frag_spirv.size());
        header.comp_spirv_size = static_cast<u32_t>(res.compute_spirv.size());
        header.gbuffer_spirv_size = static_cast<u32_t>(res.gbuffer_spirv.size());
        header.target_format_count = static_cast<u32_t>(res.target_formats.size());
        header.gbuffer_target_format_count = static_cast<u32_t>(res.gbuffer_target_formats.size());
        header.descriptor_binding_count = static_cast<u32_t>(res.descriptor_bindings.size());
        out.write(reinterpret_cast<const char*>(&header), sizeof(shader_header_t));

        out.write(reinterpret_cast<const char*>(res.target_formats.data()),
                  res.target_formats.size() * sizeof(VkFormat));
        out.write(reinterpret_cast<const char*>(res.gbuffer_target_formats.data()),
                  res.gbuffer_target_formats.size() * sizeof(VkFormat));

        if (!res.shader_types.empty())
        {
            shader_module_info_t module = res.shader_types[0];

            shader_module_header_t mod_header;
            std::memset(&mod_header, 0, sizeof(mod_header));
            std::snprintf(mod_header.name, sizeof(mod_header.name), "%s", module.name.c_str());
            mod_header.domain = module.domain;
            mod_header.blend_mode = module.blend_mode;
            mod_header.size = module.size;
            mod_header.depth_write = module.depth_write;
            mod_header.depth_test = module.depth_test;
            mod_header.casts_shadow = module.casts_shadow;
            mod_header.member_count = static_cast<u32_t>(module.members.size());
            out.write(reinterpret_cast<const char*>(&mod_header), sizeof(shader_module_header_t));

            for (auto [name_hash, member] : module.members)
            {
                shader_member_header_t member_header;
                std::memset(&member_header, 0, sizeof(member_header));
                member_header.name_hash = name_hash;
                member_header.offset = member.offset;
                member_header.size = member.size;
                member_header.type = member.type;
                member_header.edit = member.edit;
                member_header.range_min = member.range_min;
                member_header.range_max = member.range_max;
                std::snprintf(member_header.name, sizeof(member_header.name), "%s", member.name.c_str());
                std::snprintf(member_header.enum_names, sizeof(member_header.enum_names), "%s",
                              member.enum_names.c_str());
                std::snprintf(member_header.tooltip, sizeof(member_header.tooltip), "%s", member.tooltip.c_str());

                out.write(reinterpret_cast<const char*>(&member_header), sizeof(shader_member_header_t));
            }
        }

        if (!res.descriptor_bindings.empty())
        {
            out.write(reinterpret_cast<const char*>(res.descriptor_bindings.data()),
                      res.descriptor_bindings.size() * sizeof(shader_descriptor_binding_t));
        }

        if (!res.vert_spirv.empty())
        {
            out.write(reinterpret_cast<const char*>(res.vert_spirv.data()), res.vert_spirv.size() * 4);
        }

        if (!res.frag_spirv.empty())
        {
            out.write(reinterpret_cast<const char*>(res.frag_spirv.data()), res.frag_spirv.size() * 4);
        }

        if (!res.gbuffer_spirv.empty())
        {
            out.write(reinterpret_cast<const char*>(res.gbuffer_spirv.data()), res.gbuffer_spirv.size() * 4);
        }

        if (!res.compute_spirv.empty())
        {
            out.write(reinterpret_cast<const char*>(res.compute_spirv.data()), res.compute_spirv.size() * 4);
        }
    }

    cook_status_e cook_shader(const std::string& input_path, const std::string& output_path,
                              const std::vector<std::string>& input_dirs,
                              std::vector<std::filesystem::path>& out_dependencies)
    {
        TAU_LOG_INFO("SHADER_COOKER", "Cooking Shader: {} -> {}", input_path, output_path);
        if (!init()) { return cook_status_e::FAILED; }

        std::ifstream file(input_path, std::ios::binary);
        if (!file.is_open())
        {
            TAU_LOG_ERROR("SHADER_COOKER", "Failed to open shader file: {}", input_path);
            return cook_status_e::FAILED;
        }

        std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::string module_name = std::filesystem::path(input_path).stem().string();

        for (const std::string& dir : input_dirs)
        {
            std::error_code ec;
            std::filesystem::path rel_path = std::filesystem::relative(input_path, dir, ec);

            if (!ec && !rel_path.empty() && rel_path.string().find("..") == std::string::npos)
            {
                module_name = rel_path.replace_extension("").generic_string();
                std::replace(module_name.begin(), module_name.end(), '/', '.');
                break;
            }
        }

        slang_compilation_res_t res = compile_slang_to_spirv(module_name, input_path, source, input_dirs);
        out_dependencies = res.dependencies;

        if (res.no_entry_point) { return cook_status_e::SKIPPED; }

        if (!res.success)
        {
            TAU_LOG_ERROR("SHADER_COOKER", "Failed to cook: {}", input_path);
            return cook_status_e::FAILED;
        }

        write_taushader(output_path, res);
        return cook_status_e::COOKED;
    }
} // namespace tau::cooker::shader