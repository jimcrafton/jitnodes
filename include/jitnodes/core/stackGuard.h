#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>

namespace jitnodes {

// Bytes left on the calling thread's stack (the largest value when the platform cannot say). Recursive code checks this instead of
// counting levels, because the size of a frame differs between debug, release and sanitizer builds.
size_t stackHeadroom();

inline constexpr size_t minimumStackHeadroom = 192 * 1024;

inline void requireStack(const char* what) {
    if (stackHeadroom() < minimumStackHeadroom) throw std::runtime_error(std::string(what) + " is nested too deeply for the stack");
}

}  // namespace jitnodes
