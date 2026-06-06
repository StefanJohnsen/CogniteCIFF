#pragma once

#include <cstddef>
#include <stdexcept>

#include "PrimitivesCIFF.h"
#include "ReadCIFF.h"
#include "ShapeCIFF.h"
#include "TessCIFF.h"

namespace ciff
{
	template <typename PrimitiveT>
	inline Mesh TessellateCanonical(const PrimitiveT& primitive)
	{
		return tess::tessellate(shape::canonicalize(primitive));
	}

	template <typename PrimitiveT>
	inline Mesh TessellateWorld(const PrimitiveT& primitive)
	{
		auto mesh = TessellateCanonical(primitive);
		tess::transform(mesh, shape::instanceTransform(primitive));
		return mesh;
	}

	inline Mesh TessellateGeometry(const Read& data, const size_t geometryIndex)
	{
		const auto& geometry = data.getGeometry(geometryIndex);

		Mesh mesh;
		switch (geometry.primitive)
		{
		case Type::Mesh:
			mesh = data.getMesh(geometry.mesh);
			break;
		case Type::Box:
			mesh = TessellateWorld(data.boxes[geometry.primitiveIndex]);
			break;
		case Type::Cylinder:
			mesh = TessellateWorld(data.cylinders[geometry.primitiveIndex]);
			break;
		case Type::CircularTorus:
			mesh = TessellateWorld(data.circularToruses[geometry.primitiveIndex]);
			break;
		case Type::Sphere:
			mesh = TessellateWorld(data.spheres[geometry.primitiveIndex]);
			break;
		case Type::SphericalDish:
			mesh = TessellateWorld(data.sphericalDishes[geometry.primitiveIndex]);
			break;
		case Type::GeneralCylinder:
			mesh = TessellateWorld(data.generalCylinders[geometry.primitiveIndex]);
			break;
		default:
			throw std::runtime_error("CIFF: cannot tessellate unsupported geometry type");
		}

		mesh.color = geometry.color;
		return mesh;
	}
} // namespace ciff
