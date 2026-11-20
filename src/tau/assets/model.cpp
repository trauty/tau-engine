#include "tau/assets/model.h"

#include "tau/assets/bytes.h"
#include "tau/assets/model_format.h"
#include "tau/log.h"
#include "tau/vfs.h"

namespace tau
{
    std::optional<model_t> asset_loader_t<model_t>::load(const std::string& path)
    {
        const std::string cooked_path = assets::cooked_path(get_type_id<model_t>(), path);
        const std::vector<u8_t> bytes = vfs::read_bytes(cooked_path);

        byte_reader_t in{bytes};
        const model_header_t header = in.read<model_header_t>();
        if (!in.ok || header.magic != TAU_MODEL_MAGIC || header.version != TAU_MODEL_VERSION)
        {
            TAU_LOG_ERROR("MODEL", "Missing, invalid or outdated model manifest, recook: {}", cooked_path);
            return std::nullopt;
        }

        model_t model;
        model.sub_assets.reserve(header.sub_asset_count);
        for (u32_t i = 0; i < header.sub_asset_count && in.ok; i++)
        {
            const model_sub_asset_record_t record = in.read<model_sub_asset_record_t>();
            model.sub_assets.push_back({in.string(record.name_length), record.type_id, record.guid});
        }

        if (!in.ok)
        {
            TAU_LOG_ERROR("MODEL", "Truncated model manifest: {}", cooked_path);
            return std::nullopt;
        }
        return model;
    }
} // namespace tau
