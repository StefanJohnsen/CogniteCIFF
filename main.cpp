#include <filesystem>
#include <iostream>
#include <vector>

#include "CadCast.h"
#include "CmdArgs.h"
#include "CmdBar.h"
#include "CmdTimer.h"

#include "FileType.h"

#include "ConvertCIFF.h"
#include "ConvertDAT.h"
#include "ConvertFBX.h"
#include "Convert3D.h"
#include "ConvertGLTF.h"
#include "ConvertJSON.h"
#include "ConvertOBJ.h"
#include "ConvertRVM.h"
#include "ConvertTXT.h"
#include "ConvertNWD.h"

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

static bool convert(const std::string& source_cad, const std::string& target_cad)
{
	if (cmd::bar)
		std::cout << std::endl;

	if (sourceType(source_cad) != CogniteCIFF)
		throw std::logic_error("Unsupported source type");

	ciff::Read data(source_cad, target_cad);
	data.load();

	const bool result = ciff::ConvertToFile(targetType(target_cad), data);

	statistics::print(data);
	return result;
}

static bool evaluateArg()
{
	const bool targetIsDirectory = !cmd::target.empty() && std::filesystem::is_directory(cmd::target);

	if (cmd::speedtest)
	{
		cmd::bar = false;
		cmd::statistics = false;
		ciff::WriteBuffer::enabled = false;
	}

	if (cmd::async)
	{
		const auto asyncTargetExtension = cmd::target.empty() || targetIsDirectory
			? "." + cmd::defaultTargetExt()
			: cmd::tolower(cmd::target.extension().string());

		if (asyncTargetExtension != ".ciff" && asyncTargetExtension != ".obj")
		{
			cmd::err("Async conversion is only supported for ciff and obj target formats.");
			return false;
		}
	}

	return true;
}

static int main_single()
{
	bar::idle(!cmd::bar);

	const auto& source = cmd::source;
	const auto& target = cmd::target;

	std::cout << "\nConverting " << source.filename().string() << " to " << target.filename().string() << '\n';

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

	size_t okCount = 0;
	size_t failCount = 0;

	for (const auto& source : sourceFiles)
	{
		timer::start();

		auto target = source;
		target.replace_extension(target_ext);

		if (!cmd::target.empty())
			target = cmd::target / target.filename();

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
