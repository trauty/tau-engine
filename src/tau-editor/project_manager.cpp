#include "tau-editor/project_manager.h"

#include "imgui/imgui.h"
#include "tau-editor/editor_context.h"
#include "tau-editor/panels/console.h"
#include "tau/engine.h"
#include "tau/log.h"
#include "tau/project.h"
#include "tau/window.h"

#include "json/json.hpp"
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace tau::editor::project_manager
{
    namespace
    {
        std::vector<std::string> g_recents;
        bool g_recents_loaded = false;

        std::string recents_file() { return user_dir() + "recent_projects.json"; }

        void load_recents()
        {
            g_recents_loaded = true;
            g_recents.clear();

            const std::string file = recents_file();
            if (file.empty() || !fs::exists(file)) { return; }

            std::ifstream in(file);
            nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
            if (j.is_discarded() || !j.is_array()) { return; }

            for (const auto& e : j)
            {
                if (e.is_string()) { g_recents.push_back(e.get<std::string>()); }
            }
        }

        void save_recents()
        {
            const std::string file = recents_file();
            if (file.empty()) { return; }

            nlohmann::json j = nlohmann::json::array();
            for (const auto& p : g_recents) { j.push_back(p); }

            std::ofstream out(file);
            out << j.dump(4);
        }

        std::string template_dir()
        {
            const char* base = SDL_GetBasePath();
            if (!base) { return ""; }

            std::string bp = base;
            while (bp.size() > 1 && (bp.back() == '/' || bp.back() == '\\')) { bp.pop_back(); }
            const fs::path start(bp);

            const fs::path sdk_tpl = start.parent_path() / "share" / "tau" / "template";
            if (fs::is_directory(sdk_tpl)) { return sdk_tpl.string(); }

            for (fs::path p = start;; p = p.parent_path())
            {
                const fs::path cand = p / "template";
                if (fs::exists(cand / "${NAME}.tauproject")) { return cand.string(); }
                if (!p.has_parent_path() || p.parent_path() == p) { break; }
            }

            return "";
        }

        bool valid_name(const std::string& name)
        {
            if (name.empty()) { return false; }
            return std::all_of(name.begin(), name.end(),
                               [](char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '-'; });
        }

        std::string instantiate_template(const std::string& name, const fs::path& out_dir, std::string& err)
        {
            const std::string tpl = template_dir();
            if (tpl.empty())
            {
                err = "Project template not found (run the editor from a tau-engine build tree or SDK install)";
                return "";
            }

            std::error_code ec;
            if (fs::exists(out_dir) && !fs::is_empty(out_dir, ec))
            {
                err = "Target directory exists and is not empty: " + out_dir.string();
                return "";
            }

            fs::create_directories(out_dir, ec);
            fs::copy(tpl, out_dir, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            if (ec)
            {
                err = "Failed to copy template: " + ec.message();
                return "";
            }

            for (auto it = fs::recursive_directory_iterator(out_dir, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                if (!it->is_regular_file()) { continue; }
                const fs::path f = it->path();

                std::ifstream in(f, std::ios::binary);
                std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                in.close();

                bool changed = false;
                const std::pair<std::string, std::string> tokens[] = {
                    {"${NAME}",           name                                    },
                    {"${ENGINE_VERSION}", tau::major_minor(tau::engine::version())},
                };
                for (const auto& [token, value] : tokens)
                {
                    std::string::size_type pos = 0;
                    while ((pos = content.find(token, pos)) != std::string::npos)
                    {
                        content.replace(pos, token.size(), value);
                        pos += value.size();
                        changed = true;
                    }
                }

                if (changed)
                {
                    std::ofstream out(f, std::ios::binary | std::ios::trunc);
                    out << content;
                }
            }

            const fs::path templated = out_dir / "${NAME}.tauproject";
            const fs::path final_proj = out_dir / (name + ".tauproject");
            if (fs::exists(templated)) { fs::rename(templated, final_proj, ec); }

            if (!fs::exists(final_proj))
            {
                err = "Template produced no .tauproject";
                return "";
            }

            return final_proj.string();
        }

        const char* user_home()
        {
            const char* home = SDL_getenv("HOME");
            if (!home || !*home) { home = SDL_getenv("USERPROFILE"); }
            return (home && *home) ? home : nullptr;
        }

        std::string g_dialog_pick;
        bool g_dialog_ready = false;
        bool g_dialog_open = false;

        std::string g_status;

        void SDLCALL open_project_dialog_cb(void* /*userdata*/, const char* const* filelist, int /*filter*/)
        {
            g_dialog_open = false;
            g_dialog_ready = true;
            g_dialog_pick.clear();
            if (filelist && filelist[0]) { g_dialog_pick = filelist[0]; }
        }
    } // namespace

    bool resolve_project_arg(const std::string& arg, std::string& out)
    {
        if (arg.empty()) { return false; }
        std::error_code ec;
        const fs::path p = arg;

        if (fs::is_regular_file(p, ec) && p.extension() == ".tauproject")
        {
            out = fs::absolute(p, ec).lexically_normal().string();
            return !ec;
        }
        if (fs::is_directory(p, ec))
        {
            fs::path found;
            int count = 0;
            for (auto it = fs::directory_iterator(p, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            {
                if (it->path().extension() == ".tauproject")
                {
                    found = it->path();
                    ++count;
                }
            }
            if (count == 1)
            {
                out = fs::absolute(found, ec).lexically_normal().string();
                return !ec;
            }
        }
        return false;
    }

    void add_recent(const std::string& project_file)
    {
        if (!g_recents_loaded) { load_recents(); }

        std::error_code ec;
        const std::string abs = fs::absolute(project_file, ec).string();
        const std::string key = ec ? project_file : abs;

        g_recents.erase(std::remove(g_recents.begin(), g_recents.end(), key), g_recents.end());
        g_recents.insert(g_recents.begin(), key);
        if (g_recents.size() > 10) { g_recents.resize(10); }

        save_recents();
    }

    bool draw(tau::editor_context_t& ctx, std::string& out_project_file, bool* p_open)
    {
        (void)ctx;
        if (!g_recents_loaded) { load_recents(); }

        bool open_now = false;

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::SetNextWindowViewport(vp->ID);
        const ImGuiWindowFlags pm_flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_NoTitleBar;
        if (!ImGui::Begin("Project Manager", nullptr, pm_flags))
        {
            ImGui::End();
            return false;
        }

        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.6f);
        ImGui::TextUnformatted("tau");
        ImGui::PopFont();

        if (p_open != nullptr)
        {
            const float btn_w = 70.0f;
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - btn_w - ImGui::GetStyle().WindowPadding.x);
            if (ImGui::Button("Close", ImVec2(btn_w, 0.0f))) { *p_open = false; }
        }

        ImGui::TextDisabled("open a project to start editing");
        ImGui::Separator();
        ImGui::Spacing();

        std::string& status = g_status;

        if (g_dialog_ready)
        {
            g_dialog_ready = false;
            if (!g_dialog_pick.empty())
            {
                std::string resolved;
                if (resolve_project_arg(g_dialog_pick, resolved))
                {
                    out_project_file = resolved;
                    open_now = true;
                }
                else
                {
                    status = "Selected file is not a .tauproject";
                }
            }
            g_dialog_pick.clear();
        }

        if (ImGui::CollapsingHeader("New Project", ImGuiTreeNodeFlags_DefaultOpen))
        {
            static char name_buf[128] = "";
            static char loc_buf[1024] = "";
            if (loc_buf[0] == '\0')
            {
                const char* home = user_home();
                if (home) { std::snprintf(loc_buf, sizeof(loc_buf), "%s", home); }
            }

            ImGui::InputText("Name", name_buf, sizeof(name_buf));
            ImGui::InputText("Location", loc_buf, sizeof(loc_buf));

            if (ImGui::Button("Create##new"))
            {
                const std::string name = name_buf;
                if (!valid_name(name)) { status = "Invalid name (use A-Z a-z 0-9 _ -)"; }
                else
                {
                    std::string err;
                    const fs::path out_dir = fs::path(loc_buf) / name;
                    const std::string proj = instantiate_template(name, out_dir, err);
                    if (proj.empty()) { status = "New: " + err; }
                    else
                    {
                        add_recent(proj);
                        status = "Created " + proj + ", open it to build and load";
                        name_buf[0] = '\0';
                    }
                }
            }
        }

        if (ImGui::CollapsingHeader("Open Project", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::BeginDisabled(g_dialog_open);
            if (ImGui::Button("Open Project..."))
            {
                static const SDL_DialogFileFilter filters[] = {
                    {"tau project", "tauproject"},
                };
                g_dialog_open = true;
                g_dialog_ready = false;
                SDL_ShowOpenFileDialog(open_project_dialog_cb, nullptr, tau::window::get_window(), filters, 1, nullptr,
                                       false);
            }
            ImGui::EndDisabled();
        }

        if (ImGui::CollapsingHeader("Recent Projects", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (g_recents.empty()) { ImGui::TextDisabled("No recent projects"); }

            int remove_idx = -1;
            for (int i = 0; i < (int)g_recents.size(); ++i)
            {
                const std::string& p = g_recents[i];
                const bool exists = fs::exists(p);

                ImGui::PushID(i);
                if (ImGui::SmallButton("x")) { remove_idx = i; }
                ImGui::SameLine();

                if (!exists) { ImGui::BeginDisabled(); }
                if (ImGui::Selectable(p.c_str()) && exists)
                {
                    out_project_file = p;
                    open_now = true;
                }
                if (!exists) { ImGui::EndDisabled(); }
                ImGui::PopID();
            }

            if (remove_idx >= 0)
            {
                g_recents.erase(g_recents.begin() + remove_idx);
                save_recents();
            }
        }

        if (!status.empty())
        {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextWrapped("%s", status.c_str());
        }

        ImGui::End();

        return open_now;
    }

    void report_status(const std::string& message) { g_status = message; }

    std::string engine_dir()
    {
        const char* base = SDL_GetBasePath();
        if (!base) { return ""; }

        std::string bp = base;
        while (bp.size() > 1 && (bp.back() == '/' || bp.back() == '\\')) { bp.pop_back(); }
        const fs::path start(bp);

        if (fs::is_directory(start.parent_path() / "share" / "tau" / "rules")) { return start.parent_path().string(); }

        for (fs::path p = start;; p = p.parent_path())
        {
            if (fs::exists(p / "xmake.lua") && fs::is_directory(p / "template")) { return p.string(); }
            if (!p.has_parent_path() || p.parent_path() == p) { break; }
        }

        return "";
    }

    namespace
    {
        bool begin_full_window(const char* name, const char* activity)
        {
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(vp->WorkPos);
            ImGui::SetNextWindowSize(vp->WorkSize);
            ImGui::SetNextWindowViewport(vp->ID);
            const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoTitleBar;
            if (!ImGui::Begin(name, nullptr, flags)) { return false; }

            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.6f);
            ImGui::TextUnformatted("tau");
            ImGui::PopFont();
            ImGui::TextDisabled("%s", activity);
            ImGui::Separator();
            ImGui::Spacing();
            return true;
        }
    } // namespace

    bool draw_waiting(const char* activity, const std::string& line, bool cancelling)
    {
        bool cancel = false;

        if (begin_full_window("Building", activity))
        {
            ImGui::TextUnformatted(line.c_str());
            ImGui::Spacing();

            const float button_width = 90.0f;
            const float bar_width = ImGui::GetContentRegionAvail().x - button_width - ImGui::GetStyle().ItemSpacing.x;
            ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(bar_width, 0.0f),
                               cancelling ? "cancelling..." : nullptr);
            ImGui::SameLine();
            ImGui::BeginDisabled(cancelling);
            if (ImGui::Button("Cancel", ImVec2(button_width, 0.0f))) { cancel = true; }
            ImGui::EndDisabled();

            ImGui::Spacing();
            ImGui::Separator();

            panels::draw_log_view();
        }
        ImGui::End();

        return cancel;
    }

    answer_e draw_question(const char* activity, const std::string& question, const char* yes, const char* no)
    {
        answer_e answer = answer_e::NONE;

        if (begin_full_window("Question", activity))
        {
            ImGui::TextWrapped("%s", question.c_str());
            ImGui::Spacing();

            if (ImGui::Button(yes)) { answer = answer_e::YES; }
            ImGui::SameLine();
            if (ImGui::Button(no)) { answer = answer_e::NO; }

            ImGui::Spacing();
            ImGui::Separator();

            panels::draw_log_view();
        }
        ImGui::End();

        return answer;
    }

    std::string user_dir()
    {
        namespace fs = std::filesystem;

        std::string root = engine_dir();
        if (root.empty())
        {
            const char* base = SDL_GetBasePath();
            root = base ? base : ".";
        }

        const fs::path dir = fs::path(root) / "user";
        std::error_code ec;
        fs::create_directories(dir, ec);
        return dir.generic_string() + "/";
    }
} // namespace tau::editor::project_manager