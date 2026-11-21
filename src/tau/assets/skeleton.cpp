#include "tau/assets/skeleton.h"

#include "tau/assets/animation_format.h"
#include "tau/assets/bytes.h"
#include "tau/log.h"
#include "tau/vfs.h"

#include <cstring>

namespace tau
{
    std::optional<skeleton_t> asset_loader_t<skeleton_t>::load(const std::string& path)
    {
        const std::string cooked_path = assets::cooked_path(get_type_id<skeleton_t>(), path);
        const std::vector<u8_t> bytes = vfs::read_bytes(cooked_path);

        byte_reader_t in{bytes};
        const skeleton_header_t header = in.read<skeleton_header_t>();
        if (!in.ok || header.magic != TAU_SKELETON_MAGIC || header.version != TAU_SKELETON_VERSION)
        {
            TAU_LOG_ERROR("SKELETON", "Missing, invalid or outdated skeleton, recook: {}", cooked_path);
            return std::nullopt;
        }

        skeleton_t skeleton;
        skeleton.joints.reserve(header.joint_count);
        for (u32_t i = 0; i < header.joint_count && in.ok; i++)
        {
            const joint_record_t record = in.read<joint_record_t>();

            joint_t& joint = skeleton.joints.emplace_back();
            joint.name = in.string(record.name_length);
            joint.parent = record.parent >= 0 && record.parent < static_cast<i32>(header.joint_count) &&
                                   record.parent != static_cast<i32>(i)
                               ? record.parent
                               : -1;
            joint.translation = {record.translation[0], record.translation[1], record.translation[2]};
            joint.rotation = {record.rotation[0], record.rotation[1], record.rotation[2], record.rotation[3]};
            joint.scale = {record.scale[0], record.scale[1], record.scale[2]};
            std::memcpy(&joint.inverse_bind, record.inverse_bind, sizeof(record.inverse_bind));
        }

        if (!in.ok)
        {
            TAU_LOG_ERROR("SKELETON", "Truncated skeleton: {}", cooked_path);
            return std::nullopt;
        }
        return skeleton;
    }
} // namespace tau
