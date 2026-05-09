#include <filesystem>
#include <iostream>
#include <vector>

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

#include "WriteStatistics.h"

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

	bool result = false;

	switch (targetType(target_cad))
	{
	case CogniteCIFF:
		result = ciff::convert(data);
		break;
	case AutodeskFBX:
		result = fbxexport::convert(data);
		break;
	case AvevaRVM:
		result = rvm::convert(data);
		break;
	case Falcon3D:
		result = f3d::convert(data);
		break;
	case WavefrontOBJ:
		result = obj::convert(data);
		break;
	case KhronosGLTF:
		result = gltf::convert(data);
		break;
	case HierarchyTXT:
		result = text::convert(data);
		break;
	case HierarchyJSON:
		result = json::convert(data);
		break;
	case GeometryData:
		result = data::convert(data);
		break;
	default:
		throw std::logic_error("Unsupported target type");
	}

	statistics::print(data);
	return result;
}

static bool evaluateArg()
{
	if (cmd::speedtest)
	{
		cmd::bar = false;
		cmd::statistics = false;
		WriteBuffer::enabled = false;
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
