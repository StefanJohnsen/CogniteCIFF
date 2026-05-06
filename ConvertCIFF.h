#pragma once

#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "Convert.h"
#include "ProcessCIFF.h"

namespace ciff::clone
{
	using namespace std;

	template <typename T>
	void write(WriteBuffer& write, const T& value)
	{
		write.write(value);
	}

	template <typename T>
	void write(WriteBuffer& write, const vector<T>& values)
	{
		write.write(values.data(), values.size());
	}

	inline void write(WriteBuffer& write, const string& value)
	{
		write.write(static_cast<uint32_t>(value.length()));
		write.write(value);
	}

	inline void write(WriteBuffer& write, const char* value)
	{
		clone::write(write, string(value));
	}

	inline void write(WriteBuffer& write, const ciff::Point& point)
	{
		clone::write(write, point.x);
		clone::write(write, point.y);
		clone::write(write, point.z);
	}

	struct Header
	{
		static void Write(WriteBuffer& write)
		{
			static constexpr uint32_t NativeMagicBytes = MagicBytes;
			static constexpr uint32_t NativeSDKVersion = 4;
			static constexpr uint64_t PlaceHolder1 = 0;
			static constexpr uint64_t PlaceHolder2 = 0;
			static constexpr uint64_t PlaceHolder3 = 0;
			static constexpr uint64_t PlaceHolder4 = 0;
			static constexpr uint32_t MetadataSize = 0;

			clone::write(write, NativeMagicBytes);
			clone::write(write, NativeSDKVersion);
			clone::write(write, PlaceHolder1);
			clone::write(write, PlaceHolder2);
			clone::write(write, PlaceHolder3);
			clone::write(write, PlaceHolder4);
			clone::write(write, MetadataSize);
			clone::write(write, "[]");
		}
	};

	struct Footer
	{
		static void Write(WriteBuffer& write)
		{
			static constexpr uint8_t Type = 0;
			clone::write(write, Type);
		}
	};

	struct Material
	{
		static void Write(ciff::Convert& convert, const size_t colorIndex)
		{
			static constexpr bool HasColor = true;

			auto& write = convert.write;
			const auto& color = convert.data.getColor(colorIndex);

			clone::write(write, HasColor);
			clone::write(write, color.r);
			clone::write(write, color.g);
			clone::write(write, color.b);
			clone::write(write, color.a);
		}
	};

	struct InternalMetadata
	{
		static void Write(WriteBuffer& write)
		{
			static constexpr bool HasInternalMetadata = false;
			clone::write(write, HasInternalMetadata);
		}
	};

	struct MetadataField
	{
		static void Write(WriteBuffer& write, const string& name, const string& value)
		{
			static constexpr uint8_t ValueType = 0;
			static constexpr uint8_t Measurement = 0;

			clone::write(write, name);
			clone::write(write, "");
			clone::write(write, ValueType);
			clone::write(write, Measurement);
			clone::write(write, value);
		}
	};

	struct UserMetadata
	{
		static void WriteRoot(ciff::Convert& convert, const ciff::Node& node)
		{
			auto& write = convert.write;
			clone::write(write, static_cast<uint32_t>(2));

			clone::write(write, "Item");
			clone::write(write, static_cast<uint32_t>(1));
			MetadataField::Write(write, "Name", node.name);

			clone::write(write, "Source");
			clone::write(write, static_cast<uint32_t>(1));
			MetadataField::Write(write, "File", convert.source_file);
		}

		static void WriteNode(ciff::Convert& convert, const ciff::Node& node)
		{
			auto& write = convert.write;
			clone::write(write, static_cast<uint32_t>(1));
			clone::write(write, "Item");
			clone::write(write, static_cast<uint32_t>(1));
			MetadataField::Write(write, "Name", node.name);
		}
	};

	struct Node
	{
		static void Write(ciff::Convert& convert, const ciff::Node& node)
		{
			static constexpr uint8_t Type = 1;
			static constexpr int64_t TreeIndex = -1;
			static constexpr int64_t SubTreeSize = -1;

			auto& write = convert.write;

			clone::write(write, Type);
			clone::write(write, static_cast<uint32_t>(node.childCount));
			clone::write(write, static_cast<int64_t>(convert.nodeIndex));
			clone::write(write, TreeIndex);
			clone::write(write, SubTreeSize);
			clone::write(write, node.name);

			Material::Write(convert, node.color);
			InternalMetadata::Write(write);

			if (convert.nodeIndex == 0)
				UserMetadata::WriteRoot(convert, node);
			else
				UserMetadata::WriteNode(convert, node);

			clone::write(write, static_cast<int64_t>(node.color));
			clone::write(write, static_cast<uint32_t>(node.geometries.size()));
		}
	};

	struct MetadataColor
	{
		static void Write(WriteBuffer& write, const string& color)
		{
			clone::write(write, R"({"DiffuseColor":")" + color + R"("})");
		}
	};

	struct MaterialNode
	{
		static void Write(WriteBuffer& write, const string& color)
		{
			static constexpr uint8_t Type = 19;
			static constexpr uint32_t TextureCount = 0;

			clone::write(write, Type);
			clone::write(write, TextureCount);
			MetadataColor::Write(write, color);
		}
	};

	inline string GetColor(const ciff::rgb& rgb)
	{
		ostringstream oss;
		oss << '#' << hex << nouppercase;
		oss << setw(2) << setfill('0') << static_cast<int>(rgb.r);
		oss << setw(2) << setfill('0') << static_cast<int>(rgb.g);
		oss << setw(2) << setfill('0') << static_cast<int>(rgb.b);
		return oss.str();
	}

	struct Materials
	{
		static void Write(ciff::Convert& convert)
		{
			for (const auto& color : convert.data.colors)
				MaterialNode::Write(convert.write, GetColor(color));
		}
	};

	struct GeometryPart
	{
		static void Write(WriteBuffer& write)
		{
			static constexpr int64_t GeometryReference = -1;
			static constexpr bool HasTransform = false;

			clone::write(write, GeometryReference);
			clone::write(write, HasTransform);
		}
	};

	struct EmptyGeometry
	{
		static void Write(WriteBuffer& write)
		{
			static constexpr uint8_t Type = 23;
			static constexpr uint32_t PointCount = 0;

			GeometryPart::Write(write);
			clone::write(write, Type);
			clone::write(write, PointCount);
		}
	};

	struct MeshGeometry
	{
		static void Write(WriteBuffer& write, const ciff::Mesh& mesh)
		{
			static constexpr uint8_t Type = 3;
			static constexpr uint32_t MeshCount = 1;
			static constexpr uint8_t TextureCount = 0;

			if (mesh.empty())
				return EmptyGeometry::Write(write);

			const auto pointCount = mesh.points();
			const auto triangleCount = mesh.triangles();
			const auto byteCount = DataSize(pointCount, triangleCount);

			GeometryPart::Write(write);

			clone::write(write, Type);
			clone::write(write, MeshCount);
			clone::write(write, byteCount);
			clone::write(write, pointCount);
			clone::write(write, triangleCount);
			clone::write(write, TextureCount);
			clone::write(write, mesh.vertices);
			clone::write(write, mesh.indices);
		}

		static uint32_t DataSize(const uint32_t pointCount, const uint32_t triangleCount) noexcept
		{
			uint32_t byteCount = 2 * sizeof(uint32_t) + sizeof(uint8_t);
			byteCount += 3 * pointCount * static_cast<uint32_t>(sizeof(double));
			byteCount += 3 * triangleCount * static_cast<uint32_t>(sizeof(uint32_t));
			return byteCount;
		}
	};

	struct Convert final : ciff::Convert
	{
		explicit Convert(ciff::Read& data) : ciff::Convert(data)
		{
		}

		void WriteHeader() override
		{
			Header::Write(write);
		}

		void WriteNode(const ciff::Node& node) override
		{
			clone::Node::Write(*this, node);
		}

		void WriteGeometry(const ciff::Node&, const size_t geometryIndex) override
		{
			const auto mesh = TessellateGeometry(data, geometryIndex);
			MeshGeometry::Write(write, mesh);
		}

		void WriteMaterial(bool) override
		{
			Materials::Write(*this);
		}

		void WriteFooter() override
		{
			Footer::Write(write);
		}
	};

	inline bool convert(ciff::Read& data)
	{
		return Convert(data).run();
	}
} // namespace ciff::clone

namespace ciff
{
	inline bool convert(ciff::Read& data)
	{
		return ciff::clone::convert(data);
	}
}
