#include "frame.h"

#include "tau/log.h"
#include "tau/os.h"
#include "tau/render/post.h"
#include "tau/rhi/renderer.h"
#include "tau/rhi/renderer_constants.h"
#include "tau/rhi/renderer_types.h"

#include <algorithm>
#include <string_view>
#include <vector>

namespace tau::renderer::frame
{
    namespace
    {
        struct producer_t
        {
            u32_t name_hash;
            i32 order;
            producer_fn_t fn;
        };

        std::vector<producer_t> g_producers;
        graph::graph_cache_t* g_cache = nullptr;
        graph::graph_t g_graph;
        bool g_failed_reported = false;
    } // namespace

    void register_producer(u32_t name_hash, i32 order, producer_fn_t fn)
    {
        for (producer_t& p : g_producers)
        {
            if (p.name_hash == name_hash)
            {
                p.order = order;
                p.fn = fn;
                return;
            }
        }
        g_producers.push_back({name_hash, order, fn});
        std::sort(g_producers.begin(), g_producers.end(), [](const producer_t& a, const producer_t& b)
                  { return a.order != b.order ? a.order < b.order : a.name_hash < b.name_hash; });
    }

    u32_t remove_producers_of(void* module_base)
    {
        const auto first =
            std::remove_if(g_producers.begin(), g_producers.end(), [module_base](const producer_t& p)
                           { return os::module_base_of(reinterpret_cast<const void*>(p.fn)) == module_base; });
        const u32_t removed = static_cast<u32_t>(g_producers.end() - first);
        g_producers.erase(first, g_producers.end());
        return removed;
    }

    u32_t count_producers_of(void* module_base)
    {
        return static_cast<u32_t>(
            std::count_if(g_producers.begin(), g_producers.end(), [module_base](const producer_t& p)
                          { return os::module_base_of(reinterpret_cast<const void*>(p.fn)) == module_base; }));
    }

    void init(void* tracy_vk_ctx)
    {
        g_cache = graph::create_cache();
        graph::set_profiler(g_cache, tracy_vk_ctx);
    }

    void shutdown()
    {
        graph::destroy_cache(g_cache);
        g_cache = nullptr;
    }

    void record(VkCommandBuffer cmd, ecs::registry_t& reg, const inputs_t& in)
    {
        graph::graph_t& g = g_graph;
        graph::begin(g, g_cache, ctx.cur_frame);

        targets_t t;
        t.primary_view_hash = in.post_view_hash;
        for (u32_t i = 0; i < in.legacy_count; i++)
        {
            const legacy_image_t& l = in.legacy[i];
            const graph::image_h h = graph::import_image(
                g, l.name, l.image, l.view, {.format = l.format, .width = l.extent.width, .height = l.extent.height},
                l.layout, VK_IMAGE_LAYOUT_UNDEFINED, l.sampled_id);
            if (std::string_view(l.name) == "SceneColor") { t.scene_color = h; }
            else if (std::string_view(l.name) == "SceneDepth") { t.scene_depth = h; }
        }

        const VkClearValue grey = {.color = {{0.01f, 0.01f, 0.01f, 1.0f}}};
        if (in.swapchain != VK_NULL_HANDLE)
        {
            t.swapchain = graph::import_image(g, "Swapchain", in.swapchain, in.swapchain_view,
                                              {.format = ctx.swapchain.format,
                                               .width = ctx.swapchain.extent.width,
                                               .height = ctx.swapchain.extent.height,
                                               .clear = grey},
                                              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        }
        if (in.to_swapchain && t.swapchain.valid())
        {
            t.final_output = t.swapchain;
            t.final_is_swapchain = true;
        }
        else
        {
            t.final_output = graph::persistent_image(g, "OutputTarget",
                                                     {.format = ctx.swapchain.format,
                                                      .width = in.output_extent.width,
                                                      .height = in.output_extent.height,
                                                      .clear = grey},
                                                     in.output_hash);
        }

        size_t i = 0;
        for (; i < g_producers.size() && g_producers[i].order < FEATURE_ORDER_AFTER_POST; i++)
        {
            g_producers[i].fn(g, t, reg);
        }
        post::build(g, t, reg, in.post_view_hash, in.post_view_pos, in.post_enabled);
        for (; i < g_producers.size(); i++) { g_producers[i].fn(g, t, reg); }

        if (!graph::compile(g))
        {
            if (!g_failed_reported) { TAU_LOG_ERROR("RENDERER", "The frame graph did not compile, see above"); }
            g_failed_reported = true;
            return;
        }

        ctx.output_texture_ids.clear();
        if (!t.final_is_swapchain) { ctx.output_texture_ids[in.output_hash] = graph::sampled_id(g, t.final_output); }

        graph::execute(g, cmd);
    }
} // namespace tau::renderer::frame
