#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "CmdBar.h"
#include "Constants.h"
#include "PrimitivesCIFF.h"
#include "ShapeCIFF.h"
#include "StreamCIFF.h"
#include "TessCIFF.h"

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
    //   Header       (magic, version, four placeholders, metadata)
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
        // the geometry's primitiveIndex refers to. Always parallel to the
        // tessellated entry in `meshes` so downstream writers can keep using
        // the mesh path while 3D can dedup against primitive parameters.
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
            header.metadata = stream.readString();
        }

        void readBody(binary::Stream& stream)
        {
            bar::start("Parse CIFF nodes and geometry", static_cast<size_t>(stream.size()));
            std::vector<OpenNode> openNodes;

            while (true)
            {
                const auto type = stream.read<uint8_t>();
                bar::step(static_cast<size_t>(stream.tell()));

                switch (type)
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
                    case 3:
                    case 4:
                    case 5:
                    case 13:
                    case 14:
                    case 17:
                    case 18:
                    case 23:
                        // Geometry records outside a node context are skipped.
                        skipGeometryPayload(stream, type);
                        break;
                    default:
                        throw std::runtime_error("CIFF: unknown record type " + std::to_string(static_cast<int>(type)));
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
                throw std::runtime_error("CIFF: internal metadata not supported");
        }

        static void readUserMetadata(binary::Stream& stream)
        {
            const auto sectionCount = stream.read<uint32_t>();

            for (uint32_t i = 0; i < sectionCount; ++i)
            {
                (void)stream.readString();
                const auto fieldCount = stream.read<uint32_t>();

                for (uint32_t j = 0; j < fieldCount; ++j)
                {
                    (void)stream.readString();    // name
                    (void)stream.readString();    // unused
                    (void)stream.read<uint8_t>(); // valueType
                    (void)stream.read<uint8_t>(); // measurement
                    (void)stream.readString();    // value
                }
            }
        }

        size_t readGeometry(binary::Stream& stream, const size_t nodeColor)
        {
            (void)stream.read<int64_t>(); // GeometryReference (-1)
            const auto hasTransform = stream.read<uint8_t>();

            if (hasTransform)
                throw std::runtime_error("CIFF: per-geometry transforms not supported");

            const auto type = stream.read<uint8_t>();

            switch (type)
            {
                case 23:                           // empty geometry placeholder
                    (void)stream.read<uint32_t>(); // pointCount = 0
                    return max_size;
                case 3:
                    return readMeshGeometry(stream, nodeColor);
                case 4:
                    return readCylinder(stream, nodeColor);
                case 5:
                    return readBox(stream, nodeColor);
                case 13:
                    return readCircularTorus(stream, nodeColor);
                case 14:
                    return readSphere(stream, nodeColor);
                case 17:
                    return readSphericalDish(stream, nodeColor);
                case 18:
                    return readGeneralCylinder(stream, nodeColor);
                default:
                    throw std::runtime_error("CIFF: unsupported geometry type " +
                                             std::to_string(static_cast<int>(type)));
            }
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
            mesh.vertices.resize(pointCount);

            if (pointCount > 0)
            {
                auto raw = stream.readArray<double>(static_cast<size_t>(pointCount) * 3);
                for (uint32_t i = 0; i < pointCount; ++i)
                {
                    mesh.vertices[i] = Point{raw[3 * i + 0], raw[3 * i + 1], raw[3 * i + 2]};
                }
            }

            if (triangleCount > 0)
                mesh.indices = stream.readArray<uint32_t>(static_cast<size_t>(triangleCount) * 3);

            MESH.FacetGroup.record(mesh);
            return emitGeometry(std::move(mesh), nodeColor, Type::Mesh, 0);
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

        size_t readBox(binary::Stream& stream, const size_t nodeColor)
        {
            Box box;
            box.angle = stream.read<double>();
            box.delta = readVector(stream);
            box.center = readPoint(stream);
            box.normal = readVector(stream);
            box.record();

            auto mesh = tess::tessellate(box);
            tess::transform(mesh, shape::instanceTransform(box));
            MESH.Box.record(mesh);

            boxes.emplace_back(box);
            return emitGeometry(std::move(mesh), nodeColor, Type::Box, boxes.size() - 1);
        }

        size_t readCylinder(binary::Stream& stream, const size_t nodeColor)
        {
            Cylinder cyl;
            cyl.radius = stream.read<double>();
            cyl.centerA = readPoint(stream);
            cyl.centerB = readPoint(stream);
            cyl.isClosed = stream.read<uint8_t>() != 0;
            cyl.record();

            auto mesh = tess::tessellate(cyl);
            tess::transform(mesh, shape::instanceTransform(cyl));
            MESH.Cylinder.record(mesh);

            cylinders.emplace_back(cyl);
            return emitGeometry(std::move(mesh), nodeColor, Type::Cylinder, cylinders.size() - 1);
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

            auto mesh = tess::tessellate(t);
            tess::transform(mesh, shape::instanceTransform(t));
            MESH.CircularTorus.record(mesh);

            circularToruses.emplace_back(t);
            return emitGeometry(std::move(mesh), nodeColor, Type::CircularTorus, circularToruses.size() - 1);
        }

        size_t readSphere(binary::Stream& stream, const size_t nodeColor)
        {
            Sphere s;
            s.radius = stream.read<double>();
            s.center = readPoint(stream);
            s.record();

            auto mesh = tess::tessellate(s);
            tess::transform(mesh, shape::instanceTransform(s));
            MESH.Sphere.record(mesh);

            spheres.emplace_back(s);
            return emitGeometry(std::move(mesh), nodeColor, Type::Sphere, spheres.size() - 1);
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

            auto mesh = tess::tessellate(d);
            tess::transform(mesh, shape::instanceTransform(d));
            MESH.SphericalDish.record(mesh);

            sphericalDishes.emplace_back(d);
            return emitGeometry(std::move(mesh), nodeColor, Type::SphericalDish, sphericalDishes.size() - 1);
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

            auto mesh = tess::tessellate(g);
            tess::transform(mesh, shape::instanceTransform(g));
            MESH.GeneralCylinder.record(mesh);

            generalCylinders.emplace_back(g);
            return emitGeometry(std::move(mesh), nodeColor, Type::GeneralCylinder, generalCylinders.size() - 1);
        }

        size_t emitGeometry(Mesh&& mesh, const size_t nodeColor, const Type primitive, const size_t primitiveIndex)
        {
            if (mesh.empty())
                return max_size;

            mesh.color = nodeColor;
            meshes.emplace_back(std::move(mesh));

            Geometry geometry{};
            geometry.color = nodeColor;
            geometry.mesh = meshes.size() - 1;
            geometry.primitive = primitive;
            geometry.primitiveIndex = primitiveIndex;
            geometries.emplace_back(geometry);
            return geometries.size() - 1;
        }

        static void skipGeometryPayload(binary::Stream& stream, const uint8_t type)
        {
            if (type == 23)
            {
                (void)stream.read<uint32_t>(); // pointCount = 0
                return;
            }

            if (type == 3)
            {
                (void)stream.read<uint32_t>(); // meshCount
                const auto byteCount = stream.read<uint32_t>();
                stream.skip(static_cast<size_t>(byteCount));
                return;
            }

            // Parametric primitives have fixed-size payloads on disk.
            switch (type)
            {
                case 4: // Cylinder: 1 + 3 + 3 doubles + bool
                    stream.skip(7 * sizeof(double) + sizeof(uint8_t));
                    return;
                case 5: // Box: 1 + 3 + 3 + 3 doubles
                    stream.skip(10 * sizeof(double));
                    return;
                case 13: // CircularTorus: 4 + 3 + 3 doubles + bool
                    stream.skip(10 * sizeof(double) + sizeof(uint8_t));
                    return;
                case 14: // Sphere: 1 + 3 doubles
                    stream.skip(4 * sizeof(double));
                    return;
                case 17: // SphericalDish: 3 + 3 + 3 doubles + bool
                    stream.skip(9 * sizeof(double) + sizeof(uint8_t));
                    return;
                case 18: // GeneralCylinder: 9 + 3 + 3 doubles + bool
                    stream.skip(15 * sizeof(double) + sizeof(uint8_t));
                    return;
                default:
                    throw std::runtime_error("CIFF: cannot skip unknown geometry type " +
                                             std::to_string(static_cast<int>(type)));
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
