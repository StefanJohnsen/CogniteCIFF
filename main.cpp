#include <filesystem>
#include <exception>
#include <iostream>
#include <new>
#include <unordered_set>
#include <vector>

#include "CadCast.h"
#include "CmdArgs.h"
#include "CmdBar.h"
#include "CmdTimer.h"

#include "FileType.h"

#include "WriteStatistics.h"

namespace cmd = ciff::cmd;
namespace bar = ciff::bar;
namespace statistics = ciff::statistics;
using ciff::CogniteCIFF;
using ciff::sourceType;
using ciff::targetType;

static std::vector<std::filesystem::path> enumAllFilesInDirectory(const std::filesystem::path& directory)
{
	std::vector<std::filesystem::path> files;

	for (const auto& entry : std::filesystem::recursive_directory_iterator(directory))
	{
		if (!entry.is_regular_file())
			continue;

		files.emplace_back(entry.path());
	}

	return files;
}

static std::string normalizedPathKey(const std::filesystem::path& value)
{
    std::error_code ec;
    auto path = std::filesystem::weakly_canonical(value, ec);

    if (ec)
        path = std::filesystem::absolute(value);

    auto key = path.lexically_normal().generic_string();
#if defined(_WIN32)
    key = cmd::tolower(std::move(key));
#endif
    return key;
}

static bool convert(const std::string& source_cad, const std::string& target_cad)
{
	if (cmd::bar)
		std::cout << std::endl;

	if (sourceType(source_cad) != CogniteCIFF)
		throw std::logic_error("Unsupported source type");

    try
    {
        ciff::Read data(source_cad, target_cad);
        data.load();

        const bool result = ciff::ConvertToFile(targetType(target_cad), data);
        statistics::print(data);
        return result;
    }
    catch (const std::bad_alloc&)
    {
        std::cerr << "CIFF conversion requires more memory than is currently available." << std::endl;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "CIFF conversion failed: " << exception.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "CIFF conversion failed with an unexpected error." << std::endl;
    }

    return false;
}

static bool evaluateArg()
{
	const bool targetIsDirectory = !cmd::target.empty() && std::filesystem::is_directory(cmd::target);
	const auto effectiveTargetExtension = cmd::target.empty() || targetIsDirectory
		? "." + cmd::defaultTargetExt()
		: cmd::tolower(cmd::target.extension().string());

	if (cmd::async)
	{
		if (effectiveTargetExtension != ".ciff" && effectiveTargetExtension != ".obj" && effectiveTargetExtension != ".3d")
		{
			cmd::err("Async is only supported for ciff, obj and 3d target formats.");
			return false;
		}
	}

    if (cmd::mesh && effectiveTargetExtension != ".ciff")
    {
        cmd::err("Mesh is only supported for ciff target format.");
        return false;
    }

	return true;
}

static int main_single()
{
	bar::idle(!cmd::bar);

	const auto& source = cmd::source;
	const auto& target = cmd::target;

	std::cout << "\nConverting " << source.filename().string() << " to " << target.filename().string();
    if (cmd::async)
        std::cout << " (async processing)";
    std::cout << '\n';

	const auto converted = ::convert(source.string(), target.string());

	std::cout << '\n';

	if (converted)
		std::cout << source.string() << " has been converted to " << target.string() << " in " << timer::stop() << std::endl;
	else
		std::cout << source.string() << " could not be converted to " << target.string() << std::endl;

	return converted ? 0 : 1;
}

static int main_multi()
{
	using namespace std::filesystem;

	bar::idle();

	const auto source_ext = "." + cmd::defaultExt(cmd::source_ext);
	const auto target_ext = "." + cmd::defaultTargetExt();
	const auto files = enumAllFilesInDirectory(cmd::source);
	const auto total_time = timer::now();

	std::vector<path> sourceFiles;
	sourceFiles.reserve(files.size());

	for (const auto& file : files)
	{
		if (cmd::tolower(file.extension().string()) != source_ext)
			continue;

		sourceFiles.emplace_back(file);
	}

    if (sourceFiles.empty())
    {
        cmd::err("No CIFF source files found in directory: " + cmd::source.string());
        return 1;
    }

    std::unordered_set<std::string> sourceKeys;
    std::unordered_set<std::string> targetKeys;
    std::vector<path> targets;
    sourceKeys.reserve(sourceFiles.size());
    targetKeys.reserve(sourceFiles.size());
    targets.reserve(sourceFiles.size());

    for (const auto& source : sourceFiles)
        sourceKeys.emplace(normalizedPathKey(source));

    for (const auto& source : sourceFiles)
    {
        auto target = source;
        target.replace_extension(target_ext);

        if (!cmd::target.empty())
            target = cmd::target / target.filename();

        const auto targetKey = normalizedPathKey(target);

        if (sourceKeys.contains(targetKey))
        {
            cmd::err("Batch target would overwrite a CIFF source file: " + target.string());
            return 1;
        }
        if (!targetKeys.emplace(targetKey).second)
        {
            cmd::err("Multiple CIFF source files resolve to the same batch target: " + target.string());
            return 1;
        }

        targets.emplace_back(std::move(target));
    }

	size_t okCount = 0;
	size_t failCount = 0;

	for (size_t index = 0; index < sourceFiles.size(); ++index)
	{
		timer::start();
        const auto& source = sourceFiles[index];
        const auto& target = targets[index];

		std::cout << source.string() << " is parsing ... ";

		if (::convert(source.string(), target.string()))
		{
			++okCount;
			std::cout << " has been converted in " << timer::stop() << std::endl;
		}
		else
		{
			++failCount;
			std::cout << " could not be converted" << std::endl;
		}
	}

	std::cout << std::endl
		<< "TOTAL FILES : " << sourceFiles.size() << " ( " << timer::stop(total_time) << " )"
		<< std::endl;

	if (failCount == 0)
		std::cout << "All files successfully converted to " << target_ext;
	else
		std::cout << okCount << " successfully converted, " << failCount << " failed";

    if (cmd::async)
        std::cout << " (async processing)";

	std::cout << std::endl << std::endl;

	return failCount == 0 ? 0 : 1;
}

int main(const int argc, char* argv[])
{
	timer::start();

	if (!cmd::parse(argc, argv))
		return 1;

	if (!evaluateArg())
		return 1;

	if (std::filesystem::is_directory(cmd::source))
		return main_multi();

	return main_single();
}
