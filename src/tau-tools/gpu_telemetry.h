#pragma once

#include "tau/defines.h"
#include "tau/rendering/device.h"

#include <string>

// clocks, temperature, power, throttling; sampled every 100 ms off the render thread
// NVIDIA: NVML (driver library), AMD / Intel on Linux: sysfs, AMD on Windows: adlx_shim.dll beside the executable
namespace tau::tools::gpu_telemetry
{
    // < 0: not available
    struct sample_t
    {
        f32 gpu_mhz = -1.0f;
        f32 mem_mhz = -1.0f;
        f32 temp_c = -1.0f;
        f32 hotspot_c = -1.0f;
        f32 power_w = -1.0f;
        f32 usage_pct = -1.0f;
        // 100 while capped, 0 otherwise (NVIDIA)
        f32 power_limited = -1.0f;
        f32 thermal_limited = -1.0f;
    };

    // false: nothing to sample, source() says why
    bool start(const renderer::device::gpu_identity_t& gpu);
    void stop();
    sample_t latest();
    const std::string& source();
} // namespace tau::tools::gpu_telemetry
