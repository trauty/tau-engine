#include "data_edit.h"

#include "imgui.h"
#include "tau-editor/asset_editors.h"
#include "tau-editor/asset_files.h"
#include "tau-editor/editor_context.h"
#include "tau-editor/events.h"
#include "tau-editor/history.h"
#include "tau-editor/panels/inspector.h"
#include "tau/asset.h"
#include "tau/asset_sources.h"
#include "tau/assets/data.h"
#include "tau/guid.h"
#include "tau/log.h"

#include "json/json.hpp"
#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <map>
#include <unordered_map>

namespace tau::editor::data_edit
{
    namespace
    {
        namespace fs = std::filesystem;

        // the source's fields as last saved or edited, unknown keys included
        struct held_t
        {
            nlohmann::json fields = nlohmann::json::object();
            std::string type_name;
            u32_t version = 0;
        };

        std::unordered_map<asset_guid_t, held_t> g_held;

        data_asset_t* asset_of(asset_handle_t handle) { return assets::get(asset_t<data_asset_t>{handle}); }

        nlohmann::json read_source(const fs::path& path)
        {
            std::ifstream file(path);
            nlohmann::json json = nlohmann::json::parse(file, nullptr, false);
            return json.is_object() ? json : nlohmann::json::object();
        }

        // read again when the asset changed outside the editor (reload, revert)
        held_t* held_of(asset_handle_t handle)
        {
            const data_asset_t* asset = asset_of(handle);
            if (asset == nullptr) { return nullptr; }

            held_t& held = g_held[handle.guid];
            if (held.version != asset->version)
            {
                const nlohmann::json source = read_source(asset_files::source_of(assets::path_of(handle)));
                const nlohmann::json fields = source.value("fields", nlohmann::json::object());
                held.fields = fields.is_object() ? fields : nlohmann::json::object();
                held.type_name = source.value("type", asset->type_name);
                held.version = asset->version;
            }
            return &held;
        }

        void set_held(asset_handle_t handle, held_t& held, nlohmann::json fields)
        {
            held.fields = std::move(fields);
            held.version = data::set_fields(handle, data::parse_fields(held.fields));
        }

        std::vector<u8_t> capture(asset_handle_t handle)
        {
            const held_t* held = held_of(handle);
            return held ? nlohmann::json::to_cbor(held->fields) : std::vector<u8_t>{};
        }

        bool apply(asset_handle_t handle, std::span<const u8_t> state)
        {
            held_t* held = held_of(handle);
            if (held == nullptr) { return false; }

            nlohmann::json fields = nlohmann::json::from_cbor(state, true, false);
            if (fields.is_discarded()) { return false; }
            set_held(handle, *held, std::move(fields));
            return true;
        }

        bool write(asset_handle_t handle, fs::path& out_source)
        {
            const held_t* held = held_of(handle);
            const fs::path target = asset_files::source_of(assets::path_of(handle));
            if (held == nullptr || target.empty())
            {
                TAU_LOG_ERROR("EDITOR", "Cannot map '{}' to a source file", assets::path_of(handle));
                return false;
            }

            const nlohmann::json out = {
                {"guid",   assets::guid_text(asset_sources::guid_for_writing(target.string()))},
                {"type",   held->type_name                                                    },
                {"fields", held->fields                                                       },
            };

            std::ofstream file(target);
            if (!file.is_open())
            {
                TAU_LOG_ERROR("EDITOR", "Cannot open {}", target.string());
                return false;
            }
            file << out.dump(4) << '\n';
            out_source = target;
            return true;
        }

        u32_t key_hash(const std::string& key)
        {
            const bool numeric = !key.empty() && std::ranges::all_of(key, [](char c) { return std::isdigit(c) != 0; });
            return numeric ? static_cast<u32_t>(std::stoul(key)) : hash_string(key);
        }

        bool is_struct(const reflection::type_t& type)
        {
            return !type.is_enum() && !type.is_sequence_container() && !reflection::pod_tag(type) &&
                   type.data().begin() != type.data().end();
        }

        bool is_struct_value(const nlohmann::json& value)
        {
            return value.is_object() && value.value("ty", -1) == static_cast<i32>(scene_value_type_e::STRUCT) &&
                   value.contains("v");
        }

        // the value's fields (set: partial types keep only these leaves) plus the keys the type no longer has
        nlohmann::json merge(const nlohmann::json& encoded, const nlohmann::json& old, const reflection::type_t& type,
                             u32_t parent, const std::vector<u32_t>* set)
        {
            nlohmann::json out = nlohmann::json::object();
            for (const auto& [key, value] : encoded.items())
            {
                const u32_t hash = key_hash(key);
                const u32_t path = data::path_hash(parent, hash);
                const reflection::data_t field = type.data(hash);

                if (field && is_struct_value(value) && is_struct(field.type()))
                {
                    const auto it = old.find(key);
                    const nlohmann::json old_fields =
                        it != old.end() && is_struct_value(*it) ? (*it)["v"] : nlohmann::json::object();
                    nlohmann::json sub = merge(value["v"], old_fields, field.type(), path, set);
                    if (set == nullptr || !sub.empty())
                    {
                        out[key] = {
                            {"ty", static_cast<u8_t>(scene_value_type_e::STRUCT)},
                            {"v",  std::move(sub)                               }
                        };
                    }
                }
                else if (set == nullptr || std::ranges::binary_search(*set, path)) { out[key] = value; }
            }

            for (const auto& [key, value] : old.items())
            {
                if (!type.data(key_hash(key)) && !out.contains(key)) { out[key] = value; }
            }
            return out;
        }

        constexpr ImVec4 SET_COLOR = {0.45f, 0.72f, 1.0f, 1.0f};

        // partial types: every leaf editable, an edit sets it, a set leaf has a reset button
        bool draw_partial(world_t& world, reflection::any_t& instance, const reflection::type_t& type, u32_t parent,
                          std::vector<u32_t>& set)
        {
            bool changed = false;
            for (auto [id, field] : type.data())
            {
                const reflection::editor_prop_t* prop = field.custom();
                if (prop && (has_flag(prop->flags, reflection::prop_flags_e::HIDDEN) ||
                             has_flag(prop->flags, reflection::prop_flags_e::NOT_SAVED)))
                {
                    continue;
                }

                const u32_t path = data::path_hash(parent, id);
                const char* label = prop ? prop->name : "Unknown";
                ImGui::PushID(static_cast<i32>(id));

                if (is_struct(field.type()) && !(prop && prop->asset_type_hash != 0))
                {
                    if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        reflection::any_t value = field.get(instance);
                        if (draw_partial(world, value, field.type(), path, set))
                        {
                            field.set(instance, value);
                            changed = true;
                        }
                        ImGui::TreePop();
                    }
                }
                else if (ImGui::BeginTable("##leaf", 2))
                {
                    const auto at = std::ranges::lower_bound(set, path);
                    const bool is_set = at != set.end() && *at == path;

                    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("reset", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();

                    if (is_set)
                    {
                        const ImVec2 pos = ImGui::GetCursorScreenPos();
                        ImGui::GetWindowDrawList()->AddRectFilled(
                            ImVec2(pos.x - 6.0f, pos.y + 1.0f),
                            ImVec2(pos.x - 3.0f, pos.y + ImGui::GetFrameHeight() - 1.0f),
                            ImGui::GetColorU32(SET_COLOR));
                    }

                    if (panels::draw_field(world, instance, id, field))
                    {
                        if (!is_set) { set.insert(at, path); }
                        changed = true;
                    }

                    ImGui::TableNextColumn();
                    if (is_set)
                    {
                        if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0.0f)))
                        {
                            set.erase(std::ranges::find(set, path));
                            changed = true;
                        }
                        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Reset to the default"); }
                    }
                    ImGui::EndTable();
                }

                ImGui::PopID();
            }
            return changed;
        }

        void draw_inspector(world_t& world, editor_context_t&, const std::string& vfs, asset_handle_t handle)
        {
            const data_asset_t* asset = asset_of(handle);
            if (asset == nullptr)
            {
                ImGui::TextDisabled("not loaded");
                return;
            }

            const std::optional<data::data_type_t> type = data::type_by_hash(asset->type_hash);
            const reflection::type_t meta = reflection::resolve(*world.reflection_ctx, asset->type_hash);
            if (!type || !meta)
            {
                ImGui::TextDisabled("'%s' is no registered data type", asset->type_name.c_str());
                return;
            }

            ImGui::TextDisabled("%s%s", type->label, type->partial ? ", partial: edited fields are set" : "");
            ImGui::Separator();

            held_t* held = held_of(handle);
            if (held == nullptr) { return; }

            // decoded each frame: nothing outlives a library reload
            void* value = type->construct();
            data::decode(asset->type_hash, value, asset->fields, vfs);
            reflection::any_t object = meta.from_void(value);

            const history::target_t target = history::asset_target(get_type_id<data_asset_t>(), handle);
            history::watch(world, target);

            std::vector<u32_t> set = asset->field_set;
            const bool changed =
                type->partial ? draw_partial(world, object, meta, 0, set) : panels::draw_fields(world, object);
            if (changed)
            {
                set_held(handle, *held,
                         merge(data::encode(asset->type_hash, value), held->fields, meta, 0,
                               type->partial ? &set : nullptr));
                history::edited(world, target, "Edit Data");
            }

            type->destroy(value);
        }
    } // namespace

    void init(editor_context_t&)
    {
        history::reg_editable(get_type_id<data_asset_t>(), &capture, &apply, &write);
        asset_editors::add({
            .type_id = get_type_id<data_asset_t>(),
            .color = IM_COL32(120, 170, 90, 255),
            .load_when_inspected = true,
            .draw_inspector = &draw_inspector,
        });
        events::on(events::event_e::PROJECT_OPENED, [](world_t&, editor_context_t&) { g_held.clear(); });
    }

    u32_t new_menu(bool enabled)
    {
        u32_t picked = 0;
        std::vector<data::data_type_t> types = data::types();
        if (!ImGui::BeginMenu("Data", enabled && !types.empty())) { return 0; }

        std::ranges::sort(types, [](const data::data_type_t& a, const data::data_type_t& b)
                          { return std::string_view(a.label) < std::string_view(b.label); });
        for (const data::data_type_t& type : types)
        {
            if (ImGui::MenuItem(type.label)) { picked = type.hash; }
        }
        ImGui::EndMenu();
        return picked;
    }

    std::string create(world_t& world, const std::string& dir, u32_t type_hash)
    {
        const std::optional<data::data_type_t> type = data::type_by_hash(type_hash);
        if (!type) { return {}; }

        nlohmann::json fields = nlohmann::json::object();
        if (!type->partial)
        {
            void* value = type->construct();
            fields = data::encode(type_hash, value);
            type->destroy(value);
        }

        const nlohmann::json file = {
            {"guid",   guid::to_string(guid::generate())},
            {"type",   std::string(type->name)          },
            {"fields", std::move(fields)                },
        };
        const std::string label = std::string("Create ") + type->label;
        const std::string rel = asset_files::unique_path(dir, std::string("New ") + type->label, ".data");
        return asset_files::create_text(world, label, rel, file.dump(4) + "\n");
    }

    void report_unknown(const fs::path& root)
    {
        // what -> files, one line each
        std::map<std::string, std::vector<std::string>> files_of;

        std::error_code ec;
        for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root, ec))
        {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".data") { continue; }

            const nlohmann::json source = read_source(entry.path());
            const std::string type_name = source.value("type", std::string{});
            const u32_t hash = hash_string(type_name);
            const std::string rel = entry.path().lexically_relative(root).generic_string();

            if (!data::type_by_hash(hash))
            {
                files_of[std::format("Data asset type '{}' matches no registered data type. Renamed or removed?",
                                     type_name)]
                    .push_back(rel);
                continue;
            }

            const std::vector<scene_property_t> fields =
                data::parse_fields(source.value("fields", nlohmann::json::object()));
            for (const std::string& field : data::unknown_fields(hash, fields))
            {
                files_of[std::format("Saved field '{}' of data type '{}' matches no field, kept as saved. Renamed? "
                                     "stable_id{{\"{}\"}} loads it",
                                     field, type_name, field.substr(field.find_last_of('.') + 1))]
                    .push_back(rel);
            }
        }

        for (const auto& [what, files] : files_of)
        {
            std::string where = files.front();
            for (std::size_t i = 1; i < std::min<std::size_t>(files.size(), 3); i++) { where += ", " + files[i]; }
            if (files.size() > 3) { where += std::format(" and {} more", files.size() - 3); }
            TAU_LOG_WARN("DATA", "{} ({})", what, where);
        }
    }
} // namespace tau::editor::data_edit
