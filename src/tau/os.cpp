#include "tau/os.h"

#include "tau/defines.h"

#include <array>

namespace tau::os
{
    namespace
    {
        // hands complete lines to the caller as output arrives
        // a partial line waits for the next read, or for the end if it is the last
        struct line_feed_t
        {
            const output_line_fn& on_line;
            std::size_t handed_out = 0; // offset of the first byte not yet passed to on_line

            void feed(const std::string& output)
            {
                if (!on_line) { return; }

                std::size_t end = 0;
                while ((end = output.find('\n', handed_out)) != std::string::npos)
                {
                    std::string_view line(output.data() + handed_out, end - handed_out);
                    if (!line.empty() && line.back() == '\r') { line.remove_suffix(1); }
                    on_line(line);
                    handed_out = end + 1;
                }
            }

            void finish(const std::string& output)
            {
                feed(output);
                if (on_line && handed_out < output.size())
                {
                    on_line(std::string_view(output).substr(handed_out));
                    handed_out = output.size();
                }
            }
        };

        void kill_group(void* group, bool whole_group);
    } // namespace

    void process_cancel_t::cancel()
    {
        std::scoped_lock lock(m_mutex);
        m_requested = true;
        if (m_group != nullptr) { kill_group(m_group, m_whole_group); }
    }

    void process_cancel_t::reset()
    {
        std::scoped_lock lock(m_mutex);
        m_requested = false;
    }

    void process_cancel_t::attach(void* group, bool whole_group)
    {
        std::scoped_lock lock(m_mutex);
        m_group = group;
        m_whole_group = whole_group;
        // cancelled before the child even existed: it never gets to run
        if (m_requested) { kill_group(m_group, m_whole_group); }
    }

    void process_cancel_t::detach()
    {
        std::scoped_lock lock(m_mutex);
        m_group = nullptr;
    }
} // namespace tau::os

#if TAU_PLATFORM_WIN
    #include <windows.h>
// untested
namespace tau::os
{
    lib_handle_t load_lib(const char* path) { return (lib_handle_t)LoadLibraryA(path); }

    void* get_proc_address(lib_handle_t lib, const char* func_name)
    { return (void*)GetProcAddress((HMODULE)lib, func_name); }

    void free_lib(lib_handle_t lib) { FreeLibrary((HMODULE)lib); }

    std::string lib_error()
    {
        const DWORD code = GetLastError();
        char* text = nullptr;
        const DWORD size =
            FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           nullptr, code, 0, reinterpret_cast<LPSTR>(&text), 0, nullptr);
        std::string message = size ? std::string(text, size) : "error " + std::to_string(code);
        if (text) { LocalFree(text); }
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) { message.pop_back(); }
        return message;
    }

    void* module_base_of(const void* address)
    {
        HMODULE module = nullptr;
        const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
        if (!GetModuleHandleExA(flags, static_cast<LPCSTR>(address), &module)) { return nullptr; }
        return module; // an HMODULE is its image's base address
    }

    void* module_base_of_lib(lib_handle_t lib) { return lib; }

    namespace
    {
        void kill_group(void* group, bool whole_group)
        {
            if (whole_group) { TerminateJobObject(static_cast<HANDLE>(group), 1); }
            else
            {
                TerminateProcess(static_cast<HANDLE>(group), 1);
            }
        }
    } // namespace

    namespace
    {
        // the command line is one string on Windows but holds only the arguments, the executable is passed
        // separately so its path is never parsed as text. these are the rules CommandLineToArgvW undoes
        void append_arg(std::string& out, const std::string& arg)
        {
            if (!out.empty()) { out += ' '; }

            const bool needs_quotes = arg.empty() || arg.find_first_of(" \t\"") != std::string::npos;
            if (!needs_quotes)
            {
                out += arg;
                return;
            }

            out += '"';
            for (std::size_t i = 0; i < arg.size(); i++)
            {
                std::size_t slashes = 0;
                while (i < arg.size() && arg[i] == '\\')
                {
                    slashes++;
                    i++;
                }

                if (i == arg.size())
                {
                    // trailing backslashes would escape the closing quote
                    out.append(slashes * 2, '\\');
                    break;
                }

                if (arg[i] == '"')
                {
                    out.append(slashes * 2 + 1, '\\');
                    out += '"';
                }
                else
                {
                    out.append(slashes, '\\');
                    out += arg[i];
                }
            }
            out += '"';
        }
    } // namespace

    process_result_t run_process(const std::string& exe, const std::vector<std::string>& args,
                                 const std::string& working_dir, const output_line_fn& on_line,
                                 process_cancel_t* cancel)
    {
        process_result_t result;
        line_feed_t lines{on_line};

        // a path is taken as given, a bare name goes through the normal search
        // which CreateProcess only does when handed no application name
        const bool bare_name = exe.find_first_of("/\\") == std::string::npos;

        SECURITY_ATTRIBUTES sec = {};
        sec.nLength = sizeof(sec);
        sec.bInheritHandle = TRUE;

        HANDLE read_end = nullptr;
        HANDLE write_end = nullptr;
        if (!CreatePipe(&read_end, &write_end, &sec, 0)) { return result; }

        // only the write end crosses into the child
        SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

        // the command line's first token is argv[0] to the child, leave it out and every argument shifts
        std::string command_line;
        append_arg(command_line, exe);
        for (const std::string& arg : args) { append_arg(command_line, arg); }

        STARTUPINFOA startup = {};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = write_end;
        startup.hStdError = write_end;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

        PROCESS_INFORMATION process = {};

        // a cancellable run starts its child suspended inside a job object
        // so the child and everything it starts can be ended together
        HANDLE job = cancel != nullptr ? CreateJobObjectA(nullptr, nullptr) : nullptr;
        const DWORD flags = CREATE_NO_WINDOW | (job != nullptr ? CREATE_SUSPENDED : 0);

        const BOOL ok =
            CreateProcessA(bare_name ? nullptr : exe.c_str(), command_line.data(), nullptr, nullptr, TRUE, flags,
                           nullptr, working_dir.empty() ? nullptr : working_dir.c_str(), &startup, &process);

        CloseHandle(write_end);

        if (!ok)
        {
            CloseHandle(read_end);
            if (job != nullptr) { CloseHandle(job); }
            return result;
        }

        result.started = true;

        if (cancel != nullptr)
        {
            // without a job only the child itself can be stopped, which still beats nothing
            const bool in_job = job != nullptr && AssignProcessToJobObject(job, process.hProcess);
            cancel->attach(in_job ? job : process.hProcess, in_job);
            if (job != nullptr) { ResumeThread(process.hThread); }
        }

        // drain while it runs: a child that fills the pipe blocks until someone reads it
        std::array<char, 4096> buffer;
        DWORD read = 0;
        while (ReadFile(read_end, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read > 0)
        {
            result.output.append(buffer.data(), read);
            lines.feed(result.output);
        }
        lines.finish(result.output);

        CloseHandle(read_end);
        WaitForSingleObject(process.hProcess, INFINITE);

        DWORD code = 0;
        GetExitCodeProcess(process.hProcess, &code);
        result.exit_code = static_cast<i32>(code);

        if (cancel != nullptr)
        {
            cancel->detach();
            result.cancelled = cancel->requested();
        }
        if (job != nullptr) { CloseHandle(job); }

        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);

        return result;
    }
} // namespace tau::os
#elif TAU_PLATFORM_LINUX
    #include <csignal>
    #include <cstdint>
    #include <dlfcn.h>
    #include <fcntl.h>
    #include <link.h>
    #include <sys/wait.h>
    #include <unistd.h>
    #include <vector>
namespace tau::os
{
    lib_handle_t load_lib(const char* path) { return dlopen(path, RTLD_NOW); }

    void* get_proc_address(lib_handle_t lib, const char* func_name) { return dlsym(lib, func_name); }

    void free_lib(lib_handle_t lib) { dlclose(lib); }

    std::string lib_error()
    {
        const char* error = dlerror();
        return error ? error : "unknown error";
    }

    void* module_base_of(const void* address)
    {
        Dl_info info = {};
        return dladdr(address, &info) != 0 ? info.dli_fbase : nullptr;
    }

    void* module_base_of_lib(lib_handle_t lib)
    {
        // a shared object is linked at 0, so where it was loaded is its base address
        link_map* map = nullptr;
        if (dlinfo(lib, RTLD_DI_LINKMAP, &map) != 0 || map == nullptr) { return nullptr; }
        return reinterpret_cast<void*>(map->l_addr);
    }

    namespace
    {
        void kill_group(void* group, bool whole_group)
        {
            const pid_t pid = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(group));
            kill(whole_group ? -pid : pid, SIGTERM);
        }
    } // namespace

    process_result_t run_process(const std::string& exe, const std::vector<std::string>& args,
                                 const std::string& working_dir, const output_line_fn& on_line,
                                 process_cancel_t* cancel)
    {
        process_result_t result;
        line_feed_t lines{on_line};

        // close-on-exec, else a child forked meanwhile by another thread inherits the write end
        // and read() below sees no EOF until that unrelated child and everything it started exit
        // dup2 clears the flag on the child's stdout/stderr, so its own output still arrives
        int pipe_fds[2] = {-1, -1};
        if (pipe2(pipe_fds, O_CLOEXEC) != 0) { return result; }

        const pid_t pid = fork();
        if (pid < 0)
        {
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            return result;
        }

        if (pid == 0)
        {
            // its own process group, so a cancel can signal it and everything it starts at once
            if (cancel != nullptr) { setpgid(0, 0); }

            close(pipe_fds[0]);
            dup2(pipe_fds[1], STDOUT_FILENO);
            dup2(pipe_fds[1], STDERR_FILENO);
            close(pipe_fds[1]);

            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(exe.c_str()));
            for (const std::string& arg : args) { argv.push_back(const_cast<char*>(arg.c_str())); }
            argv.push_back(nullptr);

            if (!working_dir.empty() && chdir(working_dir.c_str()) != 0) { _exit(127); }

            // execvp searches PATH for a bare name and takes a path as given, same as Windows
            execvp(exe.c_str(), argv.data());
            _exit(127); // only reached if exec failed
        }

        close(pipe_fds[1]);
        result.started = true;

        if (cancel != nullptr)
        {
            setpgid(pid, pid); // also from this side: whichever runs first, the group exists
            cancel->attach(reinterpret_cast<void*>(static_cast<std::intptr_t>(pid)), true);
        }

        std::array<char, 4096> buffer;
        ssize_t read_bytes = 0;
        while ((read_bytes = read(pipe_fds[0], buffer.data(), buffer.size())) > 0)
        {
            result.output.append(buffer.data(), static_cast<std::size_t>(read_bytes));
            lines.feed(result.output);
        }
        lines.finish(result.output);
        close(pipe_fds[0]);

        int status = 0;
        waitpid(pid, &status, 0);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

        if (cancel != nullptr)
        {
            cancel->detach();
            result.cancelled = cancel->requested();
        }

        return result;
    }
} // namespace tau::os
#endif
