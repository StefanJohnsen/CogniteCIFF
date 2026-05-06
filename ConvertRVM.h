/*----------------------------------------------------------------
  ConvertRVM.h

  Binary AVEVA RVM exporter for the CIFF scene format.
  Emits a valid RVM v2 file with HEAD / MODL / COLR palette /
  CNTB hierarchy / PRIM (FacetGroup) geometry / CNTE / END:
  blocks. CIFF meshes are pre-tessellated triangle soup, so each
  triangle becomes a single 3-vertex Facet polygon inside a
  FacetGroup primitive (one PRIM per CIFF Geometry).

  All scalars except uint8 are written big-endian.
----------------------------------------------------------------*/

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "Convert.h"
#include "ProcessCIFF.h"
#include "Util.h"

namespace rvm
{
	using namespace std;

	template <typename T>
	static array<uint8_t, sizeof(T)> bigEndianBytes(const T& value)
	{
		static_assert(is_trivially_copyable_v<T>, "T must be trivially copyable");

		array<uint8_t, sizeof(T)> bytes = {};
		std::memcpy(bytes.data(), &value, sizeof(T));

		if constexpr (std::endian::native == std::endian::little)
			std::reverse(bytes.begin(), bytes.end());

		return bytes;
	}

	static uint32_t asUint32(const size_t value)
	{
		if (value > numeric_limits<uint32_t>::max())
			throw overflow_error("RVM writer only supports files up to 4GB");

		return static_cast<uint32_t>(value);
	}

	template <typename T>
	static void write(WriteBuffer& write, const T& value)
	{
		static_assert(is_integral_v<T> || is_floating_point_v<T> || is_enum_v<T>,
			"RVM scalar write only supports arithmetic and enum types");

		if constexpr (sizeof(T) == 1)
		{
			write.write(value);
		}
		else
		{
			const auto bytes = bigEndianBytes(value);
			write.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		}
	}

	static void write(WriteBuffer& write, const string& value)
	{
		const auto paddedBytes = ((value.size() + 3ULL) / 4ULL) * 4ULL;
		rvm::write(write, asUint32(paddedBytes / 4ULL));

		if (paddedBytes == 0)
			return;

		write.write(value);

		static constexpr char zero[4] = {};
		write.write(zero, paddedBytes - value.size());
	}

	struct Point3
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
	};

	struct BoundingBox
	{
		Point3 min = {};
		Point3 max = {};
	};

	using Matrix3x4 = array<float, 12>;

	static void write(WriteBuffer& write, const Point3& point)
	{
		rvm::write(write, point.x);
		rvm::write(write, point.y);
		rvm::write(write, point.z);
	}

	static void write(WriteBuffer& write, const BoundingBox& box)
	{
		rvm::write(write, box.min);
		rvm::write(write, box.max);
	}

	static void write(WriteBuffer& write, const Matrix3x4& matrix)
	{
		for (const auto value : matrix)
			rvm::write(write, value);
	}

	static void writeAlpha(WriteBuffer& write, const uint8_t transparency)
	{
		rvm::write(write, transparency);
		rvm::write(write, static_cast<uint8_t>(0));
		rvm::write(write, static_cast<uint8_t>(0));
		rvm::write(write, static_cast<uint8_t>(0));
	}

	template <typename T>
	static size_t overwriteAt(WriteBuffer& write, const size_t position, const T& value)
	{
		static_assert(is_integral_v<T> || is_enum_v<T>, "RVM overwriteAt only supports integral and enum types");

		if constexpr (sizeof(T) == 1)
			return write.overwriteAt(position, value);

		const auto bytes = bigEndianBytes(value);
		return write.overwriteAt(position, bytes);
	}

	struct Block
	{
		static void WriteName(WriteBuffer& write, const char* name)
		{
			array<uint8_t, 16> bytes = {};

			for (size_t index = 0; index < 4; ++index)
				bytes[4 * index + 3] = static_cast<uint8_t>(name[index]);

			write.write(bytes.data(), bytes.size());
		}

		static size_t WriteBegin(WriteBuffer& write, const char* name)
		{
			WriteName(write, name);

			const auto tailPosition = write.tell();

			rvm::write(write, static_cast<uint32_t>(0));
			rvm::write(write, static_cast<uint32_t>(0));

			return tailPosition;
		}

		static void WriteEnd(WriteBuffer& write, const size_t tailPosition)
		{
			rvm::overwriteAt(write, tailPosition, asUint32(write.tell()));
		}
	};

	enum class PrimitiveType : uint32_t
	{
		FacetGroup = 11,
	};

	static Matrix3x4 identityMatrix()
	{
		return Matrix3x4{
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
		};
	}

	static Point3 toPoint(const ciff::Point& p)
	{
		return Point3{
			static_cast<float>(p.x),
			static_cast<float>(p.y),
			static_cast<float>(p.z),
		};
	}

	static BoundingBox computeBoundingBox(const ciff::Mesh& mesh)
	{
		BoundingBox box;
		bool hasPoint = false;

		for (const auto& v : mesh.vertices)
		{
			const auto p = toPoint(v);

			if (!hasPoint)
			{
				box.min = p;
				box.max = p;
				hasPoint = true;
				continue;
			}

			box.min.x = std::min(box.min.x, p.x);
			box.min.y = std::min(box.min.y, p.y);
			box.min.z = std::min(box.min.z, p.z);
			box.max.x = std::max(box.max.x, p.x);
			box.max.y = std::max(box.max.y, p.y);
			box.max.z = std::max(box.max.z, p.z);
		}

		return box;
	}

	static Point3 computeNormal(const ciff::Mesh& mesh, const uint32_t i0, const uint32_t i1, const uint32_t i2)
	{
		const auto a = toPoint(mesh.vertices[i0]);
		const auto b = toPoint(mesh.vertices[i1]);
		const auto c = toPoint(mesh.vertices[i2]);

		const float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
		const float vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;

		Point3 normal{
			uy * vz - uz * vy,
			uz * vx - ux * vz,
			ux * vy - uy * vx,
		};

		const auto length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);

		if (length <= numeric_limits<float>::epsilon())
			return Point3{ 0.0f, 0.0f, 1.0f };

		normal.x /= length;
		normal.y /= length;
		normal.z /= length;
		return normal;
	}

	struct Head
	{
		static void Write(WriteBuffer& write)
		{
			static constexpr uint32_t Version = 2;
			static const string Info = "Falcon Coding CogniteCIFF";
			static const string Note = "";
			static const string User = "";
			static const string Code = "CogniteCIFF";

			const auto tailPosition = Block::WriteBegin(write, "HEAD");

			rvm::write(write, Version);
			rvm::write(write, Info);
			rvm::write(write, Note);
			rvm::write(write, current_date_time());
			rvm::write(write, User);
			rvm::write(write, Code);

			Block::WriteEnd(write, tailPosition);
		}
	};

	struct Model
	{
		static void Write(WriteBuffer& write, const string& name)
		{
			static constexpr uint32_t Version = 1;
			static const string Info = "Falcon Coding CogniteCIFF";

			const auto tailPosition = Block::WriteBegin(write, "MODL");

			rvm::write(write, Version);
			rvm::write(write, Info);
			rvm::write(write, name);

			Block::WriteEnd(write, tailPosition);
		}
	};

	struct Color
	{
		static void Write(WriteBuffer& write, const uint32_t index, const ciff::rgb& color)
		{
			static constexpr uint32_t Type = 0;
			static constexpr uint8_t Flag = 0;

			const auto tailPosition = Block::WriteBegin(write, "COLR");

			rvm::write(write, Type);
			rvm::write(write, index);
			rvm::write(write, color.r);
			rvm::write(write, color.g);
			rvm::write(write, color.b);
			rvm::write(write, Flag);

			Block::WriteEnd(write, tailPosition);
		}
	};

	struct Palette
	{
		static constexpr size_t MaxPaletteColors = 256;

		static void Write(WriteBuffer& write, const vector<ciff::rgb>& colors)
		{
			const auto count = std::min(colors.size(), MaxPaletteColors);

			for (size_t index = 0; index < count; ++index)
				Color::Write(write, asUint32(index), colors[index]);
		}
	};

	struct NodeBlock
	{
		static void Write(WriteBuffer& write, const string& name, const uint32_t material, const uint8_t transparency)
		{
			static constexpr uint32_t Version = 3;
			static constexpr Point3 Translation = {};

			const auto tailPosition = Block::WriteBegin(write, "CNTB");

			rvm::write(write, Version);
			rvm::write(write, name);
			rvm::write(write, Translation);
			rvm::write(write, material);
			writeAlpha(write, transparency);

			Block::WriteEnd(write, tailPosition);
		}
	};

	struct PrimitiveBlock
	{
		static size_t WriteBegin(WriteBuffer& write, const Matrix3x4& transform, const BoundingBox& box)
		{
			static constexpr uint32_t Version = 1;

			const auto tailPosition = Block::WriteBegin(write, "PRIM");

			rvm::write(write, Version);
			rvm::write(write, PrimitiveType::FacetGroup);
			rvm::write(write, transform);
			rvm::write(write, box);

			return tailPosition;
		}

		static void WriteEnd(WriteBuffer& write, const size_t tailPosition)
		{
			Block::WriteEnd(write, tailPosition);
		}
	};

	struct FacetGroupGeometry
	{
		// Each triangle => one Facet that contains a single 3-vertex Polygon.
		static void Write(WriteBuffer& write, const ciff::Mesh& mesh)
		{
			const auto triangleCount = mesh.triangles();

			rvm::write(write, asUint32(triangleCount));

			for (uint32_t t = 0; t < triangleCount; ++t)
			{
				const auto i0 = mesh.indices[3 * t + 0];
				const auto i1 = mesh.indices[3 * t + 1];
				const auto i2 = mesh.indices[3 * t + 2];

				// Facet: 1 polygon
				rvm::write(write, static_cast<uint32_t>(1));

				// Polygon: 3 vertices
				rvm::write(write, static_cast<uint32_t>(3));

				const auto normal = computeNormal(mesh, i0, i1, i2);

				rvm::write(write, toPoint(mesh.vertices[i0]));
				rvm::write(write, normal);
				rvm::write(write, toPoint(mesh.vertices[i1]));
				rvm::write(write, normal);
				rvm::write(write, toPoint(mesh.vertices[i2]));
				rvm::write(write, normal);
			}
		}
	};

	struct ChildEnd
	{
		static void Write(WriteBuffer& write)
		{
			static constexpr uint32_t Version = 1;

			const auto tailPosition = Block::WriteBegin(write, "CNTE");

			rvm::write(write, Version);

			Block::WriteEnd(write, tailPosition);
		}
	};

	struct Footer
	{
		static void Write(WriteBuffer& write)
		{
			Block::WriteName(write, "END:");
			rvm::write(write, static_cast<uint32_t>(0));
			rvm::write(write, static_cast<uint32_t>(0));
			rvm::write(write, static_cast<uint32_t>(0));
		}
	};

	struct Convert final : ciff::Convert
	{
		explicit Convert(ciff::Read& data) : ciff::Convert(data)
		{
		}

		void WriteHeader() override
		{
			openNodes.clear();
			Head::Write(write);
			Model::Write(write, data.nodes.empty() ? fileStem(source_file) : data.nodes.front().name);
			Palette::Write(write, data.colors);
		}

		void WriteNode(const ciff::Node& node) override
		{
			CloseCompletedNodes();

			if (!openNodes.empty())
			{
				if (openNodes.back() == 0)
					throw runtime_error("Invalid CIFF node hierarchy for RVM export");

				--openNodes.back();
			}

			const auto color = node.color < data.colors.size() ? data.colors[node.color] : ciff::rgb{};
			const auto transparency = static_cast<uint8_t>(255 - color.a);

			NodeBlock::Write(write, node.name, asUint32(node.color), transparency);
			openNodes.emplace_back(node.childCount);
		}

		void WriteGeometry(const ciff::Node&, const size_t geometryIndex) override
		{
			const auto mesh = ciff::TessellateGeometry(data, geometryIndex);

			if (mesh.empty())
				return;

			const auto tailPosition = PrimitiveBlock::WriteBegin(write, identityMatrix(), computeBoundingBox(mesh));
			FacetGroupGeometry::Write(write, mesh);
			PrimitiveBlock::WriteEnd(write, tailPosition);
		}

		void WriteMaterial(bool) override
		{
		}

		void WriteFooter() override
		{
			CloseAllNodes();
			Footer::Write(write);
		}

	private:
		void CloseCompletedNodes()
		{
			while (!openNodes.empty() && openNodes.back() == 0)
			{
				ChildEnd::Write(write);
				openNodes.pop_back();
			}
		}

		void CloseAllNodes()
		{
			while (!openNodes.empty())
			{
				ChildEnd::Write(write);
				openNodes.pop_back();
			}
		}

		vector<size_t> openNodes;
	};

	inline bool convert(ciff::Read& data)
	{
		return Convert(data).run();
	}
} // namespace rvm
