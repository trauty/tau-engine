#pragma once

// clang-format off
#include "tau/defines.h"
#include <volk.h>
#include <vma/vk_mem_alloc.h>
// clang-format on

namespace tau { enum class descriptor_type_e : u32_t; }

namespace tau::vulkan { VkDescriptorType map_descriptor_type(tau::descriptor_type_e type); }