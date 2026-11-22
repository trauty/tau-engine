#include "gpu_telemetry.h"

#include "fmt/format.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#else
    #include <dirent.h>
    #include <dlfcn.h>
#endif

namespace tau::tools::gpu_telemetry
{
    namespace
    {
        constexpr u32_t VENDOR_NVIDIA = 0x10de;
        constexpr u32_t VENDOR_AMD = 0x1002;
#ifndef _WIN32
        constexpr u32_t VENDOR_INTEL = 0x8086;
#endif

        std::mutex g_mutex;
        sample_t g_latest;
        std::atomic<bool> g_run = false;
        std::thread g_thread;
        std::string g_source = "not started";
        bool g_started = false;

        void* lib_open(const char* name)
        {
#ifdef _WIN32
            return reinterpret_cast<void*>(LoadLibraryA(name));
#else
            return dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
        }

        void* lib_sym(void* lib, const char* name)
        {
#ifdef _WIN32
            return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
            return dlsym(lib, name);
#endif
        }

        template <typename F>
        void load(void* lib, F& out, const char* name)
        { out = reinterpret_cast<F>(lib_sym(lib, name)); }

        // the few NVML calls used, declared here (NVML ships with the driver)
        struct nvml_util_t
        { u32_t gpu, memory; };

        struct nvml_t
        {
            void* lib = nullptr;
            void* dev = nullptr;
            int (*init)() = nullptr;
            int (*shutdown)() = nullptr;
            int (*count)(u32_t*) = nullptr;
            int (*by_index)(u32_t, void**) = nullptr;
            int (*uuid)(void*, char*, u32_t) = nullptr;
            // type 0 graphics, 2 memory
            int (*clock)(void*, int, u32_t*) = nullptr;
            int (*temp)(void*, int, u32_t*) = nullptr;
            // milliwatts
            int (*power)(void*, u32_t*) = nullptr;
            int (*reasons)(void*, unsigned long long*) = nullptr;
            int (*util)(void*, nvml_util_t*) = nullptr;
            int (*driver)(char*, u32_t) = nullptr;
        } g_nvml;

        bool nvml_start(const renderer::device::gpu_identity_t& gpu)
        {
#ifdef _WIN32
            g_nvml.lib = lib_open("nvml.dll");
#else
            g_nvml.lib = lib_open("libnvidia-ml.so.1");
#endif
            if (g_nvml.lib == nullptr)
            {
                g_source = "not available: NVML library not found";
                return false;
            }

            load(g_nvml.lib, g_nvml.init, "nvmlInit_v2");
            load(g_nvml.lib, g_nvml.shutdown, "nvmlShutdown");
            load(g_nvml.lib, g_nvml.count, "nvmlDeviceGetCount_v2");
            load(g_nvml.lib, g_nvml.by_index, "nvmlDeviceGetHandleByIndex_v2");
            load(g_nvml.lib, g_nvml.uuid, "nvmlDeviceGetUUID");
            load(g_nvml.lib, g_nvml.clock, "nvmlDeviceGetClockInfo");
            load(g_nvml.lib, g_nvml.temp, "nvmlDeviceGetTemperature");
            load(g_nvml.lib, g_nvml.power, "nvmlDeviceGetPowerUsage");
            load(g_nvml.lib, g_nvml.reasons, "nvmlDeviceGetCurrentClocksEventReasons");
            if (g_nvml.reasons == nullptr)
            {
                load(g_nvml.lib, g_nvml.reasons, "nvmlDeviceGetCurrentClocksThrottleReasons");
            }
            load(g_nvml.lib, g_nvml.util, "nvmlDeviceGetUtilizationRates");
            load(g_nvml.lib, g_nvml.driver, "nvmlSystemGetDriverVersion");

            if (!g_nvml.init || !g_nvml.count || !g_nvml.by_index || !g_nvml.clock || g_nvml.init() != 0)
            {
                g_source = "not available: NVML did not initialise";
                return false;
            }

            // Vulkan's device UUID is NVML's "GPU-xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
            const u8_t* u = gpu.uuid;
            const std::string want = fmt::format("GPU-{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{"
                                                 ":02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
                                                 u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10],
                                                 u[11], u[12], u[13], u[14], u[15]);

            u32_t n = 0;
            g_nvml.count(&n);
            for (u32_t i = 0; i < n; i++)
            {
                void* d = nullptr;
                char id[96] = {};
                if (g_nvml.by_index(i, &d) != 0) { continue; }
                if (g_nvml.uuid && g_nvml.uuid(d, id, sizeof(id)) == 0 && want == id)
                {
                    g_nvml.dev = d;
                    break;
                }
                if (n == 1) { g_nvml.dev = d; }
            }
            if (g_nvml.dev == nullptr)
            {
                g_source = "not available: no NVML device matches the Vulkan device";
                return false;
            }

            char version[80] = "?";
            if (g_nvml.driver) { g_nvml.driver(version, sizeof(version)); }
            g_source = fmt::format("NVML (NVIDIA driver {})", version);
            return true;
        }

        void nvml_sample(sample_t& s)
        {
            u32_t v = 0;
            if (g_nvml.clock(g_nvml.dev, 0, &v) == 0) { s.gpu_mhz = static_cast<f32>(v); }
            if (g_nvml.clock(g_nvml.dev, 2, &v) == 0) { s.mem_mhz = static_cast<f32>(v); }
            if (g_nvml.temp && g_nvml.temp(g_nvml.dev, 0, &v) == 0) { s.temp_c = static_cast<f32>(v); }
            if (g_nvml.power && g_nvml.power(g_nvml.dev, &v) == 0) { s.power_w = static_cast<f32>(v) * 1e-3f; }

            nvml_util_t util;
            if (g_nvml.util && g_nvml.util(g_nvml.dev, &util) == 0) { s.usage_pct = static_cast<f32>(util.gpu); }

            // 0x4 sw power cap, 0x80 hw power brake; 0x20 / 0x40 thermal, 0x8 hw slowdown
            unsigned long long r = 0;
            if (g_nvml.reasons && g_nvml.reasons(g_nvml.dev, &r) == 0)
            {
                s.power_limited = (r & (0x4ull | 0x80ull)) ? 100.0f : 0.0f;
                s.thermal_limited = (r & (0x20ull | 0x40ull | 0x8ull)) ? 100.0f : 0.0f;
            }
        }

#ifdef _WIN32
        // = adlx_shim.h version 1, built outside this repository
        struct adlx_sample_t
        { f64 gpu_mhz, mem_mhz, temp_c, hotspot_c, power_w, usage_pct; };

        struct adlx_t
        {
            void* lib = nullptr;
            int (*version)() = nullptr;
            int (*init)(u32_t, char*, int) = nullptr;
            int (*sample)(adlx_sample_t*) = nullptr;
            void (*shutdown)() = nullptr;
        } g_adlx;

        bool adlx_start(const renderer::device::gpu_identity_t& gpu)
        {
            // beside the executable, not the working directory
            char path[MAX_PATH] = "adlx_shim.dll";
            const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
            if (length > 0 && length < MAX_PATH)
            {
                if (char* slash = std::strrchr(path, '\\'))
                {
                    std::snprintf(slash + 1, static_cast<size_t>(MAX_PATH - (slash + 1 - path)), "adlx_shim.dll");
                }
            }

            g_adlx.lib = lib_open(path);
            if (g_adlx.lib == nullptr)
            {
                g_source = "not available: adlx_shim.dll not found next to the executable";
                return false;
            }
            load(g_adlx.lib, g_adlx.version, "adlx_shim_version");
            load(g_adlx.lib, g_adlx.init, "adlx_shim_init");
            load(g_adlx.lib, g_adlx.sample, "adlx_shim_sample");
            load(g_adlx.lib, g_adlx.shutdown, "adlx_shim_shutdown");
            if (!g_adlx.version || g_adlx.version() != 1 || !g_adlx.init || !g_adlx.sample)
            {
                g_source = "not available: adlx_shim.dll has another version";
                return false;
            }

            char name[128] = {};
            if (!g_adlx.init(gpu.device_id, name, static_cast<int>(sizeof(name))))
            {
                g_source = "not available: ADLX did not initialise (AMD driver too old, or no AMD GPU)";
                return false;
            }
            g_source = fmt::format("ADLX (adlx_shim.dll): {}", name);
            return true;
        }

        void adlx_sample(sample_t& s)
        {
            adlx_sample_t a;
            if (!g_adlx.sample(&a)) { return; }
            s.gpu_mhz = static_cast<f32>(a.gpu_mhz);
            s.mem_mhz = static_cast<f32>(a.mem_mhz);
            s.temp_c = static_cast<f32>(a.temp_c);
            s.hotspot_c = static_cast<f32>(a.hotspot_c);
            s.power_w = static_cast<f32>(a.power_w);
            s.usage_pct = static_cast<f32>(a.usage_pct);
        }
#else
        // /sys/bus/pci/devices/DDDD:BB:DD.F and its hwmon directory
        std::string g_pci;
        std::string g_hwmon;
        bool g_intel = false;

        bool read_num(const std::string& path, f64& v)
        {
            FILE* f = std::fopen(path.c_str(), "r");
            if (f == nullptr) { return false; }
            const bool ok = std::fscanf(f, "%lf", &v) == 1;
            std::fclose(f);
            return ok;
        }

        // pp_dpm_sclk / pp_dpm_mclk: "1: 2400Mhz *" marks the current level
        bool read_dpm(const std::string& path, f64& mhz)
        {
            FILE* f = std::fopen(path.c_str(), "r");
            if (f == nullptr) { return false; }
            char line[128];
            bool ok = false;
            while (std::fgets(line, sizeof(line), f))
            {
                if (std::strchr(line, '*') == nullptr) { continue; }
                const char* colon = std::strchr(line, ':');
                ok = colon != nullptr && std::sscanf(colon + 1, "%lf", &mhz) == 1;
                break;
            }
            std::fclose(f);
            return ok;
        }

        void sysfs_sample(sample_t& s)
        {
            f64 v = 0.0;
            if (g_intel)
            {
                // xe: tile0/gt0/freq0/act_freq, i915: drm/card*/gt_act_freq_mhz
                if (read_num(g_pci + "/tile0/gt0/freq0/act_freq", v)) { s.gpu_mhz = static_cast<f32>(v); }
                else if (DIR* d = opendir((g_pci + "/drm").c_str()))
                {
                    while (dirent* e = readdir(d))
                    {
                        if (std::strncmp(e->d_name, "card", 4) == 0 &&
                            read_num(g_pci + "/drm/" + e->d_name + "/gt_act_freq_mhz", v))
                        {
                            s.gpu_mhz = static_cast<f32>(v);
                            break;
                        }
                    }
                    closedir(d);
                }
            }
            else
            {
                // hwmon freq in Hz
                if (!g_hwmon.empty() && read_num(g_hwmon + "/freq1_input", v))
                {
                    s.gpu_mhz = static_cast<f32>(v * 1e-6);
                }
                else if (read_dpm(g_pci + "/pp_dpm_sclk", v)) { s.gpu_mhz = static_cast<f32>(v); }
                if (!g_hwmon.empty() && read_num(g_hwmon + "/freq2_input", v))
                {
                    s.mem_mhz = static_cast<f32>(v * 1e-6);
                }
                else if (read_dpm(g_pci + "/pp_dpm_mclk", v)) { s.mem_mhz = static_cast<f32>(v); }
                if (read_num(g_pci + "/gpu_busy_percent", v)) { s.usage_pct = static_cast<f32>(v); }
            }

            // millidegrees, microwatts
            if (!g_hwmon.empty())
            {
                if (read_num(g_hwmon + "/temp1_input", v)) { s.temp_c = static_cast<f32>(v * 1e-3); }
                if (read_num(g_hwmon + "/temp2_input", v)) { s.hotspot_c = static_cast<f32>(v * 1e-3); }
                if (read_num(g_hwmon + "/power1_average", v) || read_num(g_hwmon + "/power1_input", v))
                {
                    s.power_w = static_cast<f32>(v * 1e-6);
                }
            }
        }

        bool sysfs_start(const renderer::device::gpu_identity_t& gpu)
        {
            if (!gpu.has_pci)
            {
                g_source = "not available: the driver gives no PCI address (VK_EXT_pci_bus_info)";
                return false;
            }

            g_pci = fmt::format("/sys/bus/pci/devices/{:04x}:{:02x}:{:02x}.{:x}", gpu.pci_domain, gpu.pci_bus,
                                gpu.pci_device, gpu.pci_function);
            g_intel = gpu.vendor_id == VENDOR_INTEL;
            if (DIR* d = opendir((g_pci + "/hwmon").c_str()))
            {
                while (dirent* e = readdir(d))
                {
                    if (std::strncmp(e->d_name, "hwmon", 5) == 0)
                    {
                        g_hwmon = g_pci + "/hwmon/" + e->d_name;
                        break;
                    }
                }
                closedir(d);
            }

            sample_t probe;
            sysfs_sample(probe);
            if (probe.gpu_mhz < 0.0f && probe.temp_c < 0.0f && probe.power_w < 0.0f)
            {
                g_source = fmt::format("not available: nothing readable under {}", g_pci);
                return false;
            }
            g_source = fmt::format("{} sysfs ({})", g_intel ? "Intel" : "amdgpu", g_pci.substr(21));
            return true;
        }
#endif

        void sampler()
        {
            while (g_run.load())
            {
                sample_t s;
                if (g_nvml.dev != nullptr) { nvml_sample(s); }
#ifdef _WIN32
                else if (g_adlx.lib != nullptr) { adlx_sample(s); }
#else
                else if (!g_pci.empty()) { sysfs_sample(s); }
#endif
                {
                    std::scoped_lock lock(g_mutex);
                    g_latest = s;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    } // namespace

    bool start(const renderer::device::gpu_identity_t& gpu)
    {
        if (g_started) { return g_run.load(); }
        g_started = true;

        bool ok = false;
        if (gpu.vendor_id == VENDOR_NVIDIA) { ok = nvml_start(gpu); }
#ifdef _WIN32
        else if (gpu.vendor_id == VENDOR_AMD) { ok = adlx_start(gpu); }
#else
        else if (gpu.vendor_id == VENDOR_AMD || gpu.vendor_id == VENDOR_INTEL) { ok = sysfs_start(gpu); }
#endif
        else
        {
            g_source = fmt::format("not available for this vendor (0x{:04x})", gpu.vendor_id);
        }
        if (!ok) { return false; }

        g_run = true;
        g_thread = std::thread(sampler);
        return true;
    }

    void stop()
    {
        if (g_run.exchange(false) && g_thread.joinable()) { g_thread.join(); }
        if (g_nvml.lib && g_nvml.shutdown && g_nvml.dev) { g_nvml.shutdown(); }
        g_nvml.dev = nullptr;
#ifdef _WIN32
        if (g_adlx.lib && g_adlx.shutdown) { g_adlx.shutdown(); }
#endif
    }

    sample_t latest()
    {
        std::scoped_lock lock(g_mutex);
        return g_latest;
    }

    const std::string& source() { return g_source; }
} // namespace tau::tools::gpu_telemetry
