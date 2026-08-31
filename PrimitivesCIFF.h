#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "Constants.h"

namespace ciff
{
	struct rgb
	{
		uint8_t r = 128;
		uint8_t g = 128;
		uint8_t b = 128;
		uint8_t a = 255;

		[[nodiscard]] uint32_t packed() const noexcept
		{
			return (static_cast<uint32_t>(r) << 24) | (static_cast<uint32_t>(g) << 16) |
				(static_cast<uint32_t>(b) << 8) | static_cast<uint32_t>(a);
		}

		bool operator==(const rgb& other) const noexcept
		{
			return r == other.r && g == other.g && b == other.b && a == other.a;
		}
	};

	struct Point
	{
		double x = 0.0;
		double y = 0.0;
		double z = 0.0;
	};

	// Vector and Point are wire-compatible (3 doubles) but kept separate
	// to make intent explicit at the API boundary.
	struct Vector
	{
		double x = 0.0;
		double y = 0.0;
		double z = 0.0;
	};

	struct UV2
	{
		float u = 0.0f;
		float v = 0.0f;
	};

	struct RGBA4f
	{
		float r = 0.5f;
		float g = 0.5f;
		float b = 0.5f;
		float a = 1.0f;
	};

struct Cell
    {
        int64_t x = 0;
        int64_t y = 0;
        int64_t z = 0;

        [[nodiscard]] bool operator==(const Cell& other) const noexcept;
    };

    struct CellHash
    {
        [[nodiscard]] size_t operator()(const Cell& cell) const noexcept;
    };

    [[nodiscard]] Cell cellOf(const Point& point) noexcept;
    [[nodiscard]] bool equal(double a, double b) noexcept;
    [[nodiscard]] bool equal(float a, float b) noexcept;
    [[nodiscard]] bool samePoint(const Point& a, const Point& b) noexcept;
    [[nodiscard]] bool sameUV(const UV2& a, const UV2& b) noexcept;
    [[nodiscard]] bool sameColor(const RGBA4f& a, const RGBA4f& b) noexcept;

    inline constexpr double pointTolerance = 1e-6;
    inline constexpr float attributeTolerance = 1e-6f;

    [[nodiscard]] constexpr double vertexTolerance() noexcept
    {
        return pointTolerance;
    }

    [[nodiscard]] constexpr float vertexAttributeTolerance() noexcept
    {
        return attributeTolerance;
    }

    struct Mesh;

    struct VertexLookup
    {
        [[nodiscard]] uint32_t add(Mesh& mesh, const Point& point, const UV2* uv, const RGBA4f* vertexColor);
        void clear() noexcept;

      private:
        static void validateAttributeMode(const Mesh& mesh, bool hasUV, bool hasVertexColor);
        [[nodiscard]] static bool sameVertex(const Mesh& mesh, uint32_t index, const Point& point, const UV2* uv,
                                      const RGBA4f* vertexColor);
        void rebuild(const Mesh& mesh);

        std::unordered_map<Cell, std::vector<uint32_t>, CellHash> buckets;
    };

    struct MeshShapeCache
    {
        void canonicalize(Mesh& mesh);
        [[nodiscard]] uint64_t shapeHash(Mesh& mesh);
        void invalidate() noexcept;

      private:
        static void apply(Mesh& mesh);
        [[nodiscard]] static uint64_t hashCanonical(const Mesh& mesh);

        uint64_t cachedShapeHash = 0;
        bool shapeHashCached = false;
        bool canonical = false;
    };

	struct Mesh
    {
        friend struct MeshShapeCache;

        [[nodiscard]] uint32_t addVertex(const Point& point);
        [[nodiscard]] uint32_t addVertex(const Point& point, const UV2* uv, const RGBA4f* vertexColor);

        void appendTriangle(const Point& a, const Point& b, const Point& c);
        void appendTriangle(const Point& a, const Point& b, const Point& c, const UV2* uvA, const UV2* uvB,
                            const UV2* uvC, const RGBA4f* colorA, const RGBA4f* colorB, const RGBA4f* colorC);

        void canonicalize();
        [[nodiscard]] uint64_t shapeHash();
        void invalidateShapeHash() noexcept;

        void clearVertexLookup() noexcept;

		void compress() noexcept;

        std::vector<Point> vertices;
        std::vector<uint32_t> indices;
        std::vector<UV2> uvs;
        std::vector<RGBA4f> colors;
        size_t color = 0;

        [[nodiscard]] bool empty() const noexcept
        {
            return vertices.empty() || indices.empty();
        }

        [[nodiscard]] bool hasUVs() const noexcept
        {
            return !uvs.empty() && uvs.size() == vertices.size();
        }

        [[nodiscard]] bool hasColors() const noexcept
        {
            return !colors.empty() && colors.size() == vertices.size();
        }

        [[nodiscard]] uint32_t points() const noexcept
        {
            return static_cast<uint32_t>(vertices.size());
        }

        [[nodiscard]] uint32_t triangles() const noexcept
        {
            return static_cast<uint32_t>(indices.size() / 3);
        }

      private:
        VertexLookup vertexLookup;
        MeshShapeCache shapeCache;
    };

	struct BoundingBox final
	{
		Point min = { max_double, max_double, max_double };
		Point max = { min_double, min_double, min_double };

		void append(const std::vector<Point>& points) noexcept
		{
			for (const auto& point : points)
				append(point);
		}

		void append(const Point& p) noexcept
		{
			append(p.x, p.y, p.z);
		}

		void append(const BoundingBox& b) noexcept
		{
			append(b.min);
			append(b.max);
		}

		void append(const double x, const double y, const double z) noexcept
		{
			min.x = std::min(min.x, x);
			max.x = std::max(max.x, x);
			min.y = std::min(min.y, y);
			max.y = std::max(max.y, y);
			min.z = std::min(min.z, z);
			max.z = std::max(max.z, z);
		}

		void clear() noexcept
		{
			min = { max_double, max_double, max_double };
			max = { min_double, min_double, min_double };
		}
	};

	// CIFF on-disk geometry record type codes.
	// Source: AvevaRvmDebug/ConvertCIFF.h writer (the only known CIFF emitter).
	enum class Type : uint8_t
	{
		Unknown          = 0,
		Mesh             = 3,   // pointCount + triangleCount + vertices + indices
		Cylinder         = 4,   // radius + centerA + centerB + isClosed
		Box              = 5,   // angle + delta + center + normal
		CircularTorus    = 13,  // radius + tubeRadius + angle + arcAngle + center + normal + isClosed
		Sphere           = 14,  // radius + center
		SphericalDish    = 17,  // verticalRadius + horizontalRadius + height + center + normal + isClosed
		GeneralCylinder  = 18,  // snout-shaped: 9 doubles + centerA + centerB + isClosed
		EmptyPoint       = 23,  // pointCount = 0 (empty geometry placeholder)
		FacetGroup,
		Facet,
		Size
	};

	inline const char* to_string(const Type type)
	{
		switch (type)
		{
		case Type::Unknown:
			return "Unknown";
		case Type::Mesh:
			return "Mesh";
		case Type::Cylinder:
			return "Cylinder";
		case Type::Box:
			return "Box";
		case Type::CircularTorus:
			return "CircularTorus";
		case Type::Sphere:
			return "Sphere";
		case Type::SphericalDish:
			return "SphericalDish";
		case Type::GeneralCylinder:
			return "GeneralCylinder";
		case Type::EmptyPoint:
			return "EmptyPoint";
		case Type::FacetGroup:
			return "FacetGroup";
		case Type::Facet:
			return "Facet";
		case Type::Size:
			return "Size";
		}

		throw std::runtime_error("Unknown ciff::Type");
	}

	struct Primitive
	{
		size_t count = 0;
		size_t points = 0;
		size_t indices = 0;

		virtual ~Primitive() = default;

		virtual Type type()
		{
			return Type::Unknown;
		}

		bool record()
		{
			++count;
			return true;
		}

		bool record(const Mesh& mesh)
		{
			points += mesh.vertices.size();
			indices += mesh.indices.size();
			return record();
		}

		Primitive& operator+=(const Primitive& other)
		{
			count += other.count;
			points += other.points;
			indices += other.indices;
			return *this;
		}
	};

	struct Facet : Primitive
	{
		std::vector<Point> exterior;
		std::vector<std::vector<Point>> interior;

		Type type() override
		{
			return Type::Facet;
		}
	};

	struct FacetGroup : Primitive
	{
		std::vector<Facet> facets;

		Type type() override
		{
			return Type::FacetGroup;
		}
	};

	// ----- CIFF on-disk parametric primitives -----
	// These mirror the layout written by AvevaRvmDebug/ConvertCIFF.h.
	// All scalars are stored as little-endian double on disk.

	struct Box : Primitive
	{
		double angle = 0.0;
		Vector delta  = {};
		Point  center = {};
		Vector normal = {};

		Type type() override { return Type::Box; }
	};

	struct Cylinder : Primitive
	{
		double radius   = 0.0;
		Point  centerA  = {};
		Point  centerB  = {};
		bool   isClosed = true;

		Type type() override { return Type::Cylinder; }
	};

	struct CircularTorus : Primitive
	{
		double radius     = 0.0;
		double tubeRadius = 0.0;
		double angle      = 0.0;
		double arcAngle   = 0.0;
		Point  center     = {};
		Vector normal     = {};
		bool   isClosed   = true;

		Type type() override { return Type::CircularTorus; }
	};

	struct Sphere : Primitive
	{
		double radius = 0.0;
		Point  center = {};

		Type type() override { return Type::Sphere; }
	};

	struct SphericalDish : Primitive
	{
		double verticalRadius   = 0.0;
		double horizontalRadius = 0.0;
		double height           = 0.0;
		Point  center           = {};
		Vector normal           = {};
		bool   isClosed         = true;

		Type type() override { return Type::SphericalDish; }
	};

	// AvevaRvmDebug calls this "GeneralCylinderGeometry" (snout-shaped).
	struct GeneralCylinder : Primitive
	{
		double radiusA   = 0.0;
		double radiusB   = 0.0;
		double slopeA    = 0.0;
		double slopeB    = 0.0;
		double zAngleA   = 0.0;
		double zAngleB   = 0.0;
		double angle     = 0.0;
		double arcAngle  = 0.0;
		double thickness = 0.0;
		Point  centerA   = {};
		Point  centerB   = {};
		bool   isClosed  = true;

		Type type() override { return Type::GeneralCylinder; }
	};

	struct ReadPrimitives
	{
		ReadPrimitives& operator+=(const ReadPrimitives& other)
		{
			FacetGroup      += other.FacetGroup;
			Box             += other.Box;
			Cylinder        += other.Cylinder;
			CircularTorus   += other.CircularTorus;
			Sphere          += other.Sphere;
			SphericalDish   += other.SphericalDish;
			GeneralCylinder += other.GeneralCylinder;
			return *this;
		}

		Primitive SumMesh() const
		{
			Primitive sum;
			sum += FacetGroup;
			return sum;
		}

		Primitive SumParametric() const
		{
			Primitive sum;
			sum += Box;
			sum += Cylinder;
			sum += CircularTorus;
			sum += Sphere;
			sum += SphericalDish;
			sum += GeneralCylinder;
			return sum;
		}

		ciff::FacetGroup      FacetGroup;
		ciff::Box             Box;
		ciff::Cylinder        Cylinder;
		ciff::CircularTorus   CircularTorus;
		ciff::Sphere          Sphere;
		ciff::SphericalDish   SphericalDish;
		ciff::GeneralCylinder GeneralCylinder;
	};

	// Geometry references either mesh payload read from disk or the originating
	// parametric primitive. Type::Mesh uses `mesh` as an index into Read::meshes.
	// Parametric types use `primitiveIndex` into the matching Read::<typed> array
	// and are tessellated lazily by the converter that needs a mesh.
	struct Geometry
	{
		size_t color          = 0;
		size_t mesh           = max_size;     // index into Read::meshes for Type::Mesh
		Type   primitive      = Type::Mesh;   // discriminator for the source primitive
		size_t primitiveIndex = 0;            // index into the matching Read::<typed> array
	};

	struct Node
	{
		std::string name;
		size_t parentIndex = 0;
		size_t childCount = 0;
		size_t color = 0;
		std::vector<size_t> geometries; // indices into Read::geometries
	};
}
