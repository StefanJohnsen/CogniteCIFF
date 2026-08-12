#pragma once

// CIFF does not carry vertex normals. This is the repository-local copy of
// 3DViewer's scene_processing::BuildRenderGeometry fallback. Keep the
// constants and grouping rules in sync until geometry processing is moved to
// a shared module.

#include "PrimitivesCIFF.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace ciff::normal_processing
{
    struct RenderGeometry
    {
        [[nodiscard]] bool empty() const noexcept
        {
            return positions.empty() || indices.empty();
        }

        [[nodiscard]] uint32_t points() const noexcept
        {
            return static_cast<uint32_t>(positions.size() / 3);
        }

        [[nodiscard]] uint32_t triangles() const noexcept
        {
            return static_cast<uint32_t>(indices.size() / 3);
        }

        [[nodiscard]] bool hasNormals() const noexcept
        {
            return !normals.empty() && normals.size() == positions.size();
        }

        std::vector<float> positions;
        std::vector<float> normals;
        std::vector<uint32_t> indices;
    };

    namespace detail
    {
        constexpr auto InvalidWeldedPoint = std::numeric_limits<uint32_t>::max();
        constexpr double SmoothCreaseCosine = 0.5;
        constexpr double WeldTolerance = 1.0e-6;
        constexpr double DegenerateSineSquared =
            static_cast<double>(std::numeric_limits<float>::epsilon()) *
            static_cast<double>(std::numeric_limits<float>::epsilon());

        struct FloatPoint
        {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
        };

        struct WeldKey
        {
            int64_t x = 0;
            int64_t y = 0;
            int64_t z = 0;

            bool operator==(const WeldKey&) const noexcept = default;
        };

        struct WeldKeyHash
        {
            size_t operator()(const WeldKey& key) const noexcept
            {
                size_t hash = std::hash<int64_t>{}(key.x);
                hash ^= std::hash<int64_t>{}(key.y) + 0x9e3779b9ULL + (hash << 6) + (hash >> 2);
                hash ^= std::hash<int64_t>{}(key.z) + 0x9e3779b9ULL + (hash << 6) + (hash >> 2);
                return hash;
            }
        };

        struct Face
        {
            std::array<uint32_t, 3> sourcePoints{};
            std::array<uint32_t, 3> weldedPoints{};
            std::array<double, 3> cornerAngles{};
            double nx = 0.0;
            double ny = 0.0;
            double nz = 0.0;
        };

        struct EdgeUse
        {
            size_t faceIndex = 0;
            size_t lowCorner = 0;
            size_t highCorner = 0;
        };

        struct EdgeState
        {
            void add(const EdgeUse& use) noexcept
            {
                if (count == 0)
                    first = use;
                else if (count == 1)
                    second = use;
                ++count;
            }

            EdgeUse first;
            EdgeUse second;
            size_t count = 0;
        };

        struct NormalSum
        {
            double x = 0.0;
            double y = 0.0;
            double z = 0.0;
        };

        struct OutputVertexKey
        {
            uint32_t sourcePoint = 0;
            size_t normalGroup = 0;

            bool operator==(const OutputVertexKey&) const noexcept = default;
        };

        struct OutputVertexKeyHash
        {
            size_t operator()(const OutputVertexKey& key) const noexcept
            {
                size_t hash = std::hash<uint32_t>{}(key.sourcePoint);
                hash ^= std::hash<size_t>{}(key.normalGroup) + 0x9e3779b9ULL + (hash << 6) + (hash >> 2);
                return hash;
            }
        };

        class DisjointSet
        {
        public:
            explicit DisjointSet(const size_t size) : parent_(size)
            {
                std::iota(parent_.begin(), parent_.end(), size_t{0});
            }

            size_t find(const size_t value) noexcept
            {
                auto root = value;
                while (parent_[root] != root)
                    root = parent_[root];

                auto cursor = value;
                while (parent_[cursor] != cursor)
                {
                    const auto next = parent_[cursor];
                    parent_[cursor] = root;
                    cursor = next;
                }
                return root;
            }

            void unite(const size_t first, const size_t second) noexcept
            {
                const auto firstRoot = find(first);
                const auto secondRoot = find(second);
                if (firstRoot == secondRoot)
                    return;
                const auto low = std::min(firstRoot, secondRoot);
                const auto high = std::max(firstRoot, secondRoot);
                parent_[high] = low;
            }

        private:
            std::vector<size_t> parent_;
        };

        class WeldedPointCatalog
        {
        public:
            explicit WeldedPointCatalog(const double tolerance)
                : tolerance_(tolerance), scale_(1.0 / tolerance)
            {
            }

            uint32_t addOrFind(const FloatPoint& point)
            {
                const auto key = makeKey(point);
                auto candidate = InvalidWeldedPoint;
                for (int64_t x = -1; x <= 1; ++x)
                {
                    for (int64_t y = -1; y <= 1; ++y)
                    {
                        for (int64_t z = -1; z <= 1; ++z)
                        {
                            const WeldKey neighbor{key.x + x, key.y + y, key.z + z};
                            const auto found = pointByKey_.find(neighbor);
                            if (found == pointByKey_.end())
                                continue;

                            const auto index = found->second;
                            const auto& existing = points_[index];
                            if (std::abs(static_cast<double>(existing.x) - point.x) <= tolerance_ &&
                                std::abs(static_cast<double>(existing.y) - point.y) <= tolerance_ &&
                                std::abs(static_cast<double>(existing.z) - point.z) <= tolerance_)
                            {
                                candidate = std::min(candidate, index);
                            }
                        }
                    }
                }

                if (candidate != InvalidWeldedPoint)
                    return candidate;
                if (points_.size() >= static_cast<size_t>(InvalidWeldedPoint))
                    throw std::runtime_error("Source scene mesh has too many welded points.");

                const auto index = static_cast<uint32_t>(points_.size());
                points_.push_back(point);
                pointByKey_.emplace(key, index);
                return index;
            }

        private:
            WeldKey makeKey(const FloatPoint& point) const
            {
                return {quantize(point.x), quantize(point.y), quantize(point.z)};
            }

            int64_t quantize(const float value) const
            {
                const auto scaled = static_cast<double>(value) * scale_;
                constexpr auto minimum = static_cast<double>(std::numeric_limits<int64_t>::min() + 1);
                constexpr auto maximum = static_cast<double>(std::numeric_limits<int64_t>::max() - 1);
                if (!std::isfinite(scaled) || scaled < minimum || scaled > maximum)
                    throw std::runtime_error("Source scene mesh position exceeds the normal-welding range.");
                return static_cast<int64_t>(std::llround(scaled));
            }

            double tolerance_ = 0.0;
            double scale_ = 0.0;
            std::vector<FloatPoint> points_;
            std::unordered_map<WeldKey, uint32_t, WeldKeyHash> pointByKey_;
        };

        inline FloatPoint readPoint(const RenderGeometry& source, const uint32_t index)
        {
            const auto base = static_cast<size_t>(index) * 3;
            return {source.positions[base + 0], source.positions[base + 1], source.positions[base + 2]};
        }

        inline double squaredLength(const double x, const double y, const double z) noexcept
        {
            return x * x + y * y + z * z;
        }

        inline uint64_t edgeKey(const uint32_t first, const uint32_t second) noexcept
        {
            const auto low = std::min(first, second);
            const auto high = std::max(first, second);
            return (static_cast<uint64_t>(low) << 32) | high;
        }

        inline void addEdge(std::unordered_map<uint64_t, EdgeState>& edges, const Face& face,
                            const size_t faceIndex, const size_t firstCorner, const size_t secondCorner)
        {
            const auto firstPoint = face.weldedPoints[firstCorner];
            const auto secondPoint = face.weldedPoints[secondCorner];
            const auto base = faceIndex * 3;
            const auto firstGlobalCorner = base + firstCorner;
            const auto secondGlobalCorner = base + secondCorner;
            const EdgeUse use = (firstPoint < secondPoint)
                ? EdgeUse{faceIndex, firstGlobalCorner, secondGlobalCorner}
                : EdgeUse{faceIndex, secondGlobalCorner, firstGlobalCorner};
            edges[edgeKey(firstPoint, secondPoint)].add(use);
        }
    }

    inline RenderGeometry BuildRenderGeometry(const RenderGeometry& source)
    {
        using namespace detail;

        if (!source.normals.empty())
            throw std::runtime_error("Normal generation requires a source scene mesh without normals.");
        if (source.positions.size() % 3 != 0)
            throw std::runtime_error("Source scene mesh has an invalid position buffer.");
        if (source.indices.size() % 3 != 0)
            throw std::runtime_error("Source scene mesh has an invalid index buffer.");

        const auto sourcePointCount = source.positions.size() / 3;
        const auto sourceTriangleCount = source.indices.size() / 3;
        if (sourcePointCount > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("Source scene mesh point count exceeds the 32-bit range.");
        if (sourceTriangleCount > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("Source scene mesh triangle count exceeds the 32-bit range.");
        if (source.indices.empty())
            return {};

        for (const auto index : source.indices)
        {
            if (index >= sourcePointCount)
                throw std::runtime_error("Source scene mesh index is outside its position buffer.");
            const auto point = readPoint(source, index);
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
                throw std::runtime_error("Source scene mesh contains a non-finite position.");
        }

        WeldedPointCatalog weldedPoints(WeldTolerance);
        std::vector<uint32_t> weldedPointBySource(sourcePointCount, InvalidWeldedPoint);
        for (const auto index : source.indices)
        {
            if (weldedPointBySource[index] == InvalidWeldedPoint)
                weldedPointBySource[index] = weldedPoints.addOrFind(readPoint(source, index));
        }

        std::vector<Face> faces;
        faces.reserve(sourceTriangleCount);
        for (size_t triangle = 0; triangle < sourceTriangleCount; ++triangle)
        {
            Face face;
            for (size_t corner = 0; corner < 3; ++corner)
            {
                face.sourcePoints[corner] = source.indices[triangle * 3 + corner];
                face.weldedPoints[corner] = weldedPointBySource[face.sourcePoints[corner]];
            }

            if (face.weldedPoints[0] == face.weldedPoints[1] ||
                face.weldedPoints[1] == face.weldedPoints[2] ||
                face.weldedPoints[2] == face.weldedPoints[0])
            {
                continue;
            }

            const auto p0 = readPoint(source, face.sourcePoints[0]);
            const auto p1 = readPoint(source, face.sourcePoints[1]);
            const auto p2 = readPoint(source, face.sourcePoints[2]);
            const double ux = static_cast<double>(p1.x) - p0.x;
            const double uy = static_cast<double>(p1.y) - p0.y;
            const double uz = static_cast<double>(p1.z) - p0.z;
            const double vx = static_cast<double>(p2.x) - p0.x;
            const double vy = static_cast<double>(p2.y) - p0.y;
            const double vz = static_cast<double>(p2.z) - p0.z;
            const double nx = uy * vz - uz * vy;
            const double ny = uz * vx - ux * vz;
            const double nz = ux * vy - uy * vx;
            const auto normalLengthSquared = squaredLength(nx, ny, nz);
            const auto uLengthSquared = squaredLength(ux, uy, uz);
            const auto vLengthSquared = squaredLength(vx, vy, vz);

            if (!std::isfinite(normalLengthSquared) || normalLengthSquared <= 0.0 ||
                uLengthSquared <= 0.0 || vLengthSquared <= 0.0 ||
                normalLengthSquared <= uLengthSquared * vLengthSquared * DegenerateSineSquared)
            {
                continue;
            }

            const auto normalLength = std::sqrt(normalLengthSquared);
            face.nx = nx / normalLength;
            face.ny = ny / normalLength;
            face.nz = nz / normalLength;
            face.cornerAngles[0] = std::atan2(normalLength, ux * vx + uy * vy + uz * vz);

            const double p1x = static_cast<double>(p0.x) - p1.x;
            const double p1y = static_cast<double>(p0.y) - p1.y;
            const double p1z = static_cast<double>(p0.z) - p1.z;
            const double p1vx = static_cast<double>(p2.x) - p1.x;
            const double p1vy = static_cast<double>(p2.y) - p1.y;
            const double p1vz = static_cast<double>(p2.z) - p1.z;
            face.cornerAngles[1] = std::atan2(normalLength, p1x * p1vx + p1y * p1vy + p1z * p1vz);

            const double p2x = static_cast<double>(p0.x) - p2.x;
            const double p2y = static_cast<double>(p0.y) - p2.y;
            const double p2z = static_cast<double>(p0.z) - p2.z;
            const double p2vx = static_cast<double>(p1.x) - p2.x;
            const double p2vy = static_cast<double>(p1.y) - p2.y;
            const double p2vz = static_cast<double>(p1.z) - p2.z;
            face.cornerAngles[2] = std::atan2(normalLength, p2x * p2vx + p2y * p2vy + p2z * p2vz);

            if (!std::isfinite(face.cornerAngles[0]) || !std::isfinite(face.cornerAngles[1]) ||
                !std::isfinite(face.cornerAngles[2]))
            {
                continue;
            }
            faces.push_back(face);
        }

        if (faces.empty())
            return {};

        const auto cornerCount = faces.size() * 3;
        DisjointSet normalGroups(cornerCount);
        std::unordered_map<uint64_t, EdgeState> edges;
        edges.reserve(faces.size() * 2);
        for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex)
        {
            const auto& face = faces[faceIndex];
            addEdge(edges, face, faceIndex, 0, 1);
            addEdge(edges, face, faceIndex, 1, 2);
            addEdge(edges, face, faceIndex, 2, 0);
        }

        for (const auto& [key, edge] : edges)
        {
            static_cast<void>(key);
            if (edge.count != 2)
                continue;
            const auto& firstFace = faces[edge.first.faceIndex];
            const auto& secondFace = faces[edge.second.faceIndex];
            const auto cosine =
                firstFace.nx * secondFace.nx + firstFace.ny * secondFace.ny + firstFace.nz * secondFace.nz;
            if (cosine < SmoothCreaseCosine)
                continue;
            normalGroups.unite(edge.first.lowCorner, edge.second.lowCorner);
            normalGroups.unite(edge.first.highCorner, edge.second.highCorner);
        }

        std::vector<NormalSum> normalSums(cornerCount);
        for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex)
        {
            const auto& face = faces[faceIndex];
            for (size_t corner = 0; corner < 3; ++corner)
            {
                const auto root = normalGroups.find(faceIndex * 3 + corner);
                auto& sum = normalSums[root];
                const auto weight = face.cornerAngles[corner];
                sum.x += face.nx * weight;
                sum.y += face.ny * weight;
                sum.z += face.nz * weight;
            }
        }

        RenderGeometry result;
        result.indices.reserve(cornerCount);
        const auto initialPointCapacity = std::min(sourcePointCount, cornerCount);
        result.positions.reserve(initialPointCapacity * 3);
        result.normals.reserve(initialPointCapacity * 3);
        std::unordered_map<OutputVertexKey, uint32_t, OutputVertexKeyHash> outputVertexByKey;
        outputVertexByKey.reserve(initialPointCapacity);

        for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex)
        {
            const auto& face = faces[faceIndex];
            for (size_t corner = 0; corner < 3; ++corner)
            {
                const auto root = normalGroups.find(faceIndex * 3 + corner);
                const OutputVertexKey key{face.sourcePoints[corner], root};
                const auto found = outputVertexByKey.find(key);
                if (found != outputVertexByKey.end())
                {
                    result.indices.push_back(found->second);
                    continue;
                }

                const auto pointCount = result.positions.size() / 3;
                if (pointCount >= std::numeric_limits<uint32_t>::max())
                    throw std::runtime_error("Final render geometry exceeds the 32-bit point range.");
                const auto& sum = normalSums[root];
                const auto normalLengthSquared = squaredLength(sum.x, sum.y, sum.z);
                if (!std::isfinite(normalLengthSquared) || normalLengthSquared <= 0.0)
                    throw std::runtime_error("Final render geometry contains an invalid normal.");

                const auto inverseNormalLength = 1.0 / std::sqrt(normalLengthSquared);
                const auto point = readPoint(source, face.sourcePoints[corner]);
                const auto outputIndex = static_cast<uint32_t>(pointCount);
                result.positions.insert(result.positions.end(), {point.x, point.y, point.z});
                result.normals.insert(result.normals.end(), {
                    static_cast<float>(sum.x * inverseNormalLength),
                    static_cast<float>(sum.y * inverseNormalLength),
                    static_cast<float>(sum.z * inverseNormalLength)
                });
                result.indices.push_back(outputIndex);
                outputVertexByKey.emplace(key, outputIndex);
            }
        }
        return result;
    }

    inline RenderGeometry FinalizeMesh(const ciff::Mesh& mesh)
    {
        if (mesh.vertices.size() > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("Source scene mesh point count exceeds the 32-bit range.");

        RenderGeometry source;
        source.positions.reserve(mesh.vertices.size() * 3);
        for (const auto& point : mesh.vertices)
        {
            source.positions.push_back(static_cast<float>(point.x));
            source.positions.push_back(static_cast<float>(point.y));
            source.positions.push_back(static_cast<float>(point.z));
        }
        source.indices = mesh.indices;
        return BuildRenderGeometry(source);
    }
}
