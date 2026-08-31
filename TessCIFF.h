/*----------------------------------------------------------------
  TessCIFF.h

  LOCAL-frame tessellators for CIFF parametric primitives plus a
  Matrix3x4 type and a Mesh-transform helper.

  Each tessellate(prim) overload produces a mesh in the primitive's
  CANONICAL LOCAL FRAME:

    - origin at (0,0,0)
    - principal axis along +Z
    - sweep / first vertex along +X
    - dimensions taken only from the primitive's dimensional fields
      (radius, height, delta, ...). Placement fields (center, normal,
      angle, centerA/B) are IGNORED.

  This mirrors the AvevaRvmDebug TessRVM.h convention. The CIFF reader
  (ReadCIFF.h) tessellates locally, then transforms the mesh to world
  space using shape::instanceTransform(prim). The 3D writer
  (Convert3D.h) uses the local mesh directly as a deduplicated form,
  with the same instanceTransform as the per-instance Matrix3x4.

  Algorithms favour clarity and correctness over fidelity. Default
  segment counts are pragmatic for plant-model scales.
----------------------------------------------------------------*/

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include "PrimitivesCIFF.h"

namespace ciff
{
	namespace tess
	{
		inline constexpr double pi    = std::numbers::pi;
		inline constexpr double twoPi = 2.0 * pi;

		// -------- vector helpers --------

		struct V3
		{
			double x = 0.0, y = 0.0, z = 0.0;

			V3 operator+(const V3& o) const noexcept { return { x + o.x, y + o.y, z + o.z }; }
			V3 operator-(const V3& o) const noexcept { return { x - o.x, y - o.y, z - o.z }; }
			V3 operator*(double s)    const noexcept { return { x * s, y * s, z * s }; }
		};

		inline V3 toV3(const Point& p)  noexcept { return { p.x, p.y, p.z }; }
		inline V3 toV3(const Vector& v) noexcept { return { v.x, v.y, v.z }; }
		inline Point toPoint(const V3& v) noexcept { return Point{ v.x, v.y, v.z }; }

		inline double dot(const V3& a, const V3& b) noexcept
		{
			return a.x * b.x + a.y * b.y + a.z * b.z;
		}

		inline V3 cross(const V3& a, const V3& b) noexcept
		{
			return {
				a.y * b.z - a.z * b.y,
				a.z * b.x - a.x * b.z,
				a.x * b.y - a.y * b.x,
			};
		}

		inline double length(const V3& v) noexcept
		{
			return std::sqrt(dot(v, v));
		}

		inline V3 normalize(const V3& v) noexcept
		{
			const double n = length(v);
			if (n < 1e-30) return { 0.0, 0.0, 1.0 };
			const double inv = 1.0 / n;
			return { v.x * inv, v.y * inv, v.z * inv };
		}

		// Build an orthonormal frame { x, y, z } where z is the supplied axis.
		// The x axis is constructed from a stable perpendicular and then rotated
		// around z by `angle` radians.
		struct Frame
		{
			V3 x;
			V3 y;
			V3 z;
		};

		// Construct the frame using the SAME convention as
		// AvevaRvmDebug GeometryMath::RotationFromTo(AxisZ, Normal):
		// the shortest-arc rotation that maps +Z onto z. This makes our
		// `angle` parameter agree with the writer's
		// RotationAngleInXYPlane(tx), so that arc primitives (CircularTorus,
		// GeneralCylinder, Box) start at the correct rotated +X.
		inline Frame buildFrame(const V3& zAxis, const double angle)
		{
			const V3 z = normalize(zAxis);

			V3 x0, y0;
			const double dz = z.z; // dot(+Z, z)
			if (dz > 1.0 - 1e-9)
			{
				x0 = V3{ 1.0, 0.0, 0.0 };
				y0 = V3{ 0.0, 1.0, 0.0 };
			}
			else if (dz < -1.0 + 1e-9)
			{
				// 180° flip about +X (matches a deterministic choice; the
				// writer's RotationFromTo returns IdentityNegative here, but
				// in practice CIFF rarely emits a fully inverted normal).
				x0 = V3{ 1.0, 0.0, 0.0 };
				y0 = V3{ 0.0, -1.0, 0.0 };
			}
			else
			{
				// Rodrigues rotation around k = normalize(+Z x z) by acos(dz),
				// applied to +X and +Y.
				const V3 k = normalize(V3{ -z.y, z.x, 0.0 });
				const double a = std::acos(std::clamp(dz, -1.0, 1.0));
				const double ca = std::cos(a);
				const double sa = std::sin(a);
				const double omc = 1.0 - ca;

				const auto rot = [&](const V3& v) -> V3
				{
					const double kv = dot(k, v);
					const V3 cv = cross(k, v);
					return V3{
						v.x * ca + cv.x * sa + k.x * kv * omc,
						v.y * ca + cv.y * sa + k.y * kv * omc,
						v.z * ca + cv.z * sa + k.z * kv * omc,
					};
				};

				x0 = rot(V3{ 1.0, 0.0, 0.0 });
				y0 = rot(V3{ 0.0, 1.0, 0.0 });
			}

			// Rotate by `angle` (CCW around z) in the (x0, y0) plane.
			const double c = std::cos(angle);
			const double s = std::sin(angle);

			V3 x = { x0.x * c + y0.x * s, x0.y * c + y0.y * s, x0.z * c + y0.z * s };
			V3 y = { -x0.x * s + y0.x * c, -x0.y * s + y0.y * c, -x0.z * s + y0.z * c };

			return { x, y, z };
		}

		// -------- mesh assembly helpers --------

		inline void emitTri(Mesh& mesh, uint32_t a, uint32_t b, uint32_t c)
		{
			mesh.indices.push_back(a);
			mesh.indices.push_back(b);
			mesh.indices.push_back(c);
		}

		inline void emitQuad(Mesh& mesh, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
		{
			emitTri(mesh, a, b, c);
			emitTri(mesh, a, c, d);
		}

		// -------- segment count heuristics --------

		inline uint32_t arcSegments(const double radius, const double arc, const uint32_t minSeg = 8,
			const uint32_t maxSeg = 64)
		{
			// Roughly one segment per ~6 degrees, scaled mildly with radius.
			const double base = std::max(8.0, std::abs(arc) * 12.0 / pi);
			const double scaled = base * std::clamp(0.5 + std::log10(std::max(radius, 1e-3)) * 0.1, 0.5, 1.5);
			return std::clamp(static_cast<uint32_t>(std::lround(scaled)), minSeg, maxSeg);
		}

		// -------- Matrix3x4 (column-major: X col, Y col, Z col, T col) --------

		using Matrix3x4 = std::array<float, 12>;

		inline Matrix3x4 identityMatrix() noexcept
		{
			return Matrix3x4{
				1.f, 0.f, 0.f,
				0.f, 1.f, 0.f,
				0.f, 0.f, 1.f,
				0.f, 0.f, 0.f,
			};
		}

		inline Point applyMatrix(const Matrix3x4& m, const Point& p) noexcept
		{
			return Point{
				m[0] * p.x + m[3] * p.y + m[6] * p.z + m[9],
				m[1] * p.x + m[4] * p.y + m[7] * p.z + m[10],
				m[2] * p.x + m[5] * p.y + m[8] * p.z + m[11],
			};
		}

		inline void transform(Mesh& mesh, const Matrix3x4& m) noexcept
		{
			for (auto& v : mesh.vertices)
				v = applyMatrix(m, v);
		}

		// -------- Box (LOCAL: centred at origin, axis-aligned) --------

		inline Mesh tessellate(const Box& box)
		{
			Mesh mesh;

			const double hx = 0.5 * std::abs(box.delta.x);
			const double hy = 0.5 * std::abs(box.delta.y);
			const double hz = 0.5 * std::abs(box.delta.z);

			if (hx < 1e-12 && hy < 1e-12 && hz < 1e-12)
				return mesh;

			mesh.vertices = {
				Point{ -hx, -hy, -hz }, Point{ +hx, -hy, -hz }, Point{ +hx, +hy, -hz }, Point{ -hx, +hy, -hz },
				Point{ -hx, -hy, +hz }, Point{ +hx, -hy, +hz }, Point{ +hx, +hy, +hz }, Point{ -hx, +hy, +hz },
			};

			// CCW from outside.
			emitQuad(mesh, 0, 3, 2, 1); // -z
			emitQuad(mesh, 4, 5, 6, 7); // +z
			emitQuad(mesh, 0, 1, 5, 4); // -y
			emitQuad(mesh, 2, 3, 7, 6); // +y
			emitQuad(mesh, 1, 2, 6, 5); // +x
			emitQuad(mesh, 3, 0, 4, 7); // -x

			return mesh;
		}

		// -------- Sphere (LOCAL: centred at origin) --------

		inline Mesh tessellate(const Sphere& sphere)
		{
			Mesh mesh;

			if (sphere.radius < 1e-12)
				return mesh;

			const uint32_t stacks = 16;
			const uint32_t slices = 24;
			const double r = sphere.radius;

			// Layout: [northPole, ring(1)*slices, ..., ring(stacks-1)*slices, southPole].
			// Poles are single vertices and longitude seam is closed by wrapping j.
			mesh.vertices.reserve(2 + static_cast<size_t>(stacks - 1) * slices);

			const uint32_t northPole = static_cast<uint32_t>(mesh.vertices.size());
			mesh.vertices.push_back(Point{ 0.0, 0.0, r });

			for (uint32_t i = 1; i < stacks; ++i)
			{
				const double phi = pi * static_cast<double>(i) / stacks;
				const double sp = std::sin(phi);
				const double cp = std::cos(phi);
				for (uint32_t j = 0; j < slices; ++j)
				{
					const double th = twoPi * static_cast<double>(j) / slices;
					mesh.vertices.push_back(Point{ r * sp * std::cos(th), r * sp * std::sin(th), r * cp });
				}
			}

			const uint32_t southPole = static_cast<uint32_t>(mesh.vertices.size());
			mesh.vertices.push_back(Point{ 0.0, 0.0, -r });

			const auto ring = [slices, northPole](uint32_t i, uint32_t j) -> uint32_t
			{
				return northPole + 1 + (i - 1) * slices + (j % slices);
			};

			// Top cap fan (degenerate-quad reduction: north + ring(1)).
			for (uint32_t j = 0; j < slices; ++j)
				emitTri(mesh, northPole, ring(1, j), ring(1, j + 1));

			// Middle bands.
			for (uint32_t i = 1; i + 1 < stacks; ++i)
				for (uint32_t j = 0; j < slices; ++j)
					emitQuad(mesh, ring(i, j), ring(i + 1, j), ring(i + 1, j + 1), ring(i, j + 1));

			// Bottom cap fan (south + ring(stacks-1)).
			for (uint32_t j = 0; j < slices; ++j)
				emitTri(mesh, southPole, ring(stacks - 1, j + 1), ring(stacks - 1, j));

			return mesh;
		}

		// -------- Cylinder (LOCAL: axis +Z, midpoint at origin) --------

		inline Mesh tessellate(const Cylinder& cyl)
		{
			Mesh mesh;

			const double h = length(toV3(cyl.centerB) - toV3(cyl.centerA));

			if (h < 1e-12 || cyl.radius < 1e-12)
				return mesh;

			const uint32_t slices = arcSegments(cyl.radius, twoPi);
			const double zA = -0.5 * h;
			const double zB = +0.5 * h;

			// Use `slices` unique angular samples (no seam duplicate) and close the
			// loop with wrap().
			mesh.vertices.reserve(static_cast<size_t>(slices) * 2 + (cyl.isClosed ? 2 : 0));

			for (uint32_t j = 0; j < slices; ++j)
			{
				const double th = twoPi * static_cast<double>(j) / slices;
				const double cx = cyl.radius * std::cos(th);
				const double sy = cyl.radius * std::sin(th);
				mesh.vertices.push_back(Point{ cx, sy, zA });
				mesh.vertices.push_back(Point{ cx, sy, zB });
			}

			const auto wrap = [slices](uint32_t j) { return j % slices; };

			for (uint32_t j = 0; j < slices; ++j)
			{
				const uint32_t j1 = wrap(j + 1);
				const uint32_t i00 = 2 * j;
				const uint32_t i10 = 2 * j + 1;
				const uint32_t i01 = 2 * j1;
				const uint32_t i11 = 2 * j1 + 1;
				emitQuad(mesh, i00, i01, i11, i10);
			}

			if (cyl.isClosed)
			{
				const uint32_t centerA = static_cast<uint32_t>(mesh.vertices.size());
				mesh.vertices.push_back(Point{ 0.0, 0.0, zA });
				const uint32_t centerB = static_cast<uint32_t>(mesh.vertices.size());
				mesh.vertices.push_back(Point{ 0.0, 0.0, zB });

				for (uint32_t j = 0; j < slices; ++j)
				{
					const uint32_t j1 = wrap(j + 1);
					emitTri(mesh, centerA, 2 * j1, 2 * j);
					emitTri(mesh, centerB, 2 * j + 1, 2 * j1 + 1);
				}
			}

			return mesh;
		}

		// -------- CircularTorus (LOCAL: axis +Z, sweep starts at +X) --------
		// Ported from AvevaRvmDebug/TessRVM.h Tessellate(CircularTorus).
		// CIFF: t.radius = major (RVM "offset"), t.tubeRadius = minor (RVM "radius"),
		//       t.arcAngle = sweep. The writer normalises sweep to [0, 2*pi), so
		//       arcAngle == 0 on disk encodes a full sweep.

		inline Mesh tessellate(const CircularTorus& t)
		{
			Mesh mesh;

			const double offset = t.radius;       // major
			const double minor  = t.tubeRadius;   // tube

			if (offset < 1e-12 || minor < 1e-12)
				return mesh;

			const double sweep = (std::abs(t.arcAngle) < 1e-9) ? twoPi : t.arcAngle;
			const bool full    = std::abs(std::abs(sweep) - twoPi) < 1e-6;

			const uint32_t segLong  = arcSegments(offset + minor, sweep);
			const uint32_t segShort = arcSegments(minor, twoPi);
			// Full sweep: `segLong` rings closed via u-wrap (no first/last duplicate).
			// Partial sweep: `segLong + 1` rings; caps reuse the first/last cross-
			// section ring instead of duplicating it.
			const uint32_t samplesL = full ? segLong : (segLong + 1);
			const uint32_t samplesS = segShort;

			mesh.vertices.reserve(static_cast<size_t>(samplesL) * samplesS);

			for (uint32_t u = 0; u < samplesL; ++u)
			{
				const double a = full
					? sweep * static_cast<double>(u) / static_cast<double>(samplesL)
					: sweep * static_cast<double>(u) / static_cast<double>(samplesL - 1);
				const double cu = std::cos(a);
				const double su = std::sin(a);
				for (uint32_t v = 0; v < samplesS; ++v)
				{
					const double b = twoPi * static_cast<double>(v) / static_cast<double>(samplesS);
					const double cb = std::cos(b);
					const double sb = std::sin(b);
					const double r  = minor * cb + offset;
					mesh.vertices.push_back(Point{ r * cu, r * su, minor * sb });
				}
			}

			const uint32_t spans = full ? samplesL : (samplesL - 1);
			const auto uWrap = [&](uint32_t u) -> uint32_t { return full ? (u % samplesL) : u; };

			for (uint32_t u = 0; u < spans; ++u)
			{
				const uint32_t u0 = u;
				const uint32_t u1 = uWrap(u + 1);
				for (uint32_t v = 0; v < samplesS; ++v)
				{
					const uint32_t vn  = (v + 1) % samplesS;
					const uint32_t i00 = u0 * samplesS + v;
					const uint32_t i10 = u1 * samplesS + v;
					const uint32_t i11 = u1 * samplesS + vn;
					const uint32_t i01 = u0 * samplesS + vn;

					emitTri(mesh, i00, i10, i11);
					emitTri(mesh, i00, i11, i01);
				}
			}

			// End caps for non-full sweep: triangle-fan over existing first/last
			// cross-section ring; the ring is planar so any ring vertex works as
			// the fan apex (no new vertices needed).
			if (!full && t.isClosed && samplesS >= 3)
			{
				const uint32_t base0 = 0;
				for (uint32_t v = 1; v + 1 < samplesS; ++v)
					emitTri(mesh, base0, base0 + v + 1, base0 + v);

				const uint32_t base1 = (samplesL - 1) * samplesS;
				for (uint32_t v = 1; v + 1 < samplesS; ++v)
					emitTri(mesh, base1, base1 + v, base1 + v + 1);
			}

			return mesh;
		}

		// -------- SphericalDish (LOCAL: apex on +Z, base ring on XY) --------

		inline Mesh tessellate(const SphericalDish& d)
		{
			Mesh mesh;

			if (d.height < 1e-12 || d.horizontalRadius < 1e-12)
				return mesh;

			const uint32_t stacks = 12;
			const uint32_t slices = 24;

			// Layout: [apex, ring(1)*slices, ..., ring(stacks)*slices]. Apex is a
			// single vertex; longitude seam is closed by wrapping j.
			mesh.vertices.reserve(1 + static_cast<size_t>(stacks) * slices);

			const uint32_t apex = static_cast<uint32_t>(mesh.vertices.size());
			mesh.vertices.push_back(Point{ 0.0, 0.0, d.verticalRadius });

			for (uint32_t i = 1; i <= stacks; ++i)
			{
				const double tt  = static_cast<double>(i) / stacks; // 0 = apex, 1 = base
				const double phi = 0.5 * pi * tt;
				const double cp  = std::cos(phi);
				const double sp  = std::sin(phi);
				const double rh  = d.horizontalRadius * sp;
				const double zv  = d.verticalRadius * cp;

				for (uint32_t j = 0; j < slices; ++j)
				{
					const double th = twoPi * static_cast<double>(j) / slices;
					mesh.vertices.push_back(Point{ rh * std::cos(th), rh * std::sin(th), zv });
				}
			}

			const auto ring = [slices, apex](uint32_t i, uint32_t j) -> uint32_t
			{
				return apex + 1 + (i - 1) * slices + (j % slices);
			};

			// Apex fan to ring(1).
			for (uint32_t j = 0; j < slices; ++j)
				emitTri(mesh, apex, ring(1, j + 1), ring(1, j));

			// Middle bands.
			for (uint32_t i = 1; i < stacks; ++i)
				for (uint32_t j = 0; j < slices; ++j)
					emitQuad(mesh, ring(i, j), ring(i, j + 1), ring(i + 1, j + 1), ring(i + 1, j));

			return mesh;
		}

		// -------- GeneralCylinder / Snout (LOCAL: axis +Z, midpoint at origin) --------

		inline Mesh tessellate(const GeneralCylinder& g)
		{
			Mesh mesh;

			const double h = length(toV3(g.centerB) - toV3(g.centerA));

			if (h < 1e-12)
				return mesh;
			if (g.radiusA < 1e-12 && g.radiusB < 1e-12)
				return mesh;

			const double sweep   = (std::abs(g.arcAngle) > 1e-9) ? g.arcAngle : twoPi;
			const bool   full    = std::abs(std::abs(sweep) - twoPi) < 1e-6;
			const uint32_t slices = arcSegments(std::max(g.radiusA, g.radiusB), sweep);
			const uint32_t segVerts = full ? slices : (slices + 1);
			const double zA = -0.5 * h;
			const double zB = +0.5 * h;

			// Snout / cone tip: a degenerate end is emitted as a single apex vertex
			// instead of `segVerts` coincident copies.
			const bool tipA = g.radiusA < 1e-12;
			const bool tipB = g.radiusB < 1e-12;

			mesh.vertices.reserve(
				(tipA ? 1u : segVerts) +
				(tipB ? 1u : segVerts) +
				(g.isClosed ? 2u : 0u));

			const auto pushRing = [&](double radius, double z) -> uint32_t
			{
				const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
				for (uint32_t j = 0; j < segVerts; ++j)
				{
					const double frac = static_cast<double>(j) / static_cast<double>(slices);
					const double th = full ? twoPi * frac : sweep * frac;
					mesh.vertices.push_back(Point{ radius * std::cos(th), radius * std::sin(th), z });
				}
				return base;
			};

			uint32_t tipAIndex = 0, tipBIndex = 0;
			uint32_t baseA = 0, baseB = 0;

			if (tipA)
			{
				tipAIndex = static_cast<uint32_t>(mesh.vertices.size());
				mesh.vertices.push_back(Point{ 0.0, 0.0, zA });
			}
			else
			{
				baseA = pushRing(g.radiusA, zA);
			}

			if (tipB)
			{
				tipBIndex = static_cast<uint32_t>(mesh.vertices.size());
				mesh.vertices.push_back(Point{ 0.0, 0.0, zB });
			}
			else
			{
				baseB = pushRing(g.radiusB, zB);
			}

			const auto wrap = [&](uint32_t j) { return full ? (j % slices) : j; };

			// Side: quad strip in the normal case, triangle fan when one end is a tip.
			for (uint32_t j = 0; j < slices; ++j)
			{
				const uint32_t j0 = wrap(j);
				const uint32_t j1 = wrap(j + 1);

				if (tipA)
					emitTri(mesh, tipAIndex, baseB + j1, baseB + j0);
				else if (tipB)
					emitTri(mesh, tipBIndex, baseA + j0, baseA + j1);
				else
					emitQuad(mesh, baseA + j0, baseA + j1, baseB + j1, baseB + j0);
			}

			if (g.isClosed)
			{
				if (!tipA)
				{
					const uint32_t centerA = static_cast<uint32_t>(mesh.vertices.size());
					mesh.vertices.push_back(Point{ 0.0, 0.0, zA });
					for (uint32_t j = 0; j < slices; ++j)
					{
						const uint32_t j0 = wrap(j);
						const uint32_t j1 = wrap(j + 1);
						emitTri(mesh, centerA, baseA + j1, baseA + j0);
					}
				}
				if (!tipB)
				{
					const uint32_t centerB = static_cast<uint32_t>(mesh.vertices.size());
					mesh.vertices.push_back(Point{ 0.0, 0.0, zB });
					for (uint32_t j = 0; j < slices; ++j)
					{
						const uint32_t j0 = wrap(j);
						const uint32_t j1 = wrap(j + 1);
						emitTri(mesh, centerB, baseB + j0, baseB + j1);
					}
				}
			}

			return mesh;
		}
	}
}

