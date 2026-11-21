#include "rendergraph.h"

#include "tau/defines.h"
#include "tau/log.h"
#include "tau/profiling.h"
#include "tau/rendering/gpu_timers.h"
#include "tau/rendering/renderer.h"
#include "tau/rendering/renderer_constants.h"
#include "tau/rendering/renderer_types.h"
#include "tau/rendering/vulkan.h"
#include "vulkan/vulkan_core.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <queue>
#include <string>
#ifdef TAU_ENABLE_PROFILING
    #include <tracy/TracyVulkan.hpp>
#endif

#include <vector>

namespace tau::renderer
{
    void rendergraph_t::clear()
    {
        active_resource_count = 0;
        active_pass_count = 0;

        sorted_passes.clear();
        aliases.clear();
    }

    rg_resource_id rendergraph_t::create_image(u32_t name_hash, const char* debug_name, const image_desc_t& desc)
    {
        if (active_resource_count >= resources.size()) { resources.push_back({}); }

        rg_resource_id id = active_resource_count++;
        rg_resource_t& res = resources[id];

        res.name_hash = name_hash;
        res.debug_name = debug_name;
        res.desc = desc;
        res.image = VK_NULL_HANDLE;
        res.view = VK_NULL_HANDLE;
        res.bindless_id = BINDLESS_NULL_HANDLE;
        res.cur_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        res.is_imported = false;

        return id;
    }

    rg_resource_id rendergraph_t::import_image(u32_t name_hash, const char* debug_name, VkImage image, VkImageView view,
                                               VkFormat format, u32_t width, u32_t height)
    {
        if (active_resource_count >= resources.size()) { resources.push_back({}); }

        rg_resource_id id = active_resource_count++;
        rg_resource_t& res = resources[id];

        res.name_hash = name_hash;
        res.debug_name = debug_name;
        res.desc = {width, height, format, 0, VK_IMAGE_ASPECT_COLOR_BIT};
        res.image = image;
        res.view = view;
        res.bindless_id = BINDLESS_NULL_HANDLE;
        res.cur_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        res.is_imported = true;

        return id;
    }

    rg_resource_id rendergraph_t::import_persistent_image(u32_t name_hash, const char* debug_name, VkImage image,
                                                          VkImageView view, const image_desc_t& desc, u32_t bindless_id,
                                                          VkImageLayout cur_layout)
    {
        if (active_resource_count >= resources.size()) { resources.push_back({}); }

        rg_resource_id id = active_resource_count++;
        rg_resource_t& res = resources[id];

        res.name_hash = name_hash;
        res.debug_name = debug_name;
        res.desc = desc;
        res.image = image;
        res.view = view;
        res.bindless_id = bindless_id;
        res.cur_layout = cur_layout;
        res.is_imported = true;

        return id;
    }

    rg_pass_t& rendergraph_t::add_pass(u32_t name_hash, const char* debug_name)
    {
        if (active_pass_count >= passes.size()) { passes.push_back({}); }

        rg_pass_t& pass = passes[active_pass_count++];

        pass.name_hash = name_hash;
        pass.debug_name = debug_name;
        pass.color_writes.clear();
        pass.storage_writes.clear();
        pass.texture_reads.clear();
        pass.depth_stencil = RG_NULL_ID;
        pass.render_rect = {};
        pass.force_depth_clear = false;
        pass.execute_callback = {};

        return pass;
    }

    void rendergraph_t::compile(per_frame_t& frame_data)
    {
        for (size_t i = 0; i < active_pass_count; i++)
        {
            const rg_pass_t& pass = passes[i];

            for (rg_resource_id id : pass.color_writes)
            {
                if (id == RG_NULL_ID)
                {
                    TAU_LOG_FATAL("RENDERGRAPH", "Pass '{}' has invalid color write", pass.debug_name);
                }
            }

            for (rg_resource_id id : pass.texture_reads)
            {
                if (id == RG_NULL_ID)
                {
                    TAU_LOG_FATAL("RENDERGRAPH", "Pass '{}' has invalid texture read", pass.debug_name);
                }
            }
        }

        {
            std::vector<size_t> first_writer(active_resource_count, SIZE_MAX);
            for (size_t i = active_pass_count; i-- > 0;)
            {
                const rg_pass_t& pass = passes[i];
                for (rg_resource_id res : pass.color_writes) { first_writer[res] = i; }
                for (rg_resource_id res : pass.storage_writes) { first_writer[res] = i; }
                if (pass.depth_stencil != RG_NULL_ID) { first_writer[pass.depth_stencil] = i; }
            }

            for (size_t i = 0; i < active_pass_count; i++)
            {
                const rg_pass_t& pass = passes[i];
                for (rg_resource_id res : pass.texture_reads)
                {
                    const size_t writer = first_writer[res];
                    if (writer == SIZE_MAX || writer <= i) { continue; }

                    const rg_pass_t& writer_pass = passes[writer];
                    const u64_t key = (static_cast<u64_t>(pass.name_hash) * 0x9e3779b97f4a7c15ull) ^
                                      (static_cast<u64_t>(writer_pass.name_hash) * 0xc2b2ae3d27d4eb4full) ^
                                      static_cast<u64_t>(resources[res].name_hash);
                    if (std::find(reported_hazards.begin(), reported_hazards.end(), key) != reported_hazards.end())
                    {
                        continue;
                    }
                    reported_hazards.push_back(key);

                    TAU_LOG_ERROR("RENDERGRAPH",
                                  "Pass '{}' reads '{}' but every writer is declared later (first is '{}'). "
                                  "It will sample last frame's contents. Declare '{}' before '{}'.",
                                  pass.debug_name ? pass.debug_name : "<unnamed>",
                                  resources[res].debug_name ? resources[res].debug_name : "<unnamed>",
                                  writer_pass.debug_name ? writer_pass.debug_name : "<unnamed>",
                                  writer_pass.debug_name ? writer_pass.debug_name : "<unnamed>",
                                  pass.debug_name ? pass.debug_name : "<unnamed>");
                }
            }
        }

        std::vector<std::vector<size_t>> adjacency_list(active_pass_count);
        std::vector<size_t> in_degree(active_pass_count, 0);
        std::vector<size_t> latest_writer(active_resource_count, SIZE_MAX);

        for (size_t i = 0; i < active_pass_count; i++)
        {
            rg_pass_t& pass = passes[i];

            auto add_edge_from_writer = [&](rg_resource_id res)
            {
                const size_t writer_idx = latest_writer[res];
                if (writer_idx == SIZE_MAX || writer_idx == i) { return; }

                auto& adj = adjacency_list[writer_idx];
                if (std::find(adj.begin(), adj.end(), i) == adj.end())
                {
                    adj.push_back(i);
                    in_degree[i]++;
                }
            };

            for (rg_resource_id res : pass.texture_reads) { add_edge_from_writer(res); }

            auto add_dep = [&](rg_resource_id res)
            {
                add_edge_from_writer(res);
                latest_writer[res] = i;
            };

            for (rg_resource_id res : pass.color_writes) { add_dep(res); }
            for (rg_resource_id res : pass.storage_writes) { add_dep(res); }
            if (pass.depth_stencil != RG_NULL_ID) { add_dep(pass.depth_stencil); }
        }

        std::queue<size_t> queue;
        for (size_t i = 0; i < active_pass_count; i++)
        {
            if (in_degree[i] == 0) { queue.push(i); }
        }

        sorted_passes.clear();
        while (!queue.empty())
        {
            size_t cur_idx = queue.front();
            queue.pop();
            sorted_passes.push_back(cur_idx);

            for (size_t neighbor_idx : adjacency_list[cur_idx])
            {
                in_degree[neighbor_idx]--;
                if (in_degree[neighbor_idx] == 0) { queue.push(neighbor_idx); }
            }
        }

        if (sorted_passes.size() != active_pass_count)
        {
            TAU_LOG_FATAL("RENDERGRAPH", "Circular dependency detected");
            abort();
        }

        for (size_t i = 0; i < active_resource_count; i++)
        {
            rg_resource_t& res = resources[i];
            if (res.is_imported) { continue; }

            transient_image_t* transient = frame_data.transient_pool.acquire(res.desc);
            res.image = transient->image;
            res.view = transient->view;
            res.bindless_id = transient->bindless_id;
        }
    }

    void rendergraph_t::execute(VkCommandBuffer cmd, ecs::registry_t& reg)
    {
        for (size_t pass_idx : sorted_passes)
        {
            const rg_pass_t& pass = passes[pass_idx];
#ifdef TAU_ENABLE_PROFILING
            const char* rg_zone_name = pass.debug_name ? pass.debug_name : "rg_pass";
            TracyVkZoneTransient(static_cast<TracyVkCtx>(profiling_vk_ctx), rg_vk_zone, cmd, rg_zone_name, true);
#endif
            gpu_timers::begin_pass(cmd, pass.debug_name);
            bool is_compute = pass.color_writes.empty() && pass.depth_stencil == RG_NULL_ID;

            for (rg_resource_id id : pass.storage_writes)
            {
                rg_resource_t& res = resources[id];
                if (res.cur_layout != VK_IMAGE_LAYOUT_GENERAL)
                {
                    transition_image(cmd, res.image, res.cur_layout, VK_IMAGE_LAYOUT_GENERAL, res.desc.aspect);
                    res.cur_layout = VK_IMAGE_LAYOUT_GENERAL;
                }
            }

            std::vector<bool> color_needs_clear(pass.color_writes.size(), false);
            for (size_t i = 0; i < pass.color_writes.size(); i++)
            {
                rg_resource_t& res = resources[pass.color_writes[i]];

                if (res.cur_layout == VK_IMAGE_LAYOUT_UNDEFINED) { color_needs_clear[i] = true; }

                if (res.cur_layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
                {
                    transition_image(cmd, res.image, res.cur_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     res.desc.aspect);
                    res.cur_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                }
            }

            bool depth_needs_clear = false;
            if (pass.depth_stencil != RG_NULL_ID)
            {
                rg_resource_t& res = resources[pass.depth_stencil];

                if (res.cur_layout == VK_IMAGE_LAYOUT_UNDEFINED) { depth_needs_clear = true; }

                if (res.cur_layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
                {
                    transition_image(cmd, res.image, res.cur_layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                     res.desc.aspect);
                    res.cur_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                }
            }

            for (rg_resource_id id : pass.texture_reads)
            {
                rg_resource_t& res = resources[id];
                if (res.cur_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                {
                    transition_image(cmd, res.image, res.cur_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     res.desc.aspect);
                    res.cur_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                }
            }

            if (!is_compute)
            {
                std::vector<VkRenderingAttachmentInfo> color_attachments;
                u32_t render_width = 0;
                u32_t render_height = 0;

                for (size_t i = 0; i < pass.color_writes.size(); i++)
                {
                    rg_resource_t& res = resources[pass.color_writes[i]];
                    render_width = res.desc.width;
                    render_height = res.desc.height;

                    VkClearValue clear_color = {.color = res.desc.color_clear};

                    color_attachments.push_back({
                        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                        .imageView = res.view,
                        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        .loadOp = color_needs_clear[i] ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
                        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                        .clearValue = clear_color,
                    });
                }

                VkRenderingAttachmentInfo depth_attachment = {};
                if (pass.depth_stencil != RG_NULL_ID)
                {
                    rg_resource_t& res = resources[pass.depth_stencil];
                    render_width = res.desc.width;
                    render_height = res.desc.height;

                    VkClearValue clear_depth = {
                        .depthStencil = {res.desc.depth_clear, 0}
                    };

                    depth_attachment = {
                        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                        .imageView = res.view,
                        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                        .loadOp = (depth_needs_clear || pass.force_depth_clear) ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                                                : VK_ATTACHMENT_LOAD_OP_LOAD,
                        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                        .clearValue = clear_depth,
                    };
                }

                VkRect2D render_area = {
                    .offset = {0,            0            },
                      .extent = {render_width, render_height}
                };
                if (pass.render_rect.extent.width > 0 && pass.render_rect.extent.height > 0)
                {
                    render_area = pass.render_rect;
                }

                VkRenderingInfo rendering_info = {
                    .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                    .renderArea = render_area,
                    .layerCount = 1,
                    .colorAttachmentCount = static_cast<u32_t>(color_attachments.size()),
                    .pColorAttachments = color_attachments.empty() ? nullptr : color_attachments.data(),
                    .pDepthAttachment = (pass.depth_stencil != RG_NULL_ID) ? &depth_attachment : nullptr,
                };

                vkCmdBeginRendering(cmd, &rendering_info);

                VkViewport vp = {
                    .x = static_cast<f32>(render_area.offset.x),
                    .y = static_cast<f32>(render_area.offset.y),
                    .width = static_cast<f32>(render_area.extent.width),
                    .height = static_cast<f32>(render_area.extent.height),
                    .minDepth = 0.0f,
                    .maxDepth = 1.0f,
                };
                vkCmdSetViewport(cmd, 0, 1, &vp);

                vkCmdSetScissor(cmd, 0, 1, &render_area);

                if (pass.execute_callback) { pass.execute_callback(cmd, reg); }

                vkCmdEndRendering(cmd);
            }
            else
            {
                if (pass.execute_callback) { pass.execute_callback(cmd, reg); }
            }

            gpu_timers::end_pass(cmd);
        }
    }

    rg_resource_id rendergraph_t::get_resource(u32_t name_hash) const
    {
        u32_t search_hash = name_hash;

        for (const rg_alias_t& alias : aliases)
        {
            if (alias.alias_hash == search_hash)
            {
                search_hash = alias.target_hash;
                break;
            }
        }

        for (u32_t i = 0; i < active_resource_count; i++)
        {
            if (resources[i].name_hash == search_hash) { return i; }
        }

        return RG_NULL_ID;
    }

    VkImageLayout rendergraph_t::get_layout(rg_resource_id id) const { return resources[id].cur_layout; }

    VkFormat rendergraph_t::get_format(rg_resource_id id) const { return resources[id].desc.format; }

    VkImage rendergraph_t::get_image(rg_resource_id id) const { return resources[id].image; }

    u32_t rendergraph_t::get_bindless_id(rg_resource_id id) const { return resources[id].bindless_id; }

    void rendergraph_t::add_alias(u32_t alias_name, u32_t target_name) { aliases.push_back({alias_name, target_name}); }
} // namespace tau::renderer