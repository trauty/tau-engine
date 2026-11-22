#include "tau/assets/clip.h"

#include "tau/assets/animation_format.h"
#include "tau/assets/bytes.h"
#include "tau/log.h"

namespace tau
{
    std::optional<clip_t> asset_loader_t<clip_t>::load(const std::string& path, std::span<const u8_t> bytes)
    {
        const std::string cooked_path = assets::cooked_path(get_type_id<clip_t>(), path);

        byte_reader_t in{bytes};
        const clip_header_t header = in.read<clip_header_t>();
        if (!in.ok || header.magic != TAU_CLIP_MAGIC || header.version != TAU_CLIP_VERSION)
        {
            TAU_LOG_ERROR("CLIP", "Missing, invalid or outdated clip, recook: {}", cooked_path);
            return std::nullopt;
        }

        clip_t clip;
        clip.duration = header.duration;
        clip.channels.reserve(header.channel_count);
        for (u32_t i = 0; i < header.channel_count && in.ok; i++)
        {
            const clip_channel_record_t record = in.read<clip_channel_record_t>();

            clip_channel_t& channel = clip.channels.emplace_back();
            channel.target = in.string(record.target_length);
            channel.path = static_cast<clip_path_e>(record.path);
            channel.interpolation = static_cast<clip_interpolation_e>(record.interpolation);
            channel.times = in.array<f32>(record.key_count);
            channel.values = in.array<f32>(record.value_count);
        }

        if (!in.ok)
        {
            TAU_LOG_ERROR("CLIP", "Truncated clip: {}", cooked_path);
            return std::nullopt;
        }
        return clip;
    }
} // namespace tau
