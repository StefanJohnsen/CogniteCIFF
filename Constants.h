#pragma once

#include <cstddef>
#include <limits>

inline constexpr double Pi = 3.14159265358979323846;

inline constexpr auto max_size = std::numeric_limits<size_t>::max();
inline constexpr auto max_double = std::numeric_limits<double>::max();
inline constexpr auto min_double = std::numeric_limits<double>::lowest();
