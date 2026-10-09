#pragma once
#include <cstddef>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

#include "WriteBuffer.h"

namespace ciff
{
    // Shared byte and text writes; file-format conversions belong in the converter.
    template <typename T> requires(std::is_arithmetic_v<T> || std::is_enum_v<T>)
    void write(WriteBuffer& buffer, const T& value)
    {
        buffer.write(value);
    }

    template <typename T>
    void write(WriteBuffer& buffer, const T* data, const std::size_t elementCount)
    {
        buffer.write(data, elementCount);
    }

    template <typename T>
    void write(WriteBuffer& buffer, const std::vector<T>& values)
    {
        buffer.write(values.data(), values.size());
    }

    template <typename T>
        requires std::is_convertible_v<const T&, std::string_view>
    void write(WriteBuffer& buffer, const T& text)
    {
        const std::string_view view = text;
        buffer.write(view.data(), view.size());
    }

    inline void write(WriteBuffer& buffer, const std::ostringstream& text)
    {
        buffer.write(text.str());
    }

    template <typename... Args>
    void writeFormatted(WriteBuffer& buffer, const char* format, Args... args)
    {
        char text[1024];
        const int length = std::snprintf(text, sizeof(text), format, args...);

        if (length < 0)
            throw std::runtime_error("Error formatting string in WriteBuffer");
        if (static_cast<std::size_t>(length) >= sizeof(text))
            throw std::overflow_error("Formatted string exceeds temporary buffer size");

        buffer.write(text, static_cast<std::size_t>(length));
    }

    template <typename Buffer>
        requires requires(Buffer& target, Buffer& source, bool deleteSource) {
            source.close();
            target.append(source.getFile(), deleteSource);
        }
    void write(Buffer& target, Buffer& source, const bool deleteSource)
    {
        source.close();
        target.append(source.getFile(), deleteSource);
    }
}
