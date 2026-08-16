#include "ConvertToScene.h"
#include "SceneData.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "CmdBar.h"
#include "Convert.h"
#include "MeshNormals.h"
#include "PrimitiveInstanceCIFF.h"
#include "PrimitiveStatsCIFF.h"
#include "PrimitivesCIFF.h"
#include "ReadCIFF.h"

namespace
{
    constexpr std::size_t kProgressCallbackNodeInterval = 4096;
    constexpr std::size_t kProgressCallbackGeometryInterval = 256;
    // Scale sealing to the host while keeping one shared active-work budget.
    // The estimate controls scheduling pressure; it is not an allocator-level
    // RSS limit and does not include finalized geometry retained for commit.
    constexpr std::size_t kMaxGeometrySealingWorkers = 16U;
    constexpr std::size_t kMinGeometrySealingReservationBudgetBytes = 64U * 1024U * 1024U;
    constexpr std::size_t kMaxGeometrySealingReservationBudgetBytes = 512U * 1024U * 1024U;
    constexpr std::size_t kGeometrySealingEstimatedBytesPerElement = 512U;
    constexpr std::size_t kBoxTessellatedElementUpperBound = 36U;
    constexpr std::size_t kCylinderTessellatedElementUpperBound = 768U;
    constexpr std::size_t kCircularTorusTessellatedElementUpperBound = 24948U;
    constexpr std::size_t kSphereTessellatedElementUpperBound = 2160U;
    constexpr std::size_t kSphericalDishTessellatedElementUpperBound = 1656U;
    constexpr std::size_t kGeneralCylinderTessellatedElementUpperBound = 768U;
    constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
    constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

    using Matrix3x4 = ciff::primitive_instance::Matrix3x4;

    using TimingClock = std::chrono::steady_clock;

    [[nodiscard]] constexpr std::size_t SaturatingMultiply(
        const std::size_t lhs,
        const std::size_t rhs) noexcept
    {
        if (lhs != 0U && rhs > (std::numeric_limits<std::size_t>::max)() / lhs)
            return (std::numeric_limits<std::size_t>::max)();
        return lhs * rhs;
    }

    [[nodiscard]] constexpr std::size_t ParametricTessellatedElementUpperBound(
        const ciff::Type primitive) noexcept
    {
        switch (primitive)
        {
            case ciff::Type::Box:
                return kBoxTessellatedElementUpperBound;
            case ciff::Type::Cylinder:
                return kCylinderTessellatedElementUpperBound;
            case ciff::Type::CircularTorus:
                return kCircularTorusTessellatedElementUpperBound;
            case ciff::Type::Sphere:
                return kSphereTessellatedElementUpperBound;
            case ciff::Type::SphericalDish:
                return kSphericalDishTessellatedElementUpperBound;
            case ciff::Type::GeneralCylinder:
                return kGeneralCylinderTessellatedElementUpperBound;
            default:
                return (std::numeric_limits<std::size_t>::max)();
        }
    }

    [[nodiscard]] std::size_t EstimateGeometrySealingReservation(
        const ciff::Read& data,
        const ciff::Geometry& geometry) noexcept
    {
        auto sourceElementCount = std::size_t{};
        if (geometry.primitive == ciff::Type::Mesh)
        {
            if (geometry.mesh >= data.meshes.size())
                return (std::numeric_limits<std::size_t>::max)();

            const auto& mesh = data.meshes[geometry.mesh];
            sourceElementCount = (std::max)(mesh.vertices.size(), mesh.indices.size());
        }
        else
        {
            sourceElementCount = ParametricTessellatedElementUpperBound(geometry.primitive);
        }

        return SaturatingMultiply(sourceElementCount, kGeometrySealingEstimatedBytesPerElement);
    }

    [[nodiscard]] std::size_t GeometrySealingReservationBudgetBytes() noexcept
    {
        MEMORYSTATUSEX memoryStatus{};
        memoryStatus.dwLength = sizeof(memoryStatus);
        if (::GlobalMemoryStatusEx(&memoryStatus) == FALSE)
            return kMinGeometrySealingReservationBudgetBytes;

        const auto toSize = [](const DWORDLONG bytes) noexcept
        {
            constexpr auto sizeMaximum = (std::numeric_limits<std::size_t>::max)();
            if (bytes > static_cast<DWORDLONG>(sizeMaximum))
                return sizeMaximum;
            return static_cast<std::size_t>(bytes);
        };

        // Keep the active sealing allowance small relative to both the host and
        // the memory still available after CIFF source data has been loaded.
        // This remains a scheduling estimate rather than a hard RSS limit.
        const auto totalMemoryShare = toSize(memoryStatus.ullTotalPhys / 32U);
        const auto availableMemoryShare = toSize(memoryStatus.ullAvailPhys / 8U);
        const auto adaptiveBudget = (std::min)(totalMemoryShare, availableMemoryShare);
        return (std::clamp)(adaptiveBudget,
                            kMinGeometrySealingReservationBudgetBytes,
                            kMaxGeometrySealingReservationBudgetBytes);
    }

    [[nodiscard]] std::size_t ParseWindowsPhysicalCoreCount(
        const std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>& topology,
        const DWORD byteCount) noexcept
    {
        const auto capacityBytes = topology.size() * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX);
        if (byteCount == 0U || static_cast<std::size_t>(byteCount) > capacityBytes)
            return 0U;

        constexpr auto recordHeaderBytes = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor);
        const auto* bytes = reinterpret_cast<const std::byte*>(topology.data());
        auto offset = std::size_t{};
        auto coreCount = std::size_t{};
        while (offset < byteCount)
        {
            const auto remaining = static_cast<std::size_t>(byteCount) - offset;
            if (remaining < recordHeaderBytes)
                return 0U;

            const auto* record =
                reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(bytes + offset);
            const auto recordBytes = static_cast<std::size_t>(record->Size);
            if (recordBytes < recordHeaderBytes || recordBytes > remaining)
                return 0U;

            if (record->Relationship == RelationProcessorCore)
                ++coreCount;
            offset += recordBytes;
        }

        return offset == byteCount ? coreCount : 0U;
    }

    [[nodiscard]] std::size_t QueryWindowsPhysicalCoreCount() noexcept
    {
        try
        {
            DWORD requiredBytes = 0U;
            if (::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &requiredBytes) == FALSE &&
                (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || requiredBytes == 0U))
            {
                return 0U;
            }
            if (requiredBytes == 0U)
                return 0U;

            for (unsigned int attempt = 0U; attempt < 3U; ++attempt)
            {
                const auto recordSize = sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX);
                const auto elementCount = static_cast<std::size_t>(requiredBytes) / recordSize +
                                          (requiredBytes % recordSize != 0U ? 1U : 0U);
                auto topology = std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(elementCount);

                DWORD returnedBytes = requiredBytes;
                if (::GetLogicalProcessorInformationEx(RelationProcessorCore, topology.data(), &returnedBytes) != FALSE)
                    return ParseWindowsPhysicalCoreCount(topology, returnedBytes);

                if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || returnedBytes <= requiredBytes)
                    return 0U;
                requiredBytes = returnedBytes;
            }
        }
        catch (...)
        {
            // Topology-buffer failures use the conservative fallback.
        }

        return 0U;
    }

    [[nodiscard]] std::size_t FallbackPhysicalCoreCount() noexcept
    {
        const auto logicalCores = static_cast<std::size_t>(std::thread::hardware_concurrency());
        if (logicalCores <= 1U)
            return 1U;

        // Conservatively assume two SMT threads per physical core when Windows
        // topology information is unavailable.
        const auto assumedPhysicalCores = logicalCores / 2U;
        return assumedPhysicalCores == 0U ? 1U : assumedPhysicalCores;
    }

    [[nodiscard]] std::size_t GeometrySealingWorkerCount(const std::size_t taskCount) noexcept
    {
        if (taskCount == 0U)
            return 0U;

        auto physicalCores = QueryWindowsPhysicalCoreCount();
        if (physicalCores == 0U)
            physicalCores = FallbackPhysicalCoreCount();

        // Retain one physical core for cancellation polling and the surrounding
        // serial SceneData projection stages.
        const auto desiredWorkers = physicalCores > 1U ? physicalCores - 1U : 1U;
        return (std::min)({ desiredWorkers, kMaxGeometrySealingWorkers, taskCount });
    }

    [[nodiscard]] double MillisecondsSince(const TimingClock::time_point start) noexcept
    {
        return std::chrono::duration<double, std::milli>(TimingClock::now() - start).count();
    }

    class MillisecondTimer
    {
      public:
        explicit MillisecondTimer(double& result) noexcept
            : result_(result), start_(TimingClock::now())
        {
        }

        ~MillisecondTimer()
        {
            Stop();
        }

        void Stop() noexcept
        {
            if (stopped_)
                return;
            result_ = MillisecondsSince(start_);
            stopped_ = true;
        }

        MillisecondTimer(const MillisecondTimer&) = delete;
        MillisecondTimer& operator=(const MillisecondTimer&) = delete;

      private:
        double& result_;
        TimingClock::time_point start_;
        bool stopped_ = false;
    };

    class AccumulatingMillisecondTimer
    {
      public:
        explicit AccumulatingMillisecondTimer(double& result) noexcept
            : result_(result), start_(TimingClock::now())
        {
        }

        ~AccumulatingMillisecondTimer()
        {
            result_ += MillisecondsSince(start_);
        }

        AccumulatingMillisecondTimer(const AccumulatingMillisecondTimer&) = delete;
        AccumulatingMillisecondTimer& operator=(const AccumulatingMillisecondTimer&) = delete;

      private:
        double& result_;
        TimingClock::time_point start_;
    };

    // SceneConvert necessarily interleaves hierarchy traversal and instance
    // emission with geometry work. Time the traversal once, then remove the
    // explicitly measured geometry and descriptor portions. This avoids adding
    // a high-resolution clock read to every instance in large models.
    class ResidualSceneWorkTimer
    {
      public:
        explicit ResidualSceneWorkTimer(cifflib::ConvertToSceneTimings& timings) noexcept
            : timings_(timings), geometryBefore_(timings.geometryFinalizeMs),
              descriptorBefore_(timings.meshDescriptorMs), start_(TimingClock::now())
        {
        }

        ~ResidualSceneWorkTimer()
        {
            const auto elapsed = MillisecondsSince(start_);
            const auto geometry = timings_.geometryFinalizeMs - geometryBefore_;
            const auto descriptors = timings_.meshDescriptorMs - descriptorBefore_;
            timings_.instanceHierarchyMaterialMs += std::max(0.0, elapsed - geometry - descriptors);
        }

        ResidualSceneWorkTimer(const ResidualSceneWorkTimer&) = delete;
        ResidualSceneWorkTimer& operator=(const ResidualSceneWorkTimer&) = delete;

      private:
        cifflib::ConvertToSceneTimings& timings_;
        double geometryBefore_;
        double descriptorBefore_;
        TimingClock::time_point start_;
    };

    void AddHashBytes(std::uint64_t& hash, const void* data, const std::size_t byteCount) noexcept
    {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t index = 0; index < byteCount; ++index)
        {
            hash ^= bytes[index];
            hash *= kFnvPrime;
        }
    }

    [[nodiscard]] scene::Mesh BuildMeshDescriptor(const scene::GeometryChunk& chunk, const std::uint32_t chunkIndex)
    {
        if (chunk.positions.empty() || chunk.indices.empty())
            throw std::runtime_error("CIFF geometry finalization produced an empty mesh");
        if (chunk.positions.size() % 3U != 0U || chunk.normals.size() != chunk.positions.size())
            throw std::runtime_error("CIFF geometry finalization produced inconsistent vertex streams");
        if (chunk.indices.size() % 3U != 0U)
            throw std::runtime_error("CIFF geometry finalization produced a non-triangular index stream");

        const auto vertexCount = chunk.positions.size() / 3U;
        const auto triangleCount = chunk.indices.size() / 3U;
        if (vertexCount > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("CIFF scene mesh vertex count exceeds uint32_t");
        if (chunk.indices.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("CIFF scene mesh index count exceeds uint32_t");
        if (triangleCount > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("CIFF scene mesh triangle count exceeds uint32_t");

        const auto pointCount32 = static_cast<std::uint32_t>(vertexCount);
        const auto triangleCount32 = static_cast<std::uint32_t>(triangleCount);
        auto shapeHash = kFnvOffsetBasis;
        AddHashBytes(shapeHash, &pointCount32, sizeof(pointCount32));

        scene::Mesh mesh;
        mesh.chunkIndex = chunkIndex;
        mesh.vertexCount = pointCount32;
        mesh.indexCount = static_cast<std::uint32_t>(chunk.indices.size());
        for (std::size_t axis = 0; axis < 3U; ++axis)
        {
            mesh.boundsMin[axis] = std::numeric_limits<float>::max();
            mesh.boundsMax[axis] = std::numeric_limits<float>::lowest();
        }

        for (std::size_t offset = 0; offset < chunk.positions.size(); offset += 3U)
        {
            AddHashBytes(shapeHash, chunk.positions.data() + offset, 3U * sizeof(chunk.positions[0]));
            for (std::size_t axis = 0; axis < 3U; ++axis)
            {
                const auto value = chunk.positions[offset + axis];
                if (!std::isfinite(value))
                    throw std::runtime_error("CIFF scene mesh contains a non-finite position");
                mesh.boundsMin[axis] = std::min(mesh.boundsMin[axis], value);
                mesh.boundsMax[axis] = std::max(mesh.boundsMax[axis], value);
            }
        }

        for (std::size_t offset = 0; offset < chunk.normals.size(); offset += 3U)
        {
            const auto nx = chunk.normals[offset + 0U];
            const auto ny = chunk.normals[offset + 1U];
            const auto nz = chunk.normals[offset + 2U];
            const auto normalLengthSquared =
                static_cast<double>(nx) * nx + static_cast<double>(ny) * ny + static_cast<double>(nz) * nz;
            if (!std::isfinite(nx) || !std::isfinite(ny) || !std::isfinite(nz) || !std::isfinite(normalLengthSquared) ||
                normalLengthSquared <= 0.0 || std::abs(normalLengthSquared - 1.0) > 1.0e-3)
            {
                throw std::runtime_error("CIFF scene mesh contains an invalid normal");
            }
            AddHashBytes(shapeHash, chunk.normals.data() + offset, 3U * sizeof(chunk.normals[0]));
        }

        AddHashBytes(shapeHash, &triangleCount32, sizeof(triangleCount32));
        for (const auto index : chunk.indices)
        {
            if (index >= vertexCount)
                throw std::runtime_error("CIFF scene mesh index is outside its vertex range");
            AddHashBytes(shapeHash, &index, sizeof(index));
        }
        mesh.shapeHash = shapeHash == 0U ? 1U : shapeHash;
        return mesh;
    }

    void BuildMaterials(scene::SceneData& sceneData, const std::vector<ciff::rgb>& source)
    {
        if (source.size() >= scene::kInvalidIndex)
            throw std::runtime_error("CIFF scene contains too many materials");

        sceneData.materials.reserve(std::max<std::size_t>(source.size(), 1U));
        if (source.empty())
        {
            sceneData.materials.emplace_back();
            return;
        }

        for (const auto& color : source)
        {
            scene::Material material;
            material.r = color.r;
            material.g = color.g;
            material.b = color.b;
            material.a = color.a;
            sceneData.materials.push_back(material);
        }
    }

    void ValidateGeometryReference(const ciff::Read& data, const ciff::Geometry& geometry)
    {
        switch (geometry.primitive)
        {
            case ciff::Type::Mesh:
                if (geometry.mesh >= data.meshes.size())
                    throw std::runtime_error("CIFF geometry refers to a missing mesh");
                break;
            case ciff::Type::Box:
                if (geometry.primitiveIndex >= data.boxes.size())
                    throw std::runtime_error("CIFF geometry refers to a missing box");
                break;
            case ciff::Type::Cylinder:
                if (geometry.primitiveIndex >= data.cylinders.size())
                    throw std::runtime_error("CIFF geometry refers to a missing cylinder");
                break;
            case ciff::Type::CircularTorus:
                if (geometry.primitiveIndex >= data.circularToruses.size())
                    throw std::runtime_error("CIFF geometry refers to a missing circular torus");
                break;
            case ciff::Type::Sphere:
                if (geometry.primitiveIndex >= data.spheres.size())
                    throw std::runtime_error("CIFF geometry refers to a missing sphere");
                break;
            case ciff::Type::SphericalDish:
                if (geometry.primitiveIndex >= data.sphericalDishes.size())
                    throw std::runtime_error("CIFF geometry refers to a missing spherical dish");
                break;
            case ciff::Type::GeneralCylinder:
                if (geometry.primitiveIndex >= data.generalCylinders.size())
                    throw std::runtime_error("CIFF geometry refers to a missing general cylinder");
                break;
            default:
                throw std::runtime_error("CIFF geometry has an unsupported primitive type");
        }
    }

    void ValidateSealedScene(const scene::SceneData& sceneData)
    {
        if (sceneData.nodes.empty() || sceneData.rootNode != 0U || sceneData.nodes.front().parent != -1)
            throw std::runtime_error("CIFF scene does not satisfy the required root hierarchy");
        if (sceneData.materials.empty() || sceneData.materials.size() >= scene::kInvalidIndex)
            throw std::runtime_error("CIFF scene does not contain a valid material table");
        if (sceneData.geometryChunks.size() != sceneData.meshes.size())
            throw std::runtime_error("CIFF scene geometry and mesh descriptor counts differ");
        if (sceneData.meshes.size() >= scene::kInvalidIndex || sceneData.instances.size() >= scene::kInvalidIndex)
            throw std::runtime_error("CIFF scene exceeds the 32-bit object range");

        for (std::size_t meshIndex = 0; meshIndex < sceneData.meshes.size(); ++meshIndex)
        {
            const auto& mesh = sceneData.meshes[meshIndex];
            if (mesh.shapeHash == 0U || mesh.chunkIndex >= sceneData.geometryChunks.size())
                throw std::runtime_error("CIFF scene contains an invalid mesh descriptor");
            const auto& chunk = sceneData.geometryChunks[mesh.chunkIndex];
            if (chunk.positions.size() % 3U != 0U || chunk.normals.size() != chunk.positions.size())
                throw std::runtime_error("CIFF scene contains inconsistent geometry streams");

            const auto chunkVertexCount = chunk.positions.size() / 3U;
            const auto vertexEnd = static_cast<std::uint64_t>(mesh.baseVertex) + mesh.vertexCount;
            const auto indexEnd = static_cast<std::uint64_t>(mesh.firstIndex) + mesh.indexCount;
            if (vertexEnd > chunkVertexCount || indexEnd > chunk.indices.size())
                throw std::runtime_error("CIFF scene mesh range is outside its geometry chunk");
            for (std::size_t axis = 0; axis < 3U; ++axis)
            {
                if (!std::isfinite(mesh.boundsMin[axis]) || !std::isfinite(mesh.boundsMax[axis]) ||
                    mesh.boundsMin[axis] > mesh.boundsMax[axis])
                {
                    throw std::runtime_error("CIFF scene mesh contains invalid local bounds");
                }
            }
        }

        std::size_t expectedFirstInstance = 0U;
        for (std::size_t nodeIndex = 0; nodeIndex < sceneData.nodes.size(); ++nodeIndex)
        {
            const auto& node = sceneData.nodes[nodeIndex];
            if (node.firstInstance != expectedFirstInstance)
                throw std::runtime_error("CIFF scene contains non-contiguous node instance ranges");
            const auto instanceEnd = static_cast<std::uint64_t>(node.firstInstance) + node.instanceCount;
            if (instanceEnd > sceneData.instances.size())
                throw std::runtime_error("CIFF node instance range exceeds the scene instance table");
            for (std::uint64_t instanceIndex = node.firstInstance; instanceIndex < instanceEnd; ++instanceIndex)
            {
                const auto& instance = sceneData.instances[static_cast<std::size_t>(instanceIndex)];
                if (instance.nodeIndex != nodeIndex)
                    throw std::runtime_error("CIFF instance is linked to the wrong hierarchy node");
                if (instance.meshIndex >= sceneData.meshes.size() ||
                    instance.materialIndex >= sceneData.materials.size())
                {
                    throw std::runtime_error("CIFF scene contains an invalid instance reference");
                }
            }
            expectedFirstInstance = static_cast<std::size_t>(instanceEnd);
        }
        if (expectedFirstInstance != sceneData.instances.size())
            throw std::runtime_error("CIFF scene contains instances outside the hierarchy");

        std::vector<std::uint8_t> state(sceneData.nodes.size(), 0U);
        std::vector<std::size_t> path;
        for (std::size_t start = 0; start < sceneData.nodes.size(); ++start)
        {
            path.clear();
            auto cursor = start;
            while (true)
            {
                if (cursor >= sceneData.nodes.size())
                    throw std::runtime_error("CIFF scene contains an invalid parent node index");
                if (state[cursor] == 2U)
                    break;
                if (state[cursor] == 1U)
                    throw std::runtime_error("CIFF scene contains a parent cycle");

                state[cursor] = 1U;
                path.push_back(cursor);
                const auto parent = sceneData.nodes[cursor].parent;
                if (parent < 0)
                {
                    if (cursor != sceneData.rootNode)
                        throw std::runtime_error("CIFF scene contains nodes disconnected from the root");
                    break;
                }
                cursor = static_cast<std::size_t>(parent);
            }
            for (const auto nodeIndex : path)
                state[nodeIndex] = 2U;
        }
    }

    struct SceneConvert final : ciff::Convert
    {
        SceneConvert(ciff::Read& data, scene::SceneData& out, const cifflib::ConvertProgressCallback& cb,
                     cifflib::ConvertToSceneTimings& convertTimings)
            : ciff::Convert(data), sd(out), callback(cb), timings(convertTimings)
        {
        }

        bool SetFile() override
        {
            source_file = data.source_cad;
            target_file.clear();
            return true;
        }

        void RunConvert()
        {
            SetFile();
            convert();
        }

        void SealGeometryAndProjectInstances()
        {
            SealGeometryTasks();
            ProjectOccurrences();
            primitiveStats.Print(source_file);
        }

        void WriteHeader() override
        {
        }

        void WriteNode(const ciff::Node& node) override
        {
            if (sd.nodes.size() >= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
                throw std::runtime_error("CIFF scene contains too many hierarchy nodes");

            scene::Node n;
            if (sd.nodes.empty())
            {
                n.parent = -1;
            }
            else
            {
                if (node.parentIndex >= sd.nodes.size() ||
                    node.parentIndex > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
                {
                    throw std::runtime_error("CIFF node parent is out of range");
                }
                n.parent = static_cast<std::int32_t>(node.parentIndex);
            }
            n.name = node.name;
            sd.nodes.push_back(std::move(n));
        }

        void WriteGeometry(const ciff::Node&, const size_t geometryIndex) override
        {
            ++processedGeometryCount;
            if (callback && processedGeometryCount % kProgressCallbackGeometryInterval == 0U &&
                !callback(data.nodes.size(), 0U))
            {
                throw std::runtime_error("Loading was cancelled by the user.");
            }

            if (geometryIndex >= data.geometries.size())
                throw std::runtime_error("CIFF node refers to a missing geometry record");
            if (sd.nodes.empty())
                throw std::runtime_error("CIFF geometry does not belong to a hierarchy node");

            const auto& geom = data.geometries[geometryIndex];
            ValidateGeometryReference(data, geom);
            const auto material = geom.color < sd.materials.size() ? static_cast<std::uint32_t>(geom.color) : 0U;
            auto form = ciff::primitive_instance::Make(data, geom);
            const auto useTaskTransform = form.hashMesh;
            const auto taskIndex = AddOrFindSealingTask(geometryIndex, geom, form);

            PendingOccurrence occurrence;
            occurrence.geometryIndex = geometryIndex;
            occurrence.taskIndex = taskIndex;
            occurrence.nodeIndex = static_cast<std::uint32_t>(sd.nodes.size() - 1U);
            occurrence.materialIndex = material;
            occurrence.transform = form.transform;
            occurrence.useTaskTransform = useTaskTransform;
            pendingOccurrences.push_back(std::move(occurrence));
        }

        void WriteMaterial(bool) override
        {
        }

        void WriteFooter() override
        {
            // Finish the lightweight catalog pass with a cancellation probe for
            // small files that never reached the coarse geometry interval.
            if (callback && processedGeometryCount > 0U &&
                processedGeometryCount % kProgressCallbackGeometryInterval != 0U &&
                !callback(data.nodes.size(), 0U))
            {
                throw std::runtime_error("Loading was cancelled by the user.");
            }
        }

      private:
        struct SealingTask
        {
            std::size_t geometryIndex = 0U;
            ciff::primitive_instance::FormInstance form;
            ciff::normal_processing::RenderGeometry finalMesh;
            std::uint32_t committedMeshIndex = scene::kInvalidIndex;
        };

        struct PendingOccurrence
        {
            std::size_t geometryIndex = 0U;
            std::uint32_t taskIndex = scene::kInvalidIndex;
            std::uint32_t nodeIndex = scene::kInvalidIndex;
            std::uint32_t materialIndex = 0U;
            Matrix3x4 transform = ciff::shape::identity();
            bool useTaskTransform = false;
        };

        [[nodiscard]] std::uint32_t AddSealingTask(
            const std::size_t geometryIndex,
            ciff::primitive_instance::FormInstance form)
        {
            if (sealingTasks.size() >= scene::kInvalidIndex)
                throw std::runtime_error("CIFF scene contains too many geometry sealing tasks");

            const auto taskIndex = static_cast<std::uint32_t>(sealingTasks.size());
            SealingTask task;
            task.geometryIndex = geometryIndex;
            task.form = std::move(form);
            sealingTasks.push_back(std::move(task));
            return taskIndex;
        }

        [[nodiscard]] std::uint32_t AddOrFindSealingTask(
            const std::size_t geometryIndex,
            const ciff::Geometry& geometry,
            const ciff::primitive_instance::FormInstance& form)
        {
            // A nonzero pre-hash is the legacy converter's canonical primitive
            // sharing key. Reusing the first task is therefore exactly the same
            // early-deduplication decision that the serial path made.
            if (form.hash != 0U)
            {
                if (const auto found = taskByPreHash.find(form.hash); found != taskByPreHash.end())
                    return found->second;

                const auto taskIndex = AddSealingTask(geometryIndex, form);
                taskByPreHash.emplace(form.hash, taskIndex);
                return taskIndex;
            }

            // Mesh forms acquire their canonical hash during tessellation. Two
            // occurrences that reference the exact same immutable source mesh
            // necessarily produce the same form, normalized transform and
            // render streams, so seal that source allocation only once.
            if (geometry.primitive == ciff::Type::Mesh)
            {
                if (const auto found = taskBySourceMesh.find(geometry.mesh); found != taskBySourceMesh.end())
                    return found->second;

                const auto taskIndex = AddSealingTask(geometryIndex, form);
                taskBySourceMesh.emplace(geometry.mesh, taskIndex);
                return taskIndex;
            }

            return AddSealingTask(geometryIndex, form);
        }

        [[nodiscard]] std::vector<std::size_t> GeometryTaskReservations(
            const std::vector<std::uint32_t>& taskIndices) const
        {
            auto reservations = std::vector<std::size_t>{};
            reservations.reserve(taskIndices.size());
            for (const auto taskIndex : taskIndices)
            {
                if (taskIndex >= sealingTasks.size())
                    throw std::runtime_error("CIFF geometry reservation refers to a missing sealing task");
                const auto& task = sealingTasks[taskIndex];
                if (task.geometryIndex >= data.geometries.size())
                    throw std::runtime_error("CIFF geometry reservation refers to a missing geometry record");

                reservations.push_back(
                    EstimateGeometrySealingReservation(data, data.geometries[task.geometryIndex]));
            }

            return reservations;
        }

        template <typename Work>
        void RunGeometryWorkers(const std::vector<std::size_t>& taskReservations, Work work)
        {
            const auto taskCount = taskReservations.size();
            if (taskCount == 0U)
                return;
            const auto reservationBudgetBytes = GeometrySealingReservationBudgetBytes();

            struct SchedulerState final
            {
                std::mutex mutex;
                std::condition_variable changed;
                std::exception_ptr workerError;
                std::size_t nextTaskIndex = 0U;
                std::size_t activeTaskCount = 0U;
                std::size_t activeReservationBytes = 0U;
                std::size_t finishedWorkerCount = 0U;
                bool stopRequested = false;
            } scheduler;

            const auto canAssignNextTask = [&]() noexcept
            {
                if (scheduler.nextTaskIndex >= taskCount)
                    return false;

                const auto reservation = taskReservations[scheduler.nextTaskIndex];
                if (scheduler.activeReservationBytes <= reservationBudgetBytes &&
                    reservation <=
                        reservationBudgetBytes - scheduler.activeReservationBytes)
                {
                    return true;
                }

                // A task estimated above the shared budget can still make
                // progress, but only as the sole active sealing task.
                return scheduler.activeTaskCount == 0U && scheduler.activeReservationBytes == 0U;
            };

            const auto workerCount = GeometrySealingWorkerCount(taskCount);
            const auto sealWorker = [&]() noexcept
            {
                auto activeReservationBytes = std::size_t{};
                auto ownsActiveReservation = false;
                try
                {
                    while (true)
                    {
                        auto taskIndex = std::size_t{};
                        {
                            auto lock = std::unique_lock{ scheduler.mutex };
                            scheduler.changed.wait(lock, [&]() noexcept
                            {
                                return scheduler.stopRequested ||
                                       scheduler.nextTaskIndex >= taskCount ||
                                       canAssignNextTask();
                            });

                            if (scheduler.stopRequested || scheduler.nextTaskIndex >= taskCount)
                                break;

                            taskIndex = scheduler.nextTaskIndex++;
                            activeReservationBytes = taskReservations[taskIndex];
                            scheduler.activeReservationBytes += activeReservationBytes;
                            ++scheduler.activeTaskCount;
                            ownsActiveReservation = true;
                        }

                        work(taskIndex);

                        {
                            const auto lock = std::lock_guard{ scheduler.mutex };
                            scheduler.activeReservationBytes -= activeReservationBytes;
                            --scheduler.activeTaskCount;
                            ownsActiveReservation = false;
                        }
                        scheduler.changed.notify_all();
                    }
                }
                catch (...)
                {
                    {
                        const auto lock = std::lock_guard{ scheduler.mutex };
                        if (ownsActiveReservation)
                        {
                            scheduler.activeReservationBytes -= activeReservationBytes;
                            --scheduler.activeTaskCount;
                            ownsActiveReservation = false;
                        }
                        if (!scheduler.workerError)
                            scheduler.workerError = std::current_exception();
                        scheduler.stopRequested = true;
                    }
                    scheduler.changed.notify_all();
                }

                {
                    const auto lock = std::lock_guard{ scheduler.mutex };
                    ++scheduler.finishedWorkerCount;
                }
                scheduler.changed.notify_all();
            };

            auto workers = std::vector<std::thread>{};
            workers.reserve(workerCount);
            const auto joinWorkers = [&workers]()
            {
                for (auto& worker : workers)
                {
                    if (worker.joinable())
                        worker.join();
                }
            };

            const auto requestStop = [&]() noexcept
            {
                {
                    const auto lock = std::lock_guard{ scheduler.mutex };
                    scheduler.stopRequested = true;
                }
                scheduler.changed.notify_all();
            };

            try
            {
                for (std::size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex)
                    workers.emplace_back(sealWorker);
            }
            catch (...)
            {
                requestStop();
                joinWorkers();
                throw;
            }

            auto cancelledByUser = false;
            try
            {
                while (callback)
                {
                    {
                        const auto lock = std::lock_guard{ scheduler.mutex };
                        if (scheduler.finishedWorkerCount >= workerCount || scheduler.workerError)
                            break;
                    }

                    if (!callback(data.nodes.size(), 0U))
                    {
                        cancelledByUser = true;
                        requestStop();
                        break;
                    }

                    auto lock = std::unique_lock{ scheduler.mutex };
                    scheduler.changed.wait_for(lock, std::chrono::milliseconds{ 8 }, [&]() noexcept
                    {
                        return scheduler.finishedWorkerCount >= workerCount || scheduler.workerError;
                    });
                }
                joinWorkers();
            }
            catch (...)
            {
                requestStop();
                joinWorkers();
                throw;
            }

            auto workerError = std::exception_ptr{};
            {
                const auto lock = std::lock_guard{ scheduler.mutex };
                workerError = scheduler.workerError;
            }
            if (workerError)
                std::rethrow_exception(workerError);
            if (cancelledByUser)
                throw std::runtime_error("Loading was cancelled by the user.");
        }

        void SealGeometryTasks()
        {
            AccumulatingMillisecondTimer geometryTimer(timings.geometryFinalizeMs);
            if (sealingTasks.empty())
                return;

            // Mesh records do not acquire their canonical sharing hash or
            // normalized instance transform until Tessellate runs. Establish
            // those lightweight form identities first and discard the temporary
            // mesh immediately. This is the bounded-memory analogue of NWD's
            // catalog pass: no RenderGeometry result exists yet.
            auto identityTaskIndices = std::vector<std::uint32_t>{};
            identityTaskIndices.reserve(sealingTasks.size());
            for (std::size_t taskIndex = 0; taskIndex < sealingTasks.size(); ++taskIndex)
            {
                if (sealingTasks[taskIndex].form.hash == 0U)
                    identityTaskIndices.push_back(static_cast<std::uint32_t>(taskIndex));
            }

            {
                const auto identityTaskReservations = GeometryTaskReservations(identityTaskIndices);
                RunGeometryWorkers(identityTaskReservations, [&](const std::size_t identityIndex)
                {
                    auto& task = sealingTasks[identityTaskIndices[identityIndex]];
                    const auto& geometry = data.geometries[task.geometryIndex];
                    auto form = ciff::primitive_instance::Make(data, geometry);
                    [[maybe_unused]] auto identityMesh =
                        ciff::primitive_instance::Tessellate(data, geometry, form);
                    task.form = std::move(form);
                });
            }

            // Resolve canonical sharing on the coordinator in original
            // occurrence order. Besides keeping IDs deterministic, this captures
            // each baked mesh's own normalized transform before occurrences that
            // share a shape are redirected to the first representative task.
            auto representativeByHash = std::unordered_map<std::uint64_t, std::uint32_t>{};
            representativeByHash.reserve(sealingTasks.size());
            uniqueSealingTaskIndices.clear();
            uniqueSealingTaskIndices.reserve(sealingTasks.size());
            for (auto& occurrence : pendingOccurrences)
            {
                if (occurrence.taskIndex >= sealingTasks.size())
                    throw std::runtime_error("CIFF occurrence refers to a missing geometry task");
                auto& sourceTask = sealingTasks[occurrence.taskIndex];
                if (occurrence.useTaskTransform)
                {
                    occurrence.transform = sourceTask.form.transform;
                    occurrence.useTaskTransform = false;
                }

                const auto shapeHash = sourceTask.form.hash;
                if (shapeHash == 0U)
                {
                    occurrence.taskIndex = scene::kInvalidIndex;
                    continue;
                }

                if (const auto found = representativeByHash.find(shapeHash);
                    found != representativeByHash.end())
                {
                    occurrence.taskIndex = found->second;
                    continue;
                }

                representativeByHash.emplace(shapeHash, occurrence.taskIndex);
                uniqueSealingTaskIndices.push_back(occurrence.taskIndex);
            }

            // Each worker now owns one canonical form and writes only its stable
            // task slot. Re-tessellating only the unique representatives keeps
            // peak memory at source data + final unique output, while avoiding
            // expensive render-normal generation for duplicate occurrences.
            {
                const auto uniqueTaskReservations = GeometryTaskReservations(uniqueSealingTaskIndices);
                RunGeometryWorkers(uniqueTaskReservations, [&](const std::size_t uniqueIndex)
                {
                    auto& task = sealingTasks[uniqueSealingTaskIndices[uniqueIndex]];
                    const auto expectedHash = task.form.hash;
                    if (expectedHash == 0U)
                        throw std::runtime_error("CIFF unique geometry task has no resolved identity");
                    const auto& geometry = data.geometries[task.geometryIndex];
                    auto form = task.form;
                    auto localMesh = ciff::primitive_instance::TessellateResolved(data, geometry, form);
                    if (form.hash != expectedHash)
                        throw std::runtime_error("CIFF canonical geometry identity changed between sealing passes");
                    task.form = std::move(form);
                    if (!localMesh.empty())
                        task.finalMesh = ciff::normal_processing::FinalizeMeshNormals(std::move(localMesh));
                });
            }
        }

        void ProjectOccurrences()
        {
            sd.geometryChunks.reserve(uniqueSealingTaskIndices.size());
            sd.meshes.reserve(uniqueSealingTaskIndices.size());
            sd.instances.reserve(pendingOccurrences.size());
            formIndexByHash.reserve(uniqueSealingTaskIndices.size());

            auto occurrenceIndex = std::size_t{ 0U };
            for (std::size_t nodeIndex = 0; nodeIndex < sd.nodes.size(); ++nodeIndex)
            {
                if (sd.instances.size() >= scene::kInvalidIndex)
                    throw std::runtime_error("CIFF scene contains too many instances");
                auto& node = sd.nodes[nodeIndex];
                node.firstInstance = static_cast<std::uint32_t>(sd.instances.size());

                while (occurrenceIndex < pendingOccurrences.size() &&
                       pendingOccurrences[occurrenceIndex].nodeIndex == nodeIndex)
                {
                    const auto& occurrence = pendingOccurrences[occurrenceIndex++];
                    if (occurrence.taskIndex == scene::kInvalidIndex)
                        continue;
                    if (occurrence.taskIndex >= sealingTasks.size())
                        throw std::runtime_error("CIFF occurrence refers to a missing geometry task");
                    auto& task = sealingTasks[occurrence.taskIndex];
                    if (task.form.hash == 0U)
                        continue;

                    if (task.committedMeshIndex == scene::kInvalidIndex)
                    {
                        if (task.finalMesh.empty())
                            continue;
                        task.committedMeshIndex = AddOrFindForm(task.form.hash, std::move(task.finalMesh));
                    }

                    const auto& geometry = data.geometries[occurrence.geometryIndex];
                    primitiveStats.Record(geometry, task.form.hash);
                    EmitInstance(
                        task.committedMeshIndex,
                        occurrence.materialIndex,
                        static_cast<std::uint32_t>(nodeIndex),
                        occurrence.transform);
                }

                const auto instanceEnd = static_cast<std::uint32_t>(sd.instances.size());
                node.instanceCount = instanceEnd - node.firstInstance;

                const auto completed = nodeIndex + 1U;
                const bool progressCallbackDue =
                    completed % kProgressCallbackNodeInterval == 0U || completed == sd.nodes.size();
                if (callback && progressCallbackDue && !callback(sd.nodes.size(), completed))
                    throw std::runtime_error("Loading was cancelled by the user.");
            }

            if (occurrenceIndex != pendingOccurrences.size())
                throw std::runtime_error("CIFF scene contains geometry outside the hierarchy");
        }

        uint32_t AddOrFindForm(const uint64_t shapeHash, ciff::normal_processing::RenderGeometry&& mesh)
        {
            if (const auto it = formIndexByHash.find(shapeHash); it != formIndexByHash.end())
            {
                mesh = {};
                return it->second;
            }

            scene::GeometryChunk chunk;
            chunk.positions = std::move(mesh.positions);
            chunk.normals = std::move(mesh.normals);
            chunk.indices = std::move(mesh.indices);
            return AddForm(shapeHash, std::move(chunk));
        }

        uint32_t AddForm(const uint64_t formHash, scene::GeometryChunk&& chunk)
        {
            if (sd.meshes.size() >= scene::kInvalidIndex || sd.geometryChunks.size() >= scene::kInvalidIndex)
                throw std::runtime_error("CIFF scene contains too many unique meshes");
            if (sd.meshes.size() != sd.geometryChunks.size())
                throw std::runtime_error("CIFF scene geometry and descriptor order is inconsistent");

            const auto index = static_cast<uint32_t>(sd.meshes.size());
            {
                AccumulatingMillisecondTimer descriptorTimer(timings.meshDescriptorMs);
                auto descriptor = BuildMeshDescriptor(chunk, index);
                sd.geometryChunks.push_back(std::move(chunk));
                sd.meshes.push_back(descriptor);
                formIndexByHash.emplace(formHash, index);
            }
            return index;
        }

        void EmitInstance(const uint32_t formIndex, const uint32_t materialIndex, const std::uint32_t nodeIndex,
                          const Matrix3x4& tx)
        {
            if (formIndex >= sd.meshes.size())
                throw std::runtime_error("CIFF instance refers to a missing mesh");
            if (materialIndex >= sd.materials.size())
                throw std::runtime_error("CIFF instance refers to a missing material");
            if (nodeIndex >= sd.nodes.size())
                throw std::runtime_error("CIFF instance refers to a missing hierarchy node");
            if (sd.instances.size() >= scene::kInvalidIndex)
                throw std::runtime_error("CIFF scene contains too many instances");

            scene::Instance inst;
            inst.meshIndex = formIndex;
            inst.materialIndex = materialIndex;
            inst.nodeIndex = nodeIndex;
            std::memcpy(inst.transform, tx.data(), sizeof(inst.transform));
            for (const auto value : inst.transform)
            {
                if (!std::isfinite(value))
                    throw std::runtime_error("CIFF geometry transform exceeds the SceneData float range");
            }
            sd.instances.push_back(inst);
        }

        scene::SceneData& sd;
        const cifflib::ConvertProgressCallback& callback;
        cifflib::ConvertToSceneTimings& timings;
        std::vector<SealingTask> sealingTasks;
        std::vector<std::uint32_t> uniqueSealingTaskIndices;
        std::vector<PendingOccurrence> pendingOccurrences;
        std::unordered_map<std::uint64_t, std::uint32_t> taskByPreHash;
        std::unordered_map<std::size_t, std::uint32_t> taskBySourceMesh;
        std::unordered_map<std::uint64_t, std::uint32_t> formIndexByHash;
        ciff::primitive_stats::Stats primitiveStats;
        std::size_t processedGeometryCount = 0U;
    };
} // namespace

namespace cifflib
{
    ConvertToSceneResult ConvertToScene(const std::filesystem::path& ciffPath, scene::SceneData& out,
                                        const ConvertProgressCallback& progressCallback)
    {
        ConvertToSceneResult result;
        const auto totalStart = TimingClock::now();

        const auto finish = [&]() -> ConvertToSceneResult
        {
            result.timings.totalMs = MillisecondsSince(totalStart);
            return std::move(result);
        };

        try
        {
            bar::progress_idle = true;

            ciff::Read data(ciffPath.string(), std::string{});
            {
                MillisecondTimer dataLoadTimer(result.timings.dataLoadMs);
                data.load();
            }

            MillisecondTimer sceneProjectionTimer(result.timings.sceneProjectionMs);

            if (data.nodes.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
                throw std::runtime_error("CIFF scene contains too many hierarchy nodes");

            auto sceneData = scene::SceneData{};
            {
                AccumulatingMillisecondTimer setupTimer(result.timings.instanceHierarchyMaterialMs);
                sceneData.upAxis = scene::UpAxis::Z;
                sceneData.frontAxis = scene::FrontAxis::Y;
                sceneData.mirrorXAxisInWorld = true;
                sceneData.geometryChunksGpuReady = false;
                sceneData.rootNode = 0;
                sceneData.nodes.reserve(data.nodes.size());
                BuildMaterials(sceneData, data.colors);
            }

            SceneConvert convert(data, sceneData, progressCallback, result.timings);
            {
                ResidualSceneWorkTimer traversalTimer(result.timings);
                convert.RunConvert();
            }
            {
                ResidualSceneWorkTimer projectionTimer(result.timings);
                convert.SealGeometryAndProjectInstances();
            }

            if (sceneData.meshes.empty() || sceneData.instances.empty())
            {
                result.message = L"The CIFF file produced no drawable geometry.";
                sceneProjectionTimer.Stop();
                return finish();
            }

            {
                AccumulatingMillisecondTimer sealTimer(result.timings.instanceHierarchyMaterialMs);
                ValidateSealedScene(sceneData);
                out = std::move(sceneData);
            }
            result.success = true;
            sceneProjectionTimer.Stop();
            return finish();
        }
        catch (const std::exception& ex)
        {
            const std::string what(ex.what());
            result.message.assign(what.begin(), what.end());
            ::OutputDebugStringW((L"[CogniteCiffLib] " + result.message + L"\n").c_str());
            return finish();
        }
    }
} // namespace cifflib
