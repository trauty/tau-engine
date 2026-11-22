#pragma once

#include "tau/defines.h"
#include "tau/rhi/vulkan.h"

#include <cstring>
#include <type_traits>
#include <vector>

// render graph: resources and passes as data, compiled per frame
// passes run in declaration order; the compile step culls unused ones, derives barriers and layouts, aliases transients
namespace tau::renderer::graph
{
    constexpr u32_t NONE = 0xffffffff;

    struct image_h
    {
        u32_t index = NONE;
        bool valid() const { return index != NONE; }
    };

    struct buffer_h
    {
        u32_t index = NONE;
        bool valid() const { return index != NONE; }
    };

    struct pass_h
    { u32_t index = NONE; };

    struct image_desc_t
    {
        VkFormat format = VK_FORMAT_UNDEFINED;
        // fixed size, or relative to a named extent (set_extent) when extent_hash != 0
        u32_t width = 0;
        u32_t height = 0;
        u32_t extent_hash = 0;
        f32 scale = 1.0f;
        u32_t depth = 1;
        u32_t mips = 1;
        u32_t layers = 1;
        VkClearValue clear = {};
    };

    struct buffer_desc_t
    { VkDeviceSize size = 0; };

    enum class access_e : u8_t
    {
        COLOR_WRITE,
        // blending onto what is there
        COLOR_READ_WRITE,
        DEPTH_WRITE,
        DEPTH_READ,
        SAMPLED,
        STORAGE_READ,
        STORAGE_WRITE,
        STORAGE_READ_WRITE,
        TRANSFER_SRC,
        TRANSFER_DST,
        // buffers only
        UNIFORM,
        INDIRECT,
        INDEX,
    };

    enum class load_e : u8_t
    {
        LOAD,
        CLEAR,
        DONT_CARE,
    };

    enum class queue_e : u8_t
    {
        GRAPHICS,
    };

    enum pass_flags_e : u8_t
    {
        PASS_NONE = 0,
        // never culled (readbacks, captures)
        PASS_SIDE_EFFECT = 1 << 0,
    };

    struct graph_t;

    struct record_ctx_t
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        const graph_t* graph = nullptr;
        // attachment size for raster passes
        VkRect2D area = {};
    };

    using record_fn_t = void (*)(const record_ctx_t& ctx, const void* data);

    // per graph instance across frames: heaps, cached images, persistent resources
    struct graph_cache_t;

    struct graph_t
    {
        graph_cache_t* cache = nullptr;
        u32_t frame_slot = 0;

        struct extent_t
        {
            u32_t hash;
            VkExtent2D extent;
        };

        struct image_t
        {
            const char* name = nullptr;
            image_desc_t desc;
            u32_t kind = 0;
            u32_t persistent_hash = 0;
            VkImage image = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            u32_t sampled_id = NONE;
            u32_t storage_id = NONE;
            VkImageLayout initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkImageLayout final_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkImageUsageFlags usage = 0;
            VkImageAspectFlags aspect = 0;
            u32_t first = NONE, last = NONE;
            u32_t alias_prev = NONE;
            VkDeviceSize offset = 0;
        };

        struct buffer_t
        {
            const char* name = nullptr;
            buffer_desc_t desc;
            u32_t kind = 0;
            u32_t persistent_hash = 0;
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceAddress address = 0;
            VkBufferUsageFlags usage = 0;
            u32_t first = NONE, last = NONE;
            u32_t alias_prev = NONE;
            VkDeviceSize offset = 0;
        };

        struct use_t
        {
            u32_t pass;
            u32_t resource;
            bool is_image;
            access_e access;
            load_e load;
            VkPipelineStageFlags2 stages;
        };

        struct pass_t
        {
            const char* name = nullptr;
            queue_e queue = queue_e::GRAPHICS;
            u8_t flags = PASS_NONE;
            record_fn_t record = nullptr;
            u32_t data_offset = NONE;
            VkRect2D area = {};
            bool needed = false;
            u32_t first_barrier = 0, barrier_count = 0;
            u32_t first_image_barrier = 0, image_barrier_count = 0;
        };

        std::vector<extent_t> extents;
        std::vector<image_t> images;
        std::vector<buffer_t> buffers;
        std::vector<pass_t> passes;
        std::vector<use_t> uses;
        std::vector<u8_t> data;

        std::vector<VkMemoryBarrier2> memory_barriers;
        std::vector<VkImageMemoryBarrier2> image_barriers;
        std::vector<VkImageMemoryBarrier2> final_barriers;

        bool compiled = false;
        bool failed = false;
    };

    TAU_ENGINE_API graph_cache_t* create_cache();
    TAU_ENGINE_API void destroy_cache(graph_cache_t* cache);
    // Tracy GPU zone per pass (TracyVkCtx), null without profiling
    TAU_ENGINE_API void set_profiler(graph_cache_t* cache, void* tracy_vk_ctx);

    // frame_slot: the frame in flight, transients of one slot alias only among themselves
    TAU_ENGINE_API void begin(graph_t& g, graph_cache_t* cache, u32_t frame_slot);

    TAU_ENGINE_API void set_extent(graph_t& g, u32_t hash, VkExtent2D extent);

    TAU_ENGINE_API image_h create_image(graph_t& g, const char* name, const image_desc_t& desc);
    // kept across frames by name, contents and layout survive; a changed desc starts it over
    TAU_ENGINE_API image_h persistent_image(graph_t& g, const char* name, const image_desc_t& desc);
    // final_layout UNDEFINED: left as the last pass used it
    TAU_ENGINE_API image_h import_image(graph_t& g, const char* name, VkImage image, VkImageView view,
                                        const image_desc_t& desc, VkImageLayout layout, VkImageLayout final_layout,
                                        u32_t sampled_id = NONE);

    TAU_ENGINE_API buffer_h create_buffer(graph_t& g, const char* name, const buffer_desc_t& desc);
    TAU_ENGINE_API buffer_h persistent_buffer(graph_t& g, const char* name, const buffer_desc_t& desc);
    TAU_ENGINE_API buffer_h import_buffer(graph_t& g, const char* name, VkBuffer buffer, VkDeviceSize size,
                                          VkDeviceAddress address = 0);

    TAU_ENGINE_API pass_h add_pass(graph_t& g, const char* name, record_fn_t record, const void* data = nullptr,
                                   u32_t data_size = 0, u8_t flags = PASS_NONE);

    // data copied into the graph, alive until the next begin()
    template <typename T>
    pass_h add_pass(graph_t& g, const char* name, record_fn_t record, const T& data, u8_t flags = PASS_NONE)
    {
        static_assert(std::is_trivially_copyable_v<T>, "pass data is copied bytewise");
        return add_pass(g, name, record, &data, static_cast<u32_t>(sizeof(T)), flags);
    }

    // stages 0: derived from the access (and the pass kind)
    TAU_ENGINE_API void use(graph_t& g, pass_h pass, image_h image, access_e access, load_e load = load_e::LOAD,
                            VkPipelineStageFlags2 stages = 0);
    TAU_ENGINE_API void use(graph_t& g, pass_h pass, buffer_h buffer, access_e access,
                            VkPipelineStageFlags2 stages = 0);
    // default: the size of the first attachment
    TAU_ENGINE_API void set_area(graph_t& g, pass_h pass, VkRect2D area);

    TAU_ENGINE_API image_h find_image(const graph_t& g, const char* name);

    // false: a declaration error, logged; nothing runs
    TAU_ENGINE_API bool compile(graph_t& g);
    TAU_ENGINE_API void execute(graph_t& g, VkCommandBuffer cmd);

    TAU_ENGINE_API VkImage image(const graph_t& g, image_h h);
    TAU_ENGINE_API VkImageView view(const graph_t& g, image_h h);
    TAU_ENGINE_API VkExtent2D extent(const graph_t& g, image_h h);
    TAU_ENGINE_API VkFormat format(const graph_t& g, image_h h);
    TAU_ENGINE_API u32_t sampled_id(const graph_t& g, image_h h);
    TAU_ENGINE_API u32_t storage_id(const graph_t& g, image_h h);
    TAU_ENGINE_API VkBuffer buffer(const graph_t& g, buffer_h h);
    TAU_ENGINE_API VkDeviceAddress address(const graph_t& g, buffer_h h);

    struct stats_t
    {
        u32_t passes = 0;
        u32_t culled = 0;
        u32_t barriers = 0;
        u32_t transient_images = 0;
        // bytes the transients would take unaliased, and the heap they took
        VkDeviceSize transient_bytes = 0;
        VkDeviceSize heap_bytes = 0;
        u32_t images_created = 0;
        f64 compile_ms = 0.0;
    };
    TAU_ENGINE_API stats_t stats(const graph_t& g);
} // namespace tau::renderer::graph
