#pragma once

#include "tau/defines.h"

#include <algorithm>
#include <vector>

namespace tau::editor
{
    // guids: survive reload and Play/Stop
    struct selection_t
    {
        std::vector<u64_t> guids;

        bool empty() const { return guids.empty(); }
        u64_t primary() const { return guids.empty() ? 0 : guids.back(); }
        bool contains(u64_t guid) const { return std::find(guids.begin(), guids.end(), guid) != guids.end(); }

        void clear() { guids.clear(); }

        void set(u64_t guid)
        {
            guids.clear();
            if (guid != 0) { guids.push_back(guid); }
        }

        void toggle(u64_t guid)
        {
            if (guid == 0) { return; }

            const auto it = std::find(guids.begin(), guids.end(), guid);
            if (it != guids.end()) { guids.erase(it); }
            else
            {
                guids.push_back(guid);
            }
        }

        void remove(u64_t guid) { std::erase(guids, guid); }
    };
} // namespace tau::editor
