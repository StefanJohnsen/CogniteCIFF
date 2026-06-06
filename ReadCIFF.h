#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "CmdBar.h"
#include "Constants.h"
#include "PrimitivesCIFF.h"
#include "StreamCIFF.h"

namespace ciff
{
    // Magic identifier of the CIFF format. 0x46443343 little-endian = bytes 'C','3','D','F'.
    inline constexpr uint32_t MagicBytes = 0x46443343;

    struct Header
    {
        uint32_t magic = 0;
        uint32_t version = 0;
        uint64_t placeholder1 = 0;
        uint64_t placeholder2 = 0;
        uint64_t placeholder3 = 0;
        uint64_t placeholder4 = 0;
        uint32_t metadataSize = 0;
        std::string metadata;
    };

    // CIFF reader.
    //
    // Parses a Cognite Reveal CIFF file produced by writers such as
    // ConvertCIFF.h. The format is derived directly from that writer
    // implementation:
    //
    //   Header       (magic, version, four placeholders, metadata sections)
    //   Records*     (loop reading a uint8 type byte and dispatching)
    //                  type = 0  -> Footer (stop)
    //                  type = 1  -> Node
    //                  type = 3  -> Mesh geometry
    //                  type = 19 -> Material
    //                  type = 23 -> Empty geometry
    //
    // The structures populated here are intentionally analogous to
    // fbx::Read so that downstream writers (FBX, RVM, GLTF, 3D, ...)
    // can use the same Convert framework.
    class Read
    {
      public:
        Read(std::string sourceCad, std::string targetCad)
            : source_cad(std::move(sourceCad)), target_cad(std::move(targetCad))
        {
        }

        void load()
        {
            binary::Stream stream(source_cad);

            readHeader(stream);
            readBody(stream);
        }

        [[nodiscard]] const Mesh& getMesh(const size_t index) const
        {
            return meshes.at(index);
        }

        [[nodiscard]] const Geometry& getGeometry(const size_t index) const
        {
            return geometries.at(index);
        }

        [[nodiscard]] const rgb& getColor(const size_t index) const
        {
            return colors.at(index);
        }

        size_t addColor(const rgb& color)
        {
            for (size_t i = 0; i < colors.size(); ++i)
            {
                if (colors[i] == color)
                    return i;
            }

            colors.emplace_back(color);
            return colors.size() - 1;
        }

        std::string source_cad;
        std::string target_cad;

        Header header;
        std::vector<rgb> colors;
        std::vector<Mesh> meshes;
        std::vector<Geometry> geometries;
        std::vector<Node> nodes;

        // Parametric primitive storage. Geometry::primitive selects which array
        // the geometry's primitiveIndex refers to. Meshes are generated lazily by
        // converters that need tessellated output; only Type::Mesh payloads live
        // in `meshes`.
        std::vector<Box> boxes;
        std::vector<Cylinder> cylinders;
        std::vector<CircularTorus> circularToruses;
        std::vector<Sphere> spheres;
        std::vector<SphericalDish> sphericalDishes;
        std::vector<GeneralCylinder> generalCylinders;

        ReadPrimitives MESH;

      private:
        struct OpenNode
        {
            size_t index = 0;
            size_t remainingChildren = 0;
        };

        static constexpr uint8_t typeCode(const Type type) noexcept
        {
            return static_cast<uint8_t>(type);
        }

        static constexpr int typeNumber(const Type type) noexcept
        {
            return static_cast<int>(typeCode(type));
        }

        void readHeader(binary::Stream& stream)
        {
            header.magic = stream.read<uint32_t>();

            if (header.magic != MagicBytes)
                throw std::runtime_error("Not a valid CIFF file (magic bytes mismatch)");

            header.version = stream.read<uint32_t>();
            header.placeholder1 = stream.read<uint64_t>();
            header.placeholder2 = stream.read<uint64_t>();
            header.placeholder3 = stream.read<uint64_t>();
            header.placeholder4 = stream.read<uint64_t>();
            header.metadataSize = stream.read<uint32_t>();

            // Older clone files written by this tool store a single JSON-ish
            // metadata string after a zero count. Real CIFF files store typed
            // metadata sections, followed by a reserved uint32.
            if (header.metadataSize == 0)
            {
                header.metadata = stream.readString();
                return;
            }

            readMetadataSections(stream, header.metadataSize);
            (void)stream.read<uint32_t>(); // reserved / unknown
        }

        void readBody(binary::Stream& stream)
        {
            bar::start("Parse CIFF nodes and geometry", static_cast<size_t>(stream.size()));
            std::vector<OpenNode> openNodes;

            while (true)
            {
                const auto recordType = stream.read<uint8_t>();
                bar::step(static_cast<size_t>(stream.tell()));

                switch (recordType)
                {
                    case 0:
                        bar::stop();
                        return;
                    case 1:
                        readNode(stream, openNodes);
                        break;
                    case 19:
                        readMaterial(stream);
                        break;
                    case typeCode(Type::Mesh):
                    case typeCode(Type::Cylinder):
                    case typeCode(Type::Box):
                    case typeCode(Type::CircularTorus):
                    case typeCode(Type::Sphere):
                    case typeCode(Type::SphericalDish):
                    case typeCode(Type::GeneralCylinder):
                    case typeCode(Type::EmptyPoint):
                        // Geometry records outside a node context are skipped.
                        skipGeometryPayload(stream, static_cast<Type>(recordType));
                        break;
                    default:
                        throw std::runtime_error("CIFF: unknown record type " +
                                                 std::to_string(static_cast<int>(recordType)));
                }
            }
        }

        void readNode(binary::Stream& stream, std::vector<OpenNode>& openNodes)
        {
            Node node;
            const auto childCount = stream.read<uint32_t>();
            (void)stream.read<int64_t>(); // nodeIndex (writer-side)
            (void)stream.read<int64_t>(); // treeIndex
            (void)stream.read<int64_t>(); // subTreeSize
            node.name = stream.readString();
            node.childCount = childCount;

            node.color = readMaterialField(stream);
            readInternalMetadata(stream);
            readUserMetadata(stream);

            // The file also stores the color as a writer-side palette int64.
            // We discard it: our reader builds its own deduplicated palette
            // via addColor() and that order is not guaranteed to match the
            // writer's. node.color above is the authoritative index into our
            // data.colors array.
            (void)stream.read<int64_t>();
            const auto geometryCount = stream.read<uint32_t>();

            node.geometries.reserve(geometryCount);

            for (uint32_t i = 0; i < geometryCount; ++i)
            {
                const auto geometryIndex = readGeometry(stream, node.color);

                if (geometryIndex != max_size)
                    node.geometries.emplace_back(geometryIndex);
            }

            while (!openNodes.empty() && openNodes.back().remainingChildren == 0)
                openNodes.pop_back();

            const auto nodeIndex = nodes.size();
            if (nodeIndex != 0 && !openNodes.empty())
            {
                node.parentIndex = openNodes.back().index;
                --openNodes.back().remainingChildren;
            }

            if (node.childCount > 0)
                openNodes.push_back(OpenNode{nodeIndex, node.childCount});

            nodes.emplace_back(std::move(node));
        }

        size_t readMaterialField(binary::Stream& stream)
        {
            const auto hasColor = stream.read<uint8_t>();

            if (!hasColor)
                return 0;

            rgb color{};
            color.r = stream.read<uint8_t>();
            color.g = stream.read<uint8_t>();
            color.b = stream.read<uint8_t>();
            color.a = stream.read<uint8_t>();
            return addColor(color);
        }

        static void readInternalMetadata(binary::Stream& stream)
        {
            const auto present = stream.read<uint8_t>();

            if (present)
                (void)stream.readString();
        }

        static void readUserMetadata(binary::Stream& stream)
        {
            readMetadataSections(stream, stream.read<uint32_t>());
        }

        static void readMetadataSections(binary::Stream& stream, const uint32_t sectionCount)
        {

            for (uint32_t i = 0; i < sectionCount; ++i)
            {
                (void)stream.readString();
                const auto fieldCount = stream.read<uint32_t>();

                for (uint32_t j = 0; j < fieldCount; ++j)
                {
                    (void)stream.readString();    // name
                    (void)stream.readString();    // unused
                    const auto valueType = stream.read<uint8_t>();
                    (void)stream.read<uint8_t>(); // measurement
                    readMetadataValue(stream, valueType);
                }
            }
        }

        static void readMetadataValue(binary::Stream& stream, const uint8_t valueType)
        {
            switch (valueType)
            {
                case 0: // string
                    (void)stream.readString();
                    return;
                case 1: // bool
                    (void)stream.read<uint8_t>();
                    return;
                case 2: // int32
                    (void)stream.read<int32_t>();
                    return;
                case 3: // double
                    (void)stream.read<double>();
                    return;
                case 4: // coordinate2, float U/V
                    (void)stream.read<float>();
                    (void)stream.read<float>();
                    return;
                case 5: // coordinate3, double X/Y/Z
                    (void)stream.read<double>();
                    (void)stream.read<double>();
                    (void)stream.read<double>();
                    return;
                case 6: // date/time, milliseconds since epoch
                    (void)stream.read<int64_t>();
                    return;
                default:
                    throw std::runtime_error("CIFF: unsupported metadata value type " +
                                             std::to_string(static_cast<int>(valueType)));
            }
        }

        size_t readGeometry(binary::Stream& stream, const size_t nodeColor)
        {
            (void)stream.read<int64_t>(); // GeometryReference (-1)
            const auto hasTransform = stream.read<uint8_t>();

            if (hasTransform)
                throw std::runtime_error("CIFF: per-geometry transforms not supported");

            const auto type = static_cast<Type>(stream.read<uint8_t>());

            switch (type)
            {
                case Type::Mesh:
                    return readMeshGeometry(stream, nodeColor);
                case Type::Cylinder:
                    return readCylinder(stream, nodeColor);
                case Type::Box:
                    return readBox(stream, nodeColor);
                case Type::CircularTorus:
                    return readCircularTorus(stream, nodeColor);
                case Type::Sphere:
                    return readSphere(stream, nodeColor);
                case Type::SphericalDish:
                    return readSphericalDish(stream, nodeColor);
                case Type::GeneralCylinder:
                    return readGeneralCylinder(stream, nodeColor);
                case Type::EmptyPoint:
                    return readEmpty(stream);
                default:
                    throw std::runtime_error("CIFF: unsupported geometry type " +
                                             std::to_string(typeNumber(type)));
            }
        }

        static size_t readEmpty(binary::Stream& stream)
        {
            (void)stream.read<uint32_t>();
            return max_size;
        }

        size_t readMeshGeometry(binary::Stream& stream, const size_t nodeColor)
        {
            (void)stream.read<uint32_t>(); // meshCount (1)
            (void)stream.read<uint32_t>(); // byteCount
            const auto pointCount = stream.read<uint32_t>();
            const auto triangleCount = stream.read<uint32_t>();
            (void)stream.read<uint8_t>(); // textureCount (0)

            Mesh mesh;
            mesh.color = nodeColor;
            static_assert(sizeof(Point) == 3 * sizeof(double), "CIFF Point payload must be three doubles");
            mesh.vertices = stream.readArray<Point>(pointCount);
            mesh.indices = stream.readArray<uint32_t>(static_cast<size_t>(triangleCount) * 3);

            // FacetGroup data comes from an external exporter; dedupe any
            // coincident vertices and drop orphan ones before recording.
            //mesh.compress();

            MESH.FacetGroup.record(mesh);
            return emitMeshGeometry(std::move(mesh), nodeColor);
        }

        Point readPoint(binary::Stream& stream)
        {
            Point p{};
            p.x = stream.read<double>();
            p.y = stream.read<double>();
            p.z = stream.read<double>();
            return p;
        }

        Vector readVector(binary::Stream& stream)
        {
            Vector v{};
            v.x = stream.read<double>();
            v.y = stream.read<double>();
            v.z = stream.read<double>();
            return v;
        }

        static bool hasLength(const Point& a, const Point& b) noexcept
        {
            const auto dx = b.x - a.x;
            const auto dy = b.y - a.y;
            const auto dz = b.z - a.z;
            return dx * dx + dy * dy + dz * dz >= 1e-24;
        }

        static bool drawable(const Box& box) noexcept
        {
            return std::abs(box.delta.x) >= 2e-12 ||
                   std::abs(box.delta.y) >= 2e-12 ||
                   std::abs(box.delta.z) >= 2e-12;
        }

        static bool drawable(const Cylinder& cyl) noexcept
        {
            return cyl.radius >= 1e-12 && hasLength(cyl.centerA, cyl.centerB);
        }

        static bool drawable(const CircularTorus& torus) noexcept
        {
            return torus.radius >= 1e-12 && torus.tubeRadius >= 1e-12;
        }

        static bool drawable(const Sphere& sphere) noexcept
        {
            return sphere.radius >= 1e-12;
        }

        static bool drawable(const SphericalDish& dish) noexcept
        {
            return dish.height >= 1e-12 && dish.horizontalRadius >= 1e-12;
        }

        static bool drawable(const GeneralCylinder& cyl) noexcept
        {
            return hasLength(cyl.centerA, cyl.centerB) && (cyl.radiusA >= 1e-12 || cyl.radiusB >= 1e-12);
        }

        size_t readBox(binary::Stream& stream, const size_t nodeColor)
        {
            Box box;
            box.angle = stream.read<double>();
            box.delta = readVector(stream);
            box.center = readPoint(stream);
            box.normal = readVector(stream);
            box.record();
            MESH.Box.record();

            boxes.emplace_back(box);
            return emitPrimitiveGeometry(nodeColor, Type::Box, boxes.size() - 1, drawable(box));
        }

        size_t readCylinder(binary::Stream& stream, const size_t nodeColor)
        {
            Cylinder cyl;
            cyl.radius = stream.read<double>();
            cyl.centerA = readPoint(stream);
            cyl.centerB = readPoint(stream);
            cyl.isClosed = stream.read<uint8_t>() != 0;
            cyl.record();
            MESH.Cylinder.record();

            cylinders.emplace_back(cyl);
            return emitPrimitiveGeometry(nodeColor, Type::Cylinder, cylinders.size() - 1, drawable(cyl));
        }

        size_t readCircularTorus(binary::Stream& stream, const size_t nodeColor)
        {
            CircularTorus t;
            t.radius = stream.read<double>();
            t.tubeRadius = stream.read<double>();
            t.angle = stream.read<double>();
            t.arcAngle = stream.read<double>();
            t.center = readPoint(stream);
            t.normal = readVector(stream);
            t.isClosed = stream.read<uint8_t>() != 0;
            t.record();
            MESH.CircularTorus.record();

            circularToruses.emplace_back(t);
            return emitPrimitiveGeometry(nodeColor, Type::CircularTorus, circularToruses.size() - 1, drawable(t));
        }

        size_t readSphere(binary::Stream& stream, const size_t nodeColor)
        {
            Sphere s;
            s.radius = stream.read<double>();
            s.center = readPoint(stream);
            s.record();
            MESH.Sphere.record();

            spheres.emplace_back(s);
            return emitPrimitiveGeometry(nodeColor, Type::Sphere, spheres.size() - 1, drawable(s));
        }

        size_t readSphericalDish(binary::Stream& stream, const size_t nodeColor)
        {
            SphericalDish d;
            d.verticalRadius = stream.read<double>();
            d.horizontalRadius = stream.read<double>();
            d.height = stream.read<double>();
            d.center = readPoint(stream);
            d.normal = readVector(stream);
            d.isClosed = stream.read<uint8_t>() != 0;
            d.record();
            MESH.SphericalDish.record();

            sphericalDishes.emplace_back(d);
            return emitPrimitiveGeometry(nodeColor, Type::SphericalDish, sphericalDishes.size() - 1, drawable(d));
        }

        size_t readGeneralCylinder(binary::Stream& stream, const size_t nodeColor)
        {
            GeneralCylinder g;
            g.radiusA = stream.read<double>();
            g.radiusB = stream.read<double>();
            g.slopeA = stream.read<double>();
            g.slopeB = stream.read<double>();
            g.zAngleA = stream.read<double>();
            g.zAngleB = stream.read<double>();
            g.angle = stream.read<double>();
            g.arcAngle = stream.read<double>();
            g.thickness = stream.read<double>();
            g.centerA = readPoint(stream);
            g.centerB = readPoint(stream);
            g.isClosed = stream.read<uint8_t>() != 0;
            g.record();
            MESH.GeneralCylinder.record();

            generalCylinders.emplace_back(g);
            return emitPrimitiveGeometry(nodeColor, Type::GeneralCylinder, generalCylinders.size() - 1, drawable(g));
        }

        size_t emitMeshGeometry(Mesh&& mesh, const size_t nodeColor)
        {
            if (mesh.empty())
                return max_size;

            mesh.color = nodeColor;
            meshes.emplace_back(std::move(mesh));

            Geometry geometry{};
            geometry.color = nodeColor;
            geometry.mesh = meshes.size() - 1;
            geometry.primitive = Type::Mesh;
            geometries.emplace_back(geometry);
            return geometries.size() - 1;
        }

        size_t emitPrimitiveGeometry(const size_t nodeColor, const Type primitive, const size_t primitiveIndex,
                                     const bool isDrawable)
        {
            if (!isDrawable)
                return max_size;

            Geometry geometry{};
            geometry.color = nodeColor;
            geometry.primitive = primitive;
            geometry.primitiveIndex = primitiveIndex;
            geometries.emplace_back(geometry);
            return geometries.size() - 1;
        }

        static void skipGeometryPayload(binary::Stream& stream, const Type type)
        {
            switch (type)
            {
                case Type::EmptyPoint:
                    (void)readEmpty(stream);
                    return;
                case Type::Mesh:
                    (void)stream.read<uint32_t>();
                    stream.skip(static_cast<size_t>(stream.read<uint32_t>()));
                    return;
                case Type::Cylinder:
                    stream.skip(7 * sizeof(double) + sizeof(uint8_t));
                    return;
                case Type::Box:
                    stream.skip(10 * sizeof(double));
                    return;
                case Type::CircularTorus:
                    stream.skip(10 * sizeof(double) + sizeof(uint8_t));
                    return;
                case Type::Sphere:
                    stream.skip(4 * sizeof(double));
                    return;
                case Type::SphericalDish:
                    stream.skip(9 * sizeof(double) + sizeof(uint8_t));
                    return;
                case Type::GeneralCylinder:
                    stream.skip(15 * sizeof(double) + sizeof(uint8_t));
                    return;
                default:
                    throw std::runtime_error("CIFF: cannot skip unknown geometry type " +
                                             std::to_string(typeNumber(type)));
            }
        }

        void readMaterial(binary::Stream& stream)
        {
            const auto textureCount = stream.read<uint32_t>();
            (void)stream.readString(); // diffuse color JSON

            for (uint32_t i = 0; i < textureCount; ++i)
                (void)stream.readString(); // texture metadata (best effort)
        }
    };
} // namespace ciff
