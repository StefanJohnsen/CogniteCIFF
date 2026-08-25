#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "CogniteCiffLib/ConvertToScene.h"
#include "CogniteCiffLib/SceneData.h"
#include "Convert3D.h"
#include "ReadCIFF.h"

namespace
{
    constexpr std::uint32_t kCiffMagic = 0x46443343U;
    constexpr std::uint32_t kBinaryVersion = 1U;
    constexpr std::uint16_t kBinaryFlags = 0x0007U;
    constexpr std::uint32_t kBinaryHeaderBytes = 128U;
    constexpr std::uint64_t kChunkRecordBytes = 64U;
    constexpr std::uint64_t kFormRecordBytes = 64U;
    constexpr std::uint64_t kInstanceRecordBytes = 56U;
    constexpr std::uint64_t kGeometryAlignment = 64U;
    constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
    constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

    [[noreturn]] void Fail(const std::string& message)
    {
        throw std::runtime_error(message);
    }

    void Check(const bool condition, const std::string& message)
    {
        if (!condition)
            Fail(message);
    }

    void CheckNear(const float actual, const float expected, const float tolerance, const std::string& message)
    {
        if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance)
            Fail(message);
    }

    void ValidateNodeFlagsContract()
    {
        Check(static_cast<std::uint8_t>(scene::NodeType::Node) == 0U &&
                  static_cast<std::uint8_t>(scene::NodeType::Obstruction) == 1U &&
                  static_cast<std::uint8_t>(scene::NodeType::Insulation) == 2U,
              "SceneData node type values are incorrect");
        Check(!scene::HasValidNodeFlags(0x03U) && !scene::HasValidNodeFlags(0x08U),
              "SceneData accepts reserved node flags");

        auto sceneFlags = scene::MakeNodeFlags(scene::NodeType::Obstruction);
        scene::SetHidden(sceneFlags, true);
        Check(scene::HasValidNodeFlags(sceneFlags) && scene::IsHidden(sceneFlags) &&
                  scene::GetNodeType(sceneFlags) == scene::NodeType::Obstruction,
              "SceneData hidden flag changed the node type");
        scene::SetHidden(sceneFlags, false);
        Check(!scene::IsHidden(sceneFlags), "SceneData hidden flag was not cleared");

        auto binaryFlags = f3d::MakeNodeFlags(f3d::NodeType::Insulation);
        f3d::SetHidden(binaryFlags, true);
        Check(f3d::HasValidNodeFlags(binaryFlags) && f3d::IsHidden(binaryFlags) &&
                  f3d::GetNodeType(binaryFlags) == f3d::NodeType::Insulation && !f3d::HasValidNodeFlags(0x03U),
              "Falcon3D node flag contract is incorrect");
    }

    class TempDirectory final
    {
      public:
        TempDirectory()
        {
            const auto suffix = std::to_wstring(::GetCurrentProcessId()) + L"-" +
                                std::to_wstring(::GetTickCount64());
            path_ = std::filesystem::temp_directory_path() / (L"CogniteCiffSmoke-" + suffix);
            Check(std::filesystem::create_directory(path_), "Could not create the smoke-test directory");
        }

        ~TempDirectory()
        {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }

        TempDirectory(const TempDirectory&) = delete;
        TempDirectory& operator=(const TempDirectory&) = delete;

        [[nodiscard]] const std::filesystem::path& path() const noexcept
        {
            return path_;
        }

      private:
        std::filesystem::path path_;
    };

    class BinaryWriter final
    {
      public:
        explicit BinaryWriter(const std::filesystem::path& path)
            : stream_(path, std::ios::binary | std::ios::trunc)
        {
            Check(stream_.is_open(), "Could not create the CIFF fixture");
        }

        template <typename T>
        void write(const T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            stream_.write(reinterpret_cast<const char*>(&value), sizeof(value));
            Check(stream_.good(), "Could not write the CIFF fixture");
        }

        void writeString(const std::string_view value)
        {
            Check(value.size() <= std::numeric_limits<std::uint32_t>::max(), "Fixture string is too long");
            write(static_cast<std::uint32_t>(value.size()));
            stream_.write(value.data(), static_cast<std::streamsize>(value.size()));
            Check(stream_.good(), "Could not write a CIFF fixture string");
        }

      private:
        std::ofstream stream_;
    };

    void WriteMinimalCiff(const std::filesystem::path& path, const std::uint32_t geometryCount = 1U)
    {
        Check(geometryCount > 0U, "CIFF fixture must contain at least one geometry");
        BinaryWriter writer(path);

        writer.write(kCiffMagic);
        writer.write(std::uint32_t{4});
        writer.write(std::uint64_t{0});
        writer.write(std::uint64_t{0});
        writer.write(std::uint64_t{0});
        writer.write(std::uint64_t{0});
        writer.write(std::uint32_t{0});
        writer.writeString("[]");

        writer.write(std::uint8_t{1});       // node record
        writer.write(std::uint32_t{0});      // child count
        writer.write(std::int64_t{0});       // writer-side node index
        writer.write(std::int64_t{-1});      // tree index
        writer.write(std::int64_t{-1});      // subtree size
        writer.writeString("Root");
        writer.write(std::uint8_t{1});       // material is present
        writer.write(std::uint8_t{17});
        writer.write(std::uint8_t{34});
        writer.write(std::uint8_t{51});
        writer.write(std::uint8_t{255});
        writer.write(std::uint8_t{0});       // no internal metadata
        writer.write(std::uint32_t{0});      // no user-metadata sections
        writer.write(std::int64_t{0});       // writer-side color index
        writer.write(geometryCount);

        for (std::uint32_t geometryIndex = 0U; geometryIndex < geometryCount; ++geometryIndex)
        {
            writer.write(std::int64_t{-1});      // geometry reference
            writer.write(std::uint8_t{0});       // no per-geometry transform
            writer.write(std::uint8_t{3});       // mesh
            writer.write(std::uint32_t{1});      // mesh count
            writer.write(std::uint32_t{93});     // bytes after this field
            writer.write(std::uint32_t{3});      // point count
            writer.write(std::uint32_t{1});      // triangle count
            writer.write(std::uint8_t{0});       // texture count

            constexpr std::array<double, 9> positions{
                0.0, 0.0, 0.0,
                2.0, 0.0, 0.0,
                0.0, 3.0, 0.0,
            };
            for (const auto value : positions)
                writer.write(value);
            writer.write(std::uint32_t{0});
            writer.write(std::uint32_t{1});
            writer.write(std::uint32_t{2});
        }

        writer.write(std::uint8_t{0});       // footer
    }

    void AddHashBytes(std::uint64_t& hash, const void* data, const std::size_t byteCount)
    {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t index = 0; index < byteCount; ++index)
        {
            hash ^= bytes[index];
            hash *= kFnvPrime;
        }
    }

    [[nodiscard]] std::uint64_t ContentHash(const scene::GeometryChunk& chunk)
    {
        auto hash = kFnvOffsetBasis;
        const auto pointCount = static_cast<std::uint32_t>(chunk.positions.size() / 3U);
        const auto triangleCount = static_cast<std::uint32_t>(chunk.indices.size() / 3U);
        AddHashBytes(hash, &pointCount, sizeof(pointCount));
        AddHashBytes(hash, chunk.positions.data(), chunk.positions.size() * sizeof(float));
        AddHashBytes(hash, chunk.normals.data(), chunk.normals.size() * sizeof(float));
        AddHashBytes(hash, &triangleCount, sizeof(triangleCount));
        AddHashBytes(hash, chunk.indices.data(), chunk.indices.size() * sizeof(std::uint32_t));
        return hash == 0U ? 1U : hash;
    }

    template <typename T>
    void AddFingerprintValue(std::uint64_t& hash, const T& value)
    {
        AddHashBytes(hash, &value, sizeof(value));
    }

    [[nodiscard]] std::uint64_t SceneFingerprint(const scene::SceneData& sceneData)
    {
        auto hash = kFnvOffsetBasis;
        const auto upAxis = static_cast<std::uint8_t>(sceneData.upAxis);
        const auto frontAxis = static_cast<std::uint8_t>(sceneData.frontAxis);
        AddFingerprintValue(hash, upAxis);
        AddFingerprintValue(hash, frontAxis);
        AddFingerprintValue(hash, sceneData.mirrorXAxisInWorld);
        AddFingerprintValue(hash, sceneData.rootNode);

        const auto meshCount = static_cast<std::uint64_t>(sceneData.meshes.size());
        const auto instanceCount = static_cast<std::uint64_t>(sceneData.instances.size());
        const auto materialCount = static_cast<std::uint64_t>(sceneData.materials.size());
        const auto nodeCount = static_cast<std::uint64_t>(sceneData.nodes.size());
        AddFingerprintValue(hash, meshCount);
        AddFingerprintValue(hash, instanceCount);
        AddFingerprintValue(hash, materialCount);
        AddFingerprintValue(hash, nodeCount);

        for (const auto& mesh : sceneData.meshes)
        {
            AddFingerprintValue(hash, mesh.shapeHash);
            AddFingerprintValue(hash, mesh.chunkIndex);
            AddFingerprintValue(hash, mesh.baseVertex);
            AddFingerprintValue(hash, mesh.vertexCount);
            AddFingerprintValue(hash, mesh.firstIndex);
            AddFingerprintValue(hash, mesh.indexCount);
            AddHashBytes(hash, mesh.boundsMin, sizeof(mesh.boundsMin));
            AddHashBytes(hash, mesh.boundsMax, sizeof(mesh.boundsMax));
        }
        for (const auto& instance : sceneData.instances)
        {
            AddFingerprintValue(hash, instance.meshIndex);
            AddFingerprintValue(hash, instance.materialIndex);
            AddFingerprintValue(hash, instance.nodeIndex);
            AddHashBytes(hash, instance.transform, sizeof(instance.transform));
        }
        for (const auto& material : sceneData.materials)
        {
            AddFingerprintValue(hash, material.r);
            AddFingerprintValue(hash, material.g);
            AddFingerprintValue(hash, material.b);
            AddFingerprintValue(hash, material.a);
            AddFingerprintValue(hash, material.textureIndex);
        }
        for (const auto& node : sceneData.nodes)
        {
            AddFingerprintValue(hash, node.parent);
            AddFingerprintValue(hash, node.firstInstance);
            AddFingerprintValue(hash, node.instanceCount);
            AddFingerprintValue(hash, node.nodeFlags);
            const auto nameBytes = static_cast<std::uint64_t>(node.name.size());
            AddFingerprintValue(hash, nameBytes);
            AddHashBytes(hash, node.name.data(), node.name.size());
        }
        return hash;
    }

    int RunBenchmark(const std::filesystem::path& ciffPath)
    {
        scene::SceneData sceneData;
        const auto conversion = cifflib::ConvertToScene(ciffPath, sceneData);
        if (!conversion.success)
        {
            std::wcerr << L"CIFF benchmark failed: " << conversion.message << std::endl;
            return 1;
        }

        const auto& timings = conversion.timings;
        std::cout << std::fixed << std::setprecision(2)
                  << "total_ms=" << timings.totalMs << '\n'
                  << "data_load_ms=" << timings.dataLoadMs << '\n'
                  << "scene_projection_ms=" << timings.sceneProjectionMs << '\n'
                  << "geometry_finalize_ms=" << timings.geometryFinalizeMs << '\n'
                  << "mesh_descriptor_ms=" << timings.meshDescriptorMs << '\n'
                  << "instance_hierarchy_material_ms=" << timings.instanceHierarchyMaterialMs << '\n'
                  << "meshes=" << sceneData.meshes.size() << '\n'
                  << "instances=" << sceneData.instances.size() << '\n'
                  << "nodes=" << sceneData.nodes.size() << '\n'
                  << "fingerprint=0x" << std::hex << SceneFingerprint(sceneData) << std::dec << std::endl;
        return 0;
    }

    void ValidateScene(const scene::SceneData& result)
    {
        Check(result.upAxis == scene::UpAxis::Z, "CIFF scene must be Z-up");
        Check(result.frontAxis == scene::FrontAxis::Y, "CIFF scene must be Y-forward");
        Check(result.mirrorXAxisInWorld, "CIFF scene must retain its X-mirroring convention");
        Check(!result.geometryChunksGpuReady, "Per-form CIFF chunks must not claim aggregate GPU readiness");
        Check(result.rootNode == 0U, "CIFF scene root index must be zero");
        Check(result.geometryChunks.size() == 1U, "Expected one geometry chunk");
        Check(result.meshes.size() == 1U, "Expected one mesh descriptor");
        Check(result.instances.size() == 1U, "Expected one mesh instance");
        Check(result.materials.size() == 1U, "Expected one material");
        Check(result.nodes.size() == 1U, "Expected one hierarchy node");

        const auto& chunk = result.geometryChunks.front();
        const auto& mesh = result.meshes.front();
        Check(chunk.positions.size() == 9U, "Triangle must have three positions");
        Check(chunk.normals.size() == chunk.positions.size(), "Triangle must have one normal per vertex");
        Check(chunk.indices.size() == 3U, "Triangle must have three indices");
        Check(mesh.shapeHash == ContentHash(chunk), "Mesh hash does not match the canonical content transcript");
        Check(mesh.chunkIndex == 0U, "Mesh must reference the first geometry chunk");
        Check(mesh.baseVertex == 0U && mesh.vertexCount == 3U, "Mesh vertex range is incorrect");
        Check(mesh.firstIndex == 0U && mesh.indexCount == 3U, "Mesh index range is incorrect");

        std::array<float, 3> boundsMin{
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
        };
        std::array<float, 3> boundsMax{
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
        };
        for (std::size_t offset = 0; offset < chunk.positions.size(); offset += 3U)
        {
            for (std::size_t axis = 0; axis < 3U; ++axis)
            {
                const auto value = chunk.positions[offset + axis];
                Check(std::isfinite(value), "Scene contains a non-finite position");
                boundsMin[axis] = std::min(boundsMin[axis], value);
                boundsMax[axis] = std::max(boundsMax[axis], value);
            }
        }
        for (std::size_t axis = 0; axis < 3U; ++axis)
        {
            CheckNear(mesh.boundsMin[axis], boundsMin[axis], 0.0F, "Mesh minimum bound is incorrect");
            CheckNear(mesh.boundsMax[axis], boundsMax[axis], 0.0F, "Mesh maximum bound is incorrect");
        }

        for (std::size_t offset = 0; offset < chunk.normals.size(); offset += 3U)
        {
            const auto x = chunk.normals[offset];
            const auto y = chunk.normals[offset + 1U];
            const auto z = chunk.normals[offset + 2U];
            const auto lengthSquared = x * x + y * y + z * z;
            CheckNear(lengthSquared, 1.0F, 1.0e-3F, "Scene contains a non-unit normal");
        }
        for (const auto index : chunk.indices)
            Check(index < mesh.vertexCount, "Scene contains a non-local triangle index");

        const auto& instance = result.instances.front();
        Check(instance.meshIndex == 0U && instance.materialIndex == 0U && instance.nodeIndex == 0U,
              "Instance references are incorrect");
        for (const auto value : instance.transform)
            Check(std::isfinite(value), "Instance transform contains a non-finite value");

        const auto& material = result.materials.front();
        Check(material.r == 17U && material.g == 34U && material.b == 51U && material.a == 255U,
              "Material color was not preserved");
        Check(material.textureIndex == scene::kInvalidIndex, "CIFF material must be untextured");

        const auto& node = result.nodes.front();
        Check(node.parent == -1 && node.firstInstance == 0U && node.instanceCount == 1U,
              "Root instance range is incorrect");
        Check(scene::HasValidNodeFlags(node.nodeFlags) &&
                  scene::GetNodeType(node.nodeFlags) == scene::NodeType::Node &&
                  !scene::IsHidden(node.nodeFlags),
              "Root node flags are incorrect");
        Check(node.name == "Root", "Root name was not preserved");
    }

    void ValidateTimings(const cifflib::ConvertToSceneResult& conversion)
    {
        const auto& timings = conversion.timings;
        const std::array values{
            timings.dataLoadMs,
            timings.geometryFinalizeMs,
            timings.meshDescriptorMs,
            timings.instanceHierarchyMaterialMs,
            timings.sceneProjectionMs,
            timings.totalMs,
        };
        for (const auto value : values)
            Check(std::isfinite(value) && value >= 0.0, "CIFF conversion produced an invalid timing value");

        Check(timings.dataLoadMs > 0.0, "CIFF data-load timing was not recorded");
        Check(timings.sceneProjectionMs > 0.0, "CIFF SceneData projection timing was not recorded");
        Check(timings.totalMs > 0.0, "CIFF total conversion timing was not recorded");
        Check(timings.dataLoadMs + timings.sceneProjectionMs <= timings.totalMs,
              "CIFF source phases exceed the total conversion timing");
        Check(timings.geometryFinalizeMs + timings.meshDescriptorMs + timings.instanceHierarchyMaterialMs <=
                  timings.sceneProjectionMs,
              "CIFF SceneData sub-phases exceed the projection timing");
    }

    void ValidateAtomicCancellation(const std::filesystem::path& ciffPath)
    {
        scene::SceneData sentinel;
        sentinel.upAxis = scene::UpAxis::Y;
        sentinel.frontAxis = scene::FrontAxis::NegZ;
        sentinel.mirrorXAxisInWorld = false;
        sentinel.geometryChunksGpuReady = true;
        sentinel.rootNode = 73U;
        sentinel.nodes.push_back(
            scene::Node{-1, 9U, 11U, scene::MakeNodeFlags(scene::NodeType::Node, true), "unchanged"});
        sentinel.materials.push_back(scene::Material{1U, 2U, 3U, 4U, 5U});
        sentinel.meshes.emplace_back();
        sentinel.meshes.back().shapeHash = 0x123456789abcdef0ULL;

        std::size_t callbackCount = 0U;
        const auto conversion = cifflib::ConvertToScene(
            ciffPath,
            sentinel,
            [&](const std::size_t total, const std::size_t current)
            {
                ++callbackCount;
                Check(total > 0U && current <= total, "Cancellation callback received invalid progress");
                return false;
            });

        Check(!conversion.success, "Cancelled conversion unexpectedly succeeded");
        Check(callbackCount > 0U, "Cancellation callback was not invoked");
        Check(std::isfinite(conversion.timings.totalMs) && conversion.timings.totalMs > 0.0 &&
                  std::isfinite(conversion.timings.dataLoadMs) && conversion.timings.dataLoadMs > 0.0,
              "Cancelled conversion did not preserve partial timing diagnostics");
        Check(conversion.message.find(L"cancel") != std::wstring::npos ||
                  conversion.message.find(L"Cancel") != std::wstring::npos,
              "Cancelled conversion did not report cancellation");
        Check(sentinel.upAxis == scene::UpAxis::Y && sentinel.frontAxis == scene::FrontAxis::NegZ,
              "Cancelled conversion changed the sentinel axes");
        Check(!sentinel.mirrorXAxisInWorld && sentinel.geometryChunksGpuReady && sentinel.rootNode == 73U,
              "Cancelled conversion changed top-level sentinel state");
        Check(sentinel.nodes.size() == 1U && sentinel.nodes.front().name == "unchanged" &&
                  sentinel.nodes.front().firstInstance == 9U && sentinel.nodes.front().instanceCount == 11U &&
                  scene::GetNodeType(sentinel.nodes.front().nodeFlags) == scene::NodeType::Node &&
                  scene::IsHidden(sentinel.nodes.front().nodeFlags),
              "Cancelled conversion changed the sentinel hierarchy");
        Check(sentinel.materials.size() == 1U && sentinel.materials.front().textureIndex == 5U,
              "Cancelled conversion changed the sentinel material table");
        Check(sentinel.meshes.size() == 1U && sentinel.meshes.front().shapeHash == 0x123456789abcdef0ULL,
              "Cancelled conversion changed the sentinel mesh table");
        Check(sentinel.geometryChunks.empty() && sentinel.instances.empty() && sentinel.textures.empty(),
              "Cancelled conversion added data to the sentinel scene");
    }

    void ValidateSharedMeshInstances(const std::filesystem::path& ciffPath)
    {
        scene::SceneData sceneData;
        const auto conversion = cifflib::ConvertToScene(ciffPath, sceneData);
        Check(conversion.success, "ConvertToScene failed for the shared-mesh CIFF fixture");
        Check(sceneData.geometryChunks.size() == 1U && sceneData.meshes.size() == 1U,
              "Identical CIFF meshes were not shared as one sealed form");
        Check(sceneData.instances.size() == 2U, "A shared CIFF mesh occurrence was dropped");
        Check(sceneData.instances[0].meshIndex == 0U && sceneData.instances[1].meshIndex == 0U,
              "Shared CIFF occurrences do not reference the same form");
        Check(sceneData.nodes.size() == 1U && sceneData.nodes.front().firstInstance == 0U &&
                  sceneData.nodes.front().instanceCount == 2U,
              "Shared CIFF occurrences produced an incorrect node instance range");
    }

    class BinaryReader final
    {
      public:
        explicit BinaryReader(const std::filesystem::path& path)
        {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            Check(stream.is_open(), "Could not open generated .3d file");
            const auto end = stream.tellg();
            Check(end >= 0, "Could not determine generated .3d size");
            bytes_.resize(static_cast<std::size_t>(end));
            stream.seekg(0);
            if (!bytes_.empty())
                stream.read(reinterpret_cast<char*>(bytes_.data()), static_cast<std::streamsize>(bytes_.size()));
            Check(static_cast<std::size_t>(stream.gcount()) == bytes_.size(), "Could not read generated .3d file");
        }

        template <typename T>
        [[nodiscard]] T read(const std::uint64_t offset) const
        {
            static_assert(std::is_trivially_copyable_v<T>);
            Check(offset <= bytes_.size() && sizeof(T) <= bytes_.size() - static_cast<std::size_t>(offset),
                  "Generated .3d record exceeds the file");
            T value{};
            std::memcpy(&value, bytes_.data() + static_cast<std::size_t>(offset), sizeof(value));
            return value;
        }

        [[nodiscard]] std::string readString(const std::uint64_t offset) const
        {
            const auto length = read<std::uint32_t>(offset);
            const auto begin = offset + sizeof(std::uint32_t);
            Check(begin <= bytes_.size() && length <= bytes_.size() - static_cast<std::size_t>(begin),
                  "Generated .3d string exceeds the file");
            return std::string(reinterpret_cast<const char*>(bytes_.data() + static_cast<std::size_t>(begin)), length);
        }

        [[nodiscard]] const std::uint8_t* data(const std::uint64_t offset, const std::uint64_t byteCount) const
        {
            Check(offset <= bytes_.size() && byteCount <= bytes_.size() - static_cast<std::size_t>(offset),
                  "Generated .3d geometry slab exceeds the file");
            return bytes_.data() + static_cast<std::size_t>(offset);
        }

        [[nodiscard]] std::uint64_t size() const noexcept
        {
            return bytes_.size();
        }

      private:
        std::vector<std::uint8_t> bytes_;
    };

    struct CanonicalHeader
    {
        std::uint64_t chunkCount = 0;
        std::uint64_t formCount = 0;
        std::uint64_t instanceCount = 0;
        std::uint64_t nodeCount = 0;
        std::uint64_t materialCount = 0;
        std::uint64_t chunkOffset = 0;
        std::uint64_t formOffset = 0;
        std::uint64_t instanceOffset = 0;
        std::uint64_t nodeOffset = 0;
        std::uint64_t materialOffset = 0;
        std::uint64_t geometryOffset = 0;
        std::uint64_t fileSize = 0;
    };

    [[nodiscard]] CanonicalHeader ValidateHeader(const BinaryReader& file)
    {
        Check(file.size() >= kBinaryHeaderBytes, "Generated .3d header is truncated");
        Check(file.read<std::uint32_t>(0U) == kCiffMagic, "Generated .3d magic is incorrect");
        Check(file.read<std::uint32_t>(4U) == kBinaryVersion, "Generated .3d version is incorrect");
        Check(file.read<std::uint8_t>(8U) == 2U, "Generated .3d up axis is incorrect");
        Check(file.read<std::uint8_t>(9U) == 1U, "Generated .3d front axis is incorrect");
        Check(file.read<std::uint16_t>(10U) == kBinaryFlags, "Generated .3d feature flags are incorrect");
        Check(file.read<std::uint32_t>(12U) == kBinaryHeaderBytes, "Generated .3d header size is incorrect");
        Check(file.read<std::uint64_t>(112U) == 0U && file.read<std::uint64_t>(120U) == 0U,
              "Generated .3d reserved header fields must be zero");

        CanonicalHeader header;
        header.formCount = file.read<std::uint64_t>(16U);
        header.instanceCount = file.read<std::uint64_t>(24U);
        header.nodeCount = file.read<std::uint64_t>(32U);
        header.materialCount = file.read<std::uint64_t>(40U);
        header.chunkCount = file.read<std::uint64_t>(48U);
        header.chunkOffset = file.read<std::uint64_t>(56U);
        header.formOffset = file.read<std::uint64_t>(64U);
        header.instanceOffset = file.read<std::uint64_t>(72U);
        header.nodeOffset = file.read<std::uint64_t>(80U);
        header.materialOffset = file.read<std::uint64_t>(88U);
        header.geometryOffset = file.read<std::uint64_t>(96U);
        header.fileSize = file.read<std::uint64_t>(104U);

        Check(header.chunkCount == 1U && header.formCount == 1U && header.instanceCount == 1U &&
                  header.nodeCount == 1U && header.materialCount == 1U,
              "Generated .3d table counts are incorrect");
        Check(header.chunkOffset == kBinaryHeaderBytes, "Chunk table must immediately follow the header");
        Check(header.formOffset == header.chunkOffset + kChunkRecordBytes,
              "Form table does not follow the fixed chunk table");
        Check(header.instanceOffset == header.formOffset + kFormRecordBytes,
              "Instance table does not follow the fixed form table");
        Check(header.nodeOffset == header.instanceOffset + kInstanceRecordBytes,
              "Node table does not follow the fixed instance table");
        Check(header.nodeOffset < header.materialOffset && header.materialOffset + 4U <= header.geometryOffset,
              "Node/material/geometry sections are not monotonic");
        Check(header.geometryOffset % kGeometryAlignment == 0U, "Geometry section is not 64-byte aligned");
        Check(header.fileSize == file.size(), "Header fileSize does not match the generated file");
        return header;
    }

    void ValidateCanonical3D(const std::filesystem::path& ciffPath, const scene::SceneData& sceneData,
                             const std::filesystem::path& outputPath)
    {
        ciff::Read parsed(ciffPath.string(), outputPath.string());
        parsed.load();
        Check(f3d::convert(parsed), "f3d::convert failed for the minimal CIFF fixture");
        Check(std::filesystem::is_regular_file(outputPath), "f3d::convert did not create its output file");

        const BinaryReader file(outputPath);
        const auto header = ValidateHeader(file);
        const auto& expectedChunk = sceneData.geometryChunks.front();
        const auto& expectedMesh = sceneData.meshes.front();

        const auto chunk = header.chunkOffset;
        const auto positionsOffset = file.read<std::uint64_t>(chunk + 0U);
        const auto positionsBytes = file.read<std::uint64_t>(chunk + 8U);
        const auto normalsOffset = file.read<std::uint64_t>(chunk + 16U);
        const auto normalsBytes = file.read<std::uint64_t>(chunk + 24U);
        const auto indicesOffset = file.read<std::uint64_t>(chunk + 32U);
        const auto indicesBytes = file.read<std::uint64_t>(chunk + 40U);
        const auto vertexCount = file.read<std::uint32_t>(chunk + 48U);
        const auto indexCount = file.read<std::uint32_t>(chunk + 52U);
        Check(file.read<std::uint64_t>(chunk + 56U) == 0U, "Chunk reserved field must be zero");

        Check(positionsOffset >= header.geometryOffset && positionsOffset % kGeometryAlignment == 0U,
              "Position slab is not canonically aligned");
        Check(normalsOffset >= positionsOffset + positionsBytes && normalsOffset % kGeometryAlignment == 0U,
              "Normal slab is not canonically aligned");
        Check(indicesOffset >= normalsOffset + normalsBytes && indicesOffset % kGeometryAlignment == 0U,
              "Index slab is not canonically aligned");
        Check(indicesOffset + indicesBytes <= header.fileSize, "Index slab exceeds fileSize");
        Check(vertexCount == expectedChunk.positions.size() / 3U && indexCount == expectedChunk.indices.size(),
              "Chunk element counts are incorrect");
        Check(positionsBytes == expectedChunk.positions.size() * sizeof(float) &&
                  normalsBytes == expectedChunk.normals.size() * sizeof(float) &&
                  indicesBytes == expectedChunk.indices.size() * sizeof(std::uint32_t),
              "Chunk byte ranges are incorrect");
        Check(std::memcmp(file.data(positionsOffset, positionsBytes), expectedChunk.positions.data(),
                          static_cast<std::size_t>(positionsBytes)) == 0,
              "Position slab differs from ConvertToScene output");
        Check(std::memcmp(file.data(normalsOffset, normalsBytes), expectedChunk.normals.data(),
                          static_cast<std::size_t>(normalsBytes)) == 0,
              "Normal slab differs from ConvertToScene output");
        Check(std::memcmp(file.data(indicesOffset, indicesBytes), expectedChunk.indices.data(),
                          static_cast<std::size_t>(indicesBytes)) == 0,
              "Index slab differs from ConvertToScene output");

        const auto form = header.formOffset;
        Check(file.read<std::uint64_t>(form + 0U) == expectedMesh.shapeHash, "Form hash is incorrect");
        Check(file.read<std::uint32_t>(form + 8U) == 0U, "Form chunk index is incorrect");
        Check(file.read<std::uint32_t>(form + 12U) == expectedMesh.baseVertex &&
                  file.read<std::uint32_t>(form + 16U) == expectedMesh.vertexCount,
              "Form vertex range is incorrect");
        Check(file.read<std::uint32_t>(form + 20U) == expectedMesh.firstIndex &&
                  file.read<std::uint32_t>(form + 24U) == expectedMesh.indexCount,
              "Form index range is incorrect");
        Check(file.read<std::uint32_t>(form + 28U) == 0U && file.read<std::uint64_t>(form + 56U) == 0U,
              "Form reserved fields must be zero");
        for (std::uint64_t axis = 0U; axis < 3U; ++axis)
        {
            CheckNear(file.read<float>(form + 32U + axis * sizeof(float)), expectedMesh.boundsMin[axis], 0.0F,
                      "Form minimum bound is incorrect");
            CheckNear(file.read<float>(form + 44U + axis * sizeof(float)), expectedMesh.boundsMax[axis], 0.0F,
                      "Form maximum bound is incorrect");
        }

        const auto instance = header.instanceOffset;
        Check(file.read<std::uint32_t>(instance + 0U) == 0U && file.read<std::uint32_t>(instance + 4U) == 0U,
              "Serialized instance references are incorrect");
        for (std::uint64_t index = 0U; index < 12U; ++index)
        {
            CheckNear(file.read<float>(instance + 8U + index * sizeof(float)),
                      sceneData.instances.front().transform[index], 0.0F,
                      "Serialized instance transform is incorrect");
        }

        const auto node = header.nodeOffset;
        Check(file.read<std::int32_t>(node + 0U) == -1 && file.read<std::uint32_t>(node + 4U) == 0U &&
                  file.read<std::uint64_t>(node + 8U) == 1U,
              "Serialized root instance range is incorrect");
        const auto nodeFlags = file.read<std::uint8_t>(node + 16U);
        Check(f3d::HasValidNodeFlags(nodeFlags) && f3d::GetNodeType(nodeFlags) == f3d::NodeType::Node &&
                  !f3d::IsHidden(nodeFlags),
              "Serialized root node flags are incorrect");
        Check(file.readString(node + 17U) == "Root", "Serialized root name is incorrect");

        Check(file.read<std::uint8_t>(header.materialOffset + 0U) == 17U &&
                  file.read<std::uint8_t>(header.materialOffset + 1U) == 34U &&
                  file.read<std::uint8_t>(header.materialOffset + 2U) == 51U &&
                  file.read<std::uint8_t>(header.materialOffset + 3U) == 255U,
              "Serialized material is incorrect");
    }
}

int main(const int argc, char* argv[])
{
    try
    {
        if (argc == 2)
            return RunBenchmark(std::filesystem::path(argv[1]));
        Check(argc == 1, "Usage: CogniteCiffSmokeTests.exe [model.ciff]");
        ValidateNodeFlagsContract();

        const TempDirectory temp;
        const auto ciffPath = temp.path() / "triangle.ciff";
        const auto sharedCiffPath = temp.path() / "shared-triangle.ciff";
        const auto outputPath = temp.path() / "triangle.3d";
        WriteMinimalCiff(ciffPath);
        WriteMinimalCiff(sharedCiffPath, 2U);

        scene::SceneData sceneData;
        const auto conversion = cifflib::ConvertToScene(ciffPath, sceneData);
        Check(conversion.success, "ConvertToScene failed for the minimal CIFF fixture");
        ValidateTimings(conversion);
        ValidateScene(sceneData);
        ValidateAtomicCancellation(ciffPath);
        ValidateSharedMeshInstances(sharedCiffPath);
        ValidateCanonical3D(ciffPath, sceneData, outputPath);

        std::cout << "Cognite CIFF smoke tests passed." << std::endl;
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Cognite CIFF smoke tests failed: " << error.what() << std::endl;
        return 1;
    }
}
