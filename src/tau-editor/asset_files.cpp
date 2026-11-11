#include "asset_files.h"

#include "tau-editor/asset_cook.h"
#include "tau-editor/asset_watch.h"
#include "tau-editor/editor_context.h"
#include "tau-editor/history.h"
#include "tau/asset.h"
#include "tau/asset_sources.h"
#include "tau/engine.h"
#include "tau/guid.h"
#include "tau/hash.h"
#include "tau/log.h"

#include <fstream>
#include <json/json.hpp>
#include <unordered_map>

namespace fs = std::filesystem;

namespace tau::editor::asset_files
{
    namespace
    {
        constexpr std::string_view VFS_PREFIX = "game://assets/";

        editor_context_t* g_ctx = nullptr;

        std::unordered_map<u64_t, std::string> entries;
        u64_t next_entry = 1;

        fs::path source_root() { return fs::path(g_ctx->project_assets_dir); }
        fs::path cooked_root() { return fs::path(g_ctx->project_cooked_dir) / "game"; }
        fs::path trash_root() { return fs::path(g_ctx->project_cooked_dir) / "trash"; }
        fs::path trash_of(u64_t entry) { return trash_root() / std::to_string(entry); }

        std::string name_of(const std::string& rel)
        {
            const std::size_t slash = rel.rfind('/');
            return slash == std::string::npos ? rel : rel.substr(slash + 1);
        }

        std::string parent_of(const std::string& rel)
        {
            const std::size_t slash = rel.rfind('/');
            return slash == std::string::npos ? std::string{} : rel.substr(0, slash);
        }

        std::string join(const std::string& dir, const std::string& name)
        { return dir.empty() ? name : dir + "/" + name; }

        bool is_within(const std::string& rel, const std::string& dir)
        { return rel == dir || (rel.size() > dir.size() && rel.starts_with(dir) && rel[dir.size()] == '/'); }

        fs::path cooked_of(const std::string& rel, bool is_dir)
        {
            if (is_dir) { return cooked_root() / rel; }

            const asset_type_t* type = assets::type_by_path(rel);
            if (!type) { return {}; }

            fs::path cooked = cooked_root() / rel;
            cooked.replace_extension(std::string(type->cooked_extension));
            return cooked;
        }

        history::target_t target_of(u64_t entry) { return {history::PROJECT_FILES, entry}; }

        u64_t new_entry()
        {
            const u64_t entry = next_entry++;
            entries[entry] = {};
            return entry;
        }

        u64_t entry_for(const std::string& rel)
        {
            for (const auto& [entry, at] : entries)
            {
                if (at == rel) { return entry; }
            }
            const u64_t entry = new_entry();
            entries[entry] = rel;
            return entry;
        }

        std::string unique_name(const std::string& dir, const std::string& stem, const std::string& ext)
        {
            std::string name = stem + ext;
            for (u32_t i = 1; fs::exists(source_root() / join(dir, name)); i++)
            {
                name = stem + " " + std::to_string(i) + ext;
            }
            return join(dir, name);
        }

        bool move_path(const fs::path& from, const fs::path& to)
        {
            std::error_code ec;
            if (from.empty() || to.empty() || !fs::exists(from, ec)) { return true; }

            fs::create_directories(to.parent_path(), ec);
            fs::rename(from, to, ec);
            if (ec)
            {
                TAU_LOG_ERROR("ASSET_FILES", "Cannot move {} to {}: {}", from.generic_string(), to.generic_string(),
                              ec.message());
                return false;
            }
            return true;
        }

        void write_guid_map()
        {
            const fs::path map = fs::path(g_ctx->project_cooked_dir) / "guid_map.json";
            asset_sources::write_guid_map(map.generic_string(), assets::locations(VFS_PREFIX), VFS_PREFIX);
        }

        void forget_assets(const std::string& rel)
        {
            const std::string key = std::string(VFS_PREFIX) + rel;
            for (const auto& [path, guid] : assets::locations(key))
            {
                if (path == key || path.starts_with(key + "/")) { assets::remove_location(path); }
            }
        }

        // back in the map, live ones reload, watcher skips them
        void adopt_assets(const fs::path& at)
        {
            auto adopt = [](const fs::path& file)
            {
                if (!assets::type_by_path(file.generic_string())) { return; }

                std::error_code ec;
                const std::string rel = fs::relative(file, source_root(), ec).generic_string();
                if (ec) { return; }

                const std::string key = std::string(VFS_PREFIX) + rel;
                assets::set_location(key, asset_sources::find_or_create_guid(file.generic_string()));
                asset_watch::ignore_own_write(file);
            };

            std::error_code ec;
            if (!fs::is_directory(at, ec))
            {
                adopt(at);
                return;
            }
            for (fs::recursive_directory_iterator it(at, ec), end; it != end; it.increment(ec))
            {
                if (!ec && it->is_regular_file(ec)) { adopt(it->path()); }
            }
        }

        void follow(const std::string& from, const std::string& to)
        {
            if (from.empty() || to.empty()) { return; }

            auto rebase = [&](const std::string& rel) { return to + rel.substr(from.size()); };

            if (!g_ctx->current_scene_path.empty())
            {
                std::error_code ec;
                const std::string rel = fs::relative(g_ctx->current_scene_path, source_root(), ec).generic_string();
                if (!ec && is_within(rel, from))
                {
                    g_ctx->current_scene_path = source_path(rebase(rel)).string();
                    asset_watch::set_open_scene(g_ctx->current_scene_path);
                }
            }

            const std::string inspected_key = std::string(VFS_PREFIX);
            if (g_ctx->inspected_asset.starts_with(inspected_key))
            {
                const std::string rel = g_ctx->inspected_asset.substr(inspected_key.size());
                if (is_within(rel, from)) { g_ctx->inspected_asset = vfs_path(rebase(rel)); }
            }
        }

        bool relocate(u64_t entry, const std::string& to)
        {
            const std::string from = entries[entry];
            if (from == to) { return true; }

            std::error_code ec;
            const fs::path src_from = from.empty() ? trash_of(entry) / "src" : source_root() / from;
            if (!fs::exists(src_from, ec))
            {
                TAU_LOG_ERROR("ASSET_FILES", "{} is gone, it cannot be moved",
                              from.empty() ? "A deleted item" : from.c_str());
                return false;
            }

            const bool is_dir = fs::is_directory(src_from, ec);
            const fs::path src_to = to.empty() ? trash_of(entry) / "src" : source_root() / to;
            if (!to.empty() && fs::exists(src_to, ec))
            {
                TAU_LOG_ERROR("ASSET_FILES", "{} already exists", to);
                return false;
            }

            const fs::path cooked_from = from.empty() ? trash_of(entry) / "cooked" : cooked_of(from, is_dir);
            const fs::path cooked_to = to.empty() ? trash_of(entry) / "cooked" : cooked_of(to, is_dir);

            forget_assets(from);

            if (!move_path(src_from, src_to)) { return false; }
            if (!is_dir) { move_path(src_from.string() + ".meta", src_to.string() + ".meta"); }
            move_path(cooked_from, cooked_to);

            entries[entry] = to;
            if (!to.empty()) { adopt_assets(src_to); }
            follow(from, to);
            write_guid_map();
            return true;
        }

        bool run(world_t& world, std::string_view label, const std::vector<std::pair<u64_t, std::string>>& moves)
        {
            if (moves.empty()) { return false; }

            std::vector<history::target_t> targets;
            for (const auto& [entry, to] : moves) { targets.push_back(target_of(entry)); }

            history::begin(world, label, targets);
            bool ok = true;
            for (const auto& [entry, to] : moves) { ok = relocate(entry, to) && ok; }
            history::end(world);
            return ok;
        }

        std::string count_label(const char* verb, std::span<const std::string> rels)
        {
            if (rels.size() == 1) { return std::string(verb) + " " + name_of(rels.front()); }
            return std::string(verb) + " " + std::to_string(rels.size()) + " Items";
        }

        std::string create_file(world_t& world, std::string_view label, const std::string& rel, const std::string& text)
        {
            const u64_t entry = new_entry();
            const history::target_t target = target_of(entry);
            history::begin(world, label, {&target, 1});
            const bool ok = write_new_file(entry, rel, text);
            history::end(world);
            return ok ? rel : std::string{};
        }
    } // namespace

    void init(editor_context_t& ctx) { g_ctx = &ctx; }

    u64_t reserve_entry() { return new_entry(); }

    bool write_new_file(u64_t entry, const std::string& rel, const std::string& text)
    {
        const fs::path path = source_root() / rel;
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        {
            std::ofstream file(path);
            if (!file.is_open())
            {
                TAU_LOG_ERROR("ASSET_FILES", "Cannot write {}", rel);
                return false;
            }
            file << text;
        }

        std::string err;
        if (!asset_cook::cook_asset(path, err)) { TAU_LOG_ERROR("ASSET_FILES", "Cannot cook {}: {}", rel, err); }

        entries[entry] = rel;
        adopt_assets(path);
        write_guid_map();
        return true;
    }

    std::string unique_path(const std::string& dir, const std::string& stem, const std::string& ext)
    { return unique_name(dir, stem, ext); }

    void reset()
    {
        entries.clear();

        std::error_code ec;
        if (g_ctx && !g_ctx->project_cooked_dir.empty()) { fs::remove_all(trash_root(), ec); }
    }

    fs::path source_path(const std::string& rel) { return source_root() / rel; }
    std::string vfs_path(const std::string& rel) { return std::string(VFS_PREFIX) + rel; }

    std::vector<u8_t> capture(u64_t entry)
    {
        const auto it = entries.find(entry);
        if (it == entries.end()) { return {}; }
        return {it->second.begin(), it->second.end()};
    }

    bool apply(world_t&, u64_t entry, std::span<const u8_t> state)
    {
        if (!entries.contains(entry)) { return false; }
        return relocate(entry, std::string(state.begin(), state.end()));
    }

    std::string create_folder(world_t& world, const std::string& dir)
    {
        const std::string rel = unique_name(dir, "New Folder", "");

        const u64_t entry = new_entry();
        const history::target_t target = target_of(entry);
        history::begin(world, "Create Folder", {&target, 1});

        std::error_code ec;
        fs::create_directories(source_root() / rel, ec);
        entries[entry] = rel;

        history::end(world);
        return ec ? std::string{} : rel;
    }

    std::string create_material(world_t& world, const std::string& dir)
    {
        const std::string shader = "engine://assets/shaders/pbr.slang";
        nlohmann::json material = {
            {"guid",       guid::to_string(guid::generate())                                            },
            {"shader",     shader                                                                       },
            {"properties", {{"base_color", {1.0, 1.0, 1.0, 1.0}}, {"metallic", 0.0}, {"roughness", 0.5}}},
            {"textures",   {{"albedo_tex", "engine://assets/textures/default_white.png"}}               },
        };
        if (const asset_guid_t shader_guid = assets::guid_of(shader))
        {
            material["shader_guid"] = assets::guid_text(shader_guid);
        }

        return create_file(world, "Create Material", unique_name(dir, "New Material", ".mat"), material.dump(4) + "\n");
    }

    std::string create_scene(world_t& world, const std::string& dir)
    {
        const nlohmann::json scene = {
            {"guid",     guid::to_string(guid::generate())},
            {"entities", nlohmann::json::array()          },
        };
        return create_file(world, "Create Scene", unique_name(dir, "New Scene", ".scene"), scene.dump(4) + "\n");
    }

    bool rename(world_t& world, const std::string& rel, const std::string& new_name)
    {
        if (new_name.empty() || new_name == "." || new_name == ".." ||
            new_name.find_first_of("/\\") != std::string::npos)
        {
            TAU_LOG_WARN("ASSET_FILES", "'{}' is not a usable name", new_name);
            return false;
        }

        std::string name = new_name;
        std::error_code ec;
        if (!fs::is_directory(source_root() / rel, ec))
        {
            const std::string ext = fs::path(rel).extension().string();
            if (!ext.empty() && !name.ends_with(ext)) { name += ext; }
        }

        const std::string to = join(parent_of(rel), name);
        if (to == rel) { return true; }

        return run(world, "Rename " + name_of(rel),
                   {
                       {entry_for(rel), to}
        });
    }

    bool move(world_t& world, std::span<const std::string> rels, const std::string& dest_dir)
    {
        std::vector<std::pair<u64_t, std::string>> moves;
        std::vector<std::string> moved;
        for (const std::string& rel : rels)
        {
            if (parent_of(rel) == dest_dir) { continue; }
            if (is_within(dest_dir, rel))
            {
                TAU_LOG_WARN("ASSET_FILES", "Cannot move {} into itself", rel);
                continue;
            }
            moves.emplace_back(entry_for(rel), join(dest_dir, name_of(rel)));
            moved.push_back(rel);
        }
        return run(world, count_label("Move", moved), moves);
    }

    bool remove(world_t& world, std::span<const std::string> rels)
    {
        std::vector<std::pair<u64_t, std::string>> moves;
        std::vector<std::string> removed;
        for (const std::string& rel : rels)
        {
            const bool covered = std::any_of(rels.begin(), rels.end(), [&](const std::string& other)
                                             { return other != rel && is_within(rel, other); });
            if (covered) { continue; }

            moves.emplace_back(entry_for(rel), std::string{});
            removed.push_back(rel);
        }
        return run(world, count_label("Delete", removed), moves);
    }

    std::vector<std::string> duplicate(world_t& world, std::span<const std::string> rels)
    {
        std::vector<std::string> sources;
        for (const std::string& rel : rels)
        {
            std::error_code ec;
            if (fs::is_regular_file(source_root() / rel, ec)) { sources.push_back(rel); }
        }
        if (sources.empty()) { return {}; }

        std::vector<u64_t> copies;
        std::vector<history::target_t> targets;
        for (std::size_t i = 0; i < sources.size(); i++)
        {
            copies.push_back(new_entry());
            targets.push_back(target_of(copies.back()));
        }

        history::begin(world, count_label("Duplicate", sources), targets);

        std::vector<std::string> made;
        for (std::size_t i = 0; i < sources.size(); i++)
        {
            const std::string& rel = sources[i];
            const fs::path ext = fs::path(rel).extension();
            const std::string copy = unique_name(parent_of(rel), fs::path(rel).stem().string() + " Copy", ext.string());

            std::error_code ec;
            fs::copy_file(source_root() / rel, source_root() / copy, ec);
            if (ec)
            {
                TAU_LOG_ERROR("ASSET_FILES", "Cannot copy {}: {}", rel, ec.message());
                continue;
            }

            // copy gets a new guid
            const asset_type_t* type = assets::type_by_path(rel);
            if (type && type->guid_in_source)
            {
                std::ifstream in(source_root() / copy);
                nlohmann::json data = nlohmann::json::parse(in, nullptr, false);
                in.close();
                if (data.is_object())
                {
                    data["guid"] = guid::to_string(guid::generate());
                    std::ofstream(source_root() / copy) << data.dump(4) << "\n";
                }
            }
            else if (type)
            {
                std::ofstream(source_path(copy).string() + ".meta") << nlohmann::json{
                    {"guid", guid::to_string(guid::generate())}
                }.dump(2);
            }

            if (const fs::path cooked = cooked_of(rel, false); !cooked.empty())
            {
                fs::copy_file(cooked, cooked_of(copy, false), ec);
            }

            entries[copies[i]] = copy;
            adopt_assets(source_root() / copy);
            made.push_back(copy);
        }
        write_guid_map();

        history::end(world);
        return made;
    }
} // namespace tau::editor::asset_files
