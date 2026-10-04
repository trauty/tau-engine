#pragma once

#include <string>

namespace tau { struct editor_context_t; }

namespace tau::editor::project_manager
{
    bool draw(tau::editor_context_t& ctx, std::string& out_project_file, bool* p_open);

    // full window wait screen while a project builds and cooks, `line` names the step, the log shows the work
    // returns true the frame Cancel is pressed, `cancelling` greys the button out afterwards
    bool draw_waiting(const char* activity, const std::string& line, bool cancelling);

    enum class answer_e
    {
        NONE,
        YES,
        NO,
    };

    // full window question in the same style, NONE until a button is pressed
    answer_e draw_question(const char* activity, const std::string& question, const char* yes, const char* no);

    void report_status(const std::string& message);

    std::string engine_dir();

    // a .tauproject file, or a folder holding exactly one
    bool resolve_project_arg(const std::string& arg, std::string& out);

    // the editor's own state, inside its engine folder, with a trailing separator
    std::string user_dir();

    void add_recent(const std::string& project_file);
} // namespace tau::editor::project_manager
