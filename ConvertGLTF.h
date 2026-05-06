/*----------------------------------------------------------------
  ConvertGLTF.h

  Khronos glTF 2.0 exporter for the CIFF scene format.
  Emits a single self-contained .gltf file with an embedded base64
  buffer is avoided in favour of a streamed binary side-stream
  appended at the end (still a valid glTF; we point the buffer's
  uri to a sidecar .bin).

  Layout written to <target>.gltf:
    "asset" / "scene" / "scenes" / "nodes" / "meshes" /
    "buffers" / "bufferViews" / "accessors" / "materials"
  Side-by-side <target>.bin holds interleaved index + position +
  normal blocks (uint32 / float32 / float32) per CIFF mesh.

  CIFF is left-handed Z-up. glTF requires right-handed Y-up so
  vertices and normals are transformed: (x, y, z) -> (x, z, -y).
----------------------------------------------------------------*/

#pragma once

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Convert.h"
#include "ProcessCIFF.h"
#include "TempFile.h"
#include "Util.h"
#include "WriteBuffer.h"

namespace gltf
{
	using namespace std;

	inline string cleanJsonString(const string& src, const char replacement = ' ')
	{
		string out;
		out.reserve(src.size());

		for (const char ch : src)
		{
			const auto u = static_cast<unsigned char>(ch);
			if (ch == '"' || ch == '\\' || u < 0x20)
				out.push_back(replacement);
			else
				out.push_back(ch);
		}

		return out;
	}

	struct VertexF
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
	};

	// CIFF Z-up -> glTF Y-up: (x, y, z) -> (x, z, -y)
	inline VertexF zupToYup(const ciff::Point& p) noexcept
	{
		return VertexF{
			static_cast<float>(p.x),
			static_cast<float>(p.z),
			static_cast<float>(-p.y),
		};
	}

	inline VertexF zupToYupNormal(const float nx, const float ny, const float nz) noexcept
	{
		return VertexF{ nx, nz, -ny };
	}

	inline void write(WriteBuffer& target, WriteBuffer& source)
	{
		source.close();
		target.append(source.getFile(), true);
	}

	inline void write(WriteBuffer& w, const string& str)
	{
		w.write(str);
	}

	inline void write(WriteBuffer& w, const ostringstream& text)
	{
		w.write(text.str());
	}

	struct TempFiles
	{
		inline static filesystem::path directory;
		inline static optional<TempFile> meshes;
		inline static optional<TempFile> buffers;
		inline static optional<TempFile> accessors;
		inline static optional<TempFile> materials;
		inline static optional<TempFile> binData;

		static void Prepare(const string& target_file)
		{
			Cleanup();
			const auto target = filesystem::path(target_file);
			directory = target.parent_path() / (target.stem().string() + ".gltf_tmp");
		}

		static void Cleanup() noexcept
		{
			materials.reset();
			accessors.reset();
			buffers.reset();
			meshes.reset();
			binData.reset();
			directory.clear();
		}

		static string Meshes()
		{
			EnsureDirectory();
			if (!meshes.has_value())
				meshes.emplace(directory, "meshes.gltf");
			return meshes->path().string();
		}

		static string Buffers()
		{
			EnsureDirectory();
			if (!buffers.has_value())
				buffers.emplace(directory, "buffers.gltf");
			return buffers->path().string();
		}

		static string Accessors()
		{
			EnsureDirectory();
			if (!accessors.has_value())
				accessors.emplace(directory, "accessors.gltf");
			return accessors->path().string();
		}

		static string Materials()
		{
			EnsureDirectory();
			if (!materials.has_value())
				materials.emplace(directory, "materials.gltf");
			return materials->path().string();
		}

		static string BinData()
		{
			EnsureDirectory();
			if (!binData.has_value())
				binData.emplace(directory, "data.bin");
			return binData->path().string();
		}

	private:
		static void EnsureDirectory()
		{
			if (directory.empty())
				throw runtime_error("GLTF staging directory is not initialized");
		}
	};

	// Hierarchy section - writes the "nodes" array of the JSON.
	struct Hierarchy
	{
		inline static size_t meshIndex = 0;
		inline static vector<vector<size_t>> children;

		static void Reset()
		{
			meshIndex = 0;
			children.clear();
		}

		static void ConnectParentChildren(const ciff::Read& data)
		{
			children.clear();
			children.resize(data.nodes.size());

			for (size_t i = 1; i < data.nodes.size(); ++i)
			{
				const auto parent = data.nodes[i].parentIndex;
				if (parent < data.nodes.size())
					children[parent].push_back(i);
			}
		}

		static void WriteHeader(ciff::Convert& convert)
		{
			ostringstream json_str;

			json_str << "{\n";
			json_str << R"(  "asset": {)" << "\n";
			json_str << R"(    "generator": "Falcon Coding CogniteCIFF",)" << "\n";
			json_str << R"(    "version": "2.0")" << "\n";
			json_str << R"(  })";

			gltf::write(convert.write, json_str);
		}

		static void WriteScene(ciff::Convert& convert)
		{
			ostringstream json_str;

			const auto name = fileStem(convert.source_file);

			json_str << ",\n";
			json_str << R"(  "scene": 0,)" << "\n";
			json_str << R"(  "scenes": [)" << "\n";
			json_str << R"(    { "name": ")" << cleanJsonString(name, '_')
			         << R"(", "nodes": [0] })" << "\n";
			json_str << R"(  ])";

			gltf::write(convert.write, json_str);
		}

		static void WriteNodes(ciff::Convert& convert,
		                      const vector<size_t>& nodeMeshIndex)
		{
			gltf::write(convert.write, ",\n  \"nodes\": [\n");

			const auto& nodes = convert.data.nodes;

			for (size_t i = 0; i < nodes.size(); ++i)
			{
				ostringstream json_str;

				if (i > 0)
					json_str << ",\n";

				json_str << "    { \"name\": \"" << cleanJsonString(nodes[i].name, '_') << "\"";

				if (nodeMeshIndex[i] != static_cast<size_t>(-1))
					json_str << ", \"mesh\": " << nodeMeshIndex[i];

				if (i < children.size() && !children[i].empty())
				{
					json_str << ", \"children\": [";
					for (size_t k = 0; k < children[i].size(); ++k)
					{
						if (k > 0) json_str << ", ";
						json_str << children[i][k];
					}
					json_str << "]";
				}

				json_str << " }";

				gltf::write(convert.write, json_str);
			}

			gltf::write(convert.write, "\n  ]");
		}
	};

	// One mesh per CIFF node that owns geometry. Each mesh has one
	// primitive per CIFF Geometry it references.
	struct MeshTable
	{
		inline static WriteBuffer write;
		inline static size_t meshCount = 0;
		inline static size_t accessorBase = 0;
		inline static size_t lastNodeIndex = static_cast<size_t>(-1);
		inline static bool firstPrimitive = true;

		static void OpenFile()
		{
			meshCount = 0;
			accessorBase = 0;
			lastNodeIndex = static_cast<size_t>(-1);
			firstPrimitive = true;
			write.set(TempFiles::Meshes());
		}

		static void WriteHeader()
		{
			gltf::write(write, "  \"meshes\": [\n");
		}

		// returns the gltf mesh index for the current node, or -1 if no primitives yet.
		static size_t Begin(const size_t nodeIndex)
		{
			if (lastNodeIndex == nodeIndex)
				return meshCount - 1;

			if (lastNodeIndex != static_cast<size_t>(-1))
			{
				gltf::write(write, "\n      ]\n    }");
			}

			ostringstream json_str;
			if (meshCount > 0)
				json_str << ",\n";
			json_str << "    {\n      \"primitives\": [";
			gltf::write(write, json_str);

			lastNodeIndex = nodeIndex;
			firstPrimitive = true;
			return meshCount++;
		}

		static void AddPrimitive(const size_t color)
		{
			ostringstream json_str;
			if (!firstPrimitive)
				json_str << ",";
			firstPrimitive = false;

			json_str << "\n        {\n";
			json_str << "          \"attributes\": { \"POSITION\": " << accessorBase + 1
			         << ", \"NORMAL\": " << accessorBase + 2 << " },\n";
			json_str << "          \"indices\": " << accessorBase + 0 << ",\n";
			json_str << "          \"mode\": 4,\n";
			json_str << "          \"material\": " << color << "\n";
			json_str << "        }";

			gltf::write(write, json_str);
			accessorBase += 3;
		}

		static void WriteFooter()
		{
			if (lastNodeIndex != static_cast<size_t>(-1))
				gltf::write(write, "\n      ]\n    }\n");
			gltf::write(write, "  ]");
		}
	};

	struct BufferViews
	{
		inline static WriteBuffer write;
		inline static size_t count = 0;
		inline static size_t byteOffset = 0;

		static void OpenFile()
		{
			count = 0;
			byteOffset = 0;
			write.set(TempFiles::Buffers());
		}

		static void WriteHeader()
		{
			gltf::write(write, ",\n  \"bufferViews\": [\n");
		}

		static void WriteForMesh(const ciff::Mesh& mesh)
		{
			const auto vertexCount = static_cast<size_t>(mesh.points());
			const auto triCount = static_cast<size_t>(mesh.triangles());

			const auto indexBytes = triCount * 3 * sizeof(uint32_t);
			const auto vertexBytes = vertexCount * 3 * sizeof(float);
			const auto normalBytes = vertexCount * 3 * sizeof(float);

			Emit(indexBytes, 34963 /* ELEMENT_ARRAY_BUFFER */);
			Emit(vertexBytes, 34962 /* ARRAY_BUFFER */);
			Emit(normalBytes, 34962);
		}

		static void WriteFooter()
		{
			gltf::write(write, "\n  ]");
		}

	private:
		static void Emit(const size_t bytes, const int target)
		{
			ostringstream json_str;
			if (count > 0)
				json_str << ",\n";

			json_str << "    { \"buffer\": 0, \"byteOffset\": " << byteOffset
			         << ", \"byteLength\": " << bytes
			         << ", \"target\": " << target << " }";

			gltf::write(write, json_str);

			byteOffset += bytes;
			++count;
		}
	};

	struct Accessors
	{
		inline static WriteBuffer write;
		inline static size_t count = 0;

		static void OpenFile()
		{
			count = 0;
			write.set(TempFiles::Accessors());
		}

		static void WriteHeader()
		{
			gltf::write(write, ",\n  \"accessors\": [\n");
		}

		static void WriteForMesh(const ciff::Mesh& mesh, const VertexF& bbMin, const VertexF& bbMax)
		{
			const auto vertexCount = static_cast<size_t>(mesh.points());
			const auto indexCount = static_cast<size_t>(mesh.triangles()) * 3;

			// indices
			{
				ostringstream s;
				if (count > 0) s << ",\n";
				s << "    { \"bufferView\": " << count
				  << ", \"componentType\": 5125, \"count\": " << indexCount
				  << ", \"type\": \"SCALAR\" }";
				gltf::write(write, s);
				++count;
			}

			// positions
			{
				ostringstream s;
				s << ",\n    { \"bufferView\": " << count
				  << ", \"componentType\": 5126, \"count\": " << vertexCount
				  << ", \"type\": \"VEC3\""
				  << ", \"min\": [" << bbMin.x << ", " << bbMin.y << ", " << bbMin.z << "]"
				  << ", \"max\": [" << bbMax.x << ", " << bbMax.y << ", " << bbMax.z << "]"
				  << " }";
				gltf::write(write, s);
				++count;
			}

			// normals
			{
				ostringstream s;
				s << ",\n    { \"bufferView\": " << count
				  << ", \"componentType\": 5126, \"count\": " << vertexCount
				  << ", \"type\": \"VEC3\" }";
				gltf::write(write, s);
				++count;
			}
		}

		static void WriteFooter()
		{
			gltf::write(write, "\n  ]");
		}
	};

	struct Materials
	{
		inline static WriteBuffer write;
		inline static size_t count = 0;

		static void OpenFile()
		{
			count = 0;
			write.set(TempFiles::Materials());
		}

		static void WriteHeader()
		{
			gltf::write(write, ",\n  \"materials\": [\n");
		}

		static void Write(const ciff::Read& data)
		{
			for (size_t i = 0; i < data.colors.size(); ++i)
			{
				const auto& c = data.colors[i];
				const float r = static_cast<float>(c.r) / 255.0f;
				const float g = static_cast<float>(c.g) / 255.0f;
				const float b = static_cast<float>(c.b) / 255.0f;
				const float a = static_cast<float>(c.a) / 255.0f;

				ostringstream s;
				if (count > 0) s << ",\n";
				s << "    {\n";
				s << "      \"name\": \"material_" << i << "\",\n";
				s << "      \"pbrMetallicRoughness\": { \"baseColorFactor\": ["
				  << r << ", " << g << ", " << b << ", " << a
				  << "], \"metallicFactor\": 0.0, \"roughnessFactor\": 1.0 }";

				if (a < 1.0f)
					s << ",\n      \"alphaMode\": \"BLEND\"";

				s << "\n    }";

				gltf::write(write, s);
				++count;
			}
		}

		static void WriteFooter()
		{
			gltf::write(write, "\n  ]");
		}
	};

	// Buffers section: a single buffer that points to <target>.bin sidecar.
	struct Buffer
	{
		static void Write(ciff::Convert& convert, const size_t totalBytes, const string& binName)
		{
			ostringstream s;
			s << ",\n  \"buffers\": [\n";
			s << "    { \"uri\": \"" << binName
			  << "\", \"byteLength\": " << totalBytes << " }\n";
			s << "  ]";
			gltf::write(convert.write, s);
		}
	};

	// Compute per-vertex smoothed normals from CIFF mesh.
	inline vector<VertexF> ComputeNormals(const ciff::Mesh& mesh)
	{
		const auto n = mesh.points();
		vector<VertexF> normals(n, VertexF{ 0.0f, 0.0f, 0.0f });

		for (uint32_t t = 0; t < mesh.triangles(); ++t)
		{
			const auto i0 = mesh.indices[3 * t + 0];
			const auto i1 = mesh.indices[3 * t + 1];
			const auto i2 = mesh.indices[3 * t + 2];

			const auto& a = mesh.vertices[i0];
			const auto& b = mesh.vertices[i1];
			const auto& c = mesh.vertices[i2];

			const float ux = static_cast<float>(b.x - a.x);
			const float uy = static_cast<float>(b.y - a.y);
			const float uz = static_cast<float>(b.z - a.z);
			const float vx = static_cast<float>(c.x - a.x);
			const float vy = static_cast<float>(c.y - a.y);
			const float vz = static_cast<float>(c.z - a.z);

			const float nx = uy * vz - uz * vy;
			const float ny = uz * vx - ux * vz;
			const float nz = ux * vy - uy * vx;

			normals[i0].x += nx; normals[i0].y += ny; normals[i0].z += nz;
			normals[i1].x += nx; normals[i1].y += ny; normals[i1].z += nz;
			normals[i2].x += nx; normals[i2].y += ny; normals[i2].z += nz;
		}

		for (auto& v : normals)
		{
			const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
			if (len > 1e-12f)
			{
				v.x /= len; v.y /= len; v.z /= len;
			}
			else
			{
				v.x = 0.0f; v.y = 0.0f; v.z = 1.0f;
			}
		}

		return normals;
	}

	struct BinWriter
	{
		inline static WriteBuffer bin;
		inline static size_t totalBytes = 0;

		static void OpenFile()
		{
			totalBytes = 0;
			bin.set(TempFiles::BinData());
		}

		static void Append(const ciff::Mesh& mesh, VertexF& outMin, VertexF& outMax)
		{
			// indices uint32
			bin.write(mesh.indices.data(), mesh.indices.size());
			totalBytes += mesh.indices.size() * sizeof(uint32_t);

			// positions float32 (Z-up -> Y-up)
			outMin = VertexF{ +1e38f, +1e38f, +1e38f };
			outMax = VertexF{ -1e38f, -1e38f, -1e38f };

			vector<float> positions;
			positions.reserve(mesh.vertices.size() * 3);

			for (const auto& v : mesh.vertices)
			{
				const auto p = zupToYup(v);
				positions.push_back(p.x);
				positions.push_back(p.y);
				positions.push_back(p.z);

				if (p.x < outMin.x) outMin.x = p.x;
				if (p.y < outMin.y) outMin.y = p.y;
				if (p.z < outMin.z) outMin.z = p.z;
				if (p.x > outMax.x) outMax.x = p.x;
				if (p.y > outMax.y) outMax.y = p.y;
				if (p.z > outMax.z) outMax.z = p.z;
			}

			bin.write(positions.data(), positions.size());
			totalBytes += positions.size() * sizeof(float);

			// normals float32 (Z-up -> Y-up)
			const auto normals = ComputeNormals(mesh);
			vector<float> nbuf;
			nbuf.reserve(normals.size() * 3);
			for (const auto& n : normals)
			{
				const auto p = zupToYupNormal(n.x, n.y, n.z);
				nbuf.push_back(p.x);
				nbuf.push_back(p.y);
				nbuf.push_back(p.z);
			}
			bin.write(nbuf.data(), nbuf.size());
			totalBytes += nbuf.size() * sizeof(float);
		}

		static void Finalize(const string& binPath)
		{
			bin.close();
			// Rename staging bin into target sidecar
			std::error_code ec;
			std::filesystem::remove(binPath, ec);
			std::filesystem::rename(bin.getFile(), binPath, ec);
			if (ec)
			{
				// fallback: copy then remove
				std::filesystem::copy_file(bin.getFile(), binPath,
					std::filesystem::copy_options::overwrite_existing, ec);
				std::filesystem::remove(bin.getFile(), ec);
			}
		}
	};

	struct Convert final : ciff::Convert
	{
		explicit Convert(ciff::Read& data) : ciff::Convert(data)
		{
		}

		void WriteHeader() override
		{
			TempFiles::Prepare(target_file);

			Hierarchy::Reset();
			Hierarchy::ConnectParentChildren(data);

			MeshTable::OpenFile();
			BufferViews::OpenFile();
			Accessors::OpenFile();
			Materials::OpenFile();
			BinWriter::OpenFile();

			MeshTable::WriteHeader();
			BufferViews::WriteHeader();
			Accessors::WriteHeader();
			Materials::WriteHeader();

			nodeMeshIndex.assign(data.nodes.size(), static_cast<size_t>(-1));

			Hierarchy::WriteHeader(*this);
			Hierarchy::WriteScene(*this);
		}

		void WriteNode(const ciff::Node&) override
		{
			// Nodes section is emitted in WriteFooter once meshes are known.
		}

		void WriteGeometry(const ciff::Node& node, const size_t geometryIndex) override
		{
			const auto mesh = ciff::TessellateGeometry(data, geometryIndex);
			if (mesh.empty())
				return;

			const auto meshIdx = MeshTable::Begin(nodeIndex);
			nodeMeshIndex[nodeIndex] = meshIdx;

			VertexF bbMin, bbMax;
			BinWriter::Append(mesh, bbMin, bbMax);

			BufferViews::WriteForMesh(mesh);
			Accessors::WriteForMesh(mesh, bbMin, bbMax);

			const auto& geom = data.geometries[geometryIndex];
			MeshTable::AddPrimitive(geom.color);
		}

		void WriteMaterial(bool) override
		{
			Materials::Write(data);
		}

		void WriteFooter() override
		{
			Hierarchy::WriteNodes(*this, nodeMeshIndex);

			MeshTable::WriteFooter();
			BufferViews::WriteFooter();
			Accessors::WriteFooter();
			Materials::WriteFooter();

			gltf::write(write, ",\n");

			gltf::write(write, MeshTable::write);
			gltf::write(write, BufferViews::write);
			gltf::write(write, Accessors::write);
			gltf::write(write, Materials::write);

			// buffers section pointing to sidecar .bin
			const auto target = std::filesystem::path(target_file);
			const auto binName = target.stem().string() + ".bin";
			const auto binPath = (target.parent_path() / binName).string();

			BinWriter::Finalize(binPath);
			Buffer::Write(*this, BinWriter::totalBytes, binName);

			gltf::write(write, "\n}\n");

			TempFiles::Cleanup();
		}

	private:
		std::vector<size_t> nodeMeshIndex;
	};

	inline bool convert(ciff::Read& data)
	{
		return Convert(data).run();
	}
} // namespace gltf
