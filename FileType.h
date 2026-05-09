#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

#include "Util.h"

enum fileType : uint8_t
{
	UnknownCAD = 0,
	CogniteCIFF,
	AutodeskFBX,
	AvevaRVM,
	Falcon3D,
	WavefrontOBJ,
	KhronosGLTF,
	HierarchyTXT,
	HierarchyJSON,
	GeometryData
};

inline fileType sourceType(const std::string& file)
{
	const auto ext = ::tolower(fileExtension(file));

	if (ext == ".ciff")
		return CogniteCIFF;

	return UnknownCAD;
}

inline fileType targetType(const std::string& file)
{
	const auto ext = ::tolower(fileExtension(file));

	if (ext == ".ciff")
		return CogniteCIFF;

	if (ext == ".fbx")
		return AutodeskFBX;

	if (ext == ".rvm")
		return AvevaRVM;

	if (ext == ".3d")
		return Falcon3D;

	if (ext == ".obj")
		return WavefrontOBJ;

	if (ext == ".gltf")
		return KhronosGLTF;

	if (ext == ".txt")
		return HierarchyTXT;

	if (ext == ".json")
		return HierarchyJSON;

	if (ext == ".dat")
		return GeometryData;

	return UnknownCAD;
}

inline fileType type(const std::string& file, const bool source)
{
	return source ? sourceType(file) : targetType(file);
}

inline std::string mode(const std::string& file, const bool source)
{
	switch (type(file, source))
	{
	case CogniteCIFF:
	case AutodeskFBX:
	case AvevaRVM:
	case Falcon3D:
		return source ? "rb" : "wb";
	case WavefrontOBJ:
	case KhronosGLTF:
	case HierarchyTXT:
	case HierarchyJSON:
	case GeometryData:
		return source ? "r" : "w";
	case UnknownCAD:
		return {};
	default:
		throw std::runtime_error("Unknown CAD type");
	}
}
