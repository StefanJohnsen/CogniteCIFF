#include "ConvertToFile.h"

#include "../CadCast.h"
#include "../CmdArgs.h"
#include "../Convert3D.h"
#include "../ConvertCIFF.h"
#include "../ConvertDAT.h"
#include "../ConvertFBX.h"
#include "../ConvertGLTF.h"
#include "../ConvertJSON.h"
#include "../ConvertNWD.h"
#include "../ConvertOBJ.h"
#include "../ConvertRVM.h"
#include "../ConvertTXT.h"
#include "../FileType.h"
#include "../ReadCIFF.h"

#include <exception>
#include <iostream>
#include <new>

namespace ciff
{
    int ConvertToFile(const std::filesystem::path& sourcePath, const std::filesystem::path& targetPath)
    {
        try
        {
            if (sourceType(sourcePath.string()) != CogniteCIFF)
            {
                std::cerr << "CogniteCiffLib accepts only .ciff source files." << std::endl;
                return 1;
            }

            const auto target = targetType(targetPath.string());
            cmd::async = target == CogniteCIFF || target == WavefrontOBJ;

            Read data(sourcePath.string(), targetPath.string());
            data.load();

            return ConvertToFile(target, data) ? 0 : 1;
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
