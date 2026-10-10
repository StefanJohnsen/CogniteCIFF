#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

// Independent CIFF v4 wire authoring: no converter or production writer is used.
namespace async3d_fixture
{
    using Bytes = std::vector<std::uint8_t>;

    template<class T> void Append(Bytes& bytes, const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* start = reinterpret_cast<const std::uint8_t*>(&value);
        bytes.insert(bytes.end(), start, start + sizeof(T));
    }

    inline void String(Bytes& bytes, const std::string_view value)
    {
        Append(bytes, static_cast<std::uint32_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }

    inline Bytes Header()
    {
        Bytes bytes;
        Append(bytes, std::uint32_t{0x46443343}); Append(bytes, std::uint32_t{4});
        for (unsigned i = 0; i < 4; ++i) Append(bytes, std::uint64_t{0});
        Append(bytes, std::uint32_t{0}); String(bytes, "[]");
        return bytes;
    }

    inline void Node(Bytes& bytes, const std::string_view name, const std::uint32_t children,
                     const std::uint32_t geometries, const std::array<std::uint8_t, 4> color)
    {
        Append(bytes, std::uint8_t{1}); Append(bytes, children);
        Append(bytes, std::int64_t{0}); Append(bytes, std::int64_t{-1}); Append(bytes, std::int64_t{-1});
        String(bytes, name); Append(bytes, std::uint8_t{1});
        for (const auto component : color) Append(bytes, component);
        Append(bytes, std::uint8_t{0}); Append(bytes, std::uint32_t{0});
        Append(bytes, std::int64_t{0}); Append(bytes, geometries);
    }

    inline void Geometry(Bytes& bytes, const std::uint8_t type)
    {
        Append(bytes, std::int64_t{-1}); Append(bytes, std::uint8_t{0}); Append(bytes, type);
    }

    inline void Point(Bytes& bytes, const double x, const double y, const double z)
    {
        Append(bytes, x); Append(bytes, y); Append(bytes, z);
    }

    inline void Mesh(Bytes& bytes, const double shift = 0, const bool invalid = false, const bool degenerate = false)
    {
        Geometry(bytes, 3);
        Append(bytes, std::uint32_t{1}); Append(bytes, std::uint32_t{129});
        Append(bytes, std::uint32_t{4}); Append(bytes, std::uint32_t{2}); Append(bytes, std::uint8_t{0});
        Point(bytes, shift, 0, 0); Point(bytes, shift + 2, 0, 0);
        Point(bytes, shift, 3, 0); Point(bytes, shift, 0, 4);
        for (const auto index : std::array<std::uint32_t, 6>{0, 1, degenerate ? 0U : 2U, 0, 3, invalid ? 99U : (degenerate ? 0U : 1U)})
            Append(bytes, index);
    }

    inline void Box(Bytes& bytes, const double shift, const double angle = 0)
    {
        Geometry(bytes, 5); Append(bytes, angle);
        Point(bytes, 2, 3, 4); Point(bytes, shift, 10, 2); Point(bytes, 0, 0, 1);
    }

    inline void PermutedMesh(Bytes& bytes)
    {
        Geometry(bytes, 3);
        Append(bytes, std::uint32_t{1}); Append(bytes, std::uint32_t{129});
        Append(bytes, std::uint32_t{4}); Append(bytes, std::uint32_t{2}); Append(bytes, std::uint8_t{0});
        Point(bytes, 0, 3, 0); Point(bytes, 0, 0, 0); Point(bytes, 0, 0, 4); Point(bytes, 2, 0, 0);
        for (const auto index : std::array<std::uint32_t, 6>{1, 2, 3, 1, 3, 0}) Append(bytes, index);
    }

    inline void Cylinder(Bytes& bytes)
    {
        Geometry(bytes, 4); Append(bytes, 1.5);
        Point(bytes, 2, 0, 1); Point(bytes, 2, 3, 5); Append(bytes, std::uint8_t{1});
    }

    inline void Torus(Bytes& bytes)
    {
        Geometry(bytes, 13);
        for (const double value : {3.0, 0.5, 0.2, 3.0}) Append(bytes, value);
        Point(bytes, 5, 2, 3); Point(bytes, 0, 1, 1); Append(bytes, std::uint8_t{1});
    }

    inline void Sphere(Bytes& bytes)
    {
        Geometry(bytes, 14); Append(bytes, 1.25); Point(bytes, 6, 7, 8);
    }

    inline void Dish(Bytes& bytes)
    {
        Geometry(bytes, 17);
        for (const double value : {2.0, 3.0, 1.0}) Append(bytes, value);
        Point(bytes, 4, 2, 1); Point(bytes, 1, 0, 1); Append(bytes, std::uint8_t{1});
    }

    inline void GeneralCylinder(Bytes& bytes)
    {
        Geometry(bytes, 18);
        for (const double value : {2.0, 1.0, 0.1, 0.2, 0.3, 0.4, 0.0, 3.0, 0.25}) Append(bytes, value);
        Point(bytes, 0, 3, 0); Point(bytes, 0, 3, 5); Append(bytes, std::uint8_t{1});
    }

    inline void EmptyPoint(Bytes& bytes)
    {
        Geometry(bytes, 23); Append(bytes, std::uint32_t{0});
    }

    inline void Save(const std::filesystem::path& path, const Bytes& bytes)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!output) throw std::runtime_error("Could not author CIFF fixture: " + path.string());
    }

    inline Bytes Scene()
    {
        auto bytes = Header();
        Node(bytes, "Root", 3, 0, {17, 34, 51, 255});
        Node(bytes, "Native group", 1, 6, {240, 40, 30, 255});
        Box(bytes, 0); Cylinder(bytes); Torus(bytes); Sphere(bytes); Dish(bytes); GeneralCylinder(bytes);
        Node(bytes, "Shared shapes with other material", 0, 3, {30, 80, 240, 128});
        Box(bytes, 25, 0.7); Box(bytes, 50, 1.1); Mesh(bytes);
        Node(bytes, "Meshes", 0, 5, {70, 220, 60, 255});
        Mesh(bytes); Mesh(bytes, 12); PermutedMesh(bytes); Mesh(bytes, 0, false, true); EmptyPoint(bytes);
        Node(bytes, "Empty child", 0, 1, {17, 34, 51, 255}); EmptyPoint(bytes);
        Append(bytes, std::uint8_t{0});
        return bytes;
    }

    inline void WriteAll(const std::filesystem::path& folder)
    {
        std::filesystem::create_directories(folder);
        Save(folder / "scene.ciff", Scene());
        auto tiny = Header(); Node(tiny, "Tiny", 0, 1, {17, 34, 51, 255}); Mesh(tiny);
        Append(tiny, std::uint8_t{0}); Save(folder / "tiny.ciff", tiny);
        auto empty = Header(); Node(empty, "Empty", 0, 2, {17, 34, 51, 255});
        EmptyPoint(empty); Mesh(empty, 0, false, true); Append(empty, std::uint8_t{0}); Save(folder / "empty.ciff", empty);
        auto invalid = Header(); Node(invalid, "Invalid", 0, 2, {17, 34, 51, 255});
        Mesh(invalid); Mesh(invalid, 1, true); Append(invalid, std::uint8_t{0}); Save(folder / "invalid.ciff", invalid);
    }

    // Manual performance corpus, excluded from routine regression runs. Native
    // torus and snout ratios vary; other native shapes and the second pass share
    // forms while retaining separate node materials and world transforms.
    inline void WritePrimitiveStress(const std::filesystem::path& path)
    {
        constexpr unsigned variants = 300;
        auto bytes = Header();
        Node(bytes, "Native primitive performance fixture", 2 * variants, 0, {17, 34, 51, 255});
        for (unsigned pass = 0; pass < 2; ++pass)
            for (unsigned index = 0; index < variants; ++index)
            {
                const auto offset = static_cast<double>(index * 12 + pass * 4000);
                const auto ratio = static_cast<double>(index) / variants;
                Node(bytes, "Native set " + std::to_string(pass) + "/" + std::to_string(index), 0, 6,
                     pass == 0 ? std::array<std::uint8_t, 4>{240, 40, 30, 255} : std::array<std::uint8_t, 4>{30, 80, 240, 128});
                Box(bytes, offset, ratio);
                Geometry(bytes, 4); Append(bytes, 1.0 + ratio);
                Point(bytes, offset, 0, 0); Point(bytes, offset, 0, 5); Append(bytes, std::uint8_t{1});
                Geometry(bytes, 13);
                for (const double value : {2.0 + ratio, 0.4, 0.0, 3.0}) Append(bytes, value);
                Point(bytes, offset, 10, 0); Point(bytes, 0, 0, 1); Append(bytes, std::uint8_t{1});
                Geometry(bytes, 14); Append(bytes, 1.0 + ratio); Point(bytes, offset, 20, 0);
                Geometry(bytes, 17);
                for (const double value : {2.0, 3.0, 1.0}) Append(bytes, value);
                Point(bytes, offset, 30, 0); Point(bytes, 0, 0, 1); Append(bytes, std::uint8_t{1});
                Geometry(bytes, 18);
                for (const double value : {2.0, 0.5 + ratio, 0.1, 0.2, 0.3, 0.4, 0.0, 3.0, 0.25}) Append(bytes, value);
                Point(bytes, offset, 40, 0); Point(bytes, offset, 40, 5); Append(bytes, std::uint8_t{1});
            }
        Append(bytes, std::uint8_t{0}); Save(path, bytes);
    }
}
