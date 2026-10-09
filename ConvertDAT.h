#pragma once

#include <cstdint>

#include "Convert.h"
#include "Write.h"
#include "ProcessCIFF.h"

namespace ciff::data
{
	using ciff::write;
	// Raw geometry dump:
	//   uint32 nodeCount
	//   per node:
	//     uint32 nameLength + bytes
	//     uint32 meshCount
	//     per mesh:
	//       uint32 pointCount
	//       uint32 triangleCount
	//       double[3*pointCount] vertices
	//       uint32[3*triangleCount] indices
	struct Convert final : ciff::Convert
	{
		explicit Convert(ciff::Read& data) : ciff::Convert(data)
		{
		}

		void WriteHeader() override
		{
			data::write(write, static_cast<uint32_t>(data.nodes.size()));
		}

		void WriteNode(const ciff::Node& node) override
		{
			const auto length = static_cast<uint32_t>(node.name.length());
			data::write(write, length);
			data::write(write, node.name);
			data::write(write, static_cast<uint32_t>(node.geometries.size()));
		}

		void WriteGeometry(const ciff::Node&, const size_t geometryIndex) override
		{
			const auto mesh = ciff::TessellateGeometry(data, geometryIndex);
			data::write(write, mesh.points());
			data::write(write, mesh.triangles());
			data::write(write, reinterpret_cast<const char*>(mesh.vertices.data()),
				mesh.vertices.size() * sizeof(ciff::Point));
			data::write(write, mesh.indices.data(), mesh.indices.size());
		}

		void WriteMaterial(bool) override
		{
		}

		void WriteFooter() override
		{
		}
	};

	inline bool convert(ciff::Read& data)
	{
		return Convert(data).run();
	}
}
