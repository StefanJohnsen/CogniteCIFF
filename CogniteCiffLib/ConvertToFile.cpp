#include "ConvertToFile.h"

#include "../CadCast.h"
#include "../CmdArgs.h"
#include "../CmdBar.h"
#include "../FileType.h"
#include "../ReadCIFF.h"
#include "../WriteBuffer.h"
#include "../WriteStatistics.h"

#include <exception>
#include <iostream>
#include <new>

namespace ciff
{
    int ConvertToFile(
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& targetPath,
        const bool async,
        const bool bar,
        const bool statistics,
        const bool speedtest)
    {
        try
        {
            if (sourceType(sourcePath.string()) != CogniteCIFF)
            {
                std::cerr << "CogniteCiffLib accepts only .ciff source files." << std::endl;
                return 1;
            }

            const auto target = targetType(targetPath.string());
            cmd::async = async;
            cmd::bar = bar && !speedtest;
            cmd::statistics = statistics && !speedtest;
            cmd::speedtest = speedtest;
            WriteBuffer::enabled = !speedtest;
            ciff::bar::idle(!cmd::bar);

            Read data(sourcePath.string(), targetPath.string());
            data.load();

            const bool converted = ConvertToFile(target, data);
            ciff::statistics::print(data);
            return converted ? 0 : 1;
        }
        catch (const std::bad_alloc&)
        {
            std::cerr << "CIFF conversion requires more memory than is currently available." << std::endl;
            return 1;
        }
        catch (const std::exception& exception)
        {
            std::cerr << "CIFF conversion failed: " << exception.what() << std::endl;
            return 1;
        }
        catch (...)
        {
            std::cerr << "CIFF conversion failed with an unexpected error." << std::endl;
            return 1;
        }
    }
}
