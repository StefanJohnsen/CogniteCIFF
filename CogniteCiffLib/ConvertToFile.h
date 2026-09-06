#pragma once

#include <filesystem>

namespace ciff
{
    int ConvertToFile(
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& targetPath,
        bool async,
        bool bar,
        bool statistics,
        bool speedtest);
}
