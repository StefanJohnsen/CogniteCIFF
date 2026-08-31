#pragma once

#include "Convert3D.h"
#include "ConvertCIFF.h"
#include "ConvertDAT.h"
#include "ConvertFBX.h"
#include "ConvertGLTF.h"
#include "ConvertJSON.h"
#include "ConvertNWD.h"
#include "ConvertOBJ.h"
#include "ConvertRVM.h"
#include "ConvertTXT.h"
#include "FileType.h"

#include <iostream>

namespace ciff
{
    inline bool ConvertToFile(const fileType target, Read& data)
    {
        switch (target)
        {
            case CogniteCIFF:
                return ciff::convert(data);
            case AutodeskFBX:
                return fbx::convert(data);
            case AvevaRVM:
                return rvm::convert(data);
            case Falcon3D:
                return f3d::convert(data);
            case WavefrontOBJ:
                return obj::convert(data);
            case KhronosGLTF:
                return gltf::convert(data);
            case HierarchyTXT:
                return text::convert(data);
            case HierarchyJSON:
                return json::convert(data);
            case GeometryDAT:
                return data::convert(data);
            case NavisworksNWD:
                return nwd::convert(data);
            case UnknownCAD:
            default:
                std::cerr << "Unsupported target file extension." << std::endl;
                return false;
        }
    }
}
