#pragma once

#include "defines.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// the address the current function returns to, inside its caller
// a registration API records it to learn which module (engine or game library) registered
#if defined(_MSC_VER)
    #include <intrin.h>
    #define TAU_CALLER_ADDRESS() _ReturnAddress()
    #define TAU_NOINLINE __declspec(noinline)
#else
    #define TAU_CALLER_ADDRESS() __builtin_return_address(0)
    #define TAU_NOINLINE __attribute__((noinline))
#endif

namespace tau::os
{
    using lib_handle_t = void*;

    TAU_ENGINE_API lib_handle_t load_lib(const char* path);
    // only if already in the process (injected), never loads
    TAU_ENGINE_API lib_handle_t find_loaded_lib(const char* name);
    TAU_ENGINE_API void* get_proc_address(lib_handle_t lib, const char* func_name);
    TAU_ENGINE_API void free_lib(lib_handle_t lib);
    TAU_ENGINE_API std::string lib_error();

    // the module (executable or shared library) an address lies in, named by its image base, or nullptr
    // equal results mean the same module, which is what unloading a library must know about anything pointing at it
    TAU_ENGINE_API void* module_base_of(const void* address);

    // the same name for a library opened with load_lib
    TAU_ENGINE_API void* module_base_of_lib(lib_handle_t lib);

    struct process_result_t
    {
        bool started = false;
        i32 exit_code = -1;
        std::string output;     // stdout and stderr, in the order they were written
        bool cancelled = false; // stopped through a process_cancel_t rather than finishing
    };

    // lets another thread stop a run_process call. cancel() ends the child and everything it started
    // (xmake's compilers would otherwise hold the output pipe open and run_process would not return)
    // safe before, during or after the run, reset() arms it again
    class process_cancel_t
    {
      public:
        TAU_ENGINE_API void cancel();
        TAU_ENGINE_API void reset();
        bool requested() const { return m_requested.load(); }

        // run_process's side: the group the child was started in (job object on Windows, process group elsewhere)
        // or the child itself when no group could be made
        TAU_ENGINE_API void attach(void* group, bool whole_group);
        TAU_ENGINE_API void detach();

      private:
        std::mutex m_mutex;
        std::atomic<bool> m_requested = false;
        void* m_group = nullptr;
        bool m_whole_group = false;
    };

    // runs `exe` with `args` and waits, with no shell in between
    // std::system hands `cmd /c` a quoted string, so a caller that also quotes the path gets a doubled quote
    // argv straight to the OS has no quoting rules, opens no console window and needs no temp file
    // a bare `exe` is looked up on PATH, `working_dir` is the child's cwd (xmake writes state where it runs)
    // `on_line` gets each output line as printed, without its ending, on the calling thread. full output still returns
    using output_line_fn = std::function<void(std::string_view line)>;

    // `cancel`, when given, can stop the run from another thread, see process_cancel_t
    TAU_ENGINE_API process_result_t run_process(const std::string& exe, const std::vector<std::string>& args,
                                                const std::string& working_dir = {}, const output_line_fn& on_line = {},
                                                process_cancel_t* cancel = nullptr);
} // namespace tau::os
