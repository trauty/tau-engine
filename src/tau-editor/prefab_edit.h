#pragma once

#include "tau/ecs_fwd.h"
#include "tau/math.h"

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace tau
{
    struct world_t;
    struct editor_context_t;
} // namespace tau

// editor prefab ops, each one undo step
namespace tau::editor::prefab_edit
{
    void init(editor_context_t& ctx);

    std::string create(world_t& world, editor_context_t& ctx, ecs::entity_t root, const std::string& dir);
    ecs::entity_t instantiate(world_t& world, editor_context_t& ctx, const std::string& prefab_path,
                              ecs::entity_t parent, const vec3_t* position);

    ecs::entity_t instance_root(world_t& world, ecs::entity_t entity);
    std::string prefab_path(world_t& world, ecs::entity_t root);

    void revert(world_t& world, editor_context_t& ctx, ecs::entity_t root);
    void apply(world_t& world, editor_context_t& ctx, ecs::entity_t root);
    void unpack(world_t& world, editor_context_t& ctx, ecs::entity_t root);

    bool reload(world_t& world, const std::string& vfs_path);

    // per component: fields differing from the prefab, "*" if the prefab lacks it
    using overrides_t = std::unordered_map<std::string, std::unordered_set<std::string>>;
    overrides_t overrides_of(world_t& world, ecs::entity_t entity);
} // namespace tau::editor::prefab_edit
