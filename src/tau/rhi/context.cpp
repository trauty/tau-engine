#include "context.h"

#include "tau/rhi/bindless.h"
#include "tau/rhi/deletion.h"
#include "tau/rhi/frame_upload.h"
#include "tau/rhi/gpu_timers.h"
#include "tau/rhi/material_heap.h"
#include "tau/rhi/pipelines.h"
#include "tau/rhi/readback.h"
#include "tau/rhi/swapchain.h"
#include "tau/rhi/transfer.h"
#include "tau/vfs.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace tau::renderer
{
    context_t ctx;

    namespace
    {
        bool frame_slot_ready = false;

        VkFormat find_depth_format()
        {
            VkFormat formats[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT};
            for (VkFormat format : formats)
            {
                VkFormatProperties props;
                vkGetPhysicalDeviceFormatProperties(ctx.physical_device, format, &props);
                if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) { return format; }
            }
            return VK_FORMAT_UNDEFINED;
        }

        void init_slot(frame_slot_t& slot)
        {
            VkCommandPoolCreateInfo cmd_pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            cmd_pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            cmd_pool_info.queueFamilyIndex = static_cast<u32_t>(ctx.queue_fam_indices.graphics_family.value());
            VK_CHECK(vkCreateCommandPool(ctx.device, &cmd_pool_info, nullptr, &slot.command_pool));

            VkCommandBufferAllocateInfo cmd_buf_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cmd_buf_info.commandPool = slot.command_pool;
            cmd_buf_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cmd_buf_info.commandBufferCount = 1;
            VK_CHECK(vkAllocateCommandBuffers(ctx.device, &cmd_buf_info, &slot.cmd));

            VkSemaphoreCreateInfo sem_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VK_CHECK(vkCreateSemaphore(ctx.device, &sem_info, nullptr, &slot.acquire_semaphore));

            slot.allocator.init(2048 * 1024);
        }

        void shutdown_slot(frame_slot_t& slot)
        {
            if (slot.cmd != VK_NULL_HANDLE) { vkFreeCommandBuffers(ctx.device, slot.command_pool, 1, &slot.cmd); }
            if (slot.command_pool != VK_NULL_HANDLE) { vkDestroyCommandPool(ctx.device, slot.command_pool, nullptr); }
            if (slot.acquire_semaphore != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(ctx.device, slot.acquire_semaphore, nullptr);
            }
            slot = {};
        }
    } // namespace

    namespace
    {
        constexpr const char* PIPELINE_CACHE_FILE = "user://pipeline_cache.bin";

        // only save once pipeline creation has gone quiet: a scene load creates dozens in a row
        constexpr std::chrono::seconds PIPELINE_CACHE_SETTLE{2};

        // nothing is evicted and every shader edit adds pipelines that will never be built again, so past this size
        // the file is mostly stale and is dropped and rebuilt. the engine's own shaders come to about 250 KB
        constexpr size_t PIPELINE_CACHE_MAX_BYTES = 32u * 1024u * 1024u;

        struct pipeline_cache_stats_t
        {
            std::mutex mutex;
            u32_t created_total = 0;
            f64 milliseconds_total = 0.0;
            u32_t created_since_save = 0;
            std::chrono::steady_clock::time_point last_created;
        };

        pipeline_cache_stats_t g_pipeline_stats;

        // a cache from another GPU or driver is useless, not an error, check the header so no driver sees foreign data
        bool pipeline_cache_matches_device(const std::vector<char>& data)
        {
            VkPipelineCacheHeaderVersionOne header = {};
            if (data.size() < sizeof(header)) { return false; }
            std::memcpy(&header, data.data(), sizeof(header));

            VkPhysicalDeviceProperties props = {};
            vkGetPhysicalDeviceProperties(ctx.physical_device, &props);

            return header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && header.vendorID == props.vendorID &&
                   header.deviceID == props.deviceID &&
                   std::memcmp(header.pipelineCacheUUID, props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
        }

        // the physical file behind user://, or empty when there is no per user folder to use
        std::filesystem::path pipeline_cache_path()
        {
            const std::string resolved = vfs::resolve(PIPELINE_CACHE_FILE);
            if (resolved.empty() || resolved == PIPELINE_CACHE_FILE) { return {}; }
            return std::filesystem::path(resolved);
        }

        void create_pipeline_cache()
        {
            std::vector<char> data;
            const std::filesystem::path path = pipeline_cache_path();
            if (!path.empty())
            {
                std::ifstream file(path, std::ios::binary);
                data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            }

            if (!data.empty() && !pipeline_cache_matches_device(data))
            {
                TAU_LOG_INFO("VULKAN", "Pipeline cache on disk is from another GPU or driver, starting empty");
                data.clear();
            }
            if (data.size() > PIPELINE_CACHE_MAX_BYTES)
            {
                TAU_LOG_INFO("VULKAN", "Pipeline cache grew to {} MB, starting empty", data.size() / (1024 * 1024));
                data.clear();
            }

            VkPipelineCacheCreateInfo info = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                .initialDataSize = data.size(),
                .pInitialData = data.empty() ? nullptr : data.data(),
            };

            if (vkCreatePipelineCache(ctx.device, &info, nullptr, &ctx.pipeline_cache) != VK_SUCCESS)
            {
                // the data was the problem, not the cache: try once more without it
                info.initialDataSize = 0;
                info.pInitialData = nullptr;
                data.clear();
                if (vkCreatePipelineCache(ctx.device, &info, nullptr, &ctx.pipeline_cache) != VK_SUCCESS)
                {
                    ctx.pipeline_cache = VK_NULL_HANDLE; // pipelines still work, just uncached
                    TAU_LOG_WARN("VULKAN", "Could not create a pipeline cache");
                    return;
                }
            }

            if (!data.empty()) { TAU_LOG_INFO("VULKAN", "Pipeline cache: loaded {} KB", data.size() / 1024); }
        }

        void save_pipeline_cache()
        {
            if (ctx.pipeline_cache == VK_NULL_HANDLE) { return; }

            const std::filesystem::path path = pipeline_cache_path();
            if (path.empty()) { return; }

            size_t size = 0;
            if (vkGetPipelineCacheData(ctx.device, ctx.pipeline_cache, &size, nullptr) != VK_SUCCESS || size == 0)
            {
                return;
            }
            std::vector<char> data(size);
            if (vkGetPipelineCacheData(ctx.device, ctx.pipeline_cache, &size, data.data()) != VK_SUCCESS) { return; }

            // written beside and then moved over the old one, so a crash mid write leaves the previous cache
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            const std::filesystem::path temp = path.string() + ".tmp";
            {
                std::ofstream file(temp, std::ios::binary | std::ios::trunc);
                file.write(data.data(), static_cast<std::streamsize>(size));
                if (!file) { return; }
            }
            std::filesystem::rename(temp, path, ec);
            if (ec)
            {
                TAU_LOG_WARN("VULKAN", "Could not save the pipeline cache: {}", ec.message());
                return;
            }

            std::scoped_lock lock(g_pipeline_stats.mutex);
            TAU_LOG_INFO("VULKAN", "Saved pipeline cache ({} KB); {} pipelines created this session in {:.0f} ms",
                         size / 1024, g_pipeline_stats.created_total, g_pipeline_stats.milliseconds_total);
        }
    } // namespace

    void save_pipeline_cache_when_settled()
    {
        {
            std::scoped_lock lock(g_pipeline_stats.mutex);
            if (g_pipeline_stats.created_since_save == 0) { return; }
            if (std::chrono::steady_clock::now() - g_pipeline_stats.last_created < PIPELINE_CACHE_SETTLE) { return; }
            g_pipeline_stats.created_since_save = 0;
        }
        save_pipeline_cache();
    }

    void note_pipeline_created(f64 milliseconds)
    {
        std::scoped_lock lock(g_pipeline_stats.mutex);
        g_pipeline_stats.created_total++;
        g_pipeline_stats.milliseconds_total += milliseconds;
        g_pipeline_stats.created_since_save++;
        g_pipeline_stats.last_created = std::chrono::steady_clock::now();
    }

    bool init_context(const context_config_t& config, SDL_Window* window)
    {
        const device::device_config_t device_config = {
            .app_name = config.app_name,
            .validation = config.validation,
            .force_compat = config.force_compat,
            .headless = config.headless,
            .pipeline_properties = config.pipeline_properties,
            .instance_exts = config.required_instance_exts,
        };
        if (!device::create(device_config, window)) { return false; }

        create_pipeline_cache();

        {
            VkPhysicalDeviceSubgroupProperties sg_props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
            VkPhysicalDeviceProperties2 sg_props2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sg_props};
            vkGetPhysicalDeviceProperties2(ctx.physical_device, &sg_props2);

            auto has_op = [&](VkSubgroupFeatureFlagBits bit) { return (sg_props.supportedOperations & bit) != 0; };
            TAU_LOG_INFO("VULKAN",
                         "Subgroup: size={} compute_stage={} basic={} vote={} ballot={} arithmetic={} shuffle={}",
                         sg_props.subgroupSize, (sg_props.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0,
                         has_op(VK_SUBGROUP_FEATURE_BASIC_BIT), has_op(VK_SUBGROUP_FEATURE_VOTE_BIT),
                         has_op(VK_SUBGROUP_FEATURE_BALLOT_BIT), has_op(VK_SUBGROUP_FEATURE_ARITHMETIC_BIT),
                         has_op(VK_SUBGROUP_FEATURE_SHUFFLE_BIT));
        }

        bindless::init({config.bindless_limits.max_samplers, config.bindless_limits.max_sampled_textures,
                        config.bindless_limits.max_storage_images});
        material_heap::init(config.bindless_limits.material_heap_size);
        transfer::init();

        ctx.depth_format = find_depth_format();
        swapchain::init(config.vsync);

        for (frame_slot_t& slot : ctx.frames) { init_slot(slot); }

        VkSemaphoreTypeCreateInfo timeline_type_info = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue = 0,
        };

        VkSemaphoreCreateInfo timeline_sem_info = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &timeline_type_info,
        };

        VK_CHECK(vkCreateSemaphore(ctx.device, &timeline_sem_info, nullptr, &ctx.timeline_semaphore));
        ctx.timeline_value = 0;

        TAU_LOG_INFO("VULKAN", "Context initialized for GPU: {}", ctx.properties.deviceName);

        gpu_timers::init();

        pipelines::build_all();
        return true;
    }

    void shutdown_context()
    {
        readback::shutdown();
        gpu_timers::shutdown();
        frame_upload::shutdown();
        pipelines::shutdown();
        save_pipeline_cache();
        if (ctx.pipeline_cache != VK_NULL_HANDLE)
        {
            vkDestroyPipelineCache(ctx.device, ctx.pipeline_cache, nullptr);
            ctx.pipeline_cache = VK_NULL_HANDLE;
        }

        deletion::flush();

        if (ctx.timeline_semaphore != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(ctx.device, ctx.timeline_semaphore, nullptr);
        }

        for (frame_slot_t& slot : ctx.frames) { shutdown_slot(slot); }

        // releases land in the tables below
        deletion::flush();
        transfer::shutdown();
        material_heap::shutdown();
        bindless::shutdown();
        swapchain::shutdown();

        device::destroy();
    }

    void wait_for_frame()
    {
        if (frame_slot_ready) { return; }

        frame_slot_t& slot = ctx.frames[ctx.cur_frame];
        if (slot.target_timeline_value > 0)
        {
            VkSemaphoreWaitInfo wait_info = {
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                .semaphoreCount = 1,
                .pSemaphores = &ctx.timeline_semaphore,
                .pValues = &slot.target_timeline_value,
            };

            vkWaitSemaphores(ctx.device, &wait_info, UINT64_MAX);
        }
        readback::collect(ctx.cur_frame);
        gpu_timers::collect(ctx.cur_frame);
        frame_upload::reset(ctx.cur_frame);

        frame_slot_ready = true;
    }

    void advance_frame()
    {
        ctx.cur_frame = (ctx.cur_frame + 1) % MAX_FRAMES_IN_FLIGHT;
        frame_slot_ready = false;
    }

    void transition_image(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                          VkImageAspectFlags aspect_mask)
    {
        VkImageMemoryBarrier2 barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
            .oldLayout = old_layout,
            .newLayout = new_layout,
            .image = image,
            .subresourceRange = {.aspectMask = aspect_mask,
                                 .baseMipLevel = 0,
                                 .levelCount = VK_REMAINING_MIP_LEVELS,
                                 .baseArrayLayer = 0,
                                 .layerCount = VK_REMAINING_ARRAY_LAYERS},
        };

        if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
        {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.srcAccessMask = 0;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        }
        else if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
                 new_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
        {
            barrier.srcStageMask =
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            barrier.srcAccessMask = 0;
            barrier.dstStageMask =
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            barrier.dstAccessMask =
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        }
        else if (old_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
                 new_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
        {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
            barrier.dstAccessMask = 0;
        }
        else if (old_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
                 new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        }
        else if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
        {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.srcAccessMask = 0;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
            barrier.dstAccessMask = 0;
        }
        else if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                 new_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
        {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        }
        else if (old_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR &&
                 new_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
        {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
            barrier.srcAccessMask = 0;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        }
        else if (old_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
                 new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        }
        else if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                 new_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
        {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
            barrier.dstStageMask =
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            barrier.dstAccessMask =
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        }
        else if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.srcAccessMask = 0;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        }
        else if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                 new_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
        {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        }
        else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
                 new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.srcAccessMask = 0;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        }
        else
        {
            TAU_LOG_WARN("VULKAN", "transition_image: using fallback barrier for image transition. From: {}, To: {}",
                         (u32_t)old_layout, (u32_t)new_layout);
        }

        VkDependencyInfo dep_info = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &barrier,
        };

        vkCmdPipelineBarrier2(cmd, &dep_info);
    }

    VkDeviceAddress get_buffer_address(VkBuffer buffer)
    {
        VkBufferDeviceAddressInfo info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .buffer = buffer,
        };
        return vkGetBufferDeviceAddress(ctx.device, &info);
    }
} // namespace tau::renderer
