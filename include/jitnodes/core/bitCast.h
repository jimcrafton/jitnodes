#pragma once

#include <cstring>
#include <type_traits>

namespace jitnodes {

template <class To, class From>
inline To bitCast(const From& from) {
    static_assert(sizeof(To) == sizeof(From) && std::is_trivially_copyable<To>::value && std::is_trivially_copyable<From>::value,
                  "bitCast needs trivially copyable types of the same size");
    To to;
    std::memcpy(&to, &from, sizeof(To));
    return to;
}

}  // namespace jitnodes
