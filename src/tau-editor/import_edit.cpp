#include "import_edit.h"

#include "imgui.h"
#include "tau-editor/asset_files.h"
#include "tau-editor/asset_watch.h"
#include "tau-editor/editor_context.h"
#include "tau-editor/events.h"
#include "tau-editor/history.h"
#include "tau-editor/panels/inspector.h"
#include "tau/asset.h"
#include "tau/asset_sources.h"
#include "tau/assets/import_settings.h"
#include "tau/log.h"
#include "tau/project.h"
#include "tau/reflection_json.h"

#include <algorithm>
#include <filesystem>
#include <vector>

namespace tau::editor::import_edit
{
    namespace
    {
        struct entry_t
        {
            asset_guid_t guid = 0;
            std::string vfs;
            nlohmann::json layer = nlohmann::json::object();
        };

        std::vector<entry_t> g_entries;
        nlohmann::json g_defaults = nlohmann::json::object();
        std::filesystem::path g_assets_dir;

        entry_t* entry_for(asset_guid_t guid)
        {
            const auto it =
                std::find_if(g_entries.begin(), g_entries.end(), [guid](const entry_t& e) { return e.guid == guid; });
            if (it != g_entries.end()) { return &*it; }

            const std::string vfs = assets::path_of(asset_handle_t{guid});
            const std::filesystem::path source = asset_files::source_of(vfs);
            if (vfs.empty() || source.empty()) { return nullptr; }

            entry_t entry{guid, vfs, nlohmann::json::object()};
            const nlohmann::json meta = asset_sources::read_meta(source.generic_string());
            if (const auto layer = meta.find(import_settings::META_KEY); layer != meta.end() && layer->is_object())
            {
                entry.layer = *layer;
            }
            return &g_entries.emplace_back(std::move(entry));
        }

        std::vector<u8_t> capture(asset_handle_t handle)
        {
            const entry_t* entry = entry_for(handle.guid);
            return entry ? nlohmann::json::to_cbor(entry->layer) : std::vector<u8_t>{};
        }

        bool apply(asset_handle_t handle, std::span<const u8_t> state)
        {
            entry_t* entry = entry_for(handle.guid);
            if (!entry) { return false; }

            nlohmann::json layer = nlohmann::json::from_cbor(state, true, false);
            if (layer.is_discarded()) { return false; }

            entry->layer = std::move(layer);
            return true;
        }

        bool write(asset_handle_t handle, std::filesystem::path& out_source)
        {
            const entry_t* entry = entry_for(handle.guid);
            if (!entry) { return false; }

            const std::filesystem::path source = asset_files::source_of(entry->vfs);
            nlohmann::json meta = asset_sources::read_meta(source.generic_string());
            meta[import_settings::META_KEY] = entry->layer;

            asset_watch::ignore_own_write(asset_sources::meta_path(source.generic_string()));
            if (!asset_sources::write_meta(source.generic_string(), meta)) { return false; }

            out_source = source;
            return true;
        }

        void project_opened(world_t&, editor_context_t& ctx)
        {
            g_entries.clear();
            g_defaults = nlohmann::json::object();
            g_assets_dir.clear();

            project_t project;
            if (ctx.project_file.empty() || !project_t::load(ctx.project_file, project)) { return; }

            g_defaults = std::move(project.import_defaults);
            g_assets_dir = project.assets_dir;
        }
    } // namespace

    void init(editor_context_t&)
    {
        history::reg_import(&capture, &apply, &write);
        events::on(events::event_e::PROJECT_OPENED, &project_opened);
    }

    void draw(world_t& world, const std::string& vfs)
    {
        const asset_type_t* type = assets::type_by_path(vfs);
        if (!type || type->import_type == 0) { return; }

        const asset_guid_t guid = assets::guid_of(vfs);
        entry_t* entry = guid != 0 ? entry_for(guid) : nullptr;
        if (!entry) { return; }

        if (!ImGui::CollapsingHeader("Import Settings", ImGuiTreeNodeFlags_DefaultOpen)) { return; }

        const reflection::any_t base = import_settings::base(type->id, g_defaults);
        reflection::any_t value = base;
        reflection_json::apply(value, entry->layer);

        const history::target_t target = history::import_target(guid);

        ImGui::PushID("import_settings");
        history::watch(world, target);
        if (panels::draw_fields(world, value))
        {
            entry->layer = import_settings::meta_object(value, base);
            history::edited(world, target, "Import Settings");
        }
        if (history::is_dirty(target.doc)) { ImGui::TextDisabled("Save to reimport"); }
        ImGui::PopID();
        ImGui::Separator();
    }

    void revert(const std::string& vfs)
    {
        const asset_guid_t guid = assets::guid_of(vfs);
        std::erase_if(g_entries, [guid](const entry_t& e) { return e.guid == guid; });
        history::drop({history::doc_kind_e::IMPORT, guid});
    }

    const nlohmann::json& project_defaults() { return g_defaults; }

    void set_project_defaults(const nlohmann::json& defaults)
    {
        g_defaults = defaults;

        std::error_code ec;
        if (g_assets_dir.empty() || !std::filesystem::is_directory(g_assets_dir, ec)) { return; }

        std::vector<std::filesystem::path> sources;
        for (std::filesystem::recursive_directory_iterator it(g_assets_dir, ec), end; it != end; it.increment(ec))
        {
            if (ec || !it->is_regular_file(ec)) { continue; }

            const asset_type_t* type = assets::type_by_path(it->path().generic_string());
            if (type && type->import_type != 0) { sources.push_back(it->path()); }
        }

        TAU_LOG_INFO("IMPORT", "Project import defaults changed, checking {} source(s)", sources.size());
        asset_watch::cook_edited(sources);
    }
} // namespace tau::editor::import_edit
