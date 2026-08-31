#pragma once

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>

namespace ciff::util
{
inline std::string tolower(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(),
		[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	return text;
}

inline std::string fileExtension(const std::string& file)
{
	return std::filesystem::path(file).extension().string();
}

inline std::string fileStem(const std::string& file)
{
	return std::filesystem::path(file).stem().string();
}

inline std::string toUtf8String(const std::filesystem::path& path)
{
	return path.string();
}

inline std::string current_date_time()
{
	std::time_t now = std::time(nullptr);
	std::tm local = {};
#if defined(_WIN32)
	localtime_s(&local, &now);
#else
	localtime_r(&now, &local);
#endif

	std::ostringstream text;
	text << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
	return text.str();
}
}
