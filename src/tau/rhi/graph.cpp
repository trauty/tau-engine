#include "graph.h"

#include "tau/hash.h"
#include "tau/log.h"
#include "tau/profiling.h"
#include "tau/rhi/device.h"
#include "tau/rhi/gpu_timers.h"
#include "tau/rhi/renderer.h"
#include "tau/rhi/renderer_constants.h"
#include "tau/rhi/renderer_resources.h"
#include "tau/rhi/renderer_types.h"

#include <algorithm>
#include <chrono>
#include <string_view>

#ifdef TAU_ENABLE_PROFILING
    #include <tracy/TracyVulkan.hpp>
#endif

namespace tau::renderer::graph
{
    namespace
    {
        enum kind_e : u32_t
        {
            TRANSIENT,
            PERSISTENT,
            IMPORTED,
        };

        constexpr u32_t UNUSED_FRAMES_BEFORE_FREE = 8;

        struct sync_t
        {
            VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkPipelineStageFlags2 write_stages = 0;
            VkAccessFlags2 write_access = 0;
            VkPipelineStageFlags2 read_stages = 0;
            // reads already ordered after the last write
            VkPipelineStageFlags2 synced_reads = 0;
        };

        struct cached_image_t
        {
            VkImageCreateInfo info = {};
            VkDeviceSize offset = 0;
            u32_t heap_generation = 0;
            VkImage image = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            u32_t sampled_id = NONE;
            u32_t storage_id = NONE;
            u32_t unused_frames = 0;
            bool taken = false;
        };

        struct cached_buffer_t
        {
            VkDeviceSize size = 0;
            VkBufferUsageFlags usage = 0;
            VkDeviceSize offset = 0;
            u32_t heap_generation = 0;
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceAddress address = 0;
            u32_t unused_frames = 0;
            bool taken = false;
        };

        struct heap_t
        {
            VmaAllocation allocation = VK_NULL_HANDLE;
            VkDeviceSize size = 0;
            u32_t type_bits = 0;
            u32_t generation = 0;
        };

        struct slot_t
        {
            heap_t image_heap;
            heap_t buffer_heap;
            std::vector<cached_image_t> images;
            std::vector<cached_buffer_t> buffers;
        };

        struct persistent_image_t
        {
            u32_t hash = 0;
            VkImageCreateInfo info = {};
            VkImage image = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            u32_t sampled_id = NONE;
            u32_t storage_id = NONE;
            sync_t sync;
        };

        struct persistent_buffer_t
        {
            u32_t hash = 0;
            VkDeviceSize size = 0;
            VkBufferUsageFlags usage = 0;
            VkBuffer buffer = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            VkDeviceAddress address = 0;
            sync_t sync;
        };

        struct access_info_t
        {
            VkPipelineStageFlags2 stages;
            VkAccessFlags2 access;
            VkImageLayout layout;
            VkImageUsageFlags image_usage;
            VkBufferUsageFlags buffer_usage;
            bool write;
            bool read;
            bool attachment;
        };

        u32_t hash_name(const char* name) { return hash_string(name ? std::string_view(name) : std::string_view{}); }

        bool is_depth(VkFormat f)
        {
            return f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D24_UNORM_S8_UINT ||
                   f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT;
        }

        bool has_stencil(VkFormat f)
        {
            return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                   f == VK_FORMAT_D16_UNORM_S8_UINT;
        }

        VkImageAspectFlags aspect_of(VkFormat f)
        {
            if (!is_depth(f)) { return VK_IMAGE_ASPECT_COLOR_BIT; }
            return has_stencil(f) ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                  : VK_IMAGE_ASPECT_DEPTH_BIT;
        }

        // shader stages default to the pass kind
        access_info_t info_of(access_e a, bool raster, VkPipelineStageFlags2 stages)
        {
            const VkPipelineStageFlags2 shader =
                raster ? (VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT)
                       : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            const VkPipelineStageFlags2 tests =
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

            access_info_t i = {};
            switch (a)
            {
            case access_e::COLOR_WRITE:
                i = {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                     0,
                     true,
                     false,
                     true};
                break;
            case access_e::COLOR_READ_WRITE:
                i = {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                     0,
                     true,
                     true,
                     true};
                break;
            case access_e::DEPTH_WRITE:
                i = {tests,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                     0,
                     true,
                     false,
                     true};
                break;
            case access_e::DEPTH_READ:
                i = {tests,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                     0,
                     false,
                     true,
                     true};
                break;
            case access_e::SAMPLED:
                i = {shader,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_USAGE_SAMPLED_BIT,
                     0,
                     false,
                     true,
                     false};
                break;
            case access_e::STORAGE_READ:
                i = {shader,
                     VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_USAGE_STORAGE_BIT,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     false,
                     true,
                     false};
                break;
            case access_e::STORAGE_WRITE:
                i = {shader,
                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_USAGE_STORAGE_BIT,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     true,
                     false,
                     false};
                break;
            case access_e::STORAGE_READ_WRITE:
                i = {shader,
                     VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_USAGE_STORAGE_BIT,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     true,
                     true,
                     false};
                break;
            case access_e::TRANSFER_SRC:
                i = {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     false,
                     true,
                     false};
                break;
            case access_e::TRANSFER_DST:
                i = {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     true,
                     false,
                     false};
                break;
            case access_e::UNIFORM:
                i = {shader,
                     VK_ACCESS_2_UNIFORM_READ_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED,
                     0,
                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     false,
                     true,
                     false};
                break;
            case access_e::INDIRECT:
                i = {VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                     VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED,
                     0,
                     VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                     false,
                     true,
                     false};
                break;
            case access_e::INDEX:
                i = {VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
                     VK_ACCESS_2_INDEX_READ_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED,
                     0,
                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                     false,
                     true,
                     false};
                break;
            }
            if (stages != 0) { i.stages = stages; }
            return i;
        }

        // the graph cache owns everything that outlives a frame
        f64 ms_since(std::chrono::steady_clock::time_point t)
        { return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t).count(); }
    } // namespace

    struct graph_cache_t
    {
        slot_t slots[MAX_FRAMES_IN_FLIGHT];
        std::vector<persistent_image_t> persistent_images;
        std::vector<persistent_buffer_t> persistent_buffers;
        stats_t last_stats;
        stats_t stats;
        void* tracy_ctx = nullptr;
    };

    namespace
    {
        u32_t write_sampled(VkImageView view)
        {
            const u32_t id = res_system.texture_heap.acquire();
            const VkDescriptorImageInfo info = {.imageView = view,
                                                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            const VkWriteDescriptorSet write = {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = res_system.global_set,
                .dstBinding = TEXTURES_BINDING_POINT,
                .dstArrayElement = id,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                .pImageInfo = &info,
            };
            vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
            return id;
        }

        u32_t write_storage(VkImageView view)
        {
            const u32_t id = res_system.storage_image_heap.acquire();
            const VkDescriptorImageInfo info = {.imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
            const VkWriteDescriptorSet write = {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = res_system.global_set,
                .dstBinding = STORAGE_IMAGES_BINDING_POINT,
                .dstArrayElement = id,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &info,
            };
            vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
            return id;
        }

        // transfer only images get none
        VkImageView create_view(VkImage image, const VkImageCreateInfo& info, const char* name)
        {
            device::name(image, VK_OBJECT_TYPE_IMAGE, name);
            constexpr VkImageUsageFlags VIEW_USAGE = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            if ((info.usage & VIEW_USAGE) == 0) { return VK_NULL_HANDLE; }

            const VkImageViewCreateInfo view_info = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = image,
                .viewType = info.imageType == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                            : info.arrayLayers > 1             ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                               : VK_IMAGE_VIEW_TYPE_2D,
                .format = info.format,
                .subresourceRange = {aspect_of(info.format), 0, info.mipLevels, 0, info.arrayLayers},
            };
            VkImageView view = VK_NULL_HANDLE;
            VK_CHECK(vkCreateImageView(ctx.device, &view_info, nullptr, &view));
            device::name(view, VK_OBJECT_TYPE_IMAGE_VIEW, name);
            return view;
        }

        bool same_info(const VkImageCreateInfo& a, const VkImageCreateInfo& b)
        {
            return a.imageType == b.imageType && a.format == b.format && a.extent.width == b.extent.width &&
                   a.extent.height == b.extent.height && a.extent.depth == b.extent.depth &&
                   a.mipLevels == b.mipLevels && a.arrayLayers == b.arrayLayers && a.usage == b.usage;
        }

        void destroy_cached(cached_image_t& c)
        {
            if (c.sampled_id != NONE) { res_system.texture_heap.release(c.sampled_id); }
            if (c.storage_id != NONE) { res_system.storage_image_heap.release(c.storage_id); }
            if (c.view != VK_NULL_HANDLE) { vkDestroyImageView(ctx.device, c.view, nullptr); }
            vkDestroyImage(ctx.device, c.image, nullptr);
        }

        void destroy_cached(cached_buffer_t& c) { vkDestroyBuffer(ctx.device, c.buffer, nullptr); }

        void free_heap(heap_t& h)
        {
            if (h.allocation != VK_NULL_HANDLE) { vmaFreeMemory(ctx.allocator, h.allocation); }
            h.allocation = VK_NULL_HANDLE;
            h.size = 0;
            h.generation++;
        }

        bool ensure_heap(heap_t& h, VkDeviceSize size, VkDeviceSize alignment, u32_t type_bits)
        {
            if (size == 0) { return true; }
            if (h.allocation != VK_NULL_HANDLE && h.size >= size && (h.type_bits & type_bits) == type_bits)
            {
                return true;
            }

            free_heap(h);
            const VkMemoryRequirements req = {.size = size, .alignment = alignment, .memoryTypeBits = type_bits};
            const VmaAllocationCreateInfo alloc = {.usage = VMA_MEMORY_USAGE_GPU_ONLY};
            if (vmaAllocateMemory(ctx.allocator, &req, &alloc, &h.allocation, nullptr) != VK_SUCCESS)
            {
                TAU_LOG_ERROR("GRAPH", "Could not allocate a {} byte transient heap", size);
                h.allocation = VK_NULL_HANDLE;
                return false;
            }
            h.size = size;
            h.type_bits = type_bits;
            return true;
        }

        // first fit by size: lowest offset not overlapping a placed resource alive at the same time
        struct place_t
        {
            u32_t index;
            VkDeviceSize size;
            VkDeviceSize alignment;
            u32_t first, last;
            VkDeviceSize offset;
        };

        VkDeviceSize place_all(std::vector<place_t>& items)
        {
            std::sort(items.begin(), items.end(), [](const place_t& a, const place_t& b) { return a.size > b.size; });
            VkDeviceSize peak = 0;
            for (size_t i = 0; i < items.size(); i++)
            {
                place_t& it = items[i];
                VkDeviceSize offset = 0;
                bool moved = true;
                while (moved)
                {
                    moved = false;
                    offset = (offset + it.alignment - 1) / it.alignment * it.alignment;
                    for (size_t j = 0; j < i; j++)
                    {
                        const place_t& p = items[j];
                        const bool live_together = !(p.last < it.first || it.last < p.first);
                        const bool overlap = offset < p.offset + p.size && p.offset < offset + it.size;
                        if (live_together && overlap)
                        {
                            offset = p.offset + p.size;
                            moved = true;
                        }
                    }
                }
                it.offset = offset;
                peak = std::max(peak, offset + it.size);
            }
            return peak;
        }

        // the occupant that last used this memory before `it` in the frame
        template <typename R>
        void link_aliases(std::vector<R>& resources, const std::vector<place_t>& items)
        {
            for (const place_t& it : items)
            {
                u32_t best = NONE;
                u32_t best_last = 0;
                for (const place_t& p : items)
                {
                    if (p.index == it.index || p.last >= it.first) { continue; }
                    const bool overlap = it.offset < p.offset + p.size && p.offset < it.offset + it.size;
                    if (overlap && (best == NONE || p.last > best_last))
                    {
                        best = p.index;
                        best_last = p.last;
                    }
                }
                resources[it.index].alias_prev = best;
                resources[it.index].offset = it.offset;
            }
        }

        VkImageCreateInfo create_info(const graph_t::image_t& img)
        {
            return {
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType = img.desc.depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                .format = img.desc.format,
                .extent = {img.desc.width, img.desc.height, img.desc.depth},
                .mipLevels = img.desc.mips,
                .arrayLayers = img.desc.layers,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = img.usage,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            };
        }

        VkBufferCreateInfo buffer_info(VkDeviceSize size, VkBufferUsageFlags usage)
        { return {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage}; }

        VkDeviceAddress address_of(VkBuffer b)
        {
            const VkBufferDeviceAddressInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b};
            return vkGetBufferDeviceAddress(ctx.device, &info);
        }

        bool fail(graph_t& g, const char* what, const char* pass, const char* resource)
        {
            TAU_LOG_ERROR("GRAPH", "{}: pass {}, resource {}", what, pass ? pass : "?", resource ? resource : "?");
            g.failed = true;
            return false;
        }

        bool allocate_transients(graph_t& g)
        {
            slot_t& slot = g.cache->slots[g.frame_slot];
            for (cached_image_t& c : slot.images) { c.taken = false; }
            for (cached_buffer_t& c : slot.buffers) { c.taken = false; }

            std::vector<place_t> image_items;
            u32_t image_bits = ~0u;
            VkDeviceSize image_align = 1;
            for (u32_t i = 0; i < g.images.size(); i++)
            {
                const graph_t::image_t& img = g.images[i];
                if (img.kind != TRANSIENT || img.first == NONE) { continue; }

                const VkImageCreateInfo info = create_info(img);
                const VkDeviceImageMemoryRequirements query = {
                    .sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, .pCreateInfo = &info};
                VkMemoryRequirements2 req = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
                vkGetDeviceImageMemoryRequirements(ctx.device, &query, &req);

                image_bits &= req.memoryRequirements.memoryTypeBits;
                image_align = std::max(image_align, req.memoryRequirements.alignment);
                image_items.push_back(
                    {i, req.memoryRequirements.size, req.memoryRequirements.alignment, img.first, img.last, 0});
                g.cache->stats.transient_bytes += req.memoryRequirements.size;
                g.cache->stats.transient_images++;
            }

            std::vector<place_t> buffer_items;
            u32_t buffer_bits = ~0u;
            VkDeviceSize buffer_align = 1;
            for (u32_t i = 0; i < g.buffers.size(); i++)
            {
                const graph_t::buffer_t& buf = g.buffers[i];
                if (buf.kind != TRANSIENT || buf.first == NONE) { continue; }

                const VkBufferCreateInfo info = buffer_info(buf.desc.size, buf.usage);
                const VkDeviceBufferMemoryRequirements query = {
                    .sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS, .pCreateInfo = &info};
                VkMemoryRequirements2 req = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
                vkGetDeviceBufferMemoryRequirements(ctx.device, &query, &req);

                buffer_bits &= req.memoryRequirements.memoryTypeBits;
                buffer_align = std::max(buffer_align, req.memoryRequirements.alignment);
                buffer_items.push_back(
                    {i, req.memoryRequirements.size, req.memoryRequirements.alignment, buf.first, buf.last, 0});
                g.cache->stats.transient_bytes += req.memoryRequirements.size;
            }

            if (image_bits == 0 || buffer_bits == 0)
            {
                TAU_LOG_ERROR("GRAPH", "Transients have no common memory type");
                g.failed = true;
                return false;
            }

            const VkDeviceSize image_peak = place_all(image_items);
            const VkDeviceSize buffer_peak = place_all(buffer_items);
            link_aliases(g.images, image_items);
            link_aliases(g.buffers, buffer_items);

            // a heap that grows takes its images along
            const u32_t image_gen = slot.image_heap.generation;
            const u32_t buffer_gen = slot.buffer_heap.generation;
            if (!ensure_heap(slot.image_heap, image_peak, image_align, image_bits) ||
                !ensure_heap(slot.buffer_heap, buffer_peak, buffer_align, buffer_bits))
            {
                g.failed = true;
                return false;
            }
            if (slot.image_heap.generation != image_gen)
            {
                for (cached_image_t& c : slot.images) { destroy_cached(c); }
                slot.images.clear();
            }
            if (slot.buffer_heap.generation != buffer_gen)
            {
                for (cached_buffer_t& c : slot.buffers) { destroy_cached(c); }
                slot.buffers.clear();
            }
            g.cache->stats.heap_bytes = slot.image_heap.size + slot.buffer_heap.size;

            for (const place_t& it : image_items)
            {
                graph_t::image_t& img = g.images[it.index];
                const VkImageCreateInfo info = create_info(img);

                cached_image_t* found = nullptr;
                for (cached_image_t& c : slot.images)
                {
                    if (!c.taken && c.offset == img.offset && same_info(c.info, info))
                    {
                        found = &c;
                        break;
                    }
                }
                if (found == nullptr)
                {
                    cached_image_t c;
                    c.info = info;
                    c.offset = img.offset;
                    c.heap_generation = slot.image_heap.generation;
                    VK_CHECK(vmaCreateAliasingImage2(ctx.allocator, slot.image_heap.allocation, img.offset, &info,
                                                     &c.image));
                    c.view = create_view(c.image, info, img.name);
                    if (info.usage & VK_IMAGE_USAGE_SAMPLED_BIT) { c.sampled_id = write_sampled(c.view); }
                    if (info.usage & VK_IMAGE_USAGE_STORAGE_BIT) { c.storage_id = write_storage(c.view); }
                    slot.images.push_back(c);
                    found = &slot.images.back();
                    g.cache->stats.images_created++;
                }
                else
                {
                    device::name(found->image, VK_OBJECT_TYPE_IMAGE, img.name);
                }
                found->taken = true;
                found->unused_frames = 0;
                img.image = found->image;
                img.view = found->view;
                img.sampled_id = found->sampled_id;
                img.storage_id = found->storage_id;
            }

            for (const place_t& it : buffer_items)
            {
                graph_t::buffer_t& buf = g.buffers[it.index];
                cached_buffer_t* found = nullptr;
                for (cached_buffer_t& c : slot.buffers)
                {
                    if (!c.taken && c.offset == buf.offset && c.size == buf.desc.size && c.usage == buf.usage)
                    {
                        found = &c;
                        break;
                    }
                }
                if (found == nullptr)
                {
                    cached_buffer_t c;
                    c.size = buf.desc.size;
                    c.usage = buf.usage;
                    c.offset = buf.offset;
                    c.heap_generation = slot.buffer_heap.generation;
                    const VkBufferCreateInfo info = buffer_info(buf.desc.size, buf.usage);
                    VK_CHECK(vmaCreateAliasingBuffer2(ctx.allocator, slot.buffer_heap.allocation, buf.offset, &info,
                                                      &c.buffer));
                    c.address = address_of(c.buffer);
                    device::name(c.buffer, VK_OBJECT_TYPE_BUFFER, buf.name);
                    slot.buffers.push_back(c);
                    found = &slot.buffers.back();
                }
                found->taken = true;
                found->unused_frames = 0;
                buf.buffer = found->buffer;
                buf.address = found->address;
            }

            // unused for a while: gone (this slot's previous frame has finished)
            for (size_t i = 0; i < slot.images.size();)
            {
                cached_image_t& c = slot.images[i];
                if (!c.taken && ++c.unused_frames > UNUSED_FRAMES_BEFORE_FREE)
                {
                    destroy_cached(c);
                    slot.images[i] = slot.images.back();
                    slot.images.pop_back();
                    continue;
                }
                i++;
            }
            for (size_t i = 0; i < slot.buffers.size();)
            {
                cached_buffer_t& c = slot.buffers[i];
                if (!c.taken && ++c.unused_frames > UNUSED_FRAMES_BEFORE_FREE)
                {
                    destroy_cached(c);
                    slot.buffers[i] = slot.buffers.back();
                    slot.buffers.pop_back();
                    continue;
                }
                i++;
            }
            return true;
        }

        void retire(persistent_image_t& p)
        {
            std::scoped_lock lock(res_system.deletion_mutex);
            res_system.deletion_queue.push_back({
                .type = resource_type_e::TEXTURE,
                .handle = {.texture = {p.image, p.allocation, p.view}},
                .bindless_id = p.sampled_id,
                .gpu_timeline_value = deletion_timeline_value(),
            });
            if (p.storage_id != NONE)
            {
                res_system.deletion_queue.push_back({
                    .type = resource_type_e::STORAGE_IMAGE,
                    .handle = {.texture = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE}},
                    .bindless_id = p.storage_id,
                    .gpu_timeline_value = deletion_timeline_value(),
                });
            }
            p = {};
        }

        void retire(persistent_buffer_t& p)
        {
            std::scoped_lock lock(res_system.deletion_mutex);
            res_system.deletion_queue.push_back({
                .type = resource_type_e::BUFFER,
                .handle = {.buffer = {p.buffer, p.allocation}},
                .bindless_id = BINDLESS_NULL_HANDLE,
                .gpu_timeline_value = deletion_timeline_value(),
            });
            p = {};
        }

        persistent_image_t* find_persistent(graph_cache_t& c, u32_t hash)
        {
            for (persistent_image_t& p : c.persistent_images)
            {
                if (p.hash == hash) { return &p; }
            }
            return nullptr;
        }

        persistent_buffer_t* find_persistent_buffer(graph_cache_t& c, u32_t hash)
        {
            for (persistent_buffer_t& p : c.persistent_buffers)
            {
                if (p.hash == hash) { return &p; }
            }
            return nullptr;
        }

        // created or recreated on a changed desc / usage; contents restart then
        void realize_persistent(graph_t& g, graph_t::image_t& img, sync_t& sync)
        {
            const VkImageCreateInfo info = create_info(img);
            persistent_image_t* p = find_persistent(*g.cache, img.persistent_hash);
            if (p != nullptr && !same_info(p->info, info)) { retire(*p); }
            if (p == nullptr)
            {
                g.cache->persistent_images.push_back({});
                p = &g.cache->persistent_images.back();
            }
            if (p->image == VK_NULL_HANDLE)
            {
                p->hash = img.persistent_hash;
                p->info = info;
                const VmaAllocationCreateInfo alloc = {.usage = VMA_MEMORY_USAGE_GPU_ONLY};
                VK_CHECK(vmaCreateImage(ctx.allocator, &info, &alloc, &p->image, &p->allocation, nullptr));
                p->view = create_view(p->image, info, img.name);
                if (info.usage & VK_IMAGE_USAGE_SAMPLED_BIT) { p->sampled_id = write_sampled(p->view); }
                if (info.usage & VK_IMAGE_USAGE_STORAGE_BIT) { p->storage_id = write_storage(p->view); }
                p->sync = {};
            }
            img.image = p->image;
            img.view = p->view;
            img.sampled_id = p->sampled_id;
            img.storage_id = p->storage_id;
            sync = p->sync;
        }

        void realize_persistent(graph_t& g, graph_t::buffer_t& buf, sync_t& sync)
        {
            persistent_buffer_t* p = find_persistent_buffer(*g.cache, buf.persistent_hash);
            if (p != nullptr && (p->size != buf.desc.size || p->usage != buf.usage)) { retire(*p); }
            if (p == nullptr)
            {
                g.cache->persistent_buffers.push_back({});
                p = &g.cache->persistent_buffers.back();
            }
            if (p->buffer == VK_NULL_HANDLE)
            {
                p->hash = buf.persistent_hash;
                p->size = buf.desc.size;
                p->usage = buf.usage;
                const VkBufferCreateInfo info = buffer_info(buf.desc.size, buf.usage);
                const VmaAllocationCreateInfo alloc = {.usage = VMA_MEMORY_USAGE_GPU_ONLY};
                VK_CHECK(vmaCreateBuffer(ctx.allocator, &info, &alloc, &p->buffer, &p->allocation, nullptr));
                p->address = address_of(p->buffer);
                device::name(p->buffer, VK_OBJECT_TYPE_BUFFER, buf.name);
                p->sync = {};
            }
            buf.buffer = p->buffer;
            buf.address = p->address;
            sync = p->sync;
        }

        // unknown history: wait for everything before
        sync_t imported_sync(VkImageLayout layout)
        { return {layout, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, 0, 0}; }

        // returns true and fills the barrier fields when the access needs one
        bool sync_access(sync_t& s, const access_info_t& a, bool image, VkPipelineStageFlags2& src_stages,
                         VkAccessFlags2& src_access, VkImageLayout& old_layout)
        {
            old_layout = s.layout;
            src_stages = 0;
            src_access = 0;
            bool needed = false;

            const bool layout_change = image && s.layout != a.layout;
            if (layout_change || a.write)
            {
                src_stages = s.write_stages | s.read_stages;
                src_access = s.write_access;
                needed = layout_change || src_stages != 0;
                s.write_stages = a.write ? a.stages : 0;
                s.write_access =
                    a.write
                        ? (a.access & ~(VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT))
                        : 0;
                s.read_stages = a.read || !a.write ? a.stages : 0;
                s.synced_reads = s.read_stages;
                if (!a.write)
                {
                    // a layout change on a read: earlier writes become visible here
                    s.write_stages = 0;
                    s.write_access = 0;
                }
            }
            else
            {
                if (s.write_stages != 0 && (a.stages & ~s.synced_reads) != 0)
                {
                    src_stages = s.write_stages;
                    src_access = s.write_access;
                    needed = true;
                    s.synced_reads |= a.stages;
                }
                s.read_stages |= a.stages;
            }
            if (image) { s.layout = a.layout; }
            return needed;
        }
    } // namespace

    graph_cache_t* create_cache() { return new graph_cache_t(); }

    void set_profiler(graph_cache_t* cache, void* tracy_vk_ctx) { cache->tracy_ctx = tracy_vk_ctx; }

    void destroy_cache(graph_cache_t* cache)
    {
        if (cache == nullptr) { return; }
        for (slot_t& slot : cache->slots)
        {
            for (cached_image_t& c : slot.images) { destroy_cached(c); }
            for (cached_buffer_t& c : slot.buffers) { destroy_cached(c); }
            free_heap(slot.image_heap);
            free_heap(slot.buffer_heap);
        }
        for (persistent_image_t& p : cache->persistent_images)
        {
            if (p.image == VK_NULL_HANDLE) { continue; }
            if (p.sampled_id != NONE) { res_system.texture_heap.release(p.sampled_id); }
            if (p.storage_id != NONE) { res_system.storage_image_heap.release(p.storage_id); }
            if (p.view != VK_NULL_HANDLE) { vkDestroyImageView(ctx.device, p.view, nullptr); }
            vmaDestroyImage(ctx.allocator, p.image, p.allocation);
        }
        for (persistent_buffer_t& p : cache->persistent_buffers)
        {
            if (p.buffer != VK_NULL_HANDLE) { vmaDestroyBuffer(ctx.allocator, p.buffer, p.allocation); }
        }
        delete cache;
    }

    void begin(graph_t& g, graph_cache_t* cache, u32_t frame_slot)
    {
        g.cache = cache;
        g.frame_slot = frame_slot % MAX_FRAMES_IN_FLIGHT;
        g.extents.clear();
        g.images.clear();
        g.buffers.clear();
        g.passes.clear();
        g.uses.clear();
        g.data.clear();
        g.memory_barriers.clear();
        g.image_barriers.clear();
        g.final_barriers.clear();
        g.compiled = false;
        g.failed = false;
        cache->last_stats = cache->stats;
        cache->stats = {};
    }

    void set_extent(graph_t& g, u32_t hash, VkExtent2D extent)
    {
        for (graph_t::extent_t& e : g.extents)
        {
            if (e.hash == hash)
            {
                e.extent = extent;
                return;
            }
        }
        g.extents.push_back({hash, extent});
    }

    image_h create_image(graph_t& g, const char* name, const image_desc_t& desc)
    {
        graph_t::image_t img;
        img.name = name;
        img.desc = desc;
        img.kind = TRANSIENT;
        img.aspect = aspect_of(desc.format);
        g.images.push_back(img);
        return {static_cast<u32_t>(g.images.size() - 1)};
    }

    image_h persistent_image(graph_t& g, const char* name, const image_desc_t& desc)
    {
        const image_h h = create_image(g, name, desc);
        g.images[h.index].kind = PERSISTENT;
        g.images[h.index].persistent_hash = hash_name(name);
        return h;
    }

    image_h import_image(graph_t& g, const char* name, VkImage image, VkImageView view, const image_desc_t& desc,
                         VkImageLayout layout, VkImageLayout final_layout, u32_t sampled)
    {
        const image_h h = create_image(g, name, desc);
        graph_t::image_t& img = g.images[h.index];
        img.kind = IMPORTED;
        img.image = image;
        img.view = view;
        img.sampled_id = sampled;
        img.initial_layout = layout;
        img.final_layout = final_layout;
        return h;
    }

    buffer_h create_buffer(graph_t& g, const char* name, const buffer_desc_t& desc)
    {
        graph_t::buffer_t buf;
        buf.name = name;
        buf.desc = desc;
        buf.kind = TRANSIENT;
        g.buffers.push_back(buf);
        return {static_cast<u32_t>(g.buffers.size() - 1)};
    }

    buffer_h persistent_buffer(graph_t& g, const char* name, const buffer_desc_t& desc)
    {
        const buffer_h h = create_buffer(g, name, desc);
        g.buffers[h.index].kind = PERSISTENT;
        g.buffers[h.index].persistent_hash = hash_name(name);
        return h;
    }

    buffer_h import_buffer(graph_t& g, const char* name, VkBuffer buffer, VkDeviceSize size, VkDeviceAddress address)
    {
        const buffer_h h = create_buffer(g, name, {.size = size});
        graph_t::buffer_t& buf = g.buffers[h.index];
        buf.kind = IMPORTED;
        buf.buffer = buffer;
        buf.address = address;
        return h;
    }

    pass_h add_pass(graph_t& g, const char* name, record_fn_t record, const void* data, u32_t data_size, u8_t flags)
    {
        graph_t::pass_t p;
        p.name = name;
        p.record = record;
        p.flags = flags;
        if (data != nullptr && data_size > 0)
        {
            const size_t offset = (g.data.size() + 15) & ~size_t(15);
            g.data.resize(offset + data_size);
            std::memcpy(g.data.data() + offset, data, data_size);
            p.data_offset = static_cast<u32_t>(offset);
        }
        g.passes.push_back(p);
        return {static_cast<u32_t>(g.passes.size() - 1)};
    }

    void use(graph_t& g, pass_h pass, image_h image, access_e access, load_e load, VkPipelineStageFlags2 stages)
    { g.uses.push_back({pass.index, image.index, true, access, load, stages}); }

    void use(graph_t& g, pass_h pass, buffer_h buffer, access_e access, VkPipelineStageFlags2 stages)
    { g.uses.push_back({pass.index, buffer.index, false, access, load_e::LOAD, stages}); }

    void set_area(graph_t& g, pass_h pass, VkRect2D area) { g.passes[pass.index].area = area; }

    image_h find_image(const graph_t& g, const char* name)
    {
        const std::string_view wanted = name;
        for (u32_t i = 0; i < g.images.size(); i++)
        {
            if (g.images[i].name != nullptr && wanted == g.images[i].name) { return {i}; }
        }
        return {};
    }

    bool compile(graph_t& g)
    {
        ZoneScopedN("graph compile");
        const auto start = std::chrono::steady_clock::now();
        g.compiled = false;
        if (g.failed) { return false; }

        const u32_t pass_count = static_cast<u32_t>(g.passes.size());

        // uses grouped by pass, kept in declaration order inside a pass
        std::stable_sort(g.uses.begin(), g.uses.end(),
                         [](const graph_t::use_t& a, const graph_t::use_t& b) { return a.pass < b.pass; });

        std::vector<bool> raster(pass_count, false);
        for (const graph_t::use_t& u : g.uses)
        {
            if (u.pass >= pass_count) { return fail(g, "Use of an unknown pass", nullptr, nullptr); }
            if (u.is_image ? u.resource >= g.images.size() : u.resource >= g.buffers.size())
            {
                return fail(g, "Use of an unknown resource", g.passes[u.pass].name, nullptr);
            }
            const access_info_t a = info_of(u.access, false, 0);
            if (a.attachment) { raster[u.pass] = true; }
        }

        // culling, back to front: needed when it has side effects, writes something kept, or feeds a needed pass
        std::vector<bool> wanted_images(g.images.size(), false);
        std::vector<bool> wanted_buffers(g.buffers.size(), false);
        for (u32_t p = pass_count; p-- > 0;)
        {
            graph_t::pass_t& pass = g.passes[p];
            pass.needed = (pass.flags & PASS_SIDE_EFFECT) != 0;
            for (const graph_t::use_t& u : g.uses)
            {
                if (u.pass != p) { continue; }
                const access_info_t a = info_of(u.access, raster[p], u.stages);
                if (!a.write) { continue; }
                const u32_t kind = u.is_image ? g.images[u.resource].kind : g.buffers[u.resource].kind;
                const bool wanted = u.is_image ? wanted_images[u.resource] : wanted_buffers[u.resource];
                if (kind != TRANSIENT || wanted) { pass.needed = true; }
            }
            if (!pass.needed)
            {
                g.cache->stats.culled++;
                continue;
            }
            for (const graph_t::use_t& u : g.uses)
            {
                if (u.pass != p) { continue; }
                const access_info_t a = info_of(u.access, raster[p], u.stages);
                const bool reads = a.read || (a.attachment && u.load == load_e::LOAD);
                if (!reads) { continue; }
                (u.is_image ? wanted_images : wanted_buffers)[u.resource] = true;
            }
        }

        // sizes, usage, lifetimes over needed passes
        for (graph_t::image_t& img : g.images)
        {
            if (img.desc.extent_hash != 0)
            {
                bool found = false;
                for (const graph_t::extent_t& e : g.extents)
                {
                    if (e.hash != img.desc.extent_hash) { continue; }
                    img.desc.width =
                        std::max(1u, static_cast<u32_t>(static_cast<f32>(e.extent.width) * img.desc.scale));
                    img.desc.height =
                        std::max(1u, static_cast<u32_t>(static_cast<f32>(e.extent.height) * img.desc.scale));
                    found = true;
                }
                if (!found) { return fail(g, "Size relative to an extent never set", nullptr, img.name); }
            }
            if (img.desc.width == 0 || img.desc.height == 0)
            {
                return fail(g, "Image without a size", nullptr, img.name);
            }
        }

        for (const graph_t::use_t& u : g.uses)
        {
            if (!g.passes[u.pass].needed) { continue; }
            const access_info_t a = info_of(u.access, raster[u.pass], u.stages);
            if (u.is_image)
            {
                graph_t::image_t& img = g.images[u.resource];
                if (a.image_usage == 0)
                {
                    return fail(g, "Buffer access on an image", g.passes[u.pass].name, img.name);
                }
                img.usage |= a.image_usage;
                if (img.first == NONE) { img.first = u.pass; }
                img.last = u.pass;
            }
            else
            {
                graph_t::buffer_t& buf = g.buffers[u.resource];
                buf.usage |=
                    a.buffer_usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                if (buf.first == NONE) { buf.first = u.pass; }
                buf.last = u.pass;
            }
        }

        if (!allocate_transients(g)) { return false; }

        // states: persistent from last frame, imported from the caller, transient undefined
        std::vector<sync_t> image_sync(g.images.size());
        std::vector<sync_t> buffer_sync(g.buffers.size());
        std::vector<bool> image_written(g.images.size(), false);
        for (u32_t i = 0; i < g.images.size(); i++)
        {
            graph_t::image_t& img = g.images[i];
            if (img.first == NONE) { continue; }
            if (img.kind == PERSISTENT) { realize_persistent(g, img, image_sync[i]); }
            else if (img.kind == IMPORTED) { image_sync[i] = imported_sync(img.initial_layout); }
        }
        for (u32_t i = 0; i < g.buffers.size(); i++)
        {
            graph_t::buffer_t& buf = g.buffers[i];
            if (buf.first == NONE) { continue; }
            if (buf.kind == PERSISTENT) { realize_persistent(g, buf, buffer_sync[i]); }
            else if (buf.kind == IMPORTED) { buffer_sync[i] = imported_sync(VK_IMAGE_LAYOUT_UNDEFINED); }
        }

        // barriers, front to back
        size_t u_index = 0;
        for (u32_t p = 0; p < pass_count; p++)
        {
            graph_t::pass_t& pass = g.passes[p];
            const size_t u_begin = u_index;
            while (u_index < g.uses.size() && g.uses[u_index].pass == p) { u_index++; }
            if (!pass.needed) { continue; }

            VkMemoryBarrier2 merged = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            pass.first_image_barrier = static_cast<u32_t>(g.image_barriers.size());

            for (size_t k = u_begin; k < u_index; k++)
            {
                const graph_t::use_t& u = g.uses[k];
                const access_info_t a = info_of(u.access, raster[p], u.stages);
                const bool reads = a.read || (a.attachment && u.load == load_e::LOAD);

                if (u.is_image)
                {
                    graph_t::image_t& img = g.images[u.resource];
                    sync_t& s = image_sync[u.resource];

                    // declaration order: a transient is written before anything reads it
                    if (img.kind == TRANSIENT && reads && !image_written[u.resource])
                    {
                        return fail(g, "Reads a transient image nothing wrote before", pass.name, img.name);
                    }
                    for (size_t j = u_begin; j < k; j++)
                    {
                        const graph_t::use_t& o = g.uses[j];
                        if (o.is_image && o.resource == u.resource &&
                            info_of(o.access, raster[p], o.stages).layout != a.layout)
                        {
                            return fail(g, "Two layouts of one image in one pass", pass.name, img.name);
                        }
                    }

                    // first use after an alias: the previous occupant's last access is the source
                    if (img.kind == TRANSIENT && img.first == p && img.alias_prev != NONE)
                    {
                        const sync_t& prev = image_sync[img.alias_prev];
                        s.write_stages = prev.write_stages | prev.read_stages;
                        s.write_access = prev.write_access;
                    }

                    VkPipelineStageFlags2 src_stages = 0;
                    VkAccessFlags2 src_access = 0;
                    VkImageLayout old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
                    if (sync_access(s, a, true, src_stages, src_access, old_layout))
                    {
                        // a transient's first write discards whatever was there
                        if (img.kind == TRANSIENT && !image_written[u.resource])
                        {
                            old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
                        }
                        g.image_barriers.push_back({
                            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                            .srcStageMask = src_stages != 0 ? src_stages : VK_PIPELINE_STAGE_2_NONE,
                            .srcAccessMask = src_access,
                            .dstStageMask = a.stages,
                            .dstAccessMask = a.access,
                            .oldLayout = old_layout,
                            .newLayout = a.layout,
                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                            .image = img.image,
                            .subresourceRange = {img.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS},
                        });
                    }
                    if (a.write) { image_written[u.resource] = true; }
                }
                else
                {
                    graph_t::buffer_t& buf = g.buffers[u.resource];
                    sync_t& s = buffer_sync[u.resource];
                    if (buf.kind == TRANSIENT && buf.first == p && a.read)
                    {
                        return fail(g, "Reads a transient buffer nothing wrote before", pass.name, buf.name);
                    }
                    if (buf.kind == TRANSIENT && buf.first == p && buf.alias_prev != NONE)
                    {
                        const sync_t& prev = buffer_sync[buf.alias_prev];
                        s.write_stages = prev.write_stages | prev.read_stages;
                        s.write_access = prev.write_access;
                    }

                    VkPipelineStageFlags2 src_stages = 0;
                    VkAccessFlags2 src_access = 0;
                    VkImageLayout unused = VK_IMAGE_LAYOUT_UNDEFINED;
                    if (sync_access(s, a, false, src_stages, src_access, unused))
                    {
                        merged.srcStageMask |= src_stages;
                        merged.srcAccessMask |= src_access;
                        merged.dstStageMask |= a.stages;
                        merged.dstAccessMask |= a.access;
                    }
                }
            }

            pass.image_barrier_count = static_cast<u32_t>(g.image_barriers.size()) - pass.first_image_barrier;
            pass.first_barrier = static_cast<u32_t>(g.memory_barriers.size());
            pass.barrier_count = 0;
            if (merged.dstStageMask != 0)
            {
                if (merged.srcStageMask == 0) { merged.srcStageMask = VK_PIPELINE_STAGE_2_NONE; }
                g.memory_barriers.push_back(merged);
                pass.barrier_count = 1;
            }
            g.cache->stats.barriers += pass.image_barrier_count + pass.barrier_count;
            g.cache->stats.passes++;
        }

        // hand back: imported to their final layout, persistent state for the next frame
        for (u32_t i = 0; i < g.images.size(); i++)
        {
            graph_t::image_t& img = g.images[i];
            if (img.first == NONE) { continue; }
            sync_t& s = image_sync[i];
            if (img.kind == IMPORTED && img.final_layout != VK_IMAGE_LAYOUT_UNDEFINED && img.final_layout != s.layout)
            {
                g.final_barriers.push_back({
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .srcStageMask = s.write_stages | s.read_stages,
                    .srcAccessMask = s.write_access,
                    .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    .oldLayout = s.layout,
                    .newLayout = img.final_layout,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = img.image,
                    .subresourceRange = {img.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS},
                });
            }
            if (img.kind == PERSISTENT)
            {
                if (persistent_image_t* p = find_persistent(*g.cache, img.persistent_hash)) { p->sync = s; }
            }
        }
        for (u32_t i = 0; i < g.buffers.size(); i++)
        {
            graph_t::buffer_t& buf = g.buffers[i];
            if (buf.first == NONE || buf.kind != PERSISTENT) { continue; }
            if (persistent_buffer_t* p = find_persistent_buffer(*g.cache, buf.persistent_hash))
            {
                p->sync = buffer_sync[i];
            }
        }

        g.cache->stats.compile_ms = ms_since(start);
        g.compiled = true;
        return true;
    }

    void execute(graph_t& g, VkCommandBuffer cmd)
    {
        if (!g.compiled) { return; }

        // uses are grouped by pass after compile
        size_t u_index = 0;
        for (u32_t p = 0; p < g.passes.size(); p++)
        {
            const graph_t::pass_t& pass = g.passes[p];
            const size_t u_begin = u_index;
            while (u_index < g.uses.size() && g.uses[u_index].pass == p) { u_index++; }
            if (!pass.needed) { continue; }

#ifdef TAU_ENABLE_PROFILING
            TracyVkZoneTransient(static_cast<TracyVkCtx>(g.cache->tracy_ctx), graph_zone, cmd,
                                 pass.name ? pass.name : "pass", g.cache->tracy_ctx != nullptr);
#endif
            gpu_timers::begin_pass(cmd, pass.name);
            device::begin_label(cmd, pass.name);

            if (pass.barrier_count + pass.image_barrier_count > 0)
            {
                const VkDependencyInfo dep = {
                    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .memoryBarrierCount = pass.barrier_count,
                    .pMemoryBarriers = pass.barrier_count ? &g.memory_barriers[pass.first_barrier] : nullptr,
                    .imageMemoryBarrierCount = pass.image_barrier_count,
                    .pImageMemoryBarriers =
                        pass.image_barrier_count ? &g.image_barriers[pass.first_image_barrier] : nullptr,
                };
                vkCmdPipelineBarrier2(cmd, &dep);
            }

            VkRenderingAttachmentInfo colors[8] = {};
            u32_t color_count = 0;
            VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            bool has_depth = false;
            VkExtent2D size = {};
            for (size_t k = u_begin; k < u_index; k++)
            {
                const graph_t::use_t& u = g.uses[k];
                if (!u.is_image) { continue; }
                const access_info_t a = info_of(u.access, true, 0);
                if (!a.attachment) { continue; }

                const graph_t::image_t& img = g.images[u.resource];
                // later passes still need it, or it outlives the frame
                const bool keep = img.kind != TRANSIENT || img.last != p;
                VkRenderingAttachmentInfo att = {
                    .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                    .imageView = img.view,
                    .imageLayout = a.layout,
                    .loadOp = u.load == load_e::CLEAR       ? VK_ATTACHMENT_LOAD_OP_CLEAR
                              : u.load == load_e::DONT_CARE ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                                                            : VK_ATTACHMENT_LOAD_OP_LOAD,
                    .storeOp = !a.write ? VK_ATTACHMENT_STORE_OP_NONE
                               : keep   ? VK_ATTACHMENT_STORE_OP_STORE
                                        : VK_ATTACHMENT_STORE_OP_DONT_CARE,
                    .clearValue = img.desc.clear,
                };
                if (size.width == 0) { size = {img.desc.width, img.desc.height}; }
                if (u.access == access_e::DEPTH_WRITE || u.access == access_e::DEPTH_READ)
                {
                    depth = att;
                    has_depth = true;
                }
                else if (color_count < 8) { colors[color_count++] = att; }
            }

            record_ctx_t rc = {
                .cmd = cmd, .graph = &g, .area = {{0, 0}, size}
            };
            if (pass.area.extent.width > 0) { rc.area = pass.area; }

            const bool raster = color_count > 0 || has_depth;
            if (raster)
            {
                const VkRenderingInfo info = {
                    .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                    .renderArea = rc.area,
                    .layerCount = 1,
                    .colorAttachmentCount = color_count,
                    .pColorAttachments = color_count ? colors : nullptr,
                    .pDepthAttachment = has_depth ? &depth : nullptr,
                };
                vkCmdBeginRendering(cmd, &info);
                const VkViewport vp = {static_cast<f32>(rc.area.offset.x),
                                       static_cast<f32>(rc.area.offset.y),
                                       static_cast<f32>(rc.area.extent.width),
                                       static_cast<f32>(rc.area.extent.height),
                                       0.0f,
                                       1.0f};
                vkCmdSetViewport(cmd, 0, 1, &vp);
                vkCmdSetScissor(cmd, 0, 1, &rc.area);
            }

            if (pass.record != nullptr)
            {
                const void* data = pass.data_offset != NONE ? g.data.data() + pass.data_offset : nullptr;
                pass.record(rc, data);
            }

            if (raster) { vkCmdEndRendering(cmd); }
            device::end_label(cmd);
            gpu_timers::end_pass(cmd);
        }

        if (!g.final_barriers.empty())
        {
            const VkDependencyInfo dep = {
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .imageMemoryBarrierCount = static_cast<u32_t>(g.final_barriers.size()),
                .pImageMemoryBarriers = g.final_barriers.data(),
            };
            vkCmdPipelineBarrier2(cmd, &dep);
        }
    }

    VkImage image(const graph_t& g, image_h h) { return h.valid() ? g.images[h.index].image : VK_NULL_HANDLE; }
    VkImageView view(const graph_t& g, image_h h) { return h.valid() ? g.images[h.index].view : VK_NULL_HANDLE; }

    VkExtent2D extent(const graph_t& g, image_h h)
    { return h.valid() ? VkExtent2D{g.images[h.index].desc.width, g.images[h.index].desc.height} : VkExtent2D{}; }

    VkFormat format(const graph_t& g, image_h h)
    { return h.valid() ? g.images[h.index].desc.format : VK_FORMAT_UNDEFINED; }

    u32_t sampled_id(const graph_t& g, image_h h) { return h.valid() ? g.images[h.index].sampled_id : NONE; }
    u32_t storage_id(const graph_t& g, image_h h) { return h.valid() ? g.images[h.index].storage_id : NONE; }
    VkBuffer buffer(const graph_t& g, buffer_h h) { return h.valid() ? g.buffers[h.index].buffer : VK_NULL_HANDLE; }
    VkDeviceAddress address(const graph_t& g, buffer_h h) { return h.valid() ? g.buffers[h.index].address : 0; }

    stats_t stats(const graph_t& g) { return g.cache != nullptr ? g.cache->stats : stats_t{}; }
} // namespace tau::renderer::graph
