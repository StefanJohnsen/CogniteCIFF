/*----------------------------------------------------------------
  PrimitivesRVM.h

  Standalone collection of AVEVA RVM primitive type definitions.
  Mirrors AvevaRvmDebug/PrimitivesRVM.h but with no dependencies
  on the RVM project (no StreamRVM.h, no GeometryUtil.h, no
  CmdArgs.h pulling).

  These are the SOURCE primitives a CAD tool produces *before*
  emitting CIFF. AvevaRvmDebug/ConvertCIFF.h converts each of
  them into a CIFF on-disk record (see PrimitivesCIFF.h for the
  CIFF wire-format types).

  Kept here purely as a reference / collection so that future
  CIFF readers, exporters or round-trip tooling have the original
  parameter set immediately available.
----------------------------------------------------------------*/

#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace rvm
{
	struct Point
	{
		double x = 0.0;
		double y = 0.0;
		double z = 0.0;
	};

	struct Vector
	{
		double x = 0.0;
		double y = 0.0;
		double z = 0.0;
	};

	enum class Type : uint8_t
	{
		Mesh,
		Pyramid,
		Box,
		RectangularTorus,
		CircularTorus,
		EllipticalDish,
		SphericalDish,
		Snout,
		Cylinder,
		Sphere,
		Line,
		FacetGroup,
		Facet,
		Size
	};

	inline const char* to_string(const Type type)
	{
		switch (type)
		{
		case Type::Mesh:             return "Mesh";
		case Type::Pyramid:          return "Pyramid";
		case Type::Box:              return "Box";
		case Type::RectangularTorus: return "RectangularTorus";
		case Type::CircularTorus:    return "CircularTorus";
		case Type::EllipticalDish:   return "EllipticalDish";
		case Type::SphericalDish:    return "SphericalDish";
		case Type::Snout:            return "Snout";
		case Type::Cylinder:         return "Cylinder";
		case Type::Sphere:           return "Sphere";
		case Type::Line:             return "Line";
		case Type::FacetGroup:       return "FacetGroup";
		case Type::Facet:            return "Facet";
		case Type::Size:             return "Size";
		}

		throw std::runtime_error("Unknown rvm::Type");
	}

	struct Primitive
	{
		size_t count = 0;
		size_t points = 0;
		size_t indices = 0;

		virtual ~Primitive() = default;

		virtual Type type() { return Type::Mesh; }

		bool record() { ++count; return true; }

		Primitive& operator+=(const Primitive& other)
		{
			count   += other.count;
			points  += other.points;
			indices += other.indices;
			return *this;
		}
	};

	struct Box : Primitive
	{
		float xp = 0.f;
		float yp = 0.f;
		float zp = 0.f;

		Type type() override { return Type::Box; }
	};

	struct Pyramid : Primitive
	{
		float bx = 0.f;
		float by = 0.f;
		float tx = 0.f;
		float ty = 0.f;
		float ox = 0.f;
		float oy = 0.f;
		float h2 = 0.f;

		Type type() override { return Type::Pyramid; }
	};

	struct RectangularTorus : Primitive
	{
		float inner_radius = 0.f;
		float outer_radius = 0.f;
		float height       = 0.f;
		float angle        = 0.f;

		Type type() override { return Type::RectangularTorus; }
	};

	struct CircularTorus : Primitive
	{
		float offset = 0.f;
		float radius = 0.f;
		float angle  = 0.f;

		Type type() override { return Type::CircularTorus; }
	};

	struct EllipticalDish : Primitive
	{
		float baseRadius = 0.f;
		float height     = 0.f;

		Type type() override { return Type::EllipticalDish; }
	};

	struct SphericalDish : Primitive
	{
		float baseRadius = 0.f;
		float height     = 0.f;

		Type type() override { return Type::SphericalDish; }
	};

	struct Snout : Primitive
	{
		float radius_b   = 0.f;
		float radius_t   = 0.f;
		float height     = 0.f;
		float offset[2]  = { 0.f, 0.f };
		float bshear[2]  = { 0.f, 0.f };
		float tshear[2]  = { 0.f, 0.f };

		Type type() override { return Type::Snout; }
	};

	struct Cylinder : Primitive
	{
		float radius = 0.f;
		float height = 0.f;

		Type type() override { return Type::Cylinder; }
	};

	struct Sphere : Primitive
	{
		float diameter = 0.f;

		Type type() override { return Type::Sphere; }
	};

	struct Line : Primitive
	{
		float l1 = 0.f;
		float l2 = 0.f;

		Type type() override { return Type::Line; }
	};

	// All exterior polygons are clockwise, and
	// interior polygons are counter-clockwise in RVM (also known as windings).

	struct Facet : Primitive
	{
		std::vector<Point> exterior;
		std::vector<std::vector<Point>> interior;

		Type type() override { return Type::Facet; }
	};

	struct FacetGroup : Primitive
	{
		std::vector<Facet> facets;

		Type type() override { return Type::FacetGroup; }
	};
} // namespace rvm
