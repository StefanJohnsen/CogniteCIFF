#pragma once

#include <filesystem>

namespace ciff
{
    int ConvertToFile(const std::filesystem::path& sourcePath, const std::filesystem::path& targetPath);
}
