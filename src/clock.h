#pragma once

#include <windows.h>

#include <cstdint>

namespace clipper {

inline int64_t QpcFrequency() {
    static const int64_t freq = []() {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? f.QuadPart : 10'000'000LL;
    }();
    return freq;
}

inline int64_t NowQpcTicks() {
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

inline int64_t QpcDeltaToUs(int64_t deltaTicks) {
    const int64_t freq = QpcFrequency();
    int64_t secs = deltaTicks / freq;
    int64_t rem = deltaTicks % freq;
    return secs * 1'000'000LL + (rem * 1'000'000LL) / freq;
}

}
