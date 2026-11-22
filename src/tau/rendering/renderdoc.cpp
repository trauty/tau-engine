#include "renderdoc.h"

#include "tau/log.h"
#include "tau/os.h"

#include <renderdoc_app.h>

namespace tau::renderer::renderdoc
{
    namespace
    {
#ifdef _WIN32
        constexpr const char* LIB_NAME = "renderdoc.dll";
#else
        constexpr const char* LIB_NAME = "librenderdoc.so";
#endif

        RENDERDOC_API_1_6_0* g_api = nullptr;
        bool g_requested = false;
        bool g_capturing = false;
        std::string g_last;
    } // namespace

    bool init(bool load, const std::string& capture_template)
    {
        os::lib_handle_t lib = os::find_loaded_lib(LIB_NAME);
        if (lib == nullptr && load) { lib = os::load_lib(LIB_NAME); }
        if (lib == nullptr)
        {
            if (load) { TAU_LOG_WARN("RENDERDOC", "{} not found", LIB_NAME); }
            return false;
        }

        const auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(os::get_proc_address(lib, "RENDERDOC_GetAPI"));
        if (get_api == nullptr || get_api(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&g_api)) != 1)
        {
            g_api = nullptr;
            TAU_LOG_WARN("RENDERDOC", "{} has no usable in-application API", LIB_NAME);
            return false;
        }

        if (!capture_template.empty()) { g_api->SetCaptureFilePathTemplate(capture_template.c_str()); }
        g_api->MaskOverlayBits(eRENDERDOC_Overlay_None, 0);

        int major = 0, minor = 0, patch = 0;
        g_api->GetAPIVersion(&major, &minor, &patch);
        TAU_LOG_INFO("RENDERDOC", "API {}.{}.{}, captures to {}", major, minor, patch,
                     g_api->GetCaptureFilePathTemplate());
        return true;
    }

    bool active() { return g_api != nullptr; }

    void capture_next_frame()
    {
        if (g_api != nullptr) { g_requested = true; }
    }

    std::string last_capture() { return g_last; }

    void frame_begin()
    {
        if (!g_requested || g_capturing) { return; }
        g_requested = false;
        g_capturing = true;
        g_api->StartFrameCapture(nullptr, nullptr);
    }

    void frame_end()
    {
        if (!g_capturing) { return; }
        g_capturing = false;

        if (!g_api->EndFrameCapture(nullptr, nullptr))
        {
            TAU_LOG_ERROR("RENDERDOC", "Capture failed");
            return;
        }

        char path[1024] = {};
        u32_t length = sizeof(path);
        const u32_t count = g_api->GetNumCaptures();
        if (count > 0 && g_api->GetCapture(count - 1, path, &length, nullptr))
        {
            g_last = path;
            TAU_LOG_INFO("RENDERDOC", "Wrote {}", g_last);
        }
    }
} // namespace tau::renderer::renderdoc
