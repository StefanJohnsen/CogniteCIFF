/*----------------------------------------------------------------
  Convert3D.h

  FalconCoding 3D v1 binary container exporter for CIFF.

    [Header]      magic, version, axes, counts, section offsets
    [Forms]       contentHash + pointCount + positions + normals + triangleCount + indices
    [Instances]   form reference + material + localToWorld (column-major 3x4)
    [Nodes]       scene tree: parent (int32, -1 for root) + firstInstance + instanceCount + name
    [Materials]   palette (RGBA8)

  CIFF stores every primitive WORLD-BAKED. We factor each primitive
  into a transform-invariant LOCAL form (driving the form hash and
  the form-stream tessellation) plus a Matrix3x4 instance transform
  that puts the form back in world space. Identical local forms map
  to a single Form record and many Instance records, mirroring the
  AvevaRvmDebug 3D layout.

  Mesh-typed geometries (FacetGroup tessellations) fall back to a
  vertex/index mesh hash with an identity instance transform.

  Bounds (world AABB, per-form AABB, per-node subtree AABB) are
  derived data computed by the viewer at load time.
----------------------------------------------------------------*/

#pragma once

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <array>

#include "Convert.h"
#include "NormalsCIFF.h"
#include "PrimitiveInstanceCIFF.h"
#include "PrimitiveStatsCIFF.h"
#include "TempFile.h"
#include "Util.h"
#include "WriteBuffer.h"

namespace f3d
{
	using namespace std;

	using Matrix3x4 = std::array<float, 12>;

	// Column-major (X col, Y col, Z col, T col), matching AvevaRvmDebug.
	inline Matrix3x4 toF3D(const ciff::primitive_instance::Matrix3x4& m) noexcept
	{
		Matrix3x4 out{};
		for (size_t i = 0; i < 12; ++i) out[i] = m[i];
		return out;
	}

	inline void write(WriteBuffer& target, WriteBuffer& source)
	{
		source.close();
		target.append(source.getFile(), true);
	}

	template <typename T>
	void write(WriteBuffer& w, const T& t)
	{
		w.write(t);
	}

	template <typename T>
	void write(WriteBuffer& w, const vector<T>& list)
	{
		if (!list.empty())
			w.write(list.data(), list.size());
	}

	inline void write(WriteBuffer& w, const string& str)
	{
		w.write(static_cast<uint32_t>(str.length()));
		if (!str.empty())
			w.write(str);
	}

	inline void write(WriteBuffer& w, const Matrix3x4& m)
	{
		w.write(m.data(), m.size());
	}

	template <typename T>
	size_t overwriteAt(WriteBuffer& w, const size_t pos, const T& value)
	{
		return w.overwriteAt(pos, value);
	}

	struct StreamValue
	{
		uint64_t val = 0;
		size_t pos = 0;
		bool written = false;

		void write(WriteBuffer& w)
		{
			pos = w.tell();
			written = true;
			f3d::write(w, val);
		}

		void writeAt(WriteBuffer& w) const
		{
			if (!written)
				throw std::logic_error("StreamValue has not been written");

			f3d::overwriteAt(w, pos, val);
		}
	};

	inline void write(WriteBuffer& w, StreamValue& value)
	{
		value.write(w);
	}

	inline void writeAt(WriteBuffer& w, const StreamValue& value)
	{
		value.writeAt(w);
	}

	struct Catalog
	{
		// 100 MB streaming buffers for typical CIFF sizes.
		static constexpr size_t StreamBufferSize = 100ULL * 1024 * 1024;

		Catalog()
			: formStream(StreamBufferSize)
			, instanceStream(StreamBufferSize)
			, nodeStream(StreamBufferSize)
		{
		}

		WriteBuffer formStream;
		WriteBuffer instanceStream;
		WriteBuffer nodeStream;

		std::optional<TempFile> formTemp;
		std::optional<TempFile> instanceTemp;
		std::optional<TempFile> nodeTemp;

		StreamValue formCount;
		StreamValue instanceCount;
		StreamValue nodeCount;
		StreamValue materialCount;

		StreamValue formCatalogOffset;
		StreamValue instanceTableOffset;
		StreamValue nodeTreeOffset;
		StreamValue materialOffset;

		uint32_t emittedFormCount     = 0;
		uint32_t emittedInstanceCount = 0;
		uint32_t emittedNodeCount     = 0;

		// shape-hash -> form index. Identity hash maps a 64-bit FNV-1a value
		// directly to a slot; collisions resolve via the standard probing.
		struct IdentityHash64
		{
			size_t operator()(const uint64_t x) const noexcept { return static_cast<size_t>(x); }
		};
		std::unordered_map<uint64_t, uint32_t, IdentityHash64> formIndexByHash;
		ciff::primitive_stats::Stats primitiveStats;

		std::vector<StreamValue> nodeInstanceCounts;
		uint32_t pendingNodeFirstInstance = 0;
	};

	struct Convert;

	struct Header
	{
		static constexpr uint32_t NativeMagicBytes = 0x46443343;
		static constexpr uint32_t BinaryVersion = 1;
		static constexpr uint8_t  UpAxis           = 2; // Z-up
		static constexpr uint8_t  FrontAxis        = 4; // Y-forward

		static void Write(Convert& convert);
		static void WriteSum(Convert& convert);
	};

	struct FormGeometry
	{
		static constexpr uint64_t FnvOffsetBasis = 14695981039346656037ULL;
		static constexpr uint64_t FnvPrime = 1099511628211ULL;

		static void AddHashBytes(uint64_t& hash, const void* data, const size_t byteCount) noexcept
		{
			const auto* bytes = static_cast<const uint8_t*>(data);
			for (size_t i = 0; i < byteCount; ++i)
			{
				hash ^= bytes[i];
				hash *= FnvPrime;
			}
		}

		template <typename T>
		static void AddHashValue(uint64_t& hash, const T& value) noexcept
		{
			AddHashBytes(hash, &value, sizeof(value));
		}

		static void ValidateFinalNormals(const ciff::normal_processing::RenderGeometry& mesh)
		{
			if (mesh.empty() || mesh.positions.size() % 3U != 0U || mesh.indices.size() % 3U != 0U)
				throw std::runtime_error("Falcon3D form must contain triangle geometry");
			if (mesh.normals.size() != mesh.positions.size())
				throw std::runtime_error("Falcon3D form must contain one final normal per point");

			for (size_t offset = 0; offset < mesh.normals.size(); offset += 3U)
			{
				const auto x = static_cast<double>(mesh.normals[offset + 0U]);
				const auto y = static_cast<double>(mesh.normals[offset + 1U]);
				const auto z = static_cast<double>(mesh.normals[offset + 2U]);
				const auto lengthSquared = x * x + y * y + z * z;
				if (!std::isfinite(lengthSquared) || std::abs(lengthSquared - 1.0) > 1.0e-3)
					throw std::runtime_error("Falcon3D form contains an invalid final normal");
			}
		}

		static uint64_t ContentHash(const ciff::normal_processing::RenderGeometry& mesh) noexcept
		{
			auto hash = FnvOffsetBasis;
			const auto pointCount = mesh.points();
			const auto triangleCount = mesh.triangles();
			AddHashValue(hash, pointCount);
			AddHashBytes(hash, mesh.positions.data(), mesh.positions.size() * sizeof(float));
			AddHashBytes(hash, mesh.normals.data(), mesh.normals.size() * sizeof(float));
			AddHashValue(hash, triangleCount);
			AddHashBytes(hash, mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t));
			return hash == 0 ? 1 : hash;
		}

		static void Write(WriteBuffer& w, const ciff::normal_processing::RenderGeometry& mesh)
		{
			ValidateFinalNormals(mesh);

			f3d::write(w, ContentHash(mesh));
			const auto pointCount = mesh.points();
			f3d::write(w, pointCount);
			f3d::write(w, mesh.positions);
			f3d::write(w, mesh.normals);
			f3d::write(w, mesh.triangles());
			f3d::write(w, mesh.indices);
		}

		static uint32_t AddOrFind(Catalog& catalog, const uint64_t formKey,
		                          const ciff::normal_processing::RenderGeometry& mesh)
		{
			const auto it = catalog.formIndexByHash.find(formKey);
			if (it != catalog.formIndexByHash.end())
				return it->second;

			const auto index = catalog.emittedFormCount++;
			catalog.formIndexByHash.emplace(formKey, index);
			Write(catalog.formStream, mesh);
			return index;
		}
	};

	struct Instance
	{
		static void Write(WriteBuffer& w, const uint32_t formIndex, const uint32_t materialIndex,
		                  const Matrix3x4& tx)
		{
			f3d::write(w, formIndex);
			f3d::write(w, materialIndex);
			f3d::write(w, tx);
		}

		static void Emit(Convert& convert, const uint32_t formIndex, const uint32_t materialIndex,
		                 const Matrix3x4& tx);
	};

	struct Materials
	{
		static void Write(Convert& convert);
	};

	struct Node
	{
		static void Open(Convert& convert, const ciff::Node& node);
		static void Close(Convert& convert);
	};

	struct Geometry
	{
		static void Write(Convert& convert, const ciff::Node& node, size_t geometryIndex);
	};

	struct Footer
	{
		static void Write(Convert& convert);
	};

	struct Convert final : ciff::Convert
	{
		Catalog catalog;

		explicit Convert(ciff::Read& data) : ciff::Convert(data)
		{
		}

		bool SetFile() override
		{
			if (!ciff::Convert::SetFile())
				return false;

			namespace fs = std::filesystem;

			const auto target = fs::path(target_file);
			const auto stem   = target.stem().string();
			const auto dir    = target.parent_path() / (stem + ".3d_tmp");

			catalog.formTemp.emplace(dir, "forms.bin");
			catalog.instanceTemp.emplace(dir, "instances.bin");
			catalog.nodeTemp.emplace(dir, "nodes.bin");

			catalog.formStream.set(catalog.formTemp->path().string());
			catalog.instanceStream.set(catalog.instanceTemp->path().string());
			catalog.nodeStream.set(catalog.nodeTemp->path().string());

			return true;
		}

		void WriteHeader() override
		{
			Header::Write(*this);
		}

		void WriteNode(const ciff::Node& node) override
		{
			Node::Open(*this, node);
		}

		void WriteGeometry(const ciff::Node& node, const size_t geometryIndex) override
		{
			Geometry::Write(*this, node, geometryIndex);
		}

		void WriteMaterial(bool) override
		{
			// Materials section is emitted from WriteFooter once offsets are known.
		}

		void WriteFooter() override
		{
			Footer::Write(*this);
		}
	};

	inline void Header::Write(Convert& convert)
	{
		auto& w       = convert.write;
		auto& catalog = convert.catalog;

		f3d::write(w, NativeMagicBytes);
		f3d::write(w, BinaryVersion);
		f3d::write(w, UpAxis);
		f3d::write(w, FrontAxis);

		f3d::write(w, catalog.formCount);
		f3d::write(w, catalog.instanceCount);
		f3d::write(w, catalog.nodeCount);
		f3d::write(w, catalog.materialCount);

		f3d::write(w, catalog.formCatalogOffset);
		f3d::write(w, catalog.instanceTableOffset);
		f3d::write(w, catalog.nodeTreeOffset);
		f3d::write(w, catalog.materialOffset);
	}

	inline void Header::WriteSum(Convert& convert)
	{
		auto& w       = convert.write;
		auto& catalog = convert.catalog;

		catalog.formCount.val     = catalog.emittedFormCount;
		catalog.instanceCount.val = catalog.emittedInstanceCount;
		catalog.nodeCount.val     = catalog.emittedNodeCount;
		catalog.materialCount.val = convert.data.colors.size();

		f3d::writeAt(w, catalog.formCount);
		f3d::writeAt(w, catalog.instanceCount);
		f3d::writeAt(w, catalog.nodeCount);
		f3d::writeAt(w, catalog.materialCount);

		f3d::writeAt(w, catalog.formCatalogOffset);
		f3d::writeAt(w, catalog.instanceTableOffset);
		f3d::writeAt(w, catalog.nodeTreeOffset);
		f3d::writeAt(w, catalog.materialOffset);
	}

	inline void Instance::Emit(Convert& convert, const uint32_t formIndex, const uint32_t materialIndex,
	                           const Matrix3x4& tx)
	{
		auto& catalog = convert.catalog;

		Instance::Write(catalog.instanceStream, formIndex, materialIndex, tx);
		catalog.emittedInstanceCount++;
	}

	inline void Geometry::Write(Convert& convert, const ciff::Node&, const size_t geometryIndex)
	{
		const auto& geom = convert.data.geometries[geometryIndex];
		const auto material = static_cast<uint32_t>(geom.color);
		auto& catalog = convert.catalog;
		auto form = ciff::primitive_instance::Make(convert.data, geom);

		const auto preHash = form.hash;
		if (preHash != 0)
		{
			const auto it = catalog.formIndexByHash.find(preHash);
			if (it != catalog.formIndexByHash.end())
			{
				catalog.primitiveStats.Record(geom, preHash);
				Instance::Emit(convert, it->second, material, toF3D(form.transform));
				return;
			}
		}

		auto localMesh = ciff::primitive_instance::Tessellate(convert.data, geom, form);

		if (localMesh.empty())
			return;

		auto finalMesh = ciff::normal_processing::FinalizeMesh(localMesh);
		if (finalMesh.empty())
			return;

		const auto shapeHash = form.hash;
		if (shapeHash == 0)
			return;

		const auto formIndex = FormGeometry::AddOrFind(catalog, shapeHash, finalMesh);

		catalog.primitiveStats.Record(geom, shapeHash);
		Instance::Emit(convert, formIndex, material, toF3D(form.transform));
	}

	inline void Node::Close(Convert& convert)
	{
		auto& catalog = convert.catalog;

		if (catalog.nodeInstanceCounts.empty())
			return;

		auto& slot = catalog.nodeInstanceCounts.back();
		slot.val = catalog.emittedInstanceCount - catalog.pendingNodeFirstInstance;
		f3d::writeAt(catalog.nodeStream, slot);
	}

	inline void Node::Open(Convert& convert, const ciff::Node& node)
	{
		Node::Close(convert);

		auto& catalog = convert.catalog;
		auto& w       = catalog.nodeStream;

		catalog.pendingNodeFirstInstance = catalog.emittedInstanceCount;
		catalog.nodeInstanceCounts.emplace_back();

		const int32_t parent = (convert.nodeIndex == 0)
			? -1
			: static_cast<int32_t>(node.parentIndex);

		f3d::write(w, parent);
		f3d::write(w, catalog.pendingNodeFirstInstance);
		f3d::write(w, catalog.nodeInstanceCounts.back()); // patched on next Close
		f3d::write(w, node.name);

		catalog.emittedNodeCount++;
	}

	inline void Materials::Write(Convert& convert)
	{
		auto& w = convert.write;

		for (const auto& c : convert.data.colors)
		{
			f3d::write(w, c.r);
			f3d::write(w, c.g);
			f3d::write(w, c.b);
			f3d::write(w, c.a);
		}
	}

	inline void Footer::Write(Convert& convert)
	{
		auto& w       = convert.write;
		auto& catalog = convert.catalog;

		Node::Close(convert);

		catalog.formCatalogOffset.val = w.tell();
		f3d::write(w, catalog.formStream);

		catalog.instanceTableOffset.val = w.tell();
		f3d::write(w, catalog.instanceStream);

		catalog.nodeTreeOffset.val = w.tell();
		f3d::write(w, catalog.nodeStream);

		catalog.materialOffset.val = w.tell();
		Materials::Write(convert);

		Header::WriteSum(convert);
		catalog.primitiveStats.Print(convert.source_file);

		catalog.formTemp.reset();
		catalog.instanceTemp.reset();
		catalog.nodeTemp.reset();
	}

	inline bool convert(ciff::Read& data)
	{
		return Convert(data).run();
	}
} // namespace f3d
