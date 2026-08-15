#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "ProcessCIFF.h"
#include "PrimitivesCIFF.h"
#include "ReadCIFF.h"
#include "ShapeCIFF.h"
#include "TessCIFF.h"

namespace ciff::primitive_instance
{
	using Matrix3x4 = ciff::shape::Matrix3x4;

	inline constexpr bool enableCanonicalPrimitiveInstancing = true;

	struct FormInstance
	{
		uint64_t hash = 0;
		Matrix3x4 transform = ciff::shape::identity();
		Matrix3x4 meshTransform = ciff::shape::identity();
		bool transformMesh = false;
		bool hashMesh = false;
	};

	inline bool Enabled() noexcept
	{
		return enableCanonicalPrimitiveInstancing;
	}

	inline bool Positive(const double value) noexcept
	{
		return std::isfinite(value) && value > 0.0 && std::abs(value) > 1e-12;
	}

	inline Matrix3x4 Scale(const double x, const double y, const double z) noexcept
	{
		return Matrix3x4{
			static_cast<float>(x), 0.f, 0.f,
			0.f, static_cast<float>(y), 0.f,
			0.f, 0.f, static_cast<float>(z),
			0.f, 0.f, 0.f,
		};
	}

	inline Matrix3x4 Multiply(const Matrix3x4& a, const Matrix3x4& b) noexcept
	{
		Matrix3x4 result{};

		for (size_t col = 0; col < 3; ++col)
		{
			for (size_t row = 0; row < 3; ++row)
			{
				result[col * 3 + row] =
					a[0 * 3 + row] * b[col * 3 + 0] +
					a[1 * 3 + row] * b[col * 3 + 1] +
					a[2 * 3 + row] * b[col * 3 + 2];
			}
		}

		for (size_t row = 0; row < 3; ++row)
		{
			result[9 + row] =
				a[0 * 3 + row] * b[9 + 0] +
				a[1 * 3 + row] * b[9 + 1] +
				a[2 * 3 + row] * b[9 + 2] +
				a[9 + row];
		}

		return result;
	}

	inline uint64_t Hash(const Type type, const uint32_t variant = 0)
	{
		constexpr uint64_t tag = 0x7ab5140df3c6e92bULL;
		auto hash = ciff::shape::seed(type);
		hash = ciff::shape::mix(tag, hash);
		return ciff::shape::mix(variant, hash);
	}

	inline uint64_t HashMesh(const Mesh& mesh)
	{
		auto hash = Hash(Type::Mesh);
		const auto pointCount = mesh.points();
		const auto indexCount = static_cast<uint32_t>(mesh.indices.size());

		hash = ciff::shape::mix(pointCount, hash);
		hash = ciff::shape::mix(indexCount, hash);

		for (const auto& point : mesh.vertices)
		{
			hash = ciff::shape::mix(ciff::shape::quantize(point.x), hash);
			hash = ciff::shape::mix(ciff::shape::quantize(point.y), hash);
			hash = ciff::shape::mix(ciff::shape::quantize(point.z), hash);
		}

		if (!mesh.indices.empty())
			hash = ciff::shape::fnv1a(mesh.indices.data(), mesh.indices.size() * sizeof(mesh.indices[0]), hash);

		return hash;
	}

	inline Mesh CopyMesh(const Mesh& source)
	{
		Mesh mesh;
		mesh.vertices = source.vertices;
		mesh.indices = source.indices;
		return mesh;
	}

	inline int64_t Q(const double value)
	{
		return ciff::shape::quantize(value);
	}

	template <typename... Values>
	inline uint64_t HashValues(const Type type, Values... values)
	{
		auto hash = Hash(type);
		((hash = ciff::shape::mix(static_cast<int64_t>(values), hash)), ...);
		return hash;
	}

	inline double Sweep(const double arcAngle) noexcept
	{
		return std::abs(arcAngle) < 1e-9 ? ciff::tess::twoPi : arcAngle;
	}

	inline uint32_t CylinderSegments(const double radius)
	{
		return ciff::tess::arcSegments(radius, ciff::tess::twoPi);
	}

	inline uint32_t CircularTorusLongSegments(const CircularTorus& torus)
	{
		return ciff::tess::arcSegments(torus.radius + torus.tubeRadius, Sweep(torus.arcAngle));
	}

	inline uint32_t CircularTorusShortSegments(const CircularTorus& torus)
	{
		return ciff::tess::arcSegments(torus.tubeRadius, ciff::tess::twoPi);
	}

	inline uint32_t GeneralCylinderSegments(const GeneralCylinder& cylinder)
	{
		return ciff::tess::arcSegments(std::max(cylinder.radiusA, cylinder.radiusB), Sweep(cylinder.arcAngle));
	}

	inline double CylinderHeight(const Cylinder& cylinder) noexcept
	{
		return ciff::shape::cylinderHeight(cylinder);
	}

	inline double GeneralCylinderHeight(const GeneralCylinder& cylinder) noexcept
	{
		return ciff::shape::generalCylinderHeight(cylinder);
	}

	inline double AxisValue(const Point& point, const int axis) noexcept
	{
		if (axis == 0)
			return point.x;
		if (axis == 1)
			return point.y;
		return point.z;
	}

	inline void SetAxisValue(Point& point, const int axis, const double value) noexcept
	{
		if (axis == 0)
			point.x = value;
		else if (axis == 1)
			point.y = value;
		else
			point.z = value;
	}

	inline bool HasDistinctAxisLength(const double a, const double b) noexcept
	{
		const auto scale = std::max(std::max(std::abs(a), std::abs(b)), 1.0);
		return std::abs(a - b) > scale * 1e-3;
	}

	inline std::array<int, 3> CanonicalAxisOrder(const std::array<double, 3>& extents)
	{
		std::array<int, 3> axes{ 0, 1, 2 };
		std::sort(axes.begin(), axes.end(), [&](const int lhs, const int rhs)
		{
			if (HasDistinctAxisLength(extents[static_cast<size_t>(lhs)], extents[static_cast<size_t>(rhs)]))
				return extents[static_cast<size_t>(lhs)] > extents[static_cast<size_t>(rhs)];
			return lhs < rhs;
		});
		return axes;
	}

	inline bool IsOddPermutation(const std::array<int, 3>& axes) noexcept
	{
		int inversions = 0;
		for (int i = 0; i < 3; ++i)
			for (int j = i + 1; j < 3; ++j)
				if (axes[static_cast<size_t>(i)] > axes[static_cast<size_t>(j)])
					++inversions;
		return (inversions % 2) != 0;
	}

	inline bool NormalizeBakedAxisAlignedMesh(Mesh& mesh, Matrix3x4& transform)
	{
		if (mesh.empty())
			return false;

		std::array<double, 3> minPoint{
			std::numeric_limits<double>::infinity(),
			std::numeric_limits<double>::infinity(),
			std::numeric_limits<double>::infinity() };
		std::array<double, 3> maxPoint{
			-std::numeric_limits<double>::infinity(),
			-std::numeric_limits<double>::infinity(),
			-std::numeric_limits<double>::infinity() };

		for (const auto& point : mesh.vertices)
		{
			if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
				return false;

			minPoint[0] = std::min(minPoint[0], point.x);
			minPoint[1] = std::min(minPoint[1], point.y);
			minPoint[2] = std::min(minPoint[2], point.z);
			maxPoint[0] = std::max(maxPoint[0], point.x);
			maxPoint[1] = std::max(maxPoint[1], point.y);
			maxPoint[2] = std::max(maxPoint[2], point.z);
		}

		std::array<double, 3> center{};
		std::array<double, 3> extents{};
		for (int axis = 0; axis < 3; ++axis)
		{
			center[static_cast<size_t>(axis)] = (minPoint[static_cast<size_t>(axis)] + maxPoint[static_cast<size_t>(axis)]) * 0.5;
			extents[static_cast<size_t>(axis)] = maxPoint[static_cast<size_t>(axis)] - minPoint[static_cast<size_t>(axis)];
		}

		const auto axes = CanonicalAxisOrder(extents);
		std::array<double, 3> signs{ 1.0, 1.0, 1.0 };
		if (IsOddPermutation(axes))
			signs[2] = -1.0;

		for (auto& point : mesh.vertices)
		{
			const auto original = point;
			for (int axis = 0; axis < 3; ++axis)
			{
				const auto sourceAxis = axes[static_cast<size_t>(axis)];
				SetAxisValue(
					point,
					axis,
					signs[static_cast<size_t>(axis)] *
					(AxisValue(original, sourceAxis) - center[static_cast<size_t>(sourceAxis)]));
			}
		}

		transform = ciff::shape::identity();
		transform[9] = static_cast<float>(center[0]);
		transform[10] = static_cast<float>(center[1]);
		transform[11] = static_cast<float>(center[2]);

		transform[0] = 0.f;
		transform[4] = 0.f;
		transform[8] = 0.f;
		for (int axis = 0; axis < 3; ++axis)
		{
			const auto sourceAxis = axes[static_cast<size_t>(axis)];
			transform[static_cast<size_t>(axis) * 3 + static_cast<size_t>(sourceAxis)] =
				static_cast<float>(signs[static_cast<size_t>(axis)]);
		}

		return true;
	}

	struct QuantizedPoint
	{
		int64_t x = 0;
		int64_t y = 0;
		int64_t z = 0;

		bool operator<(const QuantizedPoint& other) const noexcept
		{
			if (x != other.x)
				return x < other.x;
			if (y != other.y)
				return y < other.y;
			return z < other.z;
		}

		bool operator==(const QuantizedPoint& other) const noexcept
		{
			return x == other.x && y == other.y && z == other.z;
		}
	};

	inline QuantizedPoint QuantizePoint(const Mesh& mesh, const uint32_t index)
	{
		const auto& point = mesh.vertices[index];
		return {
			ciff::shape::quantize(point.x),
			ciff::shape::quantize(point.y),
			ciff::shape::quantize(point.z)
		};
	}

	inline uint32_t FindPointIndex(const std::vector<QuantizedPoint>& points, const QuantizedPoint& point)
	{
		const auto it = std::lower_bound(points.begin(), points.end(), point);
		return static_cast<uint32_t>(it - points.begin());
	}

	struct IndexedQuantizedPoint
	{
		QuantizedPoint point;
		uint32_t sourceIndex = 0;

		bool operator<(const IndexedQuantizedPoint& other) const noexcept
		{
			if (point < other.point)
				return true;
			if (other.point < point)
				return false;
			return sourceIndex < other.sourceIndex;
		}
	};

	inline std::array<uint32_t, 3> CanonicalTriangle(const uint32_t a, const uint32_t b, const uint32_t c) noexcept
	{
		const std::array<uint32_t, 3> t0{ a, b, c };
		const std::array<uint32_t, 3> t1{ b, c, a };
		const std::array<uint32_t, 3> t2{ c, a, b };

		return std::min({ t0, t1, t2 });
	}

	inline void CanonicalizeMesh(Mesh& mesh)
	{
		if (mesh.empty() || mesh.indices.size() % 3 != 0)
			return;

		std::vector<QuantizedPoint> points;
		points.reserve(mesh.indices.size());

		for (const auto index : mesh.indices)
		{
			if (index >= mesh.vertices.size())
				return;

			points.emplace_back(QuantizePoint(mesh, index));
		}

		std::sort(points.begin(), points.end());
		points.erase(std::unique(points.begin(), points.end()), points.end());

		std::vector<std::array<uint32_t, 3>> triangles;
		triangles.reserve(mesh.indices.size() / 3);

		for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
		{
			triangles.emplace_back(CanonicalTriangle(
				FindPointIndex(points, QuantizePoint(mesh, mesh.indices[i + 0])),
				FindPointIndex(points, QuantizePoint(mesh, mesh.indices[i + 1])),
				FindPointIndex(points, QuantizePoint(mesh, mesh.indices[i + 2]))));
		}

		std::sort(triangles.begin(), triangles.end());

		mesh.vertices.clear();
		mesh.indices.clear();
		mesh.vertices.reserve(points.size());
		mesh.indices.reserve(triangles.size() * 3);

		for (const auto& point : points)
		{
			mesh.vertices.push_back(Point{
				static_cast<double>(point.x) / ciff::shape::quantizeScale,
				static_cast<double>(point.y) / ciff::shape::quantizeScale,
				static_cast<double>(point.z) / ciff::shape::quantizeScale
			});
		}

		for (const auto& triangle : triangles)
		{
			mesh.indices.emplace_back(triangle[0]);
			mesh.indices.emplace_back(triangle[1]);
			mesh.indices.emplace_back(triangle[2]);
		}
	}

	// Hash the same canonical transcript produced by CanonicalizeMesh + HashMesh,
	// without materializing a second Mesh. Each referenced source vertex is
	// quantized once and receives its canonical index while the points are sorted;
	// triangle remapping is then O(1) per index.
	inline uint64_t HashCanonicalMesh(const Mesh& mesh)
	{
		if (mesh.empty() || mesh.indices.size() % 3 != 0)
			return HashMesh(mesh);

		constexpr auto invalidIndex = std::numeric_limits<uint32_t>::max();
		std::vector<uint32_t> sourceToCanonical(mesh.vertices.size(), invalidIndex);
		std::vector<IndexedQuantizedPoint> points;
		points.reserve(std::min(mesh.vertices.size(), mesh.indices.size()));

		for (const auto sourceIndex : mesh.indices)
		{
			if (sourceIndex >= mesh.vertices.size())
				return HashMesh(mesh);

			if (sourceToCanonical[sourceIndex] != invalidIndex)
				continue;

			// Any non-sentinel value marks the source vertex as already collected;
			// the final canonical index is assigned after sorting below.
			sourceToCanonical[sourceIndex] = 0;
			points.push_back({ QuantizePoint(mesh, sourceIndex), sourceIndex });
		}

		std::sort(points.begin(), points.end());

		uint32_t pointCount = 0;
		for (size_t i = 0; i < points.size(); ++i)
		{
			if (i == 0 || !(points[i].point == points[i - 1].point))
				++pointCount;
			sourceToCanonical[points[i].sourceIndex] = pointCount - 1;
		}

		std::vector<std::array<uint32_t, 3>> triangles;
		triangles.reserve(mesh.indices.size() / 3);
		for (size_t i = 0; i < mesh.indices.size(); i += 3)
		{
			triangles.emplace_back(CanonicalTriangle(
				sourceToCanonical[mesh.indices[i + 0]],
				sourceToCanonical[mesh.indices[i + 1]],
				sourceToCanonical[mesh.indices[i + 2]]));
		}
		std::sort(triangles.begin(), triangles.end());

		auto hash = Hash(Type::Mesh);
		const auto indexCount = static_cast<uint32_t>(mesh.indices.size());
		hash = ciff::shape::mix(pointCount, hash);
		hash = ciff::shape::mix(indexCount, hash);

		for (size_t i = 0; i < points.size(); ++i)
		{
			if (i != 0 && points[i].point == points[i - 1].point)
				continue;

			// Preserve HashMesh's exact quantize -> double -> quantize transcript,
			// including its behavior for coordinates beyond exact double integers.
			const auto& point = points[i].point;
			hash = ciff::shape::mix(ciff::shape::quantize(
				static_cast<double>(point.x) / ciff::shape::quantizeScale), hash);
			hash = ciff::shape::mix(ciff::shape::quantize(
				static_cast<double>(point.y) / ciff::shape::quantizeScale), hash);
			hash = ciff::shape::mix(ciff::shape::quantize(
				static_cast<double>(point.z) / ciff::shape::quantizeScale), hash);
		}

		for (const auto& triangle : triangles)
			hash = ciff::shape::fnv1a(triangle.data(), sizeof(uint32_t) * triangle.size(), hash);

		return hash;
	}

	inline FormInstance Default(const Read& data, const Geometry& geometry)
	{
		FormInstance form;

		switch (geometry.primitive)
		{
		case Type::Mesh:
			form.hash = ciff::shape::hashOf(data.getMesh(geometry.mesh));
			break;
		case Type::Box:
			form.hash = ciff::shape::hashOf(data.boxes[geometry.primitiveIndex]);
			form.transform = ciff::shape::instanceTransform(data.boxes[geometry.primitiveIndex]);
			break;
		case Type::Cylinder:
			form.hash = ciff::shape::hashOf(data.cylinders[geometry.primitiveIndex]);
			form.transform = ciff::shape::instanceTransform(data.cylinders[geometry.primitiveIndex]);
			break;
		case Type::CircularTorus:
			form.hash = ciff::shape::hashOf(data.circularToruses[geometry.primitiveIndex]);
			form.transform = ciff::shape::instanceTransform(data.circularToruses[geometry.primitiveIndex]);
			break;
		case Type::Sphere:
			form.hash = ciff::shape::hashOf(data.spheres[geometry.primitiveIndex]);
			form.transform = ciff::shape::instanceTransform(data.spheres[geometry.primitiveIndex]);
			break;
		case Type::SphericalDish:
			form.hash = ciff::shape::hashOf(data.sphericalDishes[geometry.primitiveIndex]);
			form.transform = ciff::shape::instanceTransform(data.sphericalDishes[geometry.primitiveIndex]);
			break;
		case Type::GeneralCylinder:
			form.hash = ciff::shape::hashOf(data.generalCylinders[geometry.primitiveIndex]);
			form.transform = ciff::shape::instanceTransform(data.generalCylinders[geometry.primitiveIndex]);
			break;
		default:
			break;
		}

		return form;
	}

	inline FormInstance BoxInstance(const Box& box)
	{
		const auto x = std::abs(box.delta.x);
		const auto y = std::abs(box.delta.y);
		const auto z = std::abs(box.delta.z);

		FormInstance form;
		form.hash = Hash(Type::Box);
		form.transform = Multiply(ciff::shape::instanceTransform(box), Scale(x, y, z));
		form.meshTransform = Scale(1.0 / x, 1.0 / y, 1.0 / z);
		form.transformMesh = true;
		return form;
	}

	inline FormInstance CylinderInstance(const Cylinder& cylinder)
	{
		const auto height = CylinderHeight(cylinder);
		const auto segments = CylinderSegments(cylinder.radius);

		FormInstance form;
		form.hash = HashValues(Type::Cylinder, static_cast<int64_t>(segments), static_cast<int64_t>(cylinder.isClosed ? 1 : 0));
		form.transform = Multiply(ciff::shape::instanceTransform(cylinder), Scale(cylinder.radius, cylinder.radius, height));
		form.meshTransform = Scale(1.0 / cylinder.radius, 1.0 / cylinder.radius, 1.0 / height);
		form.transformMesh = true;
		return form;
	}

	inline FormInstance SphereInstance(const Sphere& sphere)
	{
		FormInstance form;
		form.hash = Hash(Type::Sphere);
		form.transform = Multiply(ciff::shape::instanceTransform(sphere), Scale(sphere.radius, sphere.radius, sphere.radius));
		form.meshTransform = Scale(1.0 / sphere.radius, 1.0 / sphere.radius, 1.0 / sphere.radius);
		form.transformMesh = true;
		return form;
	}

	inline FormInstance CircularTorusInstance(const CircularTorus& torus)
	{
		const auto sweep = Sweep(torus.arcAngle);
		const auto longSegments = CircularTorusLongSegments(torus);
		const auto shortSegments = CircularTorusShortSegments(torus);

		FormInstance form;
		form.hash = HashValues(
			Type::CircularTorus,
			Q(torus.radius / torus.tubeRadius),
			Q(sweep),
			static_cast<int64_t>(longSegments),
			static_cast<int64_t>(shortSegments),
			static_cast<int64_t>(torus.isClosed ? 1 : 0));
		form.transform = Multiply(ciff::shape::instanceTransform(torus), Scale(torus.tubeRadius, torus.tubeRadius, torus.tubeRadius));
		form.meshTransform = Scale(1.0 / torus.tubeRadius, 1.0 / torus.tubeRadius, 1.0 / torus.tubeRadius);
		form.transformMesh = true;
		return form;
	}

	inline FormInstance SphericalDishInstance(const SphericalDish& dish)
	{
		FormInstance form;
		form.hash = HashValues(Type::SphericalDish, static_cast<int64_t>(dish.isClosed ? 1 : 0));
		form.transform = Multiply(ciff::shape::instanceTransform(dish), Scale(dish.horizontalRadius, dish.horizontalRadius, dish.verticalRadius));
		form.meshTransform = Scale(1.0 / dish.horizontalRadius, 1.0 / dish.horizontalRadius, 1.0 / dish.verticalRadius);
		form.transformMesh = true;
		return form;
	}

	inline FormInstance GeneralCylinderInstance(const GeneralCylinder& cylinder)
	{
		const auto height = GeneralCylinderHeight(cylinder);
		const auto radius = std::max(cylinder.radiusA, cylinder.radiusB);
		const auto sweep = Sweep(cylinder.arcAngle);
		const auto segments = GeneralCylinderSegments(cylinder);

		FormInstance form;
		form.hash = HashValues(
			Type::GeneralCylinder,
			Q(cylinder.radiusA / radius),
			Q(cylinder.radiusB / radius),
			Q(cylinder.slopeA),
			Q(cylinder.slopeB),
			Q(cylinder.zAngleA),
			Q(cylinder.zAngleB),
			Q(sweep),
			Q(cylinder.thickness / radius),
			static_cast<int64_t>(segments),
			static_cast<int64_t>(cylinder.isClosed ? 1 : 0));
		form.transform = Multiply(ciff::shape::instanceTransform(cylinder), Scale(radius, radius, height));
		form.meshTransform = Scale(1.0 / radius, 1.0 / radius, 1.0 / height);
		form.transformMesh = true;
		return form;
	}

	inline FormInstance MeshInstance()
	{
		FormInstance form;
		form.hashMesh = true;
		return form;
	}

	inline FormInstance Make(const Read& data, const Geometry& geometry)
	{
		if (!Enabled())
			return Default(data, geometry);

		switch (geometry.primitive)
		{
		case Type::Mesh:
			return MeshInstance();
		case Type::Box:
		{
			const auto& box = data.boxes[geometry.primitiveIndex];
			if (Positive(std::abs(box.delta.x)) && Positive(std::abs(box.delta.y)) && Positive(std::abs(box.delta.z)))
				return BoxInstance(box);
			break;
		}
		case Type::Cylinder:
		{
			const auto& cylinder = data.cylinders[geometry.primitiveIndex];
			if (Positive(cylinder.radius) && Positive(CylinderHeight(cylinder)))
				return CylinderInstance(cylinder);
			break;
		}
		case Type::CircularTorus:
		{
			const auto& torus = data.circularToruses[geometry.primitiveIndex];
			if (Positive(torus.radius) && Positive(torus.tubeRadius))
				return CircularTorusInstance(torus);
			break;
		}
		case Type::Sphere:
		{
			const auto& sphere = data.spheres[geometry.primitiveIndex];
			if (Positive(sphere.radius))
				return SphereInstance(sphere);
			break;
		}
		case Type::SphericalDish:
		{
			const auto& dish = data.sphericalDishes[geometry.primitiveIndex];
			if (Positive(dish.horizontalRadius) && Positive(dish.verticalRadius))
				return SphericalDishInstance(dish);
			break;
		}
		case Type::GeneralCylinder:
		{
			const auto& cylinder = data.generalCylinders[geometry.primitiveIndex];
			const auto radius = std::max(cylinder.radiusA, cylinder.radiusB);
			if (Positive(radius) && Positive(GeneralCylinderHeight(cylinder)))
				return GeneralCylinderInstance(cylinder);
			break;
		}
		default:
			break;
		}

		return Default(data, geometry);
	}

	inline Mesh TessellatePrimitive(const Read& data, const Geometry& geometry)
	{
		switch (geometry.primitive)
		{
		case Type::Box:
			return ciff::TessellateCanonical(data.boxes[geometry.primitiveIndex]);
		case Type::Cylinder:
			return ciff::TessellateCanonical(data.cylinders[geometry.primitiveIndex]);
		case Type::CircularTorus:
			return ciff::TessellateCanonical(data.circularToruses[geometry.primitiveIndex]);
		case Type::Sphere:
			return ciff::TessellateCanonical(data.spheres[geometry.primitiveIndex]);
		case Type::SphericalDish:
			return ciff::TessellateCanonical(data.sphericalDishes[geometry.primitiveIndex]);
		case Type::GeneralCylinder:
			return ciff::TessellateCanonical(data.generalCylinders[geometry.primitiveIndex]);
		default:
			return {};
		}
	}

	inline Mesh TessellateImpl(
		const Read& data,
		const Geometry& geometry,
		FormInstance& form,
		const bool resolveCanonicalMeshHash)
	{
		auto mesh = geometry.primitive == Type::Mesh
			? data.getMesh(geometry.mesh)
			: TessellatePrimitive(data, geometry);

		if (form.transformMesh)
			ciff::tess::transform(mesh, form.meshTransform);

		if (form.hashMesh)
		{
			if (NormalizeBakedAxisAlignedMesh(mesh, form.transform))
			{
				if (resolveCanonicalMeshHash)
					form.hash = HashCanonicalMesh(mesh);
			}
		}

		return mesh;
	}

	inline Mesh Tessellate(const Read& data, const Geometry& geometry, FormInstance& form)
	{
		return TessellateImpl(data, geometry, form, true);
	}

	// Recreate render geometry for a FormInstance whose canonical mesh hash was
	// resolved in an earlier pass. The mesh normalization and transform remain
	// identical, while the already resolved hash is preserved without re-sorting.
	inline Mesh TessellateResolved(const Read& data, const Geometry& geometry, FormInstance& resolvedForm)
	{
		return TessellateImpl(data, geometry, resolvedForm, false);
	}
}
