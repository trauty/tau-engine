#include "vulkan.h"

#include "tau/assets/shader_format.h"

namespace tau::vulkan
{
    VkDescriptorType map_descriptor_type(descriptor_type_e type)
    {
        switch (type)
        {
        case descriptor_type_e::SAMPLER: return VK_DESCRIPTOR_TYPE_SAMPLER;
        case descriptor_type_e::SAMPLED_IMAGE: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case descriptor_type_e::STORAGE_IMAGE: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case descriptor_type_e::UNIFORM_BUFFER: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case descriptor_type_e::STORAGE_BUFFER: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        default: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
    }
} // namespace tau::vulkan