#include "cache_manager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace tau::cooker
{
    void asset_cache_t::load()
    {
        if (std::filesystem::exists(path))
        {
            std::ifstream in(path);
            if (in.is_open()) { in >> data; }
        }
    }

    void asset_cache_t::save()
    {
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
        std::ofstream out(path);
        out << data.dump(4);
    }

    std::string asset_cache_t::combined_dep_hash(const std::vector<std::filesystem::path>& deps,
                                                 const std::string& settings)
    {
        std::string combined_hash = cook_identity();
        if (!settings.empty())
        {
            combined_hash += "_s" + std::to_string(XXH3_64bits(settings.data(), settings.size()));
        }
        for (const std::filesystem::path& dep : deps) { combined_hash += "_" + std::to_string(hash_file(dep)); }
        return combined_hash;
    }

    bool asset_cache_t::needs_cooking(const std::string& out_path, const std::vector<std::filesystem::path>& deps,
                                      const std::string& settings)
    {
        if (!std::filesystem::exists(out_path)) { return true; }
        if (!data.contains(out_path)) { return true; }

        const nlohmann::json& entry = data[out_path];

        // prefer the deps the last cook recorded over the caller's guess
        std::vector<std::filesystem::path> effective = deps;
        if (entry.contains("deps") && entry["deps"].is_array())
        {
            effective.clear();
            for (const nlohmann::json& dep : entry["deps"]) { effective.emplace_back(dep.get<std::string>()); }
        }

        return entry.value("hash", std::string{}) != combined_dep_hash(effective, settings);
    }

    void asset_cache_t::update_cache(const std::string& out_path, const std::vector<std::filesystem::path>& deps,
                                     const std::string& source_path, const std::string& settings)
    {
        nlohmann::json dep_list = nlohmann::json::array();
        for (const std::filesystem::path& dep : deps) { dep_list.push_back(dep.generic_string()); }

        data[out_path]["hash"] = combined_dep_hash(deps, settings);
        data[out_path]["deps"] = std::move(dep_list);
        if (!source_path.empty()) { data[out_path]["source"] = source_path; }
    }

    size_t asset_cache_t::retain_only(const std::unordered_set<std::string>& keys)
    {
        std::vector<std::string> stale;
        for (const auto& [key, entry] : data.items())
        {
            if (keys.count(key) == 0) { stale.push_back(key); }
        }

        for (const std::string& key : stale) { data.erase(key); }
        return stale.size();
    }

    void asset_cache_t::remember_source(const std::string& out_path, const std::string& source_path)
    {
        if (data.contains(out_path) && data[out_path].is_object()) { data[out_path]["source"] = source_path; }
    }

    std::vector<std::string> asset_cache_t::sources_depending_on(const std::vector<std::filesystem::path>& files) const
    {
        auto canonical = [](const std::filesystem::path& path)
        {
            std::error_code ec;
            const std::filesystem::path resolved = std::filesystem::weakly_canonical(path, ec);
            return (ec ? path : resolved).generic_string();
        };

        std::vector<std::string> wanted;
        for (const std::filesystem::path& file : files) { wanted.push_back(canonical(file)); }

        std::vector<std::string> found;
        for (const auto& [key, entry] : data.items())
        {
            if (!entry.is_object() || entry.value("non_output", false)) { continue; }

            const std::string source = entry.value("source", std::string{});
            if (source.empty() || !entry.contains("deps") || !entry["deps"].is_array()) { continue; }

            for (const nlohmann::json& dep : entry["deps"])
            {
                if (!dep.is_string()) { continue; }

                const std::string dep_path = canonical(dep.get<std::string>());
                if (std::find(wanted.begin(), wanted.end(), dep_path) != wanted.end())
                {
                    found.push_back(source);
                    break;
                }
            }
        }

        return found;
    }

    void asset_cache_t::forget(const std::string& out_path) { data.erase(out_path); }

    bool asset_cache_t::is_known_non_output(const std::string& source_path) const
    {
        const auto it = data.find(source_path);
        if (it == data.end() || !it->is_object()) { return false; }
        if (!it->value("non_output", false)) { return false; }

        std::vector<std::filesystem::path> deps;
        if (it->contains("deps") && (*it)["deps"].is_array())
        {
            for (const nlohmann::json& dep : (*it)["deps"]) { deps.emplace_back(dep.get<std::string>()); }
        }
        else
        {
            deps.emplace_back(source_path);
        }

        return it->value("hash", std::string{}) == combined_dep_hash(deps, {});
    }

    void asset_cache_t::mark_non_output(const std::string& source_path, const std::vector<std::filesystem::path>& deps)
    {
        update_cache(source_path, deps);
        data[source_path]["non_output"] = true;
    }
} // namespace tau::cooker