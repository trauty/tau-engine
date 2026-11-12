#include "tau/asset_sources.h"

#include "tau/guid.h"
#include "tau/log.h"

#include "json/json.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>

namespace tau::asset_sources
{
    namespace
    {
        bool read_json_object(const std::string& path, nlohmann::json& out)
        {
            std::ifstream file(path);
            if (!file.is_open()) { return false; }

            out = nlohmann::json::parse(file, nullptr, false);
            return !out.is_discarded() && out.is_object();
        }

        asset_guid_t guid_field(const nlohmann::json& object)
        {
            const auto it = object.find("guid");
            return it != object.end() && it->is_string() ? assets::parse_guid(it->get<std::string>()) : 0;
        }

        // the text with a guid member inserted first, so hand written files keep key order,
        // compact arrays and line endings. empty when the text is not an object
        std::string with_guid_inserted(const std::string& text, const std::string& guid)
        {
            const std::size_t open = text.find('{');
            if (open == std::string::npos) { return {}; }

            const std::string nl = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";

            const std::size_t first = text.find_first_not_of(" \t\r\n", open + 1);
            if (first == std::string::npos) { return {}; }
            const bool empty_object = text[first] == '}';

            std::string indent = "    ";
            const std::size_t line_start = text.find_last_of('\n', first);
            if (!empty_object && line_start != std::string::npos && line_start > open)
            {
                indent = text.substr(line_start + 1, first - line_start - 1);
            }

            const std::string member = nl + indent + "\"guid\": \"" + guid + "\"" + (empty_object ? "" : ",");
            return text.substr(0, open + 1) + member + (empty_object ? nl : "") + text.substr(open + 1);
        }

        bool keeps_guid_in_source(const std::string& path)
        {
            const asset_type_t* type = assets::type_by_path(path);
            return type && type->guid_in_source;
        }

        asset_guid_t find_or_create_meta_guid(const std::string& source_path)
        {
            nlohmann::json meta = read_meta(source_path);
            if (const asset_guid_t guid = guid_field(meta)) { return guid; }

            const asset_guid_t guid = guid::generate();
            meta["guid"] = guid::to_string(guid);
            write_meta(source_path, meta);

            TAU_LOG_INFO("ASSET", "Created {} for guid {}", meta_path(source_path), guid::to_string(guid));
            return guid;
        }

        asset_guid_t find_or_create_source_guid(const std::string& source_path)
        {
            const std::string meta = meta_path(source_path);

            nlohmann::json doc;
            if (!read_json_object(source_path, doc))
            {
                // unreadable (half written?): guid waits in a .meta
                return find_or_create_meta_guid(source_path);
            }

            asset_guid_t guid = guid_field(doc);
            if (guid == 0)
            {
                guid = guid_field(read_meta(source_path));
                const bool moved = guid != 0;
                if (!moved) { guid = guid::generate(); }

                std::string text;
                {
                    std::ifstream in(source_path, std::ios::binary);
                    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                }

                // verified: must parse to the same doc plus the guid
                const std::string edited = with_guid_inserted(text, guid::to_string(guid));
                const nlohmann::json check = nlohmann::json::parse(edited, nullptr, false);
                doc["guid"] = guid::to_string(guid);
                if (edited.empty() || check.is_discarded() || check != doc)
                {
                    TAU_LOG_ERROR("ASSET", "Could not add a guid to {} without changing it, left as is", source_path);
                    return guid;
                }

                std::ofstream file(source_path, std::ios::binary | std::ios::trunc);
                file << edited;
                if (!file)
                {
                    TAU_LOG_ERROR("ASSET", "Could not write the guid into {}", source_path);
                    return guid;
                }

                TAU_LOG_INFO("ASSET", "{} the guid of {}", moved ? "Moved in the .meta's" : "Wrote", source_path);
            }

            std::error_code ec;
            if (std::filesystem::remove(meta, ec))
            {
                TAU_LOG_INFO("ASSET", "Removed {}, the guid lives in the file", meta);
            }

            return guid;
        }
    } // namespace

    std::string meta_path(std::string_view source_path) { return std::string(source_path) + ".meta"; }

    nlohmann::json read_meta(std::string_view source_path)
    {
        nlohmann::json meta;
        return read_json_object(meta_path(source_path), meta) ? meta : nlohmann::json::object();
    }

    bool write_meta(std::string_view source_path, const nlohmann::json& meta)
    {
        const std::string path = meta_path(source_path);
        std::ofstream file(path, std::ios::trunc);
        file << meta.dump(4) << '\n';
        if (!file)
        {
            TAU_LOG_ERROR("ASSET", "Could not write {}", path);
            return false;
        }
        return true;
    }

    asset_guid_t find_or_create_guid(const std::string& source_path)
    {
        return keeps_guid_in_source(source_path) ? find_or_create_source_guid(source_path)
                                                 : find_or_create_meta_guid(source_path);
    }

    asset_guid_t guid_for_writing(const std::string& source_path)
    {
        nlohmann::json doc;
        if (read_json_object(source_path, doc))
        {
            if (const asset_guid_t inside = guid_field(doc)) { return inside; }
        }

        const asset_guid_t sidecar = guid_field(read_meta(source_path));
        return sidecar != 0 ? sidecar : guid::generate();
    }

    void write_guid_map(const std::string& output_path, std::span<const std::pair<std::string, asset_guid_t>> entries,
                        std::string_view replace_prefix)
    {
        const std::filesystem::path out(output_path);
        std::filesystem::create_directories(out.parent_path());

        nlohmann::json merged = nlohmann::json::object();
        if (nlohmann::json prev; read_json_object(output_path, prev)) { merged = std::move(prev); }

        if (!replace_prefix.empty())
        {
            for (auto it = merged.begin(); it != merged.end();)
            {
                if (std::string_view(it.key()).starts_with(replace_prefix)) { it = merged.erase(it); }
                else
                {
                    it++;
                }
            }
        }

        for (const auto& [path, guid] : entries) { merged[path] = guid::to_string(guid); }

        const std::string text = merged.dump(4);
        std::ofstream file(output_path);
        file << text;
        TAU_LOG_INFO("ASSET", "Wrote the guid map {} ({} entries)", output_path, merged.size());
    }
} // namespace tau::asset_sources
