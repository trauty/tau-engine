#include "device.h"

#include "tau/log.h"
#include "tau/rendering/renderer.h"

#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace tau::renderer::device
{
    namespace
    {
        constexpr const char* VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation";

        std::atomic<u32_t> error_count = 0;

        VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                      VkDebugUtilsMessageTypeFlagsEXT,
                                                      const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
        {
            if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            {
                error_count.fetch_add(1, std::memory_order_relaxed);
                TAU_LOG_ERROR("VULKAN", "{}", data->pMessage);
            }
            else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            {
                TAU_LOG_WARN("VULKAN", "{}", data->pMessage);
            }
            else
            {
                TAU_LOG_INFO("VULKAN", "{}", data->pMessage);
            }
            return VK_FALSE;
        }

        bool has_layer(const char* name)
        {
            u32_t count = 0;
            vkEnumerateInstanceLayerProperties(&count, nullptr);
            std::vector<VkLayerProperties> layers(count);
            vkEnumerateInstanceLayerProperties(&count, layers.data());
            return std::any_of(layers.begin(), layers.end(),
                               [name](const VkLayerProperties& l) { return std::strcmp(l.layerName, name) == 0; });
        }

        bool has_extension(const std::vector<VkExtensionProperties>& exts, const char* name)
        {
            return std::any_of(exts.begin(), exts.end(), [name](const VkExtensionProperties& e)
                               { return std::strcmp(e.extensionName, name) == 0; });
        }

        // the feature structs every device needs, plus the ray tracing ones the full device adds
        struct features_t
        {
            VkPhysicalDeviceFeatures2 f10 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
            VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            VkPhysicalDeviceAccelerationStructureFeaturesKHR as = {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
            VkPhysicalDeviceRayQueryFeaturesKHR rq = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
            VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR pf = {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR};
            VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pe = {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};

            // chained in place, so the struct must not move afterwards
            void chain(bool rt, bool position_fetch, bool pipeline_properties)
            {
                f10.pNext = &f11;
                f11.pNext = &f12;
                f12.pNext = &f13;
                void** tail = &f13.pNext;
                *tail = nullptr;
                if (rt)
                {
                    *tail = &as;
                    as.pNext = &rq;
                    tail = &rq.pNext;
                    *tail = nullptr;
                }
                if (position_fetch)
                {
                    *tail = &pf;
                    tail = &pf.pNext;
                    *tail = nullptr;
                }
                if (pipeline_properties)
                {
                    *tail = &pe;
                    pe.pNext = nullptr;
                }
            }
        };

        // geometryShader: PrimitiveId in fragment shaders (the visibility buffer prepass), no geometry shader runs
#define TAU_FEATURES_10(X)                                                                                             \
    X(samplerAnisotropy)                                                                                               \
    X(multiDrawIndirect) X(drawIndirectFirstInstance) X(shaderInt64) X(shaderStorageImageReadWithoutFormat)            \
        X(shaderStorageImageWriteWithoutFormat) X(shaderStorageImageExtendedFormats) X(fragmentStoresAndAtomics)       \
            X(depthClamp) X(geometryShader) X(imageCubeArray) X(dualSrcBlend)
#define TAU_FEATURES_11(X) X(shaderDrawParameters)
#define TAU_FEATURES_12(X)                                                                                             \
    X(drawIndirectCount)                                                                                               \
    X(storageBuffer8BitAccess) X(shaderFloat16) X(shaderInt8) X(descriptorIndexing)                                    \
        X(shaderSampledImageArrayNonUniformIndexing) X(shaderStorageBufferArrayNonUniformIndexing)                     \
            X(shaderStorageImageArrayNonUniformIndexing) X(descriptorBindingSampledImageUpdateAfterBind)               \
                X(descriptorBindingStorageImageUpdateAfterBind) X(descriptorBindingStorageBufferUpdateAfterBind)       \
                    X(descriptorBindingUpdateUnusedWhilePending) X(descriptorBindingPartiallyBound)                    \
                        X(descriptorBindingVariableDescriptorCount) X(runtimeDescriptorArray) X(scalarBlockLayout)     \
                            X(hostQueryReset) X(timelineSemaphore) X(bufferDeviceAddress)
// Slang lowers discard to demote
#define TAU_FEATURES_13(X) X(synchronization2) X(dynamicRendering) X(maintenance4) X(shaderDemoteToHelperInvocation)

        features_t wanted(bool rt, bool position_fetch, bool pipeline_properties)
        {
            features_t f;
#define TAU_SET(field) f.f10.features.field = VK_TRUE;
            TAU_FEATURES_10(TAU_SET)
#undef TAU_SET
#define TAU_SET(field) f.f11.field = VK_TRUE;
            TAU_FEATURES_11(TAU_SET)
#undef TAU_SET
#define TAU_SET(field) f.f12.field = VK_TRUE;
            TAU_FEATURES_12(TAU_SET)
#undef TAU_SET
#define TAU_SET(field) f.f13.field = VK_TRUE;
            TAU_FEATURES_13(TAU_SET)
#undef TAU_SET
            f.f10.features.shaderInt16 = rt ? VK_TRUE : VK_FALSE;
            f.f11.storageBuffer16BitAccess = rt ? VK_TRUE : VK_FALSE;
            f.as.accelerationStructure = rt ? VK_TRUE : VK_FALSE;
            f.rq.rayQuery = rt ? VK_TRUE : VK_FALSE;
            f.pf.rayTracingPositionFetch = position_fetch ? VK_TRUE : VK_FALSE;
            f.pe.pipelineExecutableInfo = pipeline_properties ? VK_TRUE : VK_FALSE;
            f.chain(rt, position_fetch, pipeline_properties);
            return f;
        }

        void missing_features(const features_t& have, std::vector<std::string>& out)
        {
#define TAU_CHECK(field)                                                                                               \
    if (!have.f10.features.field) { out.push_back(#field); }
            TAU_FEATURES_10(TAU_CHECK)
#undef TAU_CHECK
#define TAU_CHECK(field)                                                                                               \
    if (!have.f11.field) { out.push_back(#field); }
            TAU_FEATURES_11(TAU_CHECK)
#undef TAU_CHECK
#define TAU_CHECK(field)                                                                                               \
    if (!have.f12.field) { out.push_back(#field); }
            TAU_FEATURES_12(TAU_CHECK)
#undef TAU_CHECK
#define TAU_CHECK(field)                                                                                               \
    if (!have.f13.field) { out.push_back(#field); }
            TAU_FEATURES_13(TAU_CHECK)
#undef TAU_CHECK
        }

        struct candidate_t
        {
            VkPhysicalDevice physical = VK_NULL_HANDLE;
            VkPhysicalDeviceProperties props = {};
            queue_family_indices_t families;
            bool rt = false;
            bool position_fetch = false;
            bool pipeline_properties = false;
            std::vector<std::string> missing;
        };

        candidate_t inspect(VkPhysicalDevice physical, VkSurfaceKHR surface, const device_config_t& config)
        {
            candidate_t c;
            c.physical = physical;
            vkGetPhysicalDeviceProperties(physical, &c.props);

            u32_t family_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
            for (u32_t i = 0; i < family_count; i++)
            {
                const VkQueueFlags flags = families[i].queueFlags;
                if ((flags & VK_QUEUE_GRAPHICS_BIT) && !c.families.graphics_family) { c.families.graphics_family = i; }
                if ((flags & VK_QUEUE_COMPUTE_BIT) && !(flags & VK_QUEUE_GRAPHICS_BIT) && !c.families.compute_family)
                {
                    c.families.compute_family = i;
                }
                if ((flags & VK_QUEUE_TRANSFER_BIT) && !(flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) &&
                    !c.families.transfer_family)
                {
                    c.families.transfer_family = i;
                }
                if (surface != VK_NULL_HANDLE)
                {
                    VkBool32 present = VK_FALSE;
                    vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &present);
                    if (present && (!c.families.present_family || i == c.families.graphics_family))
                    {
                        c.families.present_family = i;
                    }
                }
            }
            if (!c.families.graphics_family)
            {
                c.missing.push_back("a graphics queue");
                return c;
            }
            if (surface == VK_NULL_HANDLE) { c.families.present_family = c.families.graphics_family; }
            if (!c.families.present_family) { c.missing.push_back("presentation to the window"); }
            if (!c.families.transfer_family) { c.families.transfer_family = c.families.graphics_family; }

            if (c.props.apiVersion < VK_API_VERSION_1_3) { c.missing.push_back("Vulkan 1.3"); }

            u32_t ext_count = 0;
            vkEnumerateDeviceExtensionProperties(physical, nullptr, &ext_count, nullptr);
            std::vector<VkExtensionProperties> exts(ext_count);
            vkEnumerateDeviceExtensionProperties(physical, nullptr, &ext_count, exts.data());

            if (surface != VK_NULL_HANDLE && !has_extension(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
            {
                c.missing.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            }

            features_t have;
            const bool rt_exts = has_extension(exts, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
                                 has_extension(exts, VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
                                 has_extension(exts, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
            const bool pf_ext = has_extension(exts, VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME);
            const bool pe_ext = has_extension(exts, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
            have.chain(rt_exts, rt_exts && pf_ext, pe_ext);
            vkGetPhysicalDeviceFeatures2(physical, &have.f10);

            missing_features(have, c.missing);

            c.rt = !config.force_compat && rt_exts && have.as.accelerationStructure && have.rq.rayQuery &&
                   have.f10.features.shaderInt16 && have.f11.storageBuffer16BitAccess;
            c.position_fetch = c.rt && pf_ext && have.pf.rayTracingPositionFetch;
            c.pipeline_properties = config.pipeline_properties && pe_ext && have.pe.pipelineExecutableInfo;
            return c;
        }

        // discrete first, then hardware ray tracing (unless compat is forced)
        i32 score(const candidate_t& c)
        {
            if (!c.missing.empty()) { return -1; }
            i32 s = 0;
            if (c.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { s += 4; }
            else if (c.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) { s += 2; }
            if (c.rt) { s += 1; }
            return s;
        }

        bool create_instance(const device_config_t& config)
        {
            std::vector<const char*> exts = config.instance_exts;
            const bool validation = config.validation && has_layer(VALIDATION_LAYER);
            if (config.validation && !validation)
            {
                TAU_LOG_WARN("VULKAN", "Validation requested, but {} is not installed", VALIDATION_LAYER);
            }

            VkDebugUtilsMessengerCreateInfoEXT messenger_info = {
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                                             VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                             VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                         VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            messenger_info.pfnUserCallback = debug_callback;

            const VkValidationFeatureEnableEXT enables[] = {
                VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
            VkValidationFeaturesEXT validation_features = {VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
            validation_features.enabledValidationFeatureCount = 1;
            validation_features.pEnabledValidationFeatures = enables;
            validation_features.pNext = &messenger_info;

            if (validation)
            {
                exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                exts.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
            }

            VkApplicationInfo app_info = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
            app_info.pApplicationName = config.app_name.c_str();
            app_info.apiVersion = VK_API_VERSION_1_3;

            VkInstanceCreateInfo instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
            instance_info.pApplicationInfo = &app_info;
            instance_info.enabledExtensionCount = static_cast<u32_t>(exts.size());
            instance_info.ppEnabledExtensionNames = exts.data();
            instance_info.enabledLayerCount = validation ? 1 : 0;
            instance_info.ppEnabledLayerNames = validation ? &VALIDATION_LAYER : nullptr;
            instance_info.pNext = validation ? &validation_features : nullptr;

            if (vkCreateInstance(&instance_info, nullptr, &ctx.instance) != VK_SUCCESS)
            {
                TAU_LOG_FATAL("VULKAN", "Could not create a Vulkan 1.3 instance");
                return false;
            }
            volkLoadInstance(ctx.instance);

            if (validation)
            {
                vkCreateDebugUtilsMessengerEXT(ctx.instance, &messenger_info, nullptr, &ctx.debug_messenger);
            }
            ctx.caps.validation = validation;
            return true;
        }
    } // namespace

    bool create(const device_config_t& config, SDL_Window* window)
    {
        if (!create_instance(config)) { return false; }

        ctx.caps.headless = config.headless || window == nullptr;
        if (!ctx.caps.headless && !SDL_Vulkan_CreateSurface(window, ctx.instance, nullptr, &ctx.surface))
        {
            TAU_LOG_FATAL("VULKAN", "Failed to create SDL surface: {}", SDL_GetError());
            return false;
        }

        u32_t count = 0;
        vkEnumeratePhysicalDevices(ctx.instance, &count, nullptr);
        std::vector<VkPhysicalDevice> physicals(count);
        vkEnumeratePhysicalDevices(ctx.instance, &count, physicals.data());

        std::vector<candidate_t> candidates;
        for (VkPhysicalDevice physical : physicals) { candidates.push_back(inspect(physical, ctx.surface, config)); }
        const auto best =
            std::max_element(candidates.begin(), candidates.end(),
                             [](const candidate_t& a, const candidate_t& b) { return score(a) < score(b); });
        if (best == candidates.end() || score(*best) < 0)
        {
            for (const candidate_t& c : candidates)
            {
                std::string missing;
                for (const std::string& m : c.missing) { missing += (missing.empty() ? "" : ", ") + m; }
                TAU_LOG_ERROR("VULKAN", "{} is missing: {}", c.props.deviceName, missing);
            }
            TAU_LOG_FATAL("VULKAN", "No GPU has what tau needs");
            return false;
        }
        const candidate_t& chosen = *best;

        ctx.physical_device = chosen.physical;
        ctx.properties = chosen.props;
        ctx.queue_fam_indices = chosen.families;
        ctx.caps.ray_query = chosen.rt;
        ctx.caps.int16 = chosen.rt;
        ctx.caps.position_fetch = chosen.position_fetch;
        ctx.caps.pipeline_properties = chosen.pipeline_properties;
        ctx.caps.has_compute_family = chosen.families.compute_family.has_value();
        ctx.caps.compute_family = chosen.families.compute_family.value_or(0);

        std::vector<u32_t> unique_families = {chosen.families.graphics_family.value()};
        for (const u32_t family : {chosen.families.present_family.value(), chosen.families.transfer_family.value()})
        {
            if (std::find(unique_families.begin(), unique_families.end(), family) == unique_families.end())
            {
                unique_families.push_back(family);
            }
        }
        const f32 priority = 1.0f;
        std::vector<VkDeviceQueueCreateInfo> queue_infos;
        for (const u32_t family : unique_families)
        {
            VkDeviceQueueCreateInfo info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            info.queueFamilyIndex = family;
            info.queueCount = 1;
            info.pQueuePriorities = &priority;
            queue_infos.push_back(info);
        }

        std::vector<const char*> exts;
        if (!ctx.caps.headless) { exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME); }
        if (chosen.rt)
        {
            exts.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
            exts.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
            exts.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        }
        if (chosen.position_fetch) { exts.push_back(VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME); }
        if (chosen.pipeline_properties) { exts.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME); }

        features_t features = wanted(chosen.rt, chosen.position_fetch, chosen.pipeline_properties);

        VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_info.pNext = &features.f10;
        device_info.queueCreateInfoCount = static_cast<u32_t>(queue_infos.size());
        device_info.pQueueCreateInfos = queue_infos.data();
        device_info.enabledExtensionCount = static_cast<u32_t>(exts.size());
        device_info.ppEnabledExtensionNames = exts.data();

        if (vkCreateDevice(ctx.physical_device, &device_info, nullptr, &ctx.device) != VK_SUCCESS)
        {
            TAU_LOG_FATAL("VULKAN", "Could not create the device on {}", ctx.properties.deviceName);
            return false;
        }
        volkLoadDevice(ctx.device);

        vkGetDeviceQueue(ctx.device, chosen.families.graphics_family.value(), 0, &ctx.graphics_queue);
        vkGetDeviceQueue(ctx.device, chosen.families.present_family.value(), 0, &ctx.present_queue);
        vkGetDeviceQueue(ctx.device, chosen.families.transfer_family.value(), 0, &ctx.transfer_queue);

        VmaAllocatorCreateInfo allocator_info = {};
        allocator_info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        allocator_info.vulkanApiVersion = VK_API_VERSION_1_3;
        allocator_info.physicalDevice = ctx.physical_device;
        allocator_info.device = ctx.device;
        allocator_info.instance = ctx.instance;
        VmaVulkanFunctions functions = {};
        vmaImportVulkanFunctionsFromVolk(&allocator_info, &functions);
        allocator_info.pVulkanFunctions = &functions;
        if (vmaCreateAllocator(&allocator_info, &ctx.allocator) != VK_SUCCESS)
        {
            TAU_LOG_FATAL("VULKAN", "Could not create the memory allocator");
            return false;
        }

        TAU_LOG_INFO("VULKAN", "GPU: {} ({}{}{}{}{}{})", ctx.properties.deviceName,
                     chosen.rt ? "ray query" : "compat, no ray tracing",
                     chosen.position_fetch ? ", position fetch" : "", ctx.caps.int16 ? ", int16" : "",
                     ctx.caps.headless ? ", headless" : "", ctx.caps.validation ? ", validation + sync" : "",
                     ctx.caps.has_compute_family ? ", compute family found" : "");
        return true;
    }

    void destroy()
    {
        if (ctx.allocator != VK_NULL_HANDLE) { vmaDestroyAllocator(ctx.allocator); }
        if (ctx.device != VK_NULL_HANDLE) { vkDestroyDevice(ctx.device, nullptr); }
        if (ctx.surface != VK_NULL_HANDLE) { vkDestroySurfaceKHR(ctx.instance, ctx.surface, nullptr); }
        if (ctx.debug_messenger != VK_NULL_HANDLE)
        {
            vkDestroyDebugUtilsMessengerEXT(ctx.instance, ctx.debug_messenger, nullptr);
        }
        if (ctx.instance != VK_NULL_HANDLE) { vkDestroyInstance(ctx.instance, nullptr); }
        ctx.allocator = VK_NULL_HANDLE;
        ctx.device = VK_NULL_HANDLE;
        ctx.surface = VK_NULL_HANDLE;
        ctx.debug_messenger = VK_NULL_HANDLE;
        ctx.instance = VK_NULL_HANDLE;
    }

    u32_t validation_errors() { return error_count.load(std::memory_order_relaxed); }
} // namespace tau::renderer::device
