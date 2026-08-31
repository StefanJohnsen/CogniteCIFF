#include "PrimitivesCIFF.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <numeric>
#include <type_traits>

namespace ciff
{
    namespace
    {
        constexpr uint64_t fnvOffsetBasis = 0xcbf29ce484222325ULL;
        constexpr uint64_t fnvPrime = 0x100000001b3ULL;

        uint64_t fnv1a(const void* data, const size_t bytes, uint64_t hash = fnvOffsetBasis)
        {
            const auto* p = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < bytes; ++i)
            {
                hash ^= p[i];
                hash *= fnvPrime;
            }
            return hash;
        }

        template <typename T>
        uint64_t mix(const T& value, const uint64_t hash = fnvOffsetBasis)
        {
            static_assert(std::is_trivially_copyable_v<T>, "mix requires trivially copyable type");
            return fnv1a(&value, sizeof(T), hash);
        }

        uint64_t seed()
        {
            return mix(static_cast<uint64_t>(1));
        }

        [[nodiscard]] int64_t quantize(const double value) noexcept
        {
            return static_cast<int64_t>(std::llround(value / vertexTolerance()));
        }

        [[nodiscard]] int64_t quantize(const float value) noexcept
        {
            return static_cast<int64_t>(std::llround(value / vertexAttributeTolerance()));
        }

        [[nodiscard]] bool lessVertex(const Mesh& mesh, const uint32_t lhs, const uint32_t rhs, const bool hasUVs,
                                      const bool hasColors)
        {
            const auto& a = mesh.vertices[lhs];
            const auto& b = mesh.vertices[rhs];

            const std::array pointA{quantize(a.x), quantize(a.y), quantize(a.z)};
            const std::array pointB{quantize(b.x), quantize(b.y), quantize(b.z)};

            if (pointA != pointB)
                return pointA < pointB;

            if (hasUVs)
            {
                const auto& uvA = mesh.uvs[lhs];
                const auto& uvB = mesh.uvs[rhs];
                const std::array keyA{quantize(uvA.u), quantize(uvA.v)};
                const std::array keyB{quantize(uvB.u), quantize(uvB.v)};

                if (keyA != keyB)
                    return keyA < keyB;
            }

            if (hasColors)
            {
                const auto& colorA = mesh.colors[lhs];
                const auto& colorB = mesh.colors[rhs];
                const std::array keyA{quantize(colorA.r), quantize(colorA.g), quantize(colorA.b), quantize(colorA.a)};
                const std::array keyB{quantize(colorB.r), quantize(colorB.g), quantize(colorB.b), quantize(colorB.a)};

                if (keyA != keyB)
                    return keyA < keyB;
            }

            return lhs < rhs;
        }

        [[nodiscard]] std::array<uint32_t, 3> canonicalTriangle(const uint32_t a, const uint32_t b,
                                                                const uint32_t c) noexcept
        {
            const std::array t0{a, b, c};
            const std::array t1{b, c, a};
            const std::array t2{c, a, b};

            return std::min({t0, t1, t2});
        }
    }

    bool Cell::operator==(const Cell& other) const noexcept
    {
        return x == other.x && y == other.y && z == other.z;
    }

    size_t CellHash::operator()(const Cell& cell) const noexcept
    {
        size_t seed = std::hash<int64_t>{}(cell.x);
        seed ^= std::hash<int64_t>{}(cell.y) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        seed ^= std::hash<int64_t>{}(cell.z) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        return seed;
    }

    Cell cellOf(const Point& point) noexcept
    {
        return Cell{
            static_cast<int64_t>(std::floor(point.x / vertexTolerance())),
            static_cast<int64_t>(std::floor(point.y / vertexTolerance())),
            static_cast<int64_t>(std::floor(point.z / vertexTolerance())),
        };
    }

    bool equal(const double a, const double b) noexcept
    {
        return std::abs(a - b) <= vertexTolerance();
    }

    bool equal(const float a, const float b) noexcept
    {
        return std::abs(a - b) <= vertexAttributeTolerance();
    }

    bool samePoint(const Point& a, const Point& b) noexcept
    {
        return equal(a.x, b.x) && equal(a.y, b.y) && equal(a.z, b.z);
    }

    bool sameUV(const UV2& a, const UV2& b) noexcept
    {
        return equal(a.u, b.u) && equal(a.v, b.v);
    }

    bool sameColor(const RGBA4f& a, const RGBA4f& b) noexcept
    {
        return equal(a.r, b.r) && equal(a.g, b.g) && equal(a.b, b.b) && equal(a.a, b.a);
    }

    uint32_t VertexLookup::add(Mesh& mesh, const Point& point, const UV2* uv, const RGBA4f* vertexColor)
    {
        validateAttributeMode(mesh, uv != nullptr, vertexColor != nullptr);

        if (mesh.vertices.empty())
            buckets.clear();
        else if (buckets.empty())
            rebuild(mesh);

        const auto cell = cellOf(point);

        for (int dx = -1; dx <= 1; ++dx)
        {
            for (int dy = -1; dy <= 1; ++dy)
            {
                for (int dz = -1; dz <= 1; ++dz)
                {
                    const Cell neighbor{cell.x + dx, cell.y + dy, cell.z + dz};
                    const auto it = buckets.find(neighbor);

                    if (it == buckets.end())
                        continue;

                    for (const auto index : it->second)
                    {
                        if (sameVertex(mesh, index, point, uv, vertexColor))
                            return index;
                    }
                }
            }
        }

        const auto index = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.emplace_back(point);

        if (uv != nullptr)
            mesh.uvs.emplace_back(*uv);
        if (vertexColor != nullptr)
            mesh.colors.emplace_back(*vertexColor);

        buckets[cell].emplace_back(index);
        return index;
    }

    void VertexLookup::clear() noexcept
    {
        buckets.clear();
    }

    void VertexLookup::validateAttributeMode(const Mesh& mesh, const bool hasUV, const bool hasVertexColor)
    {
        if (!mesh.uvs.empty() && mesh.uvs.size() != mesh.vertices.size())
            throw std::logic_error("FBX mesh UV count does not match vertex count");
        if (!mesh.colors.empty() && mesh.colors.size() != mesh.vertices.size())
            throw std::logic_error("FBX mesh color count does not match vertex count");

        if (mesh.vertices.empty())
            return;

        if (hasUV != !mesh.uvs.empty())
            throw std::logic_error("Cannot mix FBX mesh vertices with and without UVs");
        if (hasVertexColor != !mesh.colors.empty())
            throw std::logic_error("Cannot mix FBX mesh vertices with and without colors");
    }

    bool VertexLookup::sameVertex(const Mesh& mesh, const uint32_t index, const Point& point, const UV2* uv,
                                  const RGBA4f* vertexColor)
    {
        if (index >= mesh.vertices.size() || !samePoint(mesh.vertices[index], point))
            return false;

        if ((uv != nullptr) != !mesh.uvs.empty())
            return false;
        if (uv != nullptr && (index >= mesh.uvs.size() || !sameUV(mesh.uvs[index], *uv)))
            return false;

        if ((vertexColor != nullptr) != !mesh.colors.empty())
            return false;
        if (vertexColor != nullptr && (index >= mesh.colors.size() || !sameColor(mesh.colors[index], *vertexColor)))
            return false;

        return true;
    }

    void VertexLookup::rebuild(const Mesh& mesh)
    {
        buckets.clear();
        buckets.reserve(mesh.vertices.size());

        for (uint32_t index = 0; index < mesh.vertices.size(); ++index)
            buckets[cellOf(mesh.vertices[index])].emplace_back(index);
    }

    void MeshShapeCache::canonicalize(Mesh& mesh)
    {
        if (canonical)
            return;

        shapeHashCached = false;
        cachedShapeHash = 0;

        apply(mesh);
        canonical = true;
    }

    uint64_t MeshShapeCache::shapeHash(Mesh& mesh)
    {
        if (shapeHashCached)
            return cachedShapeHash;

        canonicalize(mesh);
        cachedShapeHash = hashCanonical(mesh);
        shapeHashCached = true;
        return cachedShapeHash;
    }

    void MeshShapeCache::invalidate() noexcept
    {
        cachedShapeHash = 0;
        shapeHashCached = false;
        canonical = false;
    }

    void MeshShapeCache::apply(Mesh& mesh)
    {
        if (mesh.empty())
        {
            mesh.clearVertexLookup();
            return;
        }

        if (mesh.indices.size() % 3 != 0)
            throw std::logic_error("Cannot canonicalize mesh with incomplete triangle index list");

        const bool keepUVs = mesh.hasUVs();
        const bool keepColors = mesh.hasColors();

        if (!mesh.uvs.empty() && !keepUVs)
            throw std::logic_error("Cannot canonicalize mesh with partial UV data");
        if (!mesh.colors.empty() && !keepColors)
            throw std::logic_error("Cannot canonicalize mesh with partial color data");

        // Validate indices up front. All ingress paths (tess::tessellate and
        // readMeshGeometry) are expected to deliver clean meshes (no duplicates,
        // no out-of-bounds indices); canonicalization is purely a reorder.
        const auto vertexCount = mesh.vertices.size();
        for (const auto idx : mesh.indices)
        {
            if (idx >= vertexCount)
                throw std::logic_error("Cannot canonicalize mesh with out-of-bounds index");
        }

        // Sort vertices into a canonical order so identical shapes produce
        // identical hashes regardless of insertion order.
        std::vector<uint32_t> order(mesh.vertices.size());
        std::iota(order.begin(), order.end(), 0U);
        std::ranges::sort(order, [&](const uint32_t lhs, const uint32_t rhs) {
            return lessVertex(mesh, lhs, rhs, keepUVs, keepColors);
        });

        std::vector<uint32_t> remap(order.size());
        std::vector<Point> sortedVertices;
        std::vector<UV2> sortedUVs;
        std::vector<RGBA4f> sortedColors;

        sortedVertices.reserve(order.size());
        if (keepUVs)
            sortedUVs.reserve(order.size());
        if (keepColors)
            sortedColors.reserve(order.size());

        for (uint32_t newIndex = 0; newIndex < order.size(); ++newIndex)
        {
            const auto oldIndex = order[newIndex];
            remap[oldIndex] = newIndex;
            sortedVertices.emplace_back(mesh.vertices[oldIndex]);

            if (keepUVs)
                sortedUVs.emplace_back(mesh.uvs[oldIndex]);
            if (keepColors)
                sortedColors.emplace_back(mesh.colors[oldIndex]);
        }

        for (auto& index : mesh.indices)
            index = remap[index];

        // Sort triangles canonically (rotation-invariant per triangle, then sorted).
        std::vector<std::array<uint32_t, 3>> triangles;
        triangles.reserve(mesh.indices.size() / 3);

        for (size_t i = 0; i < mesh.indices.size(); i += 3)
        {
            triangles.emplace_back(
                canonicalTriangle(mesh.indices[i + 0], mesh.indices[i + 1], mesh.indices[i + 2]));
        }

        std::ranges::sort(triangles);

        std::vector<uint32_t> sortedIndices;
        sortedIndices.reserve(mesh.indices.size());

        for (const auto& triangle : triangles)
        {
            sortedIndices.emplace_back(triangle[0]);
            sortedIndices.emplace_back(triangle[1]);
            sortedIndices.emplace_back(triangle[2]);
        }

        mesh.vertices = std::move(sortedVertices);
        mesh.indices = std::move(sortedIndices);
        mesh.uvs = std::move(sortedUVs);
        mesh.colors = std::move(sortedColors);
        mesh.clearVertexLookup();
    }

    uint64_t MeshShapeCache::hashCanonical(const Mesh& mesh)
    {
        auto h = seed();

        const auto vertexCount = mesh.points();
        const auto indexCount = static_cast<uint32_t>(mesh.indices.size());
        h = mix(vertexCount, h);
        h = mix(indexCount, h);

        for (const auto& vertex : mesh.vertices)
        {
            h = mix(quantize(vertex.x), h);
            h = mix(quantize(vertex.y), h);
            h = mix(quantize(vertex.z), h);
        }

        if (!mesh.indices.empty())
            h = fnv1a(mesh.indices.data(), mesh.indices.size() * sizeof(mesh.indices[0]), h);

        return h;
    }

    uint32_t Mesh::addVertex(const Point& point)
    {
        return addVertex(point, nullptr, nullptr);
    }

    uint32_t Mesh::addVertex(const Point& point, const UV2* uv, const RGBA4f* vertexColor)
    {
        return vertexLookup.add(*this, point, uv, vertexColor);
    }

    void Mesh::appendTriangle(const Point& a, const Point& b, const Point& c)
    {
        indices.emplace_back(addVertex(a));
        indices.emplace_back(addVertex(b));
        indices.emplace_back(addVertex(c));
    }

    void Mesh::appendTriangle(const Point& a, const Point& b, const Point& c, const UV2* uvA, const UV2* uvB,
                              const UV2* uvC, const RGBA4f* colorA, const RGBA4f* colorB, const RGBA4f* colorC)
    {
        indices.emplace_back(addVertex(a, uvA, colorA));
        indices.emplace_back(addVertex(b, uvB, colorB));
        indices.emplace_back(addVertex(c, uvC, colorC));
    }

    uint64_t Mesh::shapeHash()
    {
        return shapeCache.shapeHash(*this);
    }

    void Mesh::invalidateShapeHash() noexcept
    {
        shapeCache.invalidate();
    }

    void Mesh::canonicalize()
    {
        shapeCache.canonicalize(*this);
    }

    void Mesh::clearVertexLookup() noexcept
    {
        vertexLookup.clear();
    }

    void Mesh::compress() noexcept
    {
        if (empty())
        {
            clearVertexLookup();
            invalidateShapeHash();
            return;
        }

        const bool keepUVs = hasUVs();
        const bool keepColors = hasColors();
        const auto oldCount = vertices.size();

        // Mark which vertices are actually referenced by indices. Unused vertices
        // are skipped in the dedupe pass below (their remap slot is never read).
        std::vector<uint8_t> used(oldCount, 0);
        for (const auto idx : indices)
        {
            if (idx < oldCount)
                used[idx] = 1;
        }

        std::vector<uint32_t> remap(oldCount, 0);

        std::vector<Point> newVertices;
        std::vector<UV2> newUVs;
        std::vector<RGBA4f> newColors;

        newVertices.reserve(oldCount);

        if (keepUVs)
            newUVs.reserve(oldCount);
        if (keepColors)
            newColors.reserve(oldCount);

        std::unordered_map<Cell, std::vector<uint32_t>, CellHash> buckets;
        buckets.reserve(oldCount);

        constexpr uint32_t notFound = std::numeric_limits<uint32_t>::max();

        for (uint32_t oldIndex = 0; oldIndex < oldCount; ++oldIndex)
        {
            if (!used[oldIndex])
                continue;

            const auto& point = vertices[oldIndex];
            const auto cell = cellOf(point);

            uint32_t found = notFound;
            for (int dx = -1; dx <= 1 && found == notFound; ++dx)
                for (int dy = -1; dy <= 1 && found == notFound; ++dy)
                    for (int dz = -1; dz <= 1 && found == notFound; ++dz)
                    {
                        const auto it = buckets.find(Cell{cell.x + dx, cell.y + dy, cell.z + dz});
                        if (it == buckets.end())
                            continue;
                        for (const auto idx : it->second)
                        {
                            if (!samePoint(newVertices[idx], point))
                                continue;
                            if (keepUVs && !sameUV(newUVs[idx], uvs[oldIndex]))
                                continue;
                            if (keepColors && !sameColor(newColors[idx], colors[oldIndex]))
                                continue;
                            found = idx;
                            break;
                        }
                    }

            if (found == notFound)
            {
                found = static_cast<uint32_t>(newVertices.size());
                newVertices.emplace_back(point);
                if (keepUVs)
                    newUVs.emplace_back(uvs[oldIndex]);
                if (keepColors)
                    newColors.emplace_back(colors[oldIndex]);
                buckets[cell].emplace_back(found);
            }
            remap[oldIndex] = found;
        }

        for (auto& idx : indices)
            idx = remap[idx];

        vertices = std::move(newVertices);
        uvs = std::move(newUVs);
        colors = std::move(newColors);
        clearVertexLookup();
        invalidateShapeHash();
    }
}
