#pragma once

#ifdef TAU_ENABLE_PROFILING
    #include <tracy/Tracy.hpp>
#else
    #define FrameMark
    #define ZoneScoped
    #define ZoneScopedN(name)
    #define ZoneText(txt, size)
    #define ZoneScopedC(hexcolor)
    #define TracyMessage(txt, size)
    #define TracyMessageL(txt)
    #define TracyAlloc(ptr, size)
    #define TracyFree(ptr)
#endif