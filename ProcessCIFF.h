#pragma once

#include <cstddef>
#include <vector>

#include "PrimitivesCIFF.h"
#include "ReadCIFF.h"

namespace ciff
{
	// CIFF stores meshes pre-tessellated, so "tessellation" is a
	// pass-through that returns the mesh associated with a geometry.
	inline Mesh TessellateGeometry(const Read& data, const size_t geometryIndex)
	{
		const auto& geometry = data.getGeometry(geometryIndex);
		Mesh mesh = data.getMesh(geometry.mesh);
		mesh.color = geometry.color;
		return mesh;
	}
} // namespace ciff
