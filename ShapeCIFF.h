/*----------------------------------------------------------------
  ShapeCIFF.h

  Form / instance factoring for CIFF parametric primitives.

  CIFF stores every primitive WORLD-BAKED (the source CAD transform
  has been folded into Box.center/normal/delta etc.). For 3D we
  need the inverse: a transform-invariant LOCAL form together with
  an instance Matrix3x4 that maps local space into world space.

  This header provides, for every supported primitive:

    LocalForm canonicalize(const Primitive& worldBaked)
        Local-space copy of the primitive (origin centred, +Z aligned).
        Used to drive the local-form tessellator and the form hash.

    Matrix3x4 instanceTransform(const Primitive& worldBaked)
        Column-major 3x4 (X col, Y col, Z col, T col) that takes
        the local form into the same world position as the original
        WORLD-baked primitive.

    uint64_t hashOf(const Primitive& worldBaked)
        FNV-1a 64-bit on quantised local form parameters, seeded by
        the primitive type tag.

  Quantisation: 1e6 (metres -> micrometres). Two values within
  ~1 micron hash the same.
----------------------------------------------------------------*/

#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "PrimitivesCIFF.h"
#include "TessCIFF.h"

namespace ciff
{
	namespace shape
	{
		// Column-major 3x4 (X col, Y col, Z col, T col), shared with tess.
		using Matrix3x4 = tess::Matrix3x4;

		inline Matrix3x4 identity() noexcept
		{
			return tess::identityMatrix();
		}

		inline Matrix3x4 fromFrame(const tess::Frame& f, const tess::V3& t) noexcept
		{
			return Matrix3x4{
				static_cast<float>(f.x.x), static_cast<float>(f.x.y), static_cast<float>(f.x.z),
				static_cast<float>(f.y.x), static_cast<float>(f.y.y), static_cast<float>(f.y.z),
				static_cast<float>(f.z.x), static_cast<float>(f.z.y), static_cast<float>(f.z.z),
				static_cast<float>(t.x),   static_cast<float>(t.y),   static_cast<float>(t.z),
			};
		}

		// ---------------- FNV-1a 64-bit ----------------

		inline constexpr uint64_t fnvOffsetBasis = 0xcbf29ce484222325ULL;
		inline constexpr uint64_t fnvPrime       = 0x100000001b3ULL;

		inline uint64_t fnv1a(const void* data, const size_t bytes,
			uint64_t hash = fnvOffsetBasis) noexcept
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
		inline uint64_t mix(const T& value, const uint64_t hash = fnvOffsetBasis) noexcept
		{
			static_assert(std::is_trivially_copyable_v<T>, "shape::mix requires trivially copyable type");
			return fnv1a(&value, sizeof(T), hash);
		}

		inline constexpr double quantizeScale = 1.0e6;

		inline int64_t quantize(const double v, const double scale = quantizeScale) noexcept
		{
			return static_cast<int64_t>(std::llround(v * scale));
		}

		// Offset by 1 so Type::Mesh (= 3 in CIFF wire codes; could collide with 0 elsewhere)
		// never feeds zero into the hash chain.
		inline uint64_t seed(const Type type) noexcept
		{
			return mix(static_cast<uint64_t>(type) + 1);
		}

		// ---------------- Per-primitive factoring ----------------

		// ----- Box -----

		inline Box canonicalize(const Box& b) noexcept
		{
			Box out;
			out.angle  = 0.0;
			out.delta  = Vector{ std::abs(b.delta.x), std::abs(b.delta.y), std::abs(b.delta.z) };
			out.center = Point{ 0.0, 0.0, 0.0 };
			out.normal = Vector{ 0.0, 0.0, 1.0 };
			return out;
		}

		inline Matrix3x4 instanceTransform(const Box& b) noexcept
		{
			const auto frame = tess::buildFrame(tess::toV3(b.normal), b.angle);
			// Mirror local axes if the original delta carried negative components,
			// so signed delta is recoverable from |delta| + transform.
			tess::Frame signedFrame = frame;
			if (b.delta.x < 0.0) { signedFrame.x = signedFrame.x * -1.0; }
			if (b.delta.y < 0.0) { signedFrame.y = signedFrame.y * -1.0; }
			if (b.delta.z < 0.0) { signedFrame.z = signedFrame.z * -1.0; }
			return fromFrame(signedFrame, tess::toV3(b.center));
		}

		inline uint64_t hashOf(const Box& b) noexcept
		{
			const auto local = canonicalize(b);
			uint64_t h = seed(Type::Box);
			const int64_t q[3] = {
				quantize(local.delta.x),
				quantize(local.delta.y),
				quantize(local.delta.z),
			};
			return fnv1a(q, sizeof(q), h);
		}

		// ----- Cylinder -----

		inline double cylinderHeight(const Cylinder& c) noexcept
		{
			const auto v = tess::toV3(c.centerB) - tess::toV3(c.centerA);
			return tess::length(v);
		}

		inline Cylinder canonicalize(const Cylinder& c) noexcept
		{
			Cylinder out;
			const double h = cylinderHeight(c);
			out.radius   = c.radius;
			out.centerA  = Point{ 0.0, 0.0, -0.5 * h };
			out.centerB  = Point{ 0.0, 0.0, +0.5 * h };
			out.isClosed = c.isClosed;
			return out;
		}

		inline Matrix3x4 instanceTransform(const Cylinder& c) noexcept
		{
			const auto a = tess::toV3(c.centerA);
			const auto b = tess::toV3(c.centerB);
			const auto axis = b - a;
			const auto frame = tess::buildFrame(axis, 0.0);
			const tess::V3 mid = { 0.5 * (a.x + b.x), 0.5 * (a.y + b.y), 0.5 * (a.z + b.z) };
			return fromFrame(frame, mid);
		}

		inline uint64_t hashOf(const Cylinder& c) noexcept
		{
			const auto local = canonicalize(c);
			uint64_t h = seed(Type::Cylinder);
			const int64_t q[3] = {
				quantize(local.radius),
				quantize(cylinderHeight(local)),
				static_cast<int64_t>(local.isClosed ? 1 : 0),
			};
			return fnv1a(q, sizeof(q), h);
		}

		// ----- Sphere -----

		inline Sphere canonicalize(const Sphere& s) noexcept
		{
			Sphere out;
			out.radius = s.radius;
			out.center = Point{ 0.0, 0.0, 0.0 };
			return out;
		}

		inline Matrix3x4 instanceTransform(const Sphere& s) noexcept
		{
			tess::Frame f{ tess::V3{1,0,0}, tess::V3{0,1,0}, tess::V3{0,0,1} };
			return fromFrame(f, tess::toV3(s.center));
		}

		inline uint64_t hashOf(const Sphere& s) noexcept
		{
			const auto local = canonicalize(s);
			uint64_t h = seed(Type::Sphere);
			const int64_t q = quantize(local.radius);
			return fnv1a(&q, sizeof(q), h);
		}

		// ----- CircularTorus -----

		inline CircularTorus canonicalize(const CircularTorus& t) noexcept
		{
			CircularTorus out;
			out.radius     = t.radius;
			out.tubeRadius = t.tubeRadius;
			out.angle      = 0.0;
			out.arcAngle   = t.arcAngle;
			out.center     = Point{ 0.0, 0.0, 0.0 };
			out.normal     = Vector{ 0.0, 0.0, 1.0 };
			out.isClosed   = t.isClosed;
			return out;
		}

		inline Matrix3x4 instanceTransform(const CircularTorus& t) noexcept
		{
			const auto frame = tess::buildFrame(tess::toV3(t.normal), t.angle);
			return fromFrame(frame, tess::toV3(t.center));
		}

		inline uint64_t hashOf(const CircularTorus& t) noexcept
		{
			const auto local = canonicalize(t);
			uint64_t h = seed(Type::CircularTorus);
			// Match tessellate(): arcAngle == 0 on disk encodes a full sweep.
			const double sweep = (std::abs(local.arcAngle) < 1e-9) ? (2.0 * 3.14159265358979323846) : local.arcAngle;
			const int64_t q[4] = {
				quantize(local.radius),
				quantize(local.tubeRadius),
				quantize(sweep),
				static_cast<int64_t>(local.isClosed ? 1 : 0),
			};
			return fnv1a(q, sizeof(q), h);
		}

		// ----- SphericalDish -----

		inline SphericalDish canonicalize(const SphericalDish& d) noexcept
		{
			SphericalDish out;
			out.verticalRadius   = d.verticalRadius;
			out.horizontalRadius = d.horizontalRadius;
			out.height           = d.height;
			out.center           = Point{ 0.0, 0.0, 0.0 };
			out.normal           = Vector{ 0.0, 0.0, 1.0 };
			out.isClosed         = d.isClosed;
			return out;
		}

		inline Matrix3x4 instanceTransform(const SphericalDish& d) noexcept
		{
			const auto frame = tess::buildFrame(tess::toV3(d.normal), 0.0);
			return fromFrame(frame, tess::toV3(d.center));
		}

		inline uint64_t hashOf(const SphericalDish& d) noexcept
		{
			const auto local = canonicalize(d);
			uint64_t h = seed(Type::SphericalDish);
			const int64_t q[4] = {
				quantize(local.verticalRadius),
				quantize(local.horizontalRadius),
				quantize(local.height),
				static_cast<int64_t>(local.isClosed ? 1 : 0),
			};
			return fnv1a(q, sizeof(q), h);
		}

		// ----- GeneralCylinder -----

		inline double generalCylinderHeight(const GeneralCylinder& g) noexcept
		{
			const auto v = tess::toV3(g.centerB) - tess::toV3(g.centerA);
			return tess::length(v);
		}

		inline GeneralCylinder canonicalize(const GeneralCylinder& g) noexcept
		{
			GeneralCylinder out;
			const double h = generalCylinderHeight(g);
			out.radiusA   = g.radiusA;
			out.radiusB   = g.radiusB;
			out.slopeA    = g.slopeA;
			out.slopeB    = g.slopeB;
			out.zAngleA   = g.zAngleA;
			out.zAngleB   = g.zAngleB;
			out.angle     = 0.0;
			out.arcAngle  = g.arcAngle;
			out.thickness = g.thickness;
			out.centerA   = Point{ 0.0, 0.0, -0.5 * h };
			out.centerB   = Point{ 0.0, 0.0, +0.5 * h };
			out.isClosed  = g.isClosed;
			return out;
		}

		inline Matrix3x4 instanceTransform(const GeneralCylinder& g) noexcept
		{
			const auto a = tess::toV3(g.centerA);
			const auto b = tess::toV3(g.centerB);
			const auto axis = b - a;
			const auto frame = tess::buildFrame(axis, g.angle);
			const tess::V3 mid = { 0.5 * (a.x + b.x), 0.5 * (a.y + b.y), 0.5 * (a.z + b.z) };
			return fromFrame(frame, mid);
		}

		inline uint64_t hashOf(const GeneralCylinder& g) noexcept
		{
			const auto local = canonicalize(g);
			uint64_t h = seed(Type::GeneralCylinder);
			const int64_t q[10] = {
				quantize(local.radiusA),
				quantize(local.radiusB),
				quantize(local.slopeA),
				quantize(local.slopeB),
				quantize(local.zAngleA),
				quantize(local.zAngleB),
				quantize(local.arcAngle),
				quantize(local.thickness),
				quantize(generalCylinderHeight(local)),
				static_cast<int64_t>(local.isClosed ? 1 : 0),
			};
			return fnv1a(q, sizeof(q), h);
		}

		// ----- Mesh fallback -----
		// Used for FacetGroup-tessellated meshes and any geometry whose primitive
		// is Type::Mesh. Hashes vertex counts, quantised vertices and raw indices.
		inline uint64_t hashOf(const Mesh& mesh) noexcept
		{
			uint64_t h = seed(Type::Mesh);
			const uint32_t vc = mesh.points();
			const uint32_t ic = static_cast<uint32_t>(mesh.indices.size());
			h = mix(vc, h);
			h = mix(ic, h);

			for (const auto& p : mesh.vertices)
			{
				const int64_t q[3] = { quantize(p.x), quantize(p.y), quantize(p.z) };
				h = fnv1a(q, sizeof(q), h);
			}

			if (!mesh.indices.empty())
				h = fnv1a(mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t), h);

			return h;
		}
	} // namespace shape
} // namespace ciff
