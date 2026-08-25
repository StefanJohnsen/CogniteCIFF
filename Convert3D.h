/*----------------------------------------------------------------
  Convert3D.h

  Canonical Falcon3D v1 writer for Cognite CIFF.

  The active v1 wire contract is the GPU-oriented layout shared with
  NavisworksNWD and Falcon3D:

    [128-byte header]
    [64-byte chunk table]
    [64-byte form table]
    [56-byte instances]
    [variable nodes]
    [RGBA8 materials]
    [64-byte-aligned planar position/normal/index slabs]
----------------------------------------------------------------*/

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "Convert.h"
#include "MeshNormals.h"
#include "PrimitiveInstanceCIFF.h"
#include "PrimitiveStatsCIFF.h"
#include "TempFile.h"
#include "Util.h"
#include "WriteBuffer.h"

namespace f3d
{
    using Matrix3x4 = std::array<float, 12>;

    inline constexpr std::uint32_t NativeMagicBytes = 0x46443343U;
    inline constexpr std::uint32_t BinaryVersion = 1U;
    inline constexpr std::uint16_t BinaryFlags = 0x0007U;
    inline constexpr std::uint64_t BinaryHeaderBytes = 128U;
    inline constexpr std::uint64_t ChunkRecordBytes = 64U;
    inline constexpr std::uint64_t FormRecordBytes = 64U;
    inline constexpr std::uint64_t InstanceRecordBytes = 56U;
    inline constexpr std::uint64_t MinimumNodeRecordBytes = 21U;
    inline constexpr std::uint64_t MaterialRecordBytes = 4U;
    inline constexpr std::uint64_t GeometryAlignment = 64U;
    inline constexpr std::uint64_t MaximumGeometrySlabBytes =
        1536ULL * 1024ULL * 1024ULL;
    inline constexpr std::uint32_t InvalidCacheIndex =
        std::numeric_limits<std::uint32_t>::max();

    enum class NodeType : std::uint8_t
    {
        Node = 0,
        Obstruction = 1,
        Insulation = 2
    };

    inline constexpr std::uint8_t kNodeTypeMask = 0x03u;
    inline constexpr std::uint8_t kHiddenFlag = 0x04u;
    inline constexpr std::uint8_t kReservedNodeFlagsMask = 0xF8u;

    [[nodiscard]] inline constexpr std::uint8_t MakeNodeFlags(const NodeType type, const bool hidden = false) noexcept
    {
        return static_cast<std::uint8_t>((static_cast<std::uint8_t>(type) & kNodeTypeMask) |
                                         (hidden ? kHiddenFlag : 0u));
    }

    [[nodiscard]] inline constexpr NodeType GetNodeType(const std::uint8_t nodeFlags) noexcept
    {
        return static_cast<NodeType>(nodeFlags & kNodeTypeMask);
    }

    [[nodiscard]] inline constexpr bool HasValidNodeFlags(const std::uint8_t nodeFlags) noexcept
    {
        return (nodeFlags & kReservedNodeFlagsMask) == 0u && (nodeFlags & kNodeTypeMask) != kNodeTypeMask;
    }

    [[nodiscard]] inline constexpr bool IsHidden(const std::uint8_t nodeFlags) noexcept
    {
        return (nodeFlags & kHiddenFlag) != 0u;
    }

    inline constexpr void SetHidden(std::uint8_t& nodeFlags, const bool hidden) noexcept
    {
        nodeFlags = static_cast<std::uint8_t>(hidden ? nodeFlags | kHiddenFlag : nodeFlags & ~kHiddenFlag);
    }

    static_assert(std::endian::native == std::endian::little);
    static_assert(sizeof(float) == 4 && sizeof(std::uint32_t) == 4 &&
                  sizeof(std::uint64_t) == 8);
    static_assert(4 + 4 + 1 + 1 + 2 + 4 + 5 * 8 + 7 * 8 + 2 * 8 ==
                  BinaryHeaderBytes);
    static_assert(6 * 8 + 2 * 4 + 8 == ChunkRecordBytes);
    static_assert(8 + 6 * 4 + 6 * 4 + 8 == FormRecordBytes);
    static_assert(2 * 4 + 12 * 4 == InstanceRecordBytes);

    [[nodiscard]] inline std::uint64_t CheckedAdd(
        const std::uint64_t left,
        const std::uint64_t right,
        const char* fieldName)
    {
        if (left > std::numeric_limits<std::uint64_t>::max() - right)
            throw std::overflow_error(std::string("Falcon3D ") + fieldName + " overflow");
        return left + right;
    }

    [[nodiscard]] inline std::uint64_t CheckedMultiply(
        const std::uint64_t left,
        const std::uint64_t right,
        const char* fieldName)
    {
        if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left)
            throw std::overflow_error(std::string("Falcon3D ") + fieldName + " overflow");
        return left * right;
    }

    [[nodiscard]] inline std::uint64_t AlignGeometry(
        const std::uint64_t value,
        const char* fieldName)
    {
        const auto remainder = value % GeometryAlignment;
        return remainder == 0U
            ? value
            : CheckedAdd(value, GeometryAlignment - remainder, fieldName);
    }

    template <typename T>
    inline void write(WriteBuffer& target, const T& value)
    {
        target.write(value);
    }

    template <typename T>
    inline void write(WriteBuffer& target, const std::vector<T>& values)
    {
        if (!values.empty())
            target.write(values.data(), values.size());
    }

    inline void write(WriteBuffer& target, const Matrix3x4& matrix)
    {
        target.write(matrix.data(), matrix.size());
    }

    inline void write(WriteBuffer& target, const std::string& value)
    {
        f3d::write(target, static_cast<std::uint32_t>(value.size()));
        if (!value.empty())
            target.write(value);
    }

    template <typename T>
    inline std::size_t overwriteAt(
        WriteBuffer& target,
        const std::size_t position,
        const T& value)
    {
        return target.overwriteAt(position, value);
    }

    inline void CloseChecked(WriteBuffer& stream, const char* sectionName)
    {
        stream.close();
        if (WriteBuffer::enabled && stream.getStream().fail())
        {
            throw std::ios_base::failure(
                std::string("Failed to close Falcon3D ") + sectionName + " stream");
        }
    }

    struct StreamValue
    {
        std::uint64_t value = 0U;
        std::size_t position = 0U;

        void Write(WriteBuffer& stream)
        {
            position = stream.tell();
            written = true;
            f3d::write(stream, value);
        }

        void Patch(WriteBuffer& stream) const
        {
            if (!written)
                throw std::logic_error("Falcon3D stream value was not written");
            f3d::overwriteAt(stream, position, value);
        }

      private:
        bool written = false;
    };

    inline void write(WriteBuffer& stream, StreamValue& value)
    {
        value.Write(stream);
    }

    struct IdentityHash64
    {
        std::size_t operator()(const std::uint64_t value) const noexcept
        {
            return static_cast<std::size_t>(value);
        }
    };

    struct BinaryHeader
    {
        std::uint8_t upAxis = 2U;
        std::uint8_t frontAxis = 1U;
        std::uint64_t formCount = 0U;
        std::uint64_t instanceCount = 0U;
        std::uint64_t nodeCount = 0U;
        std::uint64_t materialCount = 0U;
        std::uint64_t chunkCount = 0U;
        std::uint64_t chunkOffset = BinaryHeaderBytes;
        std::uint64_t formOffset = 0U;
        std::uint64_t instanceOffset = 0U;
        std::uint64_t nodeOffset = 0U;
        std::uint64_t materialOffset = 0U;
        std::uint64_t geometryOffset = 0U;
        std::uint64_t fileSize = 0U;
    };

    struct ChunkScratch
    {
        std::uint64_t firstForm = 0U;
        std::uint64_t formCount = 0U;
        std::uint64_t positionOffset = 0U;
        std::uint64_t positionBytes = 0U;
        std::uint64_t normalOffset = 0U;
        std::uint64_t normalBytes = 0U;
        std::uint64_t indexOffset = 0U;
        std::uint64_t indexBytes = 0U;
        std::uint32_t vertexCount = 0U;
        std::uint32_t indexCount = 0U;
        std::optional<TempFile> positionTemp;
        std::optional<TempFile> normalTemp;
        std::optional<TempFile> indexTemp;
    };

    struct Catalog
    {
        static constexpr std::size_t StreamBufferSize = 4ULL * 1024ULL * 1024ULL;

        Catalog()
            : formStream(StreamBufferSize),
              instanceStream(StreamBufferSize),
              nodeStream(StreamBufferSize),
              positionStream(StreamBufferSize),
              normalStream(StreamBufferSize),
              indexStream(StreamBufferSize)
        {
        }

        WriteBuffer formStream;
        WriteBuffer instanceStream;
        WriteBuffer nodeStream;
        WriteBuffer positionStream;
        WriteBuffer normalStream;
        WriteBuffer indexStream;

        std::optional<TempFile> formTemp;
        std::optional<TempFile> instanceTemp;
        std::optional<TempFile> nodeTemp;
        std::filesystem::path tempDirectory;
        std::vector<ChunkScratch> chunks;

        std::unordered_map<std::uint64_t, std::uint32_t, IdentityHash64>
            formIndexByHash;
        ciff::primitive_stats::Stats primitiveStats;

        std::vector<StreamValue> nodeInstanceCounts;
        std::uint32_t pendingNodeFirstInstance = 0U;
        std::uint32_t emittedFormCount = 0U;
        std::uint32_t emittedInstanceCount = 0U;
        std::uint32_t emittedNodeCount = 0U;
        std::uint64_t nodeBytes = 0U;
        std::uint64_t expectedOutputBytes = 0U;
    };

    struct Convert;

    struct Header
    {
        static BinaryHeader Build(Convert& convert);
        static void Write(Convert& convert, const BinaryHeader& header);
    };

    struct FormGeometry
    {
        static std::uint32_t AddOrFind(
            Convert& convert,
            std::uint64_t sharingHash,
            const ciff::normal_processing::RenderGeometry& mesh);

      private:
        static void Write(
            Convert& convert,
            const ciff::normal_processing::RenderGeometry& mesh);
    };

    struct Instance
    {
        static void Emit(
            Convert& convert,
            std::uint32_t formIndex,
            std::uint32_t materialIndex,
            const Matrix3x4& transform);
    };

    struct Node
    {
        static void Open(Convert& convert, const ciff::Node& node);
        static void Close(Convert& convert);
    };

    struct Materials
    {
        static void Write(Convert& convert);
    };

    struct Geometry
    {
        static void Write(Convert& convert, std::size_t geometryIndex);
    };

    struct Footer
    {
        static void Write(Convert& convert);
    };

    struct Convert final : ciff::Convert
    {
        explicit Convert(ciff::Read& data)
            : ciff::Convert(data)
        {
        }

        ~Convert() override
        {
            const auto closeSilently = [](WriteBuffer& stream) noexcept
            {
                try
                {
                    stream.close();
                }
                catch (...)
                {
                }
            };

            closeSilently(write);
            closeSilently(catalog.formStream);
            closeSilently(catalog.instanceStream);
            closeSilently(catalog.nodeStream);
            closeSilently(catalog.positionStream);
            closeSilently(catalog.normalStream);
            closeSilently(catalog.indexStream);

            if (!outputTempPath.empty())
            {
                std::error_code ignored;
                if (std::filesystem::is_regular_file(outputTempPath, ignored))
                    std::filesystem::remove(outputTempPath, ignored);
            }
        }

        bool run()
        {
            try
            {
                if (!SetFile())
                    return false;
                ciff::Convert::convert();
                PublishOutput();
                return true;
            }
            catch (const std::exception& error)
            {
                std::cerr << error.what() << std::endl;
            }
            catch (...)
            {
                std::cerr << "Unknown error occurred." << std::endl;
            }
            return false;
        }

        bool SetFile() override
        {
            if (write.good())
                return false;

            namespace fs = std::filesystem;
            source_file = data.source_cad;
            target_file = data.target_cad;
            const auto requestedTarget = fs::path(target_file);
            if (requestedTarget.empty() || requestedTarget.filename().empty())
                throw std::invalid_argument("Falcon3D output path has no file name");

            targetPath = fs::absolute(requestedTarget).lexically_normal();
            catalog.tempDirectory =
                targetPath.parent_path() / (targetPath.stem().string() + ".3d_tmp");

            if (!WriteBuffer::enabled)
                return true;

            outputTempPath = targetPath;
            outputTempPath += L".tmp";

            std::error_code error;
            const auto staleExists = fs::exists(outputTempPath, error);
            if (error)
                throw std::runtime_error("Failed to inspect Falcon3D output temp file");
            if (staleExists && !fs::is_regular_file(outputTempPath, error))
                throw std::runtime_error("Falcon3D output temp path is not a regular file");
            if (error)
                throw std::runtime_error("Failed to inspect Falcon3D output temp file");
            if (staleExists && (!fs::remove(outputTempPath, error) || error))
                throw std::runtime_error("Failed to remove stale Falcon3D output temp file");

            write.set(outputTempPath.string());

            catalog.formTemp.emplace(catalog.tempDirectory, "forms.bin");
            catalog.instanceTemp.emplace(catalog.tempDirectory, "instances.bin");
            catalog.nodeTemp.emplace(catalog.tempDirectory, "nodes.bin");
            catalog.formStream.set(catalog.formTemp->path().string());
            catalog.instanceStream.set(catalog.instanceTemp->path().string());
            catalog.nodeStream.set(catalog.nodeTemp->path().string());
            return true;
        }

        void PublishOutput()
        {
            if (!WriteBuffer::enabled)
                return;
            if (outputTempPath.empty() || catalog.expectedOutputBytes == 0U)
                throw std::logic_error(
                    "Falcon3D output was not completed before publication");

            CloseChecked(write, "output");
            std::error_code error;
            const auto outputBytes = std::filesystem::file_size(outputTempPath, error);
            if (error || outputBytes != catalog.expectedOutputBytes)
                throw std::ios_base::failure(
                    "Falcon3D completed output size is invalid");

            if (!::MoveFileExW(
                    outputTempPath.c_str(),
                    targetPath.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                throw std::system_error(
                    static_cast<int>(::GetLastError()),
                    std::system_category(),
                    "Failed to publish Falcon3D output");
            }
            outputTempPath.clear();
        }

        void WriteHeader() override;
        void WriteNode(const ciff::Node& node) override
        {
            Node::Open(*this, node);
        }
        void WriteGeometry(
            const ciff::Node&,
            const std::size_t geometryIndex) override
        {
            Geometry::Write(*this, geometryIndex);
        }
        void WriteMaterial(bool) override
        {
        }
        void WriteFooter() override
        {
            Footer::Write(*this);
        }

        Catalog catalog;
        std::filesystem::path targetPath;
        std::filesystem::path outputTempPath;
    };

    inline void ValidateNodeHierarchy(const ciff::Read& data)
    {
        if (data.nodes.empty())
            throw std::runtime_error("CIFF contains no hierarchy nodes");
        if (data.nodes.size() >
            static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) + 1ULL)
        {
            throw std::runtime_error(
                "Falcon3D node count exceeds its signed parent-index range");
        }

        const auto rootParent = data.nodes.front().parentIndex;
        if (rootParent != 0U &&
            rootParent != std::numeric_limits<std::size_t>::max())
        {
            throw std::runtime_error("CIFF root node has an invalid parent");
        }

        for (std::size_t index = 1U; index < data.nodes.size(); ++index)
        {
            const auto parent = data.nodes[index].parentIndex;
            if (parent >= data.nodes.size() || parent == index)
                throw std::runtime_error("CIFF node parent is out of range");
        }

        std::vector<std::uint8_t> state(data.nodes.size(), 0U);
        for (std::size_t start = 0U; start < data.nodes.size(); ++start)
        {
            auto cursor = start;
            while (state[cursor] == 0U)
            {
                state[cursor] = 1U;
                if (cursor == 0U)
                    break;
                cursor = data.nodes[cursor].parentIndex;
            }
            if (state[cursor] == 1U && cursor != 0U)
                throw std::runtime_error("CIFF node hierarchy contains a parent cycle");

            cursor = start;
            while (state[cursor] == 1U)
            {
                state[cursor] = 2U;
                if (cursor == 0U)
                    break;
                cursor = data.nodes[cursor].parentIndex;
            }
        }
    }

    inline void Convert::WriteHeader()
    {
        ValidateNodeHierarchy(data);
        if (data.colors.size() >
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        {
            throw std::runtime_error(
                "Falcon3D material count exceeds its 32-bit range");
        }
        catalog.formIndexByHash.reserve(data.geometries.size());
        catalog.nodeInstanceCounts.reserve(data.nodes.size());
    }

    inline void AddHashBytes(
        std::uint64_t& hash,
        const void* data,
        const std::size_t byteCount) noexcept
    {
        constexpr std::uint64_t prime = 1099511628211ULL;
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t index = 0U; index < byteCount; ++index)
        {
            hash ^= bytes[index];
            hash *= prime;
        }
    }

    template <typename T>
    inline void AddHashValue(std::uint64_t& hash, const T& value) noexcept
    {
        AddHashBytes(hash, &value, sizeof(value));
    }

    inline void StartGeometryChunk(Catalog& catalog)
    {
        CloseChecked(catalog.positionStream, "position scratch");
        CloseChecked(catalog.normalStream, "normal scratch");
        CloseChecked(catalog.indexStream, "index scratch");

        if (catalog.chunks.size() >=
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        {
            throw std::runtime_error(
                "Falcon3D chunk count exceeds its 32-bit range");
        }

        catalog.chunks.emplace_back();
        catalog.chunks.back().firstForm = catalog.emittedFormCount;
        if (!WriteBuffer::enabled)
            return;

        auto& chunk = catalog.chunks.back();
        const auto number = std::to_string(catalog.chunks.size() - 1U);
        chunk.positionTemp.emplace(
            catalog.tempDirectory,
            "chunk_" + number + "_positions.bin");
        chunk.normalTemp.emplace(
            catalog.tempDirectory,
            "chunk_" + number + "_normals.bin");
        chunk.indexTemp.emplace(
            catalog.tempDirectory,
            "chunk_" + number + "_indices.bin");
        catalog.positionStream.set(chunk.positionTemp->path().string());
        catalog.normalStream.set(chunk.normalTemp->path().string());
        catalog.indexStream.set(chunk.indexTemp->path().string());
    }

    inline void FormGeometry::Write(
        Convert& convert,
        const ciff::normal_processing::RenderGeometry& mesh)
    {
        if (mesh.empty() || mesh.positions.size() % 3U != 0U ||
            mesh.normals.size() != mesh.positions.size() ||
            mesh.indices.size() % 3U != 0U)
        {
            throw std::runtime_error(
                "Falcon3D form contains inconsistent triangle geometry");
        }

        const auto vertexCount64 = mesh.positions.size() / 3U;
        const auto indexCount64 = mesh.indices.size();
        if (vertexCount64 > std::numeric_limits<std::uint32_t>::max() ||
            indexCount64 > std::numeric_limits<std::uint32_t>::max())
        {
            throw std::runtime_error(
                "Falcon3D form exceeds its 32-bit geometry count range");
        }

        const auto vertexCount = static_cast<std::uint32_t>(vertexCount64);
        const auto indexCount = static_cast<std::uint32_t>(indexCount64);
        const auto triangleCount =
            static_cast<std::uint32_t>(indexCount64 / 3U);
        const auto positionBytes =
            static_cast<std::uint64_t>(vertexCount) * 3ULL * sizeof(float);
        const auto indexBytes =
            static_cast<std::uint64_t>(indexCount) * sizeof(std::uint32_t);
        if (positionBytes > MaximumGeometrySlabBytes ||
            indexBytes > MaximumGeometrySlabBytes)
        {
            throw std::runtime_error(
                "Falcon3D form is too large for one GPU geometry chunk");
        }

        float boundsMin[3] = {
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max()};
        float boundsMax[3] = {
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest()};

        for (std::size_t offset = 0U; offset < mesh.positions.size(); offset += 3U)
        {
            for (std::size_t axis = 0U; axis < 3U; ++axis)
            {
                const auto value = mesh.positions[offset + axis];
                if (!std::isfinite(value))
                    throw std::runtime_error(
                        "Falcon3D form contains a non-finite position");
                boundsMin[axis] = std::min(boundsMin[axis], value);
                boundsMax[axis] = std::max(boundsMax[axis], value);
            }

            const auto x = static_cast<double>(mesh.normals[offset + 0U]);
            const auto y = static_cast<double>(mesh.normals[offset + 1U]);
            const auto z = static_cast<double>(mesh.normals[offset + 2U]);
            const auto lengthSquared = x * x + y * y + z * z;
            if (!std::isfinite(lengthSquared) ||
                std::abs(lengthSquared - 1.0) > 1.0e-3)
            {
                throw std::runtime_error(
                    "Falcon3D form contains an invalid final normal");
            }
        }

        for (const auto index : mesh.indices)
        {
            if (index >= vertexCount)
                throw std::runtime_error(
                    "Falcon3D form index is outside its vertex buffer");
        }

        std::uint64_t contentHash = 14695981039346656037ULL;
        AddHashValue(contentHash, vertexCount);
        AddHashBytes(
            contentHash,
            mesh.positions.data(),
            mesh.positions.size() * sizeof(float));
        AddHashBytes(
            contentHash,
            mesh.normals.data(),
            mesh.normals.size() * sizeof(float));
        AddHashValue(contentHash, triangleCount);
        AddHashBytes(
            contentHash,
            mesh.indices.data(),
            mesh.indices.size() * sizeof(std::uint32_t));
        if (contentHash == 0U)
            contentHash = 1U;

        auto& catalog = convert.catalog;
        const auto requiresNewChunk = [&]()
        {
            if (catalog.chunks.empty())
                return true;
            const auto& chunk = catalog.chunks.back();
            return positionBytes >
                       MaximumGeometrySlabBytes - chunk.positionBytes ||
                   indexBytes > MaximumGeometrySlabBytes - chunk.indexBytes ||
                   vertexCount >
                       std::numeric_limits<std::uint32_t>::max() -
                           chunk.vertexCount ||
                   indexCount >
                       std::numeric_limits<std::uint32_t>::max() -
                           chunk.indexCount;
        }();
        if (requiresNewChunk)
            StartGeometryChunk(catalog);

        auto& chunk = catalog.chunks.back();
        const auto chunkIndex =
            static_cast<std::uint32_t>(catalog.chunks.size() - 1U);
        const auto baseVertex = chunk.vertexCount;
        const auto firstIndex = chunk.indexCount;

        f3d::write(catalog.formStream, contentHash);
        f3d::write(catalog.formStream, chunkIndex);
        f3d::write(catalog.formStream, baseVertex);
        f3d::write(catalog.formStream, vertexCount);
        f3d::write(catalog.formStream, firstIndex);
        f3d::write(catalog.formStream, indexCount);
        f3d::write(catalog.formStream, std::uint32_t{0U});
        catalog.formStream.write(boundsMin, 3U);
        catalog.formStream.write(boundsMax, 3U);
        f3d::write(catalog.formStream, std::uint64_t{0U});

        f3d::write(catalog.positionStream, mesh.positions);
        f3d::write(catalog.normalStream, mesh.normals);
        f3d::write(catalog.indexStream, mesh.indices);

        chunk.vertexCount += vertexCount;
        chunk.indexCount += indexCount;
        ++chunk.formCount;
        chunk.positionBytes += positionBytes;
        chunk.normalBytes += positionBytes;
        chunk.indexBytes += indexBytes;
    }

    inline std::uint32_t FormGeometry::AddOrFind(
        Convert& convert,
        const std::uint64_t sharingHash,
        const ciff::normal_processing::RenderGeometry& mesh)
    {
        auto& catalog = convert.catalog;
        if (const auto found = catalog.formIndexByHash.find(sharingHash);
            found != catalog.formIndexByHash.end())
        {
            if (found->second >= catalog.emittedFormCount)
                throw std::logic_error("Falcon3D form cache is inconsistent");
            return found->second;
        }

        if (catalog.emittedFormCount == std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error(
                "Falcon3D form count exceeds its 32-bit range");

        const auto formIndex = catalog.emittedFormCount;
        FormGeometry::Write(convert, mesh);
        const auto [it, inserted] =
            catalog.formIndexByHash.emplace(sharingHash, formIndex);
        if (!inserted || it->second != formIndex)
            throw std::logic_error("Falcon3D form cache insertion failed");
        ++catalog.emittedFormCount;
        return formIndex;
    }

    inline void Instance::Emit(
        Convert& convert,
        const std::uint32_t formIndex,
        const std::uint32_t materialIndex,
        const Matrix3x4& transform)
    {
        auto& catalog = convert.catalog;
        if (formIndex >= catalog.emittedFormCount)
            throw std::runtime_error(
                "Falcon3D instance refers to an invalid form");
        if (materialIndex != InvalidCacheIndex &&
            materialIndex >= convert.data.colors.size())
        {
            throw std::runtime_error(
                "Falcon3D instance refers to an invalid material");
        }
        for (const auto value : transform)
        {
            if (!std::isfinite(value))
                throw std::runtime_error(
                    "Falcon3D instance contains a non-finite transform");
        }
        if (catalog.emittedInstanceCount ==
            std::numeric_limits<std::uint32_t>::max())
        {
            throw std::runtime_error(
                "Falcon3D instance count exceeds its 32-bit range");
        }

        f3d::write(catalog.instanceStream, formIndex);
        f3d::write(catalog.instanceStream, materialIndex);
        f3d::write(catalog.instanceStream, transform);
        ++catalog.emittedInstanceCount;
    }

    inline void Geometry::Write(
        Convert& convert,
        const std::size_t geometryIndex)
    {
        if (geometryIndex >= convert.data.geometries.size())
            throw std::runtime_error(
                "CIFF node refers to a missing geometry record");

        const auto& geometry = convert.data.geometries[geometryIndex];
        auto form = ciff::primitive_instance::Make(convert.data, geometry);
        const auto materialIndex =
            geometry.color < convert.data.colors.size()
                ? static_cast<std::uint32_t>(geometry.color)
                : (convert.data.colors.empty() ? InvalidCacheIndex : 0U);

        const auto preHash = form.hash;
        if (preHash != 0U)
        {
            if (const auto found =
                    convert.catalog.formIndexByHash.find(preHash);
                found != convert.catalog.formIndexByHash.end())
            {
                convert.catalog.primitiveStats.Record(geometry, preHash);
                Instance::Emit(
                    convert,
                    found->second,
                    materialIndex,
                    Matrix3x4(form.transform));
                return;
            }
        }

        auto localMesh =
            ciff::primitive_instance::Tessellate(convert.data, geometry, form);
        if (localMesh.empty())
            return;

        // Mesh forms receive their canonical sharing hash during Tessellate.
        // Re-check the cache before the substantially more expensive normal
        // finalization so repeated baked meshes pay only the canonicalization
        // cost, not a complete render-mesh rebuild for every instance.
        if (preHash == 0U && form.hash != 0U)
        {
            if (const auto found =
                    convert.catalog.formIndexByHash.find(form.hash);
                found != convert.catalog.formIndexByHash.end())
            {
                convert.catalog.primitiveStats.Record(geometry, form.hash);
                Instance::Emit(
                    convert,
                    found->second,
                    materialIndex,
                    Matrix3x4(form.transform));
                return;
            }
        }

        auto finalMesh =
            ciff::normal_processing::FinalizeMeshNormals(localMesh);
        if (finalMesh.empty() || form.hash == 0U)
            return;

        const auto formIndex =
            FormGeometry::AddOrFind(convert, form.hash, finalMesh);
        convert.catalog.primitiveStats.Record(geometry, form.hash);
        Instance::Emit(
            convert,
            formIndex,
            materialIndex,
            Matrix3x4(form.transform));
    }

    inline void Node::Close(Convert& convert)
    {
        auto& catalog = convert.catalog;
        if (catalog.nodeInstanceCounts.empty())
            return;

        auto& count = catalog.nodeInstanceCounts.back();
        count.value =
            catalog.emittedInstanceCount - catalog.pendingNodeFirstInstance;
        count.Patch(catalog.nodeStream);
    }

    inline void Node::Open(Convert& convert, const ciff::Node& node)
    {
        Node::Close(convert);
        auto& catalog = convert.catalog;
        if (catalog.emittedNodeCount ==
            std::numeric_limits<std::uint32_t>::max())
        {
            throw std::runtime_error(
                "Falcon3D node count exceeds its 32-bit range");
        }
        if (node.name.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error(
                "Falcon3D node name exceeds its 32-bit length range");

        const auto sourceNodeIndex = convert.nodeIndex;
        std::int32_t parent = -1;
        if (sourceNodeIndex != 0U)
        {
            if (node.parentIndex >= convert.data.nodes.size() ||
                node.parentIndex >
                    static_cast<std::size_t>(
                        std::numeric_limits<std::int32_t>::max()))
            {
                throw std::runtime_error(
                    "Falcon3D node parent exceeds its signed range");
            }
            parent = static_cast<std::int32_t>(node.parentIndex);
        }

        catalog.pendingNodeFirstInstance = catalog.emittedInstanceCount;
        catalog.nodeInstanceCounts.emplace_back();

        f3d::write(catalog.nodeStream, parent);
        f3d::write(
            catalog.nodeStream,
            catalog.pendingNodeFirstInstance);
        f3d::write(
            catalog.nodeStream,
            catalog.nodeInstanceCounts.back());
        f3d::write(catalog.nodeStream, MakeNodeFlags(NodeType::Node, false));
        f3d::write(catalog.nodeStream, node.name);

        catalog.nodeBytes = CheckedAdd(
            catalog.nodeBytes,
            CheckedAdd(
                MinimumNodeRecordBytes,
                node.name.size(),
                "node record size"),
            "node table size");
        ++catalog.emittedNodeCount;
    }

    inline void Materials::Write(Convert& convert)
    {
        for (const auto& color : convert.data.colors)
        {
            f3d::write(convert.write, color.r);
            f3d::write(convert.write, color.g);
            f3d::write(convert.write, color.b);
            f3d::write(convert.write, color.a);
        }
    }

    inline BinaryHeader Header::Build(Convert& convert)
    {
        auto& catalog = convert.catalog;
        BinaryHeader header;
        header.formCount = catalog.emittedFormCount;
        header.instanceCount = catalog.emittedInstanceCount;
        header.nodeCount = catalog.emittedNodeCount;
        header.materialCount = convert.data.colors.size();
        header.chunkCount = catalog.chunks.size();

        constexpr auto maximumCount =
            static_cast<std::uint64_t>(
                std::numeric_limits<std::uint32_t>::max());
        if (header.formCount == 0U ||
            header.instanceCount == 0U ||
            header.nodeCount == 0U ||
            header.chunkCount == 0U ||
            header.chunkCount > header.formCount)
        {
            throw std::runtime_error(
                "CIFF conversion produced no drawable Falcon3D scene geometry");
        }
        if (header.formCount > maximumCount ||
            header.instanceCount > maximumCount ||
            header.nodeCount > maximumCount ||
            header.materialCount > maximumCount ||
            header.chunkCount > maximumCount)
        {
            throw std::runtime_error(
                "Falcon3D header count exceeds its 32-bit range");
        }
        if (header.nodeCount != convert.data.nodes.size() ||
            catalog.nodeBytes <
                CheckedMultiply(
                    header.nodeCount,
                    MinimumNodeRecordBytes,
                    "minimum node table size"))
        {
            throw std::logic_error(
                "Falcon3D node scratch data violates the wire contract");
        }

        header.formOffset = CheckedAdd(
            header.chunkOffset,
            CheckedMultiply(
                header.chunkCount,
                ChunkRecordBytes,
                "chunk table size"),
            "form table offset");
        header.instanceOffset = CheckedAdd(
            header.formOffset,
            CheckedMultiply(
                header.formCount,
                FormRecordBytes,
                "form table size"),
            "instance table offset");
        header.nodeOffset = CheckedAdd(
            header.instanceOffset,
            CheckedMultiply(
                header.instanceCount,
                InstanceRecordBytes,
                "instance table size"),
            "node table offset");
        header.materialOffset = CheckedAdd(
            header.nodeOffset,
            catalog.nodeBytes,
            "material table offset");
        const auto materialEnd = CheckedAdd(
            header.materialOffset,
            CheckedMultiply(
                header.materialCount,
                MaterialRecordBytes,
                "material table size"),
            "material table end");
        header.geometryOffset =
            AlignGeometry(materialEnd, "geometry offset");

        auto geometryCursor = header.geometryOffset;
        std::uint64_t expectedFirstForm = 0U;
        for (auto& chunk : catalog.chunks)
        {
            if (chunk.formCount == 0U ||
                chunk.firstForm != expectedFirstForm ||
                expectedFirstForm > header.formCount ||
                chunk.formCount >
                    header.formCount - expectedFirstForm ||
                chunk.vertexCount == 0U ||
                chunk.indexCount == 0U ||
                chunk.indexCount % 3U != 0U ||
                chunk.positionBytes !=
                    static_cast<std::uint64_t>(chunk.vertexCount) *
                        3ULL * sizeof(float) ||
                chunk.normalBytes != chunk.positionBytes ||
                chunk.indexBytes !=
                    static_cast<std::uint64_t>(chunk.indexCount) *
                        sizeof(std::uint32_t) ||
                chunk.positionBytes > MaximumGeometrySlabBytes ||
                chunk.normalBytes > MaximumGeometrySlabBytes ||
                chunk.indexBytes > MaximumGeometrySlabBytes)
            {
                throw std::logic_error(
                    "Falcon3D chunk scratch data violates the wire contract");
            }
            expectedFirstForm += chunk.formCount;

            chunk.positionOffset = geometryCursor;
            const auto positionEnd = CheckedAdd(
                chunk.positionOffset,
                chunk.positionBytes,
                "position slab end");
            chunk.normalOffset =
                AlignGeometry(positionEnd, "normal slab offset");
            const auto normalEnd = CheckedAdd(
                chunk.normalOffset,
                chunk.normalBytes,
                "normal slab end");
            chunk.indexOffset =
                AlignGeometry(normalEnd, "index slab offset");
            const auto indexEnd = CheckedAdd(
                chunk.indexOffset,
                chunk.indexBytes,
                "index slab end");
            header.fileSize = indexEnd;
            geometryCursor =
                AlignGeometry(indexEnd, "next position slab offset");
        }

        if (expectedFirstForm != header.formCount)
            throw std::logic_error(
                "Falcon3D forms do not canonically cover their chunks");
        if (header.fileSize <= header.geometryOffset ||
            header.fileSize >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()))
        {
            throw std::runtime_error(
                "Falcon3D output exceeds the supported stream range");
        }
        return header;
    }

    inline void Header::Write(
        Convert& convert,
        const BinaryHeader& header)
    {
        auto& output = convert.write;
        f3d::write(output, NativeMagicBytes);
        f3d::write(output, BinaryVersion);
        f3d::write(output, header.upAxis);
        f3d::write(output, header.frontAxis);
        f3d::write(output, BinaryFlags);
        f3d::write(
            output,
            static_cast<std::uint32_t>(BinaryHeaderBytes));
        f3d::write(output, header.formCount);
        f3d::write(output, header.instanceCount);
        f3d::write(output, header.nodeCount);
        f3d::write(output, header.materialCount);
        f3d::write(output, header.chunkCount);
        f3d::write(output, header.chunkOffset);
        f3d::write(output, header.formOffset);
        f3d::write(output, header.instanceOffset);
        f3d::write(output, header.nodeOffset);
        f3d::write(output, header.materialOffset);
        f3d::write(output, header.geometryOffset);
        f3d::write(output, header.fileSize);
        f3d::write(output, std::uint64_t{0U});
        f3d::write(output, std::uint64_t{0U});
    }

    inline void RequireOutputOffset(
        WriteBuffer& output,
        const std::uint64_t expected,
        const char* sectionName)
    {
        if (WriteBuffer::enabled &&
            static_cast<std::uint64_t>(output.tell()) != expected)
        {
            throw std::logic_error(
                std::string("Falcon3D ") + sectionName +
                " output offset is invalid");
        }
    }

    inline void PadOutputTo(
        WriteBuffer& output,
        const std::uint64_t target)
    {
        if (!WriteBuffer::enabled)
            return;

        const auto current =
            static_cast<std::uint64_t>(output.tell());
        if (target < current)
            throw std::logic_error(
                "Falcon3D padding target precedes the output position");

        static constexpr std::uint8_t zeroes[GeometryAlignment]{};
        auto remaining = target - current;
        while (remaining != 0U)
        {
            const auto bytes = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, sizeof(zeroes)));
            output.write(zeroes, bytes);
            remaining -= bytes;
        }
    }

    inline void ValidateScratchFile(
        const std::optional<TempFile>& temp,
        const std::uint64_t expected,
        const char* sectionName)
    {
        if (!WriteBuffer::enabled)
            return;
        if (!temp)
            throw std::logic_error(
                std::string("Falcon3D ") + sectionName +
                " scratch file is missing");

        std::error_code error;
        const auto bytes =
            std::filesystem::file_size(temp->path(), error);
        if (error || bytes != expected)
            throw std::logic_error(
                std::string("Falcon3D ") + sectionName +
                " scratch size is invalid");
    }

    inline void AppendScratch(
        WriteBuffer& output,
        const std::optional<TempFile>& temp,
        const char* sectionName)
    {
        if (!WriteBuffer::enabled)
            return;
        if (!temp)
            throw std::logic_error(
                std::string("Falcon3D ") + sectionName +
                " scratch file is missing");
        output.append(temp->path().string());
    }

    inline void Footer::Write(Convert& convert)
    {
        auto& catalog = convert.catalog;
        auto& output = convert.write;

        Node::Close(convert);
        CloseChecked(catalog.formStream, "form scratch");
        CloseChecked(catalog.instanceStream, "instance scratch");
        CloseChecked(catalog.nodeStream, "node scratch");
        CloseChecked(catalog.positionStream, "position scratch");
        CloseChecked(catalog.normalStream, "normal scratch");
        CloseChecked(catalog.indexStream, "index scratch");

        const auto header = Header::Build(convert);
        catalog.expectedOutputBytes = header.fileSize;
        if (!WriteBuffer::enabled)
        {
            catalog.primitiveStats.Print(convert.source_file);
            return;
        }

        ValidateScratchFile(
            catalog.formTemp,
            CheckedMultiply(
                header.formCount,
                FormRecordBytes,
                "form table size"),
            "form table");
        ValidateScratchFile(
            catalog.instanceTemp,
            CheckedMultiply(
                header.instanceCount,
                InstanceRecordBytes,
                "instance table size"),
            "instance table");
        ValidateScratchFile(
            catalog.nodeTemp,
            catalog.nodeBytes,
            "node table");
        for (const auto& chunk : catalog.chunks)
        {
            ValidateScratchFile(
                chunk.positionTemp,
                chunk.positionBytes,
                "position slab");
            ValidateScratchFile(
                chunk.normalTemp,
                chunk.normalBytes,
                "normal slab");
            ValidateScratchFile(
                chunk.indexTemp,
                chunk.indexBytes,
                "index slab");
        }

        Header::Write(convert, header);
        RequireOutputOffset(output, header.chunkOffset, "chunk table");
        for (const auto& chunk : catalog.chunks)
        {
            f3d::write(output, chunk.positionOffset);
            f3d::write(output, chunk.positionBytes);
            f3d::write(output, chunk.normalOffset);
            f3d::write(output, chunk.normalBytes);
            f3d::write(output, chunk.indexOffset);
            f3d::write(output, chunk.indexBytes);
            f3d::write(output, chunk.vertexCount);
            f3d::write(output, chunk.indexCount);
            f3d::write(output, std::uint64_t{0U});
        }

        RequireOutputOffset(output, header.formOffset, "form table");
        AppendScratch(output, catalog.formTemp, "form table");
        RequireOutputOffset(output, header.instanceOffset, "instance table");
        AppendScratch(output, catalog.instanceTemp, "instance table");
        RequireOutputOffset(output, header.nodeOffset, "node table");
        AppendScratch(output, catalog.nodeTemp, "node table");
        RequireOutputOffset(output, header.materialOffset, "material table");
        Materials::Write(convert);
        PadOutputTo(output, header.geometryOffset);

        for (std::size_t chunkIndex = 0U;
             chunkIndex < catalog.chunks.size();
             ++chunkIndex)
        {
            const auto& chunk = catalog.chunks[chunkIndex];
            RequireOutputOffset(output, chunk.positionOffset, "position slab");
            AppendScratch(output, chunk.positionTemp, "position slab");
            PadOutputTo(output, chunk.normalOffset);
            AppendScratch(output, chunk.normalTemp, "normal slab");
            PadOutputTo(output, chunk.indexOffset);
            AppendScratch(output, chunk.indexTemp, "index slab");
            if (chunkIndex + 1U < catalog.chunks.size())
            {
                PadOutputTo(
                    output,
                    AlignGeometry(
                        CheckedAdd(
                            chunk.indexOffset,
                            chunk.indexBytes,
                            "index slab end"),
                        "next position slab offset"));
            }
        }
        RequireOutputOffset(output, header.fileSize, "file end");

        catalog.primitiveStats.Print(convert.source_file);
        catalog.formTemp.reset();
        catalog.instanceTemp.reset();
        catalog.nodeTemp.reset();
        catalog.chunks.clear();
    }

    inline bool convert(ciff::Read& data)
    {
        if (data.nodes.empty())
            return false;
        return Convert(data).run();
    }
} // namespace f3d
