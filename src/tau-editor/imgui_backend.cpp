#include "imgui_backend.h"

#include "imgui/imgui.h"
#include "tau/assets/material.h"
#include "tau/assets/shader.h"
#include "tau/defines.h"
#include "tau/ecs_fwd.h"
#include "tau/engine.h"
#include "tau/hash.h"
#include "tau/math.h"
#include "tau/rendering/renderer.h"
#include "tau/rendering/renderer_constants.h"
#include "tau/rendering/renderer_resources.h"
#include "tau/rendering/renderer_types.h"
#include "tau/rendering/rendergraph.h"
#include "tau/rendering/vulkan.h"
#include "vulkan/vulkan_core.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace tau::editor::imgui
{
    struct imgui_ctx_t
    {
        asset_t<shader_t> shader;
        // built straight from the shader: nothing authors or shares it, so it is not an asset
        material_t material;

        VkImage font_image = VK_NULL_HANDLE;
        VmaAllocation font_alloc = VK_NULL_HANDLE;
        VkImageView font_view = VK_NULL_HANDLE;
        u32_t font_tex_bindless_id = renderer::BINDLESS_NULL_HANDLE;

        VkBuffer vertex_buffer[renderer::MAX_FRAMES_IN_FLIGHT] = {VK_NULL_HANDLE};
        VmaAllocation vertex_alloc[renderer::MAX_FRAMES_IN_FLIGHT] = {VK_NULL_HANDLE};
        void* vertex_mapped[renderer::MAX_FRAMES_IN_FLIGHT] = {nullptr};
        size_t vertex_buffer_size[renderer::MAX_FRAMES_IN_FLIGHT] = {0};

        VkBuffer index_buffer[renderer::MAX_FRAMES_IN_FLIGHT] = {VK_NULL_HANDLE};
        VmaAllocation index_alloc[renderer::MAX_FRAMES_IN_FLIGHT] = {VK_NULL_HANDLE};
        void* index_mapped[renderer::MAX_FRAMES_IN_FLIGHT] = {nullptr};
        size_t index_buffer_size[renderer::MAX_FRAMES_IN_FLIGHT] = {0};

        ImDrawData* draw_data = nullptr;
    };

    static imgui_ctx_t ctx;

    struct imgui_material_gpu_t
    {
        vec2_t scale;
        vec2_t translate;
        VkDeviceAddress vertex_buffer;
    };

    struct viewport_frame_t
    {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore image_available = VK_NULL_HANDLE;

        VkBuffer vertex_buffer = VK_NULL_HANDLE;
        VmaAllocation vertex_alloc = VK_NULL_HANDLE;
        void* vertex_mapped = nullptr;
        size_t vertex_size = 0;

        VkBuffer index_buffer = VK_NULL_HANDLE;
        VmaAllocation index_alloc = VK_NULL_HANDLE;
        void* index_mapped = nullptr;
        size_t index_size = 0;

        VkBuffer material_buffer = VK_NULL_HANDLE;
        VmaAllocation material_alloc = VK_NULL_HANDLE;
        void* material_mapped = nullptr;
        VkDeviceAddress material_addr = 0;
    };

    struct viewport_data_t
    {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent = {0, 0};

        std::vector<VkImage> images;
        std::vector<VkImageView> views;
        std::vector<VkSemaphore> render_finished;

        viewport_frame_t frames[renderer::MAX_FRAMES_IN_FLIGHT];
        u32_t frame_index = 0;

        u32_t acquired_image = 0;
        bool valid = false;
    };

    static bool get_imgui_pipeline(VkPipeline& out_pipeline, VkPipelineLayout& out_layout)
    {
        material_t* mat = &ctx.material;
        shader_t* shader = assets::get(mat->shader);
        if (!shader) { return false; }
        out_pipeline = shader->get_pipeline(pipeline_variant_e::FORWARD);
        out_layout = shader->pipeline_layout;
        return out_pipeline != VK_NULL_HANDLE;
    }

    static void grow_buffer(VkBuffer& buf, VmaAllocation& alloc, void*& mapped, size_t& cur_size, size_t needed,
                            VkBufferUsageFlags usage)
    {
        if (buf && cur_size >= needed) { return; }
        if (buf) { vmaDestroyBuffer(renderer::ctx.allocator, buf, alloc); }

        size_t size = needed + 5000;
        VkBufferCreateInfo buf_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = usage,
        };
        VmaAllocationCreateInfo alloc_info = {
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
            .requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        };
        VmaAllocationInfo info;
        VK_CHECK(vmaCreateBuffer(renderer::ctx.allocator, &buf_info, &alloc_info, &buf, &alloc, &info));
        mapped = info.pMappedData;
        cur_size = size;
    }

    static void destroy_viewport_swapchain(viewport_data_t& vd)
    {
        for (VkImageView view : vd.views) { vkDestroyImageView(renderer::ctx.device, view, nullptr); }
        vd.views.clear();
        for (VkSemaphore sem : vd.render_finished) { vkDestroySemaphore(renderer::ctx.device, sem, nullptr); }
        vd.render_finished.clear();
        vd.images.clear();
        if (vd.swapchain) { vkDestroySwapchainKHR(renderer::ctx.device, vd.swapchain, nullptr); }
        vd.swapchain = VK_NULL_HANDLE;
    }

    static bool build_viewport_swapchain(viewport_data_t& vd)
    {
        VkSurfaceCapabilitiesKHR caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(renderer::ctx.physical_device, vd.surface, &caps);

        VkExtent2D extent = caps.currentExtent;
        if (extent.width == 0 || extent.height == 0)
        {
            vd.valid = false;
            return false;
        }

        VkFormat want_format = renderer::ctx.swapchain.format;
        VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        {
            u32_t fmt_count = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(renderer::ctx.physical_device, vd.surface, &fmt_count, nullptr);
            std::vector<VkSurfaceFormatKHR> formats(fmt_count);
            vkGetPhysicalDeviceSurfaceFormatsKHR(renderer::ctx.physical_device, vd.surface, &fmt_count, formats.data());
            for (const VkSurfaceFormatKHR& f : formats)
            {
                if (f.format == want_format)
                {
                    color_space = f.colorSpace;
                    break;
                }
            }
        }

        VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
        {
            u32_t mode_count = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(renderer::ctx.physical_device, vd.surface, &mode_count, nullptr);
            std::vector<VkPresentModeKHR> modes(mode_count);
            vkGetPhysicalDeviceSurfacePresentModesKHR(renderer::ctx.physical_device, vd.surface, &mode_count,
                                                      modes.data());
            bool has_mailbox = false, has_immediate = false;
            for (VkPresentModeKHR m : modes)
            {
                if (m == VK_PRESENT_MODE_MAILBOX_KHR) { has_mailbox = true; }
                if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) { has_immediate = true; }
            }
            if (has_mailbox) { present_mode = VK_PRESENT_MODE_MAILBOX_KHR; }
            else if (has_immediate) { present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR; }
        }

        u32_t min_images = caps.minImageCount + 1;
        if (caps.maxImageCount > 0 && min_images > caps.maxImageCount) { min_images = caps.maxImageCount; }
        if (present_mode == VK_PRESENT_MODE_MAILBOX_KHR && min_images < 3)
        {
            min_images = 3;
            if (caps.maxImageCount > 0 && min_images > caps.maxImageCount) { min_images = caps.maxImageCount; }
        }

        VkSwapchainKHR old = vd.swapchain;
        VkSwapchainCreateInfoKHR info = {
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = vd.surface,
            .minImageCount = min_images,
            .imageFormat = want_format,
            .imageColorSpace = color_space,
            .imageExtent = extent,
            .imageArrayLayers = 1,
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .preTransform = caps.currentTransform,
            .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode = present_mode,
            .clipped = true,
            .oldSwapchain = old,
        };

        VkSwapchainKHR new_swapchain;
        VK_CHECK(vkCreateSwapchainKHR(renderer::ctx.device, &info, nullptr, &new_swapchain));

        destroy_viewport_swapchain(vd);
        vd.swapchain = new_swapchain;
        vd.format = want_format;
        vd.extent = extent;

        u32_t image_count = 0;
        vkGetSwapchainImagesKHR(renderer::ctx.device, vd.swapchain, &image_count, nullptr);
        vd.images.resize(image_count);
        vkGetSwapchainImagesKHR(renderer::ctx.device, vd.swapchain, &image_count, vd.images.data());

        vd.views.resize(image_count);
        vd.render_finished.resize(image_count);
        for (u32_t i = 0; i < image_count; i++)
        {
            VkImageViewCreateInfo view_info = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = vd.images[i],
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = vd.format,
                .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                     .baseMipLevel = 0,
                                     .levelCount = 1,
                                     .baseArrayLayer = 0,
                                     .layerCount = 1},
            };
            VK_CHECK(vkCreateImageView(renderer::ctx.device, &view_info, nullptr, &vd.views[i]));

            VkSemaphoreCreateInfo sem_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VK_CHECK(vkCreateSemaphore(renderer::ctx.device, &sem_info, nullptr, &vd.render_finished[i]));
        }

        vd.valid = true;
        return true;
    }

    static void viewport_create_window(ImGuiViewport* viewport)
    {
        viewport_data_t* vd = IM_NEW(viewport_data_t)();
        viewport->RendererUserData = vd;

        ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (pio.Platform_CreateVkSurface)
        {
            pio.Platform_CreateVkSurface(viewport, (ImU64)renderer::ctx.instance, nullptr, (ImU64*)&surface);
        }
        vd->surface = surface;

        u32_t queue_family = renderer::ctx.queue_fam_indices.graphics_family.value();
        for (viewport_frame_t& frame : vd->frames)
        {
            VkCommandPoolCreateInfo pool_info = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                .queueFamilyIndex = queue_family,
            };
            VK_CHECK(vkCreateCommandPool(renderer::ctx.device, &pool_info, nullptr, &frame.pool));

            VkCommandBufferAllocateInfo cmd_info = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = frame.pool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
            };
            VK_CHECK(vkAllocateCommandBuffers(renderer::ctx.device, &cmd_info, &frame.cmd));

            VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                            .flags = VK_FENCE_CREATE_SIGNALED_BIT};
            VK_CHECK(vkCreateFence(renderer::ctx.device, &fence_info, nullptr, &frame.fence));

            VkSemaphoreCreateInfo sem_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VK_CHECK(vkCreateSemaphore(renderer::ctx.device, &sem_info, nullptr, &frame.image_available));
        }

        build_viewport_swapchain(*vd);
    }

    static void viewport_destroy_window(ImGuiViewport* viewport)
    {
        viewport_data_t* vd = (viewport_data_t*)viewport->RendererUserData;
        if (!vd) { return; }

        vkDeviceWaitIdle(renderer::ctx.device);

        destroy_viewport_swapchain(*vd);

        for (viewport_frame_t& frame : vd->frames)
        {
            if (frame.vertex_buffer)
            {
                vmaDestroyBuffer(renderer::ctx.allocator, frame.vertex_buffer, frame.vertex_alloc);
            }
            if (frame.index_buffer)
            {
                vmaDestroyBuffer(renderer::ctx.allocator, frame.index_buffer, frame.index_alloc);
            }
            if (frame.material_buffer)
            {
                vmaDestroyBuffer(renderer::ctx.allocator, frame.material_buffer, frame.material_alloc);
            }
            if (frame.image_available) { vkDestroySemaphore(renderer::ctx.device, frame.image_available, nullptr); }
            if (frame.fence) { vkDestroyFence(renderer::ctx.device, frame.fence, nullptr); }
            if (frame.pool) { vkDestroyCommandPool(renderer::ctx.device, frame.pool, nullptr); }
        }

        if (vd->surface) { vkDestroySurfaceKHR(renderer::ctx.instance, vd->surface, nullptr); }

        IM_DELETE(vd);
        viewport->RendererUserData = nullptr;
    }

    static void viewport_set_window_size(ImGuiViewport* viewport, ImVec2)
    {
        viewport_data_t* vd = (viewport_data_t*)viewport->RendererUserData;
        if (!vd) { return; }
        vkDeviceWaitIdle(renderer::ctx.device);
        build_viewport_swapchain(*vd);
    }

    static void record_viewport_draw(viewport_frame_t& frame, viewport_data_t& vd, ImDrawData* draw_data,
                                     u32_t image_index)
    {
        VkPipeline pipeline;
        VkPipelineLayout layout;
        if (!get_imgui_pipeline(pipeline, layout)) { return; }

        size_t vertex_size = draw_data->TotalVtxCount * sizeof(ImDrawVert);
        size_t index_size = draw_data->TotalIdxCount * sizeof(u32_t);

        grow_buffer(frame.vertex_buffer, frame.vertex_alloc, frame.vertex_mapped, frame.vertex_size,
                    vertex_size == 0 ? 4 : vertex_size,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        grow_buffer(frame.index_buffer, frame.index_alloc, frame.index_mapped, frame.index_size,
                    index_size == 0 ? 4 : index_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

        if (frame.material_buffer == VK_NULL_HANDLE)
        {
            size_t msize = 0;
            grow_buffer(frame.material_buffer, frame.material_alloc, frame.material_mapped, msize,
                        sizeof(imgui_material_gpu_t),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
            frame.material_addr = renderer::get_buffer_address(frame.material_buffer);
        }

        ImDrawVert* vtx_dst = (ImDrawVert*)frame.vertex_mapped;
        u32_t* idx_dst = (u32_t*)frame.index_mapped;
        i32 global_vtx_offset = 0;
        for (i32 i = 0; i < draw_data->CmdListsCount; i++)
        {
            const ImDrawList* cmd_list = draw_data->CmdLists[i];
            std::memcpy(vtx_dst, cmd_list->VtxBuffer.Data, cmd_list->VtxBuffer.Size * sizeof(ImDrawVert));
            for (i32 j = 0; j < cmd_list->IdxBuffer.Size; j++)
            {
                idx_dst[j] = (u32_t)(cmd_list->IdxBuffer.Data[j]) + global_vtx_offset;
            }
            vtx_dst += cmd_list->VtxBuffer.Size;
            idx_dst += cmd_list->IdxBuffer.Size;
            global_vtx_offset += cmd_list->VtxBuffer.Size;
        }

        vec2_t scale = {2.0f / draw_data->DisplaySize.x, 2.0f / draw_data->DisplaySize.y};
        vec2_t translate = {-1.0f - draw_data->DisplayPos.x * scale.x, -1.0f - draw_data->DisplayPos.y * scale.y};

        imgui_material_gpu_t gpu_mat = {
            .scale = scale,
            .translate = translate,
            .vertex_buffer = renderer::get_buffer_address(frame.vertex_buffer),
        };
        std::memcpy(frame.material_mapped, &gpu_mat, sizeof(gpu_mat));

        VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                               .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(frame.cmd, &begin_info);

        VkImageMemoryBarrier to_color = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = vd.images[image_index],
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        vkCmdPipelineBarrier(frame.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_color);

        VkClearValue clear = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}};
        VkRenderingAttachmentInfo color_attachment = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = vd.views[image_index],
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = clear,
        };
        VkRenderingInfo rendering_info = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = {{0, 0}, vd.extent},
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &color_attachment,
        };
        vkCmdBeginRendering(frame.cmd, &rendering_info);

        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

        VkDescriptorSet global_set = renderer::res_system.global_set;
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &global_set, 0, nullptr);

        vkCmdBindIndexBuffer(frame.cmd, frame.index_buffer, 0, VK_INDEX_TYPE_UINT32);

        VkViewport vp = {0, 0, (f32)vd.extent.width, (f32)vd.extent.height, 0.0f, 1.0f};
        vkCmdSetViewport(frame.cmd, 0, 1, &vp);

        renderer::push_constants_t pc = {
            .material_buffer = frame.material_addr,
            .custom_data = 0,
        };

        ImVec2 clip_off = draw_data->DisplayPos;
        ImVec2 fb_scale = draw_data->FramebufferScale;
        i32 global_index_offset = 0;
        for (i32 i = 0; i < draw_data->CmdListsCount; i++)
        {
            const ImDrawList* cmd_list = draw_data->CmdLists[i];
            for (i32 cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
            {
                const ImDrawCmd* cmd_ptr = &cmd_list->CmdBuffer[cmd_i];

                ImVec2 clip_min((cmd_ptr->ClipRect.x - clip_off.x) * fb_scale.x,
                                (cmd_ptr->ClipRect.y - clip_off.y) * fb_scale.y);
                ImVec2 clip_max((cmd_ptr->ClipRect.z - clip_off.x) * fb_scale.x,
                                (cmd_ptr->ClipRect.w - clip_off.y) * fb_scale.y);

                clip_min.x = std::max(0.0f, clip_min.x);
                clip_min.y = std::max(0.0f, clip_min.y);
                clip_max.x = std::min((f32)vd.extent.width, clip_max.x);
                clip_max.y = std::min((f32)vd.extent.height, clip_max.y);
                if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y) { continue; }

                VkRect2D scissor = {
                    {(i32_t)clip_min.x,                (i32_t)clip_min.y               },
                    {(u32_t)(clip_max.x - clip_min.x), (u32_t)(clip_max.y - clip_min.y)}
                };
                vkCmdSetScissor(frame.cmd, 0, 1, &scissor);

                pc.texture_id = (u32_t)(intptr_t)cmd_ptr->GetTexID();
                vkCmdPushConstants(frame.cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(renderer::push_constants_t), &pc);
                vkCmdDrawIndexed(frame.cmd, cmd_ptr->ElemCount, 1, cmd_ptr->IdxOffset + global_index_offset, 0, 0);
            }
            global_index_offset += cmd_list->IdxBuffer.Size;
        }

        vkCmdEndRendering(frame.cmd);

        VkImageMemoryBarrier to_present = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = 0,
            .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = vd.images[image_index],
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        vkCmdPipelineBarrier(frame.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_present);

        vkEndCommandBuffer(frame.cmd);
    }

    static void viewport_render_window(ImGuiViewport* viewport, void*)
    {
        viewport_data_t* vd = (viewport_data_t*)viewport->RendererUserData;
        if (!vd) { return; }

        if (!vd->valid || vd->swapchain == VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(renderer::ctx.device);
            if (!build_viewport_swapchain(*vd)) { return; }
        }

        ImDrawData* draw_data = viewport->DrawData;
        if (!draw_data || draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f) { return; }

        viewport_frame_t& frame = vd->frames[vd->frame_index];

        vkWaitForFences(renderer::ctx.device, 1, &frame.fence, VK_TRUE, UINT64_MAX);

        u32_t image_index = 0;
        VkResult res = vkAcquireNextImageKHR(renderer::ctx.device, vd->swapchain, UINT64_MAX, frame.image_available,
                                             VK_NULL_HANDLE, &image_index);
        if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)
        {
            vkDeviceWaitIdle(renderer::ctx.device);
            if (!build_viewport_swapchain(*vd)) { return; }
            res = vkAcquireNextImageKHR(renderer::ctx.device, vd->swapchain, UINT64_MAX, frame.image_available,
                                        VK_NULL_HANDLE, &image_index);
        }
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) { return; }

        vkResetFences(renderer::ctx.device, 1, &frame.fence);
        vkResetCommandPool(renderer::ctx.device, frame.pool, 0);

        record_viewport_draw(frame, *vd, draw_data, image_index);

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &frame.image_available,
            .pWaitDstStageMask = &wait_stage,
            .commandBufferCount = 1,
            .pCommandBuffers = &frame.cmd,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &vd->render_finished[image_index],
        };
        vkQueueSubmit(renderer::ctx.graphics_queue, 1, &submit, frame.fence);

        vd->acquired_image = image_index;
    }

    static void viewport_swap_buffers(ImGuiViewport* viewport, void*)
    {
        viewport_data_t* vd = (viewport_data_t*)viewport->RendererUserData;
        if (!vd || !vd->valid || vd->swapchain == VK_NULL_HANDLE) { return; }

        u32_t image_index = vd->acquired_image;
        VkPresentInfoKHR present = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &vd->render_finished[image_index],
            .swapchainCount = 1,
            .pSwapchains = &vd->swapchain,
            .pImageIndices = &image_index,
        };
        VkResult res = vkQueuePresentKHR(renderer::ctx.present_queue, &present);
        if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)
        {
            vkDeviceWaitIdle(renderer::ctx.device);
            build_viewport_swapchain(*vd);
        }

        vd->frame_index = (vd->frame_index + 1) % renderer::MAX_FRAMES_IN_FLIGHT;
    }

    void init_multiviewport()
    {
        ImGuiIO& io = ImGui::GetIO();
        io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;

        ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
        pio.Renderer_CreateWindow = viewport_create_window;
        pio.Renderer_DestroyWindow = viewport_destroy_window;
        pio.Renderer_SetWindowSize = viewport_set_window_size;
        pio.Renderer_RenderWindow = viewport_render_window;
        pio.Renderer_SwapBuffers = viewport_swap_buffers;
    }

    void init()
    {

        ctx.shader = assets::pin(assets::load<shader_t>("engine://assets/shaders/imgui.slang"));
        ctx.material = material_t(ctx.shader);

        ImGuiIO& io = ImGui::GetIO();
        u8* pixels;
        i32 width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        VkDeviceSize image_size = width * height * 4;

        VkBuffer staging_buf;
        VmaAllocation staging_alloc;
        VkBufferCreateInfo staging_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = image_size,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        };
        VmaAllocationCreateInfo alloc_info = {
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };
        VmaAllocationInfo staging_alloc_info;

        VK_CHECK(vmaCreateBuffer(renderer::ctx.allocator, &staging_info, &alloc_info, &staging_buf, &staging_alloc,
                                 &staging_alloc_info));
        std::memcpy(staging_alloc_info.pMappedData, pixels, static_cast<size_t>(image_size));

        VkImageCreateInfo image_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = {static_cast<u32_t>(width), static_cast<u32_t>(height), 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
        };
        VmaAllocationCreateInfo img_alloc_info = {.usage = VMA_MEMORY_USAGE_AUTO};

        VK_CHECK(vmaCreateImage(renderer::ctx.allocator, &image_info, &img_alloc_info, &ctx.font_image, &ctx.font_alloc,
                                nullptr));

        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = ctx.font_image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .baseMipLevel = 0,
                                 .levelCount = 1,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1},
        };

        VK_CHECK(vkCreateImageView(renderer::ctx.device, &view_info, nullptr, &ctx.font_view));

        VkCommandBuffer cmd = renderer::begin_transfer_commands();

        VkImageMemoryBarrier barrier_to_dst = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = ctx.font_image,
            .subresourceRange = view_info.subresourceRange,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier_to_dst);

        VkBufferImageCopy copy_region = {
            .bufferOffset = 0,
            .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .mipLevel = 0,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1},
            .imageExtent = image_info.extent,
        };
        vkCmdCopyBufferToImage(cmd, staging_buf, ctx.font_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);

        VkImageMemoryBarrier release_barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = 0,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = tau::renderer::ctx.queue_fam_indices.transfer_family.value(),
            .dstQueueFamilyIndex = tau::renderer::ctx.queue_fam_indices.graphics_family.value(),
            .image = ctx.font_image,
            .subresourceRange = view_info.subresourceRange,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &release_barrier);

        u64_t signal_value = renderer::submit_transfer_commands(cmd);

        {
            std::scoped_lock lock(renderer::res_system.pending_mutex);
            VkImageMemoryBarrier acquire_barrier = release_barrier;
            acquire_barrier.srcAccessMask = 0;
            acquire_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

            renderer::res_system.pending_acquires.push_back({
                .type = renderer::resource_type_e::TEXTURE,
                .handle = {.image = ctx.font_image},
                .barrier = {.image_barrier = acquire_barrier},
            });
        }

        ctx.font_tex_bindless_id = renderer::res_system.texture_heap.acquire();

        VkDescriptorImageInfo image_desc_info = {
            .sampler = VK_NULL_HANDLE,
            .imageView = ctx.font_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };

        VkWriteDescriptorSet write_desc = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = renderer::res_system.global_set,
            .dstBinding = renderer::TEXTURES_BINDING_POINT,
            .dstArrayElement = ctx.font_tex_bindless_id,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .pImageInfo = &image_desc_info,
        };
        vkUpdateDescriptorSets(renderer::ctx.device, 1, &write_desc, 0, nullptr);

        io.Fonts->SetTexID((ImTextureID)(intptr_t)ctx.font_tex_bindless_id);

        {
            std::scoped_lock lock(renderer::res_system.deletion_mutex);
            renderer::res_system.deletion_queue.push_back({
                .type = renderer::resource_type_e::BUFFER,
                .handle = {.buffer = {staging_buf, staging_alloc}},
                .bindless_id = renderer::BINDLESS_NULL_HANDLE,
                .gpu_timeline_value = signal_value,
                .clock = renderer::deletion_clock_e::TRANSFER,
            });
        }

        renderer::register_renderer_feature(
            "ImGui"_h, renderer::FEATURE_ORDER_UI,
            [](renderer::rendergraph_t& graph, ecs::registry_t&)
            {
                renderer::rg_pass_t& pass = graph.add_pass("ImGuiPass"_h, "ImGuiPass");

                pass.color_writes = {graph.get_resource("Swapchain"_h)};
                pass.texture_reads.push_back(graph.get_resource("FinalOutput"_h));

                pass.execute_callback = [](VkCommandBuffer cmd, ecs::registry_t&)
                {
                    if (!ctx.draw_data || ctx.draw_data->CmdListsCount == 0) { return; }

                    if (ctx.draw_data->DisplaySize.x <= 0.0f || ctx.draw_data->DisplaySize.y <= 0.0f) { return; }

                    ImDrawData* draw_data = ctx.draw_data;
                    u32_t cur_frame = renderer::ctx.cur_frame;

                    size_t vertex_size = draw_data->TotalVtxCount * sizeof(ImDrawVert);
                    size_t index_size = draw_data->TotalIdxCount * sizeof(u32_t);

                    if (ctx.vertex_buffer_size[cur_frame] < vertex_size)
                    {
                        if (ctx.vertex_buffer[cur_frame])
                        {
                            vmaDestroyBuffer(renderer::ctx.allocator, ctx.vertex_buffer[cur_frame],
                                             ctx.vertex_alloc[cur_frame]);
                        }
                        VkBufferCreateInfo buf_info = {
                            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                            .size = vertex_size + 5000,
                            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                        };
                        VmaAllocationCreateInfo alloc_info = {
                            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                     VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                            .usage = VMA_MEMORY_USAGE_AUTO,
                            .requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        };
                        VmaAllocationInfo vma_alloc_info;
                        VK_CHECK(vmaCreateBuffer(renderer::ctx.allocator, &buf_info, &alloc_info,
                                                 &ctx.vertex_buffer[cur_frame], &ctx.vertex_alloc[cur_frame],
                                                 &vma_alloc_info));
                        ctx.vertex_mapped[cur_frame] = vma_alloc_info.pMappedData;
                        ctx.vertex_buffer_size[cur_frame] = vertex_size + 5000;
                    }

                    if (ctx.index_buffer_size[cur_frame] < index_size)
                    {
                        if (ctx.index_buffer[cur_frame])
                        {
                            vmaDestroyBuffer(renderer::ctx.allocator, ctx.index_buffer[cur_frame],
                                             ctx.index_alloc[cur_frame]);
                        }
                        VkBufferCreateInfo buf_info = {
                            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                            .size = index_size + 5000,
                            .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                        };
                        VmaAllocationCreateInfo alloc_info = {
                            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                     VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                            .usage = VMA_MEMORY_USAGE_AUTO,
                            .requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        };
                        VmaAllocationInfo vma_alloc_info;
                        VK_CHECK(vmaCreateBuffer(renderer::ctx.allocator, &buf_info, &alloc_info,
                                                 &ctx.index_buffer[cur_frame], &ctx.index_alloc[cur_frame],
                                                 &vma_alloc_info));
                        ctx.index_mapped[cur_frame] = vma_alloc_info.pMappedData;
                        ctx.index_buffer_size[cur_frame] = index_size + 5000;
                    }

                    ImDrawVert* vtx_dst = (ImDrawVert*)ctx.vertex_mapped[cur_frame];
                    u32_t* idx_dst = (u32_t*)ctx.index_mapped[cur_frame];

                    i32 global_vtx_offset = 0;

                    for (i32 i = 0; i < draw_data->CmdListsCount; i++)
                    {
                        const ImDrawList* cmd_list = draw_data->CmdLists[i];
                        std::memcpy(vtx_dst, cmd_list->VtxBuffer.Data, cmd_list->VtxBuffer.Size * sizeof(ImDrawVert));

                        for (i32 j = 0; j < cmd_list->IdxBuffer.Size; j++)
                        {
                            idx_dst[j] = (u32_t)(cmd_list->IdxBuffer.Data[j]) + global_vtx_offset;
                        }

                        vtx_dst += cmd_list->VtxBuffer.Size;
                        idx_dst += cmd_list->IdxBuffer.Size;
                        global_vtx_offset += cmd_list->VtxBuffer.Size;
                    }

                    vec2_t scale = {
                        2.0f / draw_data->DisplaySize.x,
                        2.0f / draw_data->DisplaySize.y,
                    };
                    vec2_t translate = {
                        -1.0f - draw_data->DisplayPos.x * scale.x,
                        -1.0f - draw_data->DisplayPos.y * scale.y,
                    };

                    material_t* mat = &ctx.material;

                    mat->set_property("scale"_h, scale);
                    mat->set_property("translate"_h, translate);
                    mat->set_property("vertex_buffer"_h, renderer::get_buffer_address(ctx.vertex_buffer[cur_frame]));
                    mat->sync();

                    shader_t* shader = assets::get(mat->shader);

                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      shader->get_pipeline(pipeline_variant_e::FORWARD));

                    VkDescriptorSet sets[] = {renderer::res_system.global_set, renderer::res_system.frame_set};

                    vkCmdBindDescriptorSets(
                        cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shader->pipeline_layout, 0, 2, sets, 1,
                        &renderer::ctx.per_frame_objects[renderer::ctx.cur_frame].global_data_offset);

                    vkCmdBindIndexBuffer(cmd, ctx.index_buffer[cur_frame], 0, VK_INDEX_TYPE_UINT32);

                    renderer::push_constants_t pc = {
                        .material_buffer = renderer::res_system.material_heap.device_address,
                        .custom_data = mat->heap_offset[cur_frame],
                    };

                    VkViewport viewport = {0, 0, draw_data->DisplaySize.x, draw_data->DisplaySize.y, 0.0f, 1.0f};
                    vkCmdSetViewport(cmd, 0, 1, &viewport);

                    i32 global_index_offset = 0;
                    ImVec2 clip_off = draw_data->DisplayPos;

                    for (i32 i = 0; i < draw_data->CmdListsCount; i++)
                    {
                        const ImDrawList* cmd_list = draw_data->CmdLists[i];
                        for (i32 cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
                        {
                            const ImDrawCmd* cmd_ptr = &cmd_list->CmdBuffer[cmd_i];

                            ImVec2 clip_min(cmd_ptr->ClipRect.x - clip_off.x, cmd_ptr->ClipRect.y - clip_off.y);
                            ImVec2 clip_max(cmd_ptr->ClipRect.z - clip_off.x, cmd_ptr->ClipRect.w - clip_off.y);

                            clip_min.x = std::max(0.0f, clip_min.x);
                            clip_min.y = std::max(0.0f, clip_min.y);

                            float max_w = (f32)renderer::ctx.swapchain.extent.width;
                            float max_h = (f32)renderer::ctx.swapchain.extent.height;

                            clip_max.x = std::min(max_w, clip_max.x);
                            clip_max.y = std::min(max_h, clip_max.y);

                            if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y) { continue; }

                            VkRect2D scissor = {
                                {(i32_t)clip_min.x,                (i32_t)clip_min.y               },
                                {(u32_t)(clip_max.x - clip_min.x), (u32_t)(clip_max.y - clip_min.y)}
                            };
                            vkCmdSetScissor(cmd, 0, 1, &scissor);

                            pc.texture_id = (u32_t)(intptr_t)cmd_ptr->GetTexID();
                            vkCmdPushConstants(cmd, shader->pipeline_layout,
                                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                               sizeof(renderer::push_constants_t), &pc);
                            vkCmdDrawIndexed(cmd, cmd_ptr->ElemCount, 1, cmd_ptr->IdxOffset + global_index_offset, 0,
                                             0);
                        }

                        global_index_offset += cmd_list->IdxBuffer.Size;
                    }
                };
            });
    }

    void shutdown()
    {
        for (size_t i = 0; i < renderer::MAX_FRAMES_IN_FLIGHT; i++)
        {
            if (ctx.vertex_buffer[i])
            {
                vmaDestroyBuffer(renderer::ctx.allocator, ctx.vertex_buffer[i], ctx.vertex_alloc[i]);
            }

            if (ctx.index_buffer[i])
            {
                vmaDestroyBuffer(renderer::ctx.allocator, ctx.index_buffer[i], ctx.index_alloc[i]);
            }
        }

        if (ctx.font_view) { vkDestroyImageView(renderer::ctx.device, ctx.font_view, nullptr); }
        if (ctx.font_image) { vmaDestroyImage(renderer::ctx.allocator, ctx.font_image, ctx.font_alloc); }

        if (ctx.font_tex_bindless_id != renderer::BINDLESS_NULL_HANDLE)
        {
            renderer::res_system.texture_heap.release(ctx.font_tex_bindless_id);
        }

        ctx.material.release_heap();
        assets::unpin(ctx.shader);
    }

    void submit(ImDrawData* draw_data) { ctx.draw_data = draw_data; }
} // namespace tau::editor::imgui