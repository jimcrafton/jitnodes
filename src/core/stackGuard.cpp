#include "jitnodes/core/stackGuard.h"

#include <cstdint>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace jitnodes {

size_t stackHeadroom() {
    thread_local uintptr_t low = 0;
    thread_local bool known = false;
    if (!known) {
        known = true;
#ifdef _WIN32
        ULONG_PTR lo = 0, hi = 0;
        GetCurrentThreadStackLimits(&lo, &hi);
        low = static_cast<uintptr_t>(lo);
#else
        pthread_attr_t attr;
        if (pthread_getattr_np(pthread_self(), &attr) == 0) {
            void* addr = nullptr;
            size_t size = 0;
            if (pthread_attr_getstack(&attr, &addr, &size) == 0) low = reinterpret_cast<uintptr_t>(addr);
            pthread_attr_destroy(&attr);
        }
#endif
    }
    if (low == 0) return SIZE_MAX;
    char marker;
    const uintptr_t here = reinterpret_cast<uintptr_t>(&marker);
    return here > low ? static_cast<size_t>(here - low) : 0;
}

}  // namespace jitnodes
