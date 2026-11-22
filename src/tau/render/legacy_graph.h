#pragma once

#include "tau/defines.h"
#include "tau/ecs_fwd.h"
#include "tau/memory/arena_function.h"
#include "tau/rhi/bindless.h"
#include "tau/rhi/vulkan.h"

#include <deque>
#include <vector>

// the legacy block's graph, deleted with it in part 3
namespace tau::renderer
{
    struct image_desc_t
    {
        u32_t width;
        u32_t height;
        VkFormat format;
        VkImageUsageFlags usage;
        VkImageAspectFlags aspect;
        f32 depth_clear = 0.0f;
        VkClearColorValue color_clear = {
            {0.01f, 0.01f, 0.01f, 1.0f}
        };

        bool operator==(const image_desc_t& other) const
        {
            return width == other.width && height == other.height && format == other.format && usage == other.usage &&
                   aspect == other.aspect;
        }
    };

    struct transient_image_t
    {
        image_desc_t desc;
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        u32_t bindless_id = BINDLESS_NULL_HANDLE;
        bindless_h bindless;
        bool in_use = false;
        u32_t frames_unused = 0;
        const char* debug_name = nullptr;
    };

    struct persistent_image_t
    {
        image_desc_t desc{};
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        u32_t bindless_id = BINDLESS_NULL_HANDLE;
        bindless_h bindless;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        bool needs_full_clear = true;
    };

    struct transient_pool_t
    {
        std::deque<transient_image_t> images;

        transient_image_t* acquire(const image_desc_t& desc);
        void reset();
        void shutdown();
    };

    using rg_resource_id = u32_t;
    constexpr rg_resource_id RG_NULL_ID = 0xffffffff;

    struct TAU_ENGINE_API rg_resource_t
    {
        u32_t name_hash;
        const char* debug_name = nullptr;

        image_desc_t desc;

        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        u32_t bindless_id = BINDLESS_NULL_HANDLE;

        VkImageLayout cur_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        bool is_imported = false;
    };

    using pass_callback_t = tau::arena_function<void(VkCommandBuffer, ecs::registry_t&)>;

    struct TAU_ENGINE_API rg_pass_t
    {
        u32_t name_hash;
        const char* debug_name = nullptr;

        std::vector<rg_resource_id> color_writes;
        std::vector<rg_resource_id> storage_writes;
        rg_resource_id depth_stencil = RG_NULL_ID;
        std::vector<rg_resource_id> texture_reads;

        VkRect2D render_rect = {};
        bool force_depth_clear = false;

        pass_callback_t execute_callback;
    };

    struct rg_alias_t
    {
        u32_t alias_hash;
        u32_t target_hash;
    };

    class TAU_ENGINE_API rendergraph_t
    {
      public:
        void clear();

        rg_resource_id create_image(u32_t name_hash, const char* debug_name, const image_desc_t& desc);
        rg_resource_id import_image(u32_t name_hash, const char* debug_name, VkImage image, VkImageView view,
                                    VkFormat format, u32_t width, u32_t height);

        // an image the renderer owns across frames, not one the graph hands out. its layout must come in from outside
        // or the graph assumes UNDEFINED and the first transition discards the contents being kept
        rg_resource_id import_persistent_image(u32_t name_hash, const char* debug_name, VkImage image, VkImageView view,
                                               const image_desc_t& desc, u32_t bindless_id, VkImageLayout cur_layout);

        rg_pass_t& add_pass(u32_t name_hash, const char* debug_name);

        void compile(transient_pool_t& pool);
        void execute(VkCommandBuffer cmd, ecs::registry_t& reg);

        rg_resource_id get_resource(u32_t name_hash) const;
        VkImageLayout get_layout(rg_resource_id id) const;
        VkFormat get_format(rg_resource_id id) const;
        VkImage get_image(rg_resource_id id) const;
        u32_t get_bindless_id(rg_resource_id id) const;
        // image, view, desc and the layout execute() left it in
        const rg_resource_t* find(rg_resource_id id) const;

        void add_alias(u32_t name_hash, u32_t alias_name);

        void* profiling_vk_ctx = nullptr;

      private:
        u32_t active_resource_count = 0;
        u32_t active_pass_count = 0;

        std::vector<rg_resource_t> resources;
        std::vector<rg_pass_t> passes;
        std::vector<size_t> sorted_passes;
        std::vector<rg_alias_t> aliases;

        // read before write hazards already reported, so a standing one does not spam every frame
        // deliberately not touched by clear(), it is process lifetime, not frame state
        std::vector<u64_t> reported_hazards;
    };
} // namespace tau::renderer